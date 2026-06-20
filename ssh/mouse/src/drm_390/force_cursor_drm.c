/**
 * force_cursor_drm_preload.c — inject a hardware cursor plane into MPC
 * by piggy-backing on MPC's own drmModeAtomicCommit() calls.
 *
 * Why this approach (see drm_data_bundle.txt from the device):
 *   - MPC opens /dev/dri/card1 and holds DRM master. A second/standalone
 *     process can't do atomic commits against an active CRTC on a card
 *     that already has a master (EACCES), and dumb-buffer allocation can
 *     also be refused outside the master's context on this driver
 *     (observed as ENOSYS after reboot). Both errors are the same root
 *     cause from two different ioctls.
 *   - MPC already links libdrm and calls drmModeAtomicCommit /
 *     drmModeAtomicAddProperty / drmModeAtomicAlloc itself (confirmed via
 *     `strings /usr/bin/MPC`) — it has its own JUCE DrmCursor code, but
 *     plane id=35 (the Cursor-type plane, confirmed via planeinfo/
 *     /sys/kernel/debug/dri/1/state) sits completely idle: crtc=(null),
 *     fb=0. MPC drives plane 33 (Primary) only.
 *   - So: don't fight master ownership and don't issue our own commits.
 *     Hook drmModeAtomicCommit, and right before MPC's own request goes
 *     to the kernel, splice in the property set that points plane 35 at
 *     our cursor framebuffer. It rides MPC's own master-authorized
 *     commit, so there is nothing to be rejected.
 *
 * Coordinate / rotation handling is intentionally NOT touched here.
 * cursor_x/cursor_y bookkeeping (axis mapping, clamping, rate) is taken
 * verbatim from the already-correct force_cursor_v14.c input_monitor —
 * this file only adds the missing piece: actually getting a buffer onto
 * a plane the compositor will draw.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "mouse_cursor_offset.h" /* cursor_data[4096], 64x64 ARGB8888 */

#define TAG "[DRMCUR v14]"

/* ----------------------------------------------------------------
 * Hardware constants — verified from planeinfo / dri debugfs state
 * ---------------------------------------------------------------- */
#define PLANE_ID        35      /* type=Cursor, confirmed idle (fb=0) */
#define CRTC_ID         37
#define PROP_FB_ID      17
#define PROP_CRTC_ID    20
#define PROP_CRTC_X     13
#define PROP_CRTC_Y     14
#define PROP_CRTC_W     15
#define PROP_CRTC_H     16
#define PROP_SRC_X      9
#define PROP_SRC_Y      10
#define PROP_SRC_W      11
#define PROP_SRC_H      12

#define SCREEN_W        800     /* physical portrait buffer, unchanged */
#define SCREEN_H        1280
#define CURSOR_W        64
#define CURSOR_H        64
#define CLICK_OFFSET_X  0
#define CLICK_OFFSET_Y  27
#define CONF_FILE       "/etc/force_cursor.conf"

/* v12: how long to sleep between rescans while waiting for a mouse to
 * show up (no mouse plugged in at all yet, or the previous one just
 * vanished). Cheap enough to be responsive, infrequent enough not to
 * matter -- this is a few /proc/bus/input/devices reads + a handful
 * of open()/ioctl()/close() per second at most, only while a mouse is
 * actually missing. */
#define MOUSE_RESCAN_INTERVAL_MS 500

/* v8: single-instance guard.
 *
 * The v7 logs caught the real bug: TWO copies of this library were
 * alive at once (almost certainly an old MPC process that never fully
 * exited still sitting in memory alongside the new one), each with
 * its OWN input-reading thread and its OWN /dev/uinput virtual touch
 * device. Both copies saw the same physical button press and each
 * independently emitted a full, valid down+up pair for it -- two
 * genuinely separate touch sequences landing on MPC for one physical
 * click. That's the "press, then immediately release" / double-fire
 * symptom; it is NOT switch bounce, and a time-based debounce filter
 * would not have fixed it (it would just as happily eat a real fast
 * double-tap as it ate this).
 *
 * Fix: take an exclusive, non-blocking flock() on a well-known file
 * as the very first thing the constructor does. If we get it, we're
 * the only live instance and proceed as normal. If we don't, another
 * instance already holds it and is actively driving input -- we log
 * that fact loudly and skip starting the input thread / uinput device
 * / DRM cursor-plane splicing entirely, rather than racing a second
 * pipeline alongside the real one. We deliberately never close this
 * fd: flock()s are released automatically by the kernel when the
 * holding process exits OR crashes, so there's no stale-lock state to
 * clean up on the next launch -- nothing to reset, nothing that can
 * get stuck "locked" by a process that's already gone. */
#define LOCK_FILE       "/tmp/force_cursor.lock"
static int is_singleton_owner = 0;

/* v7: diagnostic-only threshold. If a fresh press lands within this
 * many ms of the previous release, log it as a likely switch-bounce /
 * ghost-click candidate. This does NOT suppress or alter anything --
 * it's purely so the log can confirm or rule out "the physical button
 * is bouncing" before we touch any click-handling logic. */
#define BOUNCE_WINDOW_MS 120

/* ----------------------------------------------------------------
 * Logging
 *
 * v10: off by default. Logging (and the diagnostic-only timestamp /
 * bounce-detector work that only exists to produce log lines) costs
 * real CPU on the per-event hot path -- clock_gettime() + localtime_r()
 * + vsnprintf() on every single touch sample adds up when it's running
 * every frame for hours on an embedded ARM core. Set "log=1" as a
 * third line in /etc/force_cursor.conf to turn it back on for
 * debugging; otherwise none of this work happens at all.
 * ---------------------------------------------------------------- */
static int verbose_logging = 0;
static int log_fd = -1;

static void vlogf(const char *fmt, ...)
{
    if (!verbose_logging || log_fd < 0) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)write(log_fd, buf, strlen(buf));
}

/* ----------------------------------------------------------------
 * Real symbols, resolved lazily
 * ---------------------------------------------------------------- */
static int (*real_open)(const char*, int, ...) = NULL;

typedef struct _drmModeAtomicReq drmModeAtomicReq, *drmModeAtomicReqPtr;
typedef int (*drmModeAtomicCommit_t)(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data);
typedef int (*drmModeAtomicAddProperty_t)(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value);

static drmModeAtomicCommit_t      real_drmModeAtomicCommit      = NULL;
static drmModeAtomicAddProperty_t real_drmModeAtomicAddProperty = NULL;

/* ----------------------------------------------------------------
 * Config / autodetect
 *
 * v11: rewritten as a proper key=value parser (device=, speed=,
 * logging=) instead of positional line numbers -- order no longer
 * matters and the file is self-documenting either way. If the file
 * doesn't exist at all, we write out a default one (using whatever
 * autodetect_mouse() finds for "device=", or a placeholder comment
 * if nothing was found) so there's always a real file on disk to
 * look at/edit afterward, rather than silently running on
 * compiled-in defaults forever.
 * ---------------------------------------------------------------- */
static float  rate      = 2.0f;
static char  *mouse_dev = NULL;

/* ----------------------------------------------------------------
 * v12: kernel-verified "is this actually a mouse" check.
 *
 * Background: this device has exactly two USB ports and (per the
 * deployment's own assumption) only ever has one mouse plugged in at
 * a time -- but WHICH /dev/input/eventN that mouse lands on is not
 * fixed. It depends on kernel enumeration order at boot and on
 * exactly what else is plugged in when. One report from the field
 * had a wireless mouse's receiver land on a completely different
 * event node than the wired mouse used at install time -- so a
 * "device=" path written into the config at first boot, or compiled
 * in as a default, is only ever a snapshot, not a promise. A reboot
 * that renumbers devices, or the user swapping to a different mouse,
 * can leave that path pointing at nothing (file doesn't exist), or
 * -- worse -- at some other input node that DOES exist but isn't a
 * mouse at all (the on-screen touch controller, a keyboard, etc).
 *
 * fd_looks_like_mouse() asks the kernel directly via EVIOCGBIT,
 * rather than trusting a config file, a device name, or (the old
 * version of this function) just the presence of a "B: REL=" line in
 * /proc/bus/input/devices:
 *   - REQUIRES both REL_X and REL_Y capability bits. This is the
 *     real defining trait of a mouse, and it's also already a
 *     complete answer to "could the touchscreen masquerade as the
 *     mouse": touch panels report ABSOLUTE coordinates, not relative
 *     motion deltas, so a touch controller has no legitimate reason
 *     to ever set these bits, regardless of which multitouch
 *     protocol it speaks.
 *   - REJECTS anything that also advertises ABS_MT_SLOT (multitouch
 *     protocol B's signature). This is just belt-and-suspenders on
 *     top of the REL_X/REL_Y requirement above, for the unlikely case
 *     of some hybrid/exotic device.
 * Both checks read real capability bits off the device via ioctl, so
 * they hold regardless of what the device is named or which event
 * node it happens to have landed on this boot. */
#define BITS_PER_LONG_LOCAL (sizeof(long) * 8)
#define NBITS_LOCAL(x) ((((x) - 1) / BITS_PER_LONG_LOCAL) + 1)
#define TEST_BIT_LOCAL(bit, arr) \
    (((arr)[(bit) / BITS_PER_LONG_LOCAL] >> ((bit) % BITS_PER_LONG_LOCAL)) & 1UL)

static int fd_looks_like_mouse(int fd)
{
    unsigned long relbits[NBITS_LOCAL(REL_CNT)] = {0};
    unsigned long absbits[NBITS_LOCAL(ABS_CNT)] = {0};

    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0)
        return 0;
    if (!TEST_BIT_LOCAL(REL_X, relbits) || !TEST_BIT_LOCAL(REL_Y, relbits))
        return 0;

    /* This ABS check is independent of the REL result above on
     * purpose -- see the comment block. ioctl failing here just
     * means the device has no ABS capability at all, which is normal
     * for a plain mouse and is NOT a rejection; only an explicit
     * ABS_MT_SLOT bit disqualifies it. */
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) == 0 &&
        TEST_BIT_LOCAL(ABS_MT_SLOT, absbits)) {
        return 0;
    }

    return 1;
}

/* Opens path read-only/non-blocking just long enough to run the
 * capability checks above, then closes it again. The real, long-lived
 * fd that input_monitor actually reads from is opened separately once
 * a path passes this check, so callers never have to special-case
 * "the fd I validated isn't the fd I'm going to use." */
static int is_real_mouse_device(const char *path)
{
    if (!path) return 0;
    int fd = real_open(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) return 0;
    int ok = fd_looks_like_mouse(fd);
    close(fd);
    return ok;
}

static char *autodetect_mouse(void)
{
    FILE *fp = fopen("/proc/bus/input/devices", "r");
    if (!fp) return NULL;
    char line[256], event[32] = {0};
    int has_rel = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "H: Handlers=", 12) == 0) {
            char *ev = strstr(line, "event");
            if (ev) sscanf(ev, "%31s", event);
        }
        if (strncmp(line, "B: REL=", 7) == 0) has_rel = 1;
        if (line[0] == '\n') {
            /* Blank line = end of this device's block in
             * /proc/bus/input/devices. B: REL= just means *some* REL
             * bit is set somewhere in the device's bitmap -- it's a
             * cheap first filter (skip parsing devices that can't
             * possibly qualify), not proof this is a mouse on its
             * own. v12: before trusting a candidate, open it and ask
             * the kernel directly via is_real_mouse_device() (REL_X +
             * REL_Y present, no ABS_MT_SLOT). If that fails -- e.g. a
             * device with only a wheel's REL bit set, or the specific
             * worry that prompted this check, a touch controller that
             * for some reason also exposes unrelated REL capability
             * -- keep scanning instead of returning a false match.
             * Devices are listed in kernel enumeration order, so the
             * first candidate that actually passes is naturally
             * "first/lowest input device that's really a mouse." */
            if (has_rel && event[0]) {
                char path[64];
                snprintf(path, sizeof(path), "/dev/input/%s", event);
                if (is_real_mouse_device(path)) {
                    fclose(fp);
                    return strdup(path);
                }
            }
            event[0] = 0; has_rel = 0;
        }
    }
    fclose(fp);
    return NULL;
}

/* Writes a fresh default conf file to CONF_FILE. dev may be NULL (no
 * mouse found yet at write time) -- in that case "device=" is left
 * blank with an explanatory comment rather than guessing a path that
 * doesn't exist. */
static void write_default_config(const char *dev)
{
    FILE *fp = fopen(CONF_FILE, "w");
    if (!fp) return; /* read-only /etc, etc -- not fatal, just means no
                       * file gets written; compiled-in defaults still apply */
    fprintf(fp,
        "# force_cursor config -- key=value, one per line\n"
        "#\n"
        "# device : input event node for the mouse. Auto-detected on\n"
        "#          first boot by picking the first /dev/input/eventN\n"
        "#          that reports relative-motion (EV_REL) capability --\n"
        "#          i.e. the first thing that looks like a mouse, in\n"
        "#          kernel enumeration order, if more than one is\n"
        "#          plugged in. Edit this line directly to force a\n"
        "#          specific device instead.\n"
        "#          NOTE (v12): this is only ever a first guess. If this\n"
        "#          device later disappears -- unplugged, swapped for a\n"
        "#          different mouse, or simply renumbered by a reboot --\n"
        "#          force_cursor automatically falls back to scanning\n"
        "#          for whatever mouse IS currently connected, rather\n"
        "#          than failing. This line does not need to be kept\n"
        "#          up to date by hand.\n");
    if (dev) fprintf(fp, "device=%s\n", dev);
    else     fprintf(fp, "#device=  (none detected at install time -- "
                          "plug in a mouse and set this manually, e.g. "
                          "device=/dev/input/event2)\n");
    fprintf(fp,
        "#\n"
        "# speed  : mouse sensitivity multiplier, 0.1 - 5.0\n"
        "speed=2\n"
        "#\n"
        "# logging: 0 = off (default), 1 = on. Writes diagnostic detail\n"
        "#          to /tmp/force_cursor_drm.log. Leave off for normal\n"
        "#          use -- it costs real CPU on every mouse event.\n"
        "logging=0\n");
    fclose(fp);
}

static void read_config(void)
{
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp) {
        /* No config on disk at all -- create one now so there's
         * always a real, editable file afterward instead of relying
         * on compiled-in defaults forever. Detect a mouse first so
         * the freshly-written file already has a working device= if
         * possible, rather than leaving it blank when we didn't have
         * to. */
        char *detected = autodetect_mouse();
        write_default_config(detected);
        if (detected) {
            mouse_dev = detected; /* adopt directly -- no need to
                                    * re-open and re-parse the file we
                                    * just wrote */
        }
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and blank lines. */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0'; /* strip trailing newline */

        if (strcmp(key, "device") == 0) {
            if (val[0]) {
                free(mouse_dev);
                mouse_dev = strdup(val);
            }
        } else if (strcmp(key, "speed") == 0) {
            float v = strtof(val, NULL);
            if (v >= 0.1f && v <= 5.0f) rate = v;
        } else if (strcmp(key, "logging") == 0) {
            verbose_logging = (strtol(val, NULL, 10) != 0);
        }
    }
    fclose(fp);
}

/* ----------------------------------------------------------------
 * Cursor position — same bookkeeping as force_cursor_v14.c.
 * Axis mapping / clamping intentionally left exactly as-is (user
 * confirmed this part is already correct).
 * ---------------------------------------------------------------- */
static volatile int cursor_x = SCREEN_W - 1;
static volatile int cursor_y = SCREEN_H - 1;
static int touch_down = 0;
static int uinput_fd  = -1;

/* v6: tap-tolerance / "slop" filtering.
 *
 * Real mice are never perfectly still -- sensor jitter produces tiny
 * REL events even when the user thinks they're holding the button
 * down motionless. Every one of those was being faithfully forwarded
 * as a new touch position while held, which means a normal click
 * could arrive at MPC as press-at-(x,y) -> wobble to (x+1,y) ->
 * release-at-(x+1,y). Most touch UIs treat ANY movement beyond a
 * small threshold between press and release as the start of a
 * drag/pan, not a tap -- which cancels the pressed visual state and
 * never fires the click action. That matches "blinks like the click
 * was cancelled" exactly, and explains why it's item-dependent: some
 * widgets have a tighter movement tolerance than others.
 *
 * Fix: snap the reported touch position to the press location until
 * real movement exceeds TAP_TOLERANCE_PX, exactly like a real
 * touchscreen's own debounce/slop filter would. Once exceeded for a
 * given press, tracking switches to live position for the rest of
 * that press (so genuine drags still work). */
#define TAP_TOLERANCE_PX 4
static int press_x = 0, press_y = 0;
static int past_tap_tolerance = 0;
static int last_sent_x = 0, last_sent_y = 0; /* last (x,y) we actually wrote
    to uinput while held -- lets us suppress redundant re-sends of an
    unchanged position, so a held-but-stationary press looks like a real
    finger (silent) instead of a ~60Hz+ stream of repeated down-events. */

/* Computes where we should report the touch right now, applying the
 * tolerance snap described above. Updates past_tap_tolerance as a
 * side effect once real movement is detected. */
static void get_touch_send_pos(int *out_x, int *out_y)
{
    int cx = cursor_x + CLICK_OFFSET_X;
    int cy = cursor_y + CLICK_OFFSET_Y;
    if (cx > SCREEN_W - 1) cx = SCREEN_W - 1;
    if (cy > SCREEN_H - 1) cy = SCREEN_H - 1;

    if (!past_tap_tolerance) {
        int dx = cx - press_x, dy = cy - press_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > TAP_TOLERANCE_PX || dy > TAP_TOLERANCE_PX)
            past_tap_tolerance = 1;
    }

    if (past_tap_tolerance) { *out_x = cx;      *out_y = cy; }
    else                    { *out_x = press_x; *out_y = press_y; }
}

/* ----------------------------------------------------------------
 * v14: adopt-or-create for uinput virtual devices.
 *
 * The problem: if MPC's launcher does execve() after our thread has
 * already called UI_DEV_CREATE, the thread is killed by exec while
 * the kernel-side uinput device lives on as an orphan (its creating
 * fd is gone, so UI_DEV_DESTROY can never be called on it from the
 * outside). The next exec stage's constructor wins the flock()
 * cleanly (the old process IS gone), then the new input_monitor
 * creates a second pair of identical virtual devices alongside the
 * still-alive orphans -- giving you input4+input5 and input6+input7
 * with the same names.
 *
 * Fix: before calling UI_DEV_CREATE, scan /proc/bus/input/devices
 * for an existing entry matching our vendor:product IDs. If one is
 * found, open its /dev/input/eventN fd and USE it directly -- the
 * orphan becomes our device. If not found (clean boot, or first ever
 * run), fall through to UI_DEV_CREATE as before.
 *
 * Why eventN and not re-creating via /dev/uinput: we can't call
 * UI_DEV_DESTROY on an fd we didn't create, and we don't need to --
 * the orphan's kernel state is intact and correct; we just need a
 * read/write fd into it. For the touch device (send_touch writes to
 * its fd) we open O_WRONLY; same for the wheel device. The eventN fd
 * is a perfectly valid write target for injecting input_events.
 *
 * Caller passes vendor and product as uint16_t (matching the values
 * in uinput_setup.id), and gets back either an adopted eventN fd or
 * -1 if no orphan was found. */
static int find_orphan_uinput(uint16_t vendor, uint16_t product)
{
    FILE *fp = fopen("/proc/bus/input/devices", "r");
    if (!fp) return -1;

    char line[256];
    char event[32] = {0};
    int  found_vendor  = 0;
    int  found_product = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* "I: Bus=0003 Vendor=1234 Product=5678 Version=0000" */
        if (strncmp(line, "I: ", 3) == 0) {
            /* Reset per-device state at every new identity line. */
            event[0]      = 0;
            found_vendor  = 0;
            found_product = 0;

            unsigned int v = 0, p = 0;
            /* sscanf is fine here -- format is fixed kernel output */
            if (sscanf(line, "I: Bus=%*x Vendor=%x Product=%x", &v, &p) == 2) {
                found_vendor  = (v == vendor);
                found_product = (p == product);
            }
        }

        if (strncmp(line, "H: Handlers=", 12) == 0) {
            char *ev = strstr(line, "event");
            if (ev) sscanf(ev, "%31s", event);
        }

        if (line[0] == '\n') {
            /* End of this device block. */
            if (found_vendor && found_product && event[0]) {
                char path[64];
                snprintf(path, sizeof(path), "/dev/input/%s", event);
                fclose(fp);
                int fd = real_open(path, O_WRONLY | O_NONBLOCK, 0);
                if (fd >= 0) {
                    vlogf(TAG " adopted orphan uinput device "
                          "vendor=%04x product=%04x at %s fd=%d\n",
                          vendor, product, path, fd);
                }
                return fd; /* -1 if open failed; caller falls through
                            * to UI_DEV_CREATE in that case */
            }
            event[0]     = 0;
            found_vendor = found_product = 0;
        }
    }
    fclose(fp);
    return -1;
}

/* ----------------------------------------------------------------
 * uinput touch injection — unchanged
 * ---------------------------------------------------------------- */
static int init_uinput(void)
{
    int fd = real_open("/dev/uinput", O_WRONLY | O_NONBLOCK, 0);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT,  EV_ABS);
    ioctl(fd, UI_SET_EVBIT,  EV_KEY);
    ioctl(fd, UI_SET_EVBIT,  EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    struct uinput_abs_setup ax = { .code = ABS_X, .absinfo = { .maximum = SCREEN_W - 1 } };
    struct uinput_abs_setup ay = { .code = ABS_Y, .absinfo = { .maximum = SCREEN_H - 1 } };
    ioctl(fd, UI_ABS_SETUP, &ax);
    ioctl(fd, UI_ABS_SETUP, &ay);
    struct uinput_setup s = {0};
    snprintf(s.name, UINPUT_MAX_NAME_SIZE, "Virtual Mouse Touch");
    s.id.bustype = BUS_USB; s.id.vendor = 0x1234; s.id.product = 0x5678;
    if (ioctl(fd, UI_DEV_SETUP, &s) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* ----------------------------------------------------------------
 * v13: uinput scroll-wheel passthrough -- a SEPARATE virtual device
 * from the touch one above, on purpose.
 *
 * MPC links libinput directly (confirmed via
 * `strings /usr/bin/MPC | grep -i scroll` showing
 * libinput_event_pointer_get_scroll_value / _v120, alongside JUCE
 * Viewport/ScrollBar/DragToScrollListener and a literal
 * "Mouse Wheel Move" string) -- there's a real, wired-up scroll
 * code path sitting idle for lack of anything emitting a scroll
 * axis, not dead/stripped functionality.
 *
 * Why not just add REL_WHEEL onto the existing touch device above:
 * that device's whole job is reporting ABS_X/ABS_Y + BTN_TOUCH, and
 * no real hardware mixes that with relative wheel motion -- it's an
 * unnecessary gamble against udev's device-classification rules for
 * something that already works correctly today.
 *
 * Why not a wheel-only device either: udev's own mouse-classification
 * heuristic (what decides whether libinput's udev backend bothers
 * opening a device at all) keys off REL_X/REL_Y axis presence, not
 * REL_WHEEL alone -- a wheel-only device is a shape nothing real
 * presents and risks being silently skipped before a single event of
 * ours ever reaches libinput.
 *
 * So: advertise a normal mouse's capability set (REL_X/REL_Y/buttons)
 * so this gets tagged and watched exactly like a real mouse would --
 * but never WRITE motion or button events on this fd. Only
 * REL_WHEEL/REL_HWHEEL ever get sent here (see send_wheel() below).
 * Advertised capability and exercised capability are independent;
 * this device's actual behavior is "scroll only," its shape is just
 * "ordinary mouse" so nothing upstream has a reason to treat it
 * specially or skip it. Motion and clicks keep going through the
 * existing touch device above, completely unchanged -- this can't
 * introduce the "two mice" double-input problem (v8) because it never
 * emits the events that caused that in the first place. */
static int wheel_uinput_fd = -1;

static int init_wheel_uinput(void)
{
    int fd = real_open("/dev/uinput", O_WRONLY | O_NONBLOCK, 0);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT,  EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);      /* advertised only, never sent */
    ioctl(fd, UI_SET_RELBIT, REL_Y);      /* advertised only, never sent */
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(fd, UI_SET_EVBIT,  EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);   /* advertised only, never sent */
    ioctl(fd, UI_SET_EVBIT,  EV_SYN);
    struct uinput_setup s = {0};
    snprintf(s.name, UINPUT_MAX_NAME_SIZE, "Virtual Mouse Wheel");
    s.id.bustype = BUS_USB; s.id.vendor = 0x1234; s.id.product = 0x5679;
    if (ioctl(fd, UI_DEV_SETUP, &s) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* value is forwarded unscaled -- "rate" is a touch-position
 * sensitivity multiplier and has no business touching discrete wheel
 * notches (typically +/-1 per detent, occasionally more on a fast
 * flick); a real mouse's own wheel delta is exactly what should land
 * on the other end. */
static void send_wheel(int fd, int value, int horizontal)
{
    struct input_event ev[2] = {0};
    ev[0].type = EV_REL;
    ev[0].code = horizontal ? REL_HWHEEL : REL_WHEEL;
    ev[0].value = value;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    (void)write(fd, ev, sizeof(ev));
}

/* ----------------------------------------------------------------
 * Timestamps for click logging.
 * mono_ms() is what we use for bounce-window math (immune to clock
 * jumps); wall_clock_str() is just so a human reading the log can
 * line a timestamp up against what they saw on screen / on video.
 * ---------------------------------------------------------------- */
static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wall_clock_str(char *buf, size_t buflen)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    int ms = (int)(ts.tv_nsec / 1000000);
    snprintf(buf, buflen, "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

static void send_touch(int fd, int x, int y, int pressed)
{
    struct input_event ev[4] = {0};
    ev[0].type = EV_ABS; ev[0].code = ABS_X; ev[0].value = x;
    ev[1].type = EV_ABS; ev[1].code = ABS_Y; ev[1].value = y;
    ev[2].type = EV_KEY; ev[2].code = BTN_TOUCH; ev[2].value = pressed;
    ev[3].type = EV_SYN; ev[3].code = SYN_REPORT;
    (void)write(fd, ev, sizeof(ev));

    /* v10: everything below this point exists purely to produce log
     * output (timestamps, bounce-window detection, per-event counters
     * used only in the log line). clock_gettime() + localtime_r() +
     * vsnprintf() on every single touch sample -- including every drag
     * sample while held, not just clicks -- is real, measurable CPU
     * cost on an embedded core when it runs for hours. Skip all of it
     * outright when logging is off, rather than just suppressing the
     * final write(). */
    if (!verbose_logging) return;

    long long t = mono_ms();
    char wall[16];
    wall_clock_str(wall, sizeof(wall));

    /* Ghost-click / switch-bounce detector. This is diagnostic only --
     * it does not suppress or alter the event in any way. A fresh
     * press landing very soon after the previous release is the
     * classic signature of mechanical contact bounce on the physical
     * button: the kernel hands us two genuinely distinct down/up
     * pairs a few ms apart, and nothing upstream of this function
     * does time-based debouncing (only the spatial tap-tolerance in
     * get_touch_send_pos() above, which doesn't help here since each
     * bounce is its own full press+release at essentially the same
     * position). Flagging it here lets the log confirm or rule this
     * out before we touch any click-handling logic. */
    static long long last_release_ms = -1;
    static long total_presses = 0, total_releases = 0, total_bounce_flags = 0;
    if (pressed) {
        total_presses++;
        if (last_release_ms >= 0 && (t - last_release_ms) < BOUNCE_WINDOW_MS) {
            total_bounce_flags++;
            vlogf(TAG " [%s mono=%lld] *** possible ghost click: press "
                  "%lldms after previous release (< %dms window) -- "
                  "bounce_flags_total=%ld ***\n",
                  wall, t, t - last_release_ms, BOUNCE_WINDOW_MS, total_bounce_flags);
        }
    } else {
        total_releases++;
        last_release_ms = t;
    }

    vlogf(TAG " [%s mono=%lld] send_touch x=%d y=%d pressed=%d "
          "(presses=%ld releases=%ld)\n",
          wall, t, x, y, pressed, total_presses, total_releases);
}

/* ----------------------------------------------------------------
 * Cursor GEM buffer + FB, created lazily on the first
 * drmModeAtomicCommit() we see, using MPC's own already-open,
 * already-master fd. Dumb-buffer creation is not a master-only
 * operation, so this works even though we never call SET_MASTER.
 * ---------------------------------------------------------------- */
static int      cursor_fb_ready  = 0;
static uint32_t cursor_fb_ids[2] = {0, 0}; /* double-buffered, alternated each commit */
static int      cursor_fb_index  = 0;      /* which of the two is "current" */
static int      cursor_drm_fd    = -1;     /* the fd these fbs belong to */

/* v5: serializes everything above, since the periodic keepalive commit
 * (added below, fired from the input thread) and MPC's own commit
 * (handled in our drmModeAtomicCommit hook, MPC's thread) can now both
 * touch this state concurrently. Plain mutex, no recursion needed --
 * neither call site calls back into the other. */
static pthread_mutex_t cursor_state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* v5: zpos auto-discovery.
 *
 * Plane 35 is type=Cursor by name, but that's a labeling convention --
 * it isn't guaranteed to be hardware-topmost on every driver. If this
 * plane exposes a "zpos" property (very common on Rockchip VOP-based
 * drivers, since several overlay planes share the same hardware and
 * need explicit ordering), we look it up BY NAME via the standard
 * OBJ_GETPROPERTIES/GETPROPERTY ioctls (read-only, safe to call any
 * time) and pin it to its maximum value on every commit. This is the
 * most likely fix for "not visible on some views" -- a view that uses
 * another overlay plane at a higher zpos would otherwise draw right
 * over our cursor regardless of how correct its FB_ID/CRTC_X/Y are. */
static int      zpos_checked   = 0;
static uint32_t zpos_prop_id   = 0;   /* 0 = not found / not supported */
static uint64_t zpos_max_value = 0;

static void discover_zpos_prop(int fd, uint32_t plane_id)
{
    zpos_checked = 1; /* only ever try once per fd -- if it's not there,
                        * it's not there, no point retrying every commit */

    struct drm_mode_obj_get_properties op;
    memset(&op, 0, sizeof(op));
    op.obj_id   = plane_id;
    op.obj_type = DRM_MODE_OBJECT_PLANE;

    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0) {
        vlogf(TAG " OBJ_GETPROPERTIES(count) on plane %u failed errno=%d (%s)\n",
              plane_id, errno, strerror(errno));
        return;
    }
    uint32_t count = op.count_props;
    if (count == 0 || count > 128) {
        vlogf(TAG " plane %u reports unexpected count_props=%u, skipping zpos lookup\n",
              plane_id, count);
        return;
    }

    uint32_t *prop_ids  = calloc(count, sizeof(uint32_t));
    uint64_t *prop_vals = calloc(count, sizeof(uint64_t));
    if (!prop_ids || !prop_vals) { free(prop_ids); free(prop_vals); return; }

    op.props_ptr       = (uintptr_t)prop_ids;
    op.prop_values_ptr = (uintptr_t)prop_vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0) {
        vlogf(TAG " OBJ_GETPROPERTIES(fetch) on plane %u failed errno=%d (%s)\n",
              plane_id, errno, strerror(errno));
        free(prop_ids); free(prop_vals);
        return;
    }

    for (uint32_t i = 0; i < op.count_props; i++) {
        struct drm_mode_get_property gp;
        memset(&gp, 0, sizeof(gp));
        gp.prop_id = prop_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;
        if (strcmp(gp.name, "zpos") == 0) {
            zpos_prop_id = prop_ids[i];
            /* RANGE properties report [min, max] in values[]; fetch
             * again now that we know count_values, to read them. */
            if (gp.count_values >= 2) {
                uint64_t vals[2] = {0, 0};
                gp.values_ptr = (uintptr_t)vals;
                if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) == 0)
                    zpos_max_value = vals[1];
            }
            vlogf(TAG " plane %u has zpos prop_id=%u max=%llu -- will pin "
                  "cursor to max zpos every commit\n",
                  plane_id, zpos_prop_id, (unsigned long long)zpos_max_value);
            break;
        }
    }
    if (!zpos_prop_id)
        vlogf(TAG " plane %u: no zpos property found among %u props -- "
              "relying on driver's default cursor-plane ordering\n",
              plane_id, count);

    free(prop_ids);
    free(prop_vals);
}

static int create_one_cursor_fb(int fd, uint32_t *out_fb_id)
{
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof(cd));
    cd.width = CURSOR_W;
    cd.height = CURSOR_H;
    cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        vlogf(TAG " DRM_IOCTL_MODE_CREATE_DUMB failed errno=%d (%s)\n", errno, strerror(errno));
        return 0;
    }

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        vlogf(TAG " DRM_IOCTL_MODE_MAP_DUMB failed errno=%d (%s)\n", errno, strerror(errno));
        return 0;
    }

    void *p = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p == MAP_FAILED) {
        vlogf(TAG " mmap cursor gem failed errno=%d (%s)\n", errno, strerror(errno));
        return 0;
    }

    /* cursor_data is 64x64 tightly packed; cd.pitch may have padding,
     * so copy row by row instead of one big memcpy. */
    for (uint32_t row = 0; row < CURSOR_H; row++) {
        memcpy((uint8_t*)p + row * cd.pitch,
               (const uint8_t*)cursor_data + row * CURSOR_W * 4,
               CURSOR_W * 4);
    }
    munmap(p, cd.size);

    /* drmModeAddFB2 with DRM_FORMAT_ARGB8888 to match the AR24 format
     * MPC's own cursor framebuffers already use (see framebuffer[45]
     * etc. in /sys/kernel/debug/dri/1/framebuffer). Done via raw
     * ioctl so we don't need the libdrm symbol resolved separately. */
    struct drm_mode_fb_cmd2 fbcmd;
    memset(&fbcmd, 0, sizeof(fbcmd));
    fbcmd.width  = CURSOR_W;
    fbcmd.height = CURSOR_H;
    fbcmd.pixel_format = 0x34325241; /* DRM_FORMAT_ARGB8888, 'AR24' */
    fbcmd.handles[0] = cd.handle;
    fbcmd.pitches[0] = cd.pitch;
    fbcmd.offsets[0] = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fbcmd) < 0) {
        vlogf(TAG " DRM_IOCTL_MODE_ADDFB2 failed errno=%d (%s)\n", errno, strerror(errno));
        return 0;
    }

    *out_fb_id = fbcmd.fb_id;
    vlogf(TAG " cursor fb created: fd=%d handle=%u pitch=%u fb_id=%u\n",
        fd, cd.handle, cd.pitch, fbcmd.fb_id);
    return 1;
}

/* Caller must hold cursor_state_mutex. */
static int ensure_cursor_fb(int fd)
{
    if (cursor_fb_ready && cursor_drm_fd == fd) {
        return 1;
    }
    if (cursor_fb_ready && cursor_drm_fd != fd) {
        /* MPC re-opened card1 under a new fd (e.g. reconnect) -- the
         * old fb_ids are tied to the old fd's DRM file and are no
         * longer valid. Rebuild against the new fd. */
        vlogf(TAG " drm fd changed (%d -> %d), recreating cursor fbs\n", cursor_drm_fd, fd);
        cursor_fb_ready = 0;
        cursor_fb_ids[0] = cursor_fb_ids[1] = 0;
        zpos_checked = 0; /* re-check on the new fd too */
    }

    /* Create TWO identical cursor framebuffers and alternate between
     * their fb_ids on every commit. The content never changes, but
     * presenting a genuinely different fb_id each time forces a real
     * buffer swap rather than repeatedly re-presenting the same
     * fb_id -- this is what the primary plane already does on every
     * MPC frame, and the cursor plane's hardware/driver may require
     * the same alternation to latch on every vblank instead of every
     * other one. */
    if (!create_one_cursor_fb(fd, &cursor_fb_ids[0])) {
        return 0;
    }
    if (!create_one_cursor_fb(fd, &cursor_fb_ids[1])) {
        return 0;
    }

    cursor_drm_fd = fd;
    cursor_fb_index = 0;
    cursor_fb_ready = 1;
    vlogf(TAG " cursor double-buffer ready: fb_ids=[%u, %u]\n",
        cursor_fb_ids[0], cursor_fb_ids[1]);

    if (!zpos_checked)
        discover_zpos_prop(fd, PLANE_ID);

    return 1;
}

/* ----------------------------------------------------------------
 * Input thread — reads the mouse, updates cursor_x/cursor_y, sends
 * touch via uinput. No plane/DRM calls here; that all happens inside
 * the drmModeAtomicCommit hook below, riding MPC's own commit cadence
 * instead of issuing independent commits on our own timer.
 * ---------------------------------------------------------------- */
static pthread_t input_thread;
static int       input_running = 0;

/* defined later in the file, after ensure_cursor_fb/cursor_state_mutex;
 * forward-declared here so input_monitor (above it) can call it for
 * the periodic keepalive commit. */
static int do_cursor_commit(int fd);

/* v12: blocks (sleeping MOUSE_RESCAN_INTERVAL_MS between attempts)
 * until either a currently-connected, kernel-verified mouse is found
 * and opened, or input_running is cleared out from under us by
 * shutdown. This is the one place that knows how to "go find a
 * mouse" -- used both for the very first open at thread start and for
 * runtime hot-swap recovery inside the read loop below, so there is a
 * single code path to reason about instead of two that could drift
 * apart. autodetect_mouse() already runs every candidate through
 * is_real_mouse_device() internally, so a fd returned from here is
 * already kernel-verified, not just "a path that exists." */
static int acquire_mouse_fd(void)
{
    int fd = -1;
    while (input_running && fd < 0) {
        char *dev = autodetect_mouse();
        if (dev) {
            fd = real_open(dev, O_RDONLY | O_NONBLOCK, 0);
            if (fd < 0) {
                vlogf(TAG " open(%s) failed errno=%d (%s) -- still "
                      "scanning\n", dev, errno, strerror(errno));
            } else {
                vlogf(TAG " mouse acquired: %s  rate=%.2f\n", dev, rate);
            }
            free(dev);
        }
        if (fd < 0) usleep(MOUSE_RESCAN_INTERVAL_MS * 1000);
    }
    return fd;
}

static void *input_monitor(void *arg)
{
    (void)arg;

    /* mouse_dev is whatever read_config() / the constructor came up
     * with as a FIRST GUESS (the config's "device=" line, or whatever
     * autodetect_mouse() found at startup if the config didn't have
     * one) -- it's allowed to be NULL, stale, or simply wrong; see
     * fd_looks_like_mouse() for why we don't just trust it blindly.
     * Ownership of the string moves to this thread either way, so
     * this is the only place that frees it. */
    char *first_guess = mouse_dev;
    mouse_dev = NULL;

    int fd = -1;
    if (first_guess) {
        if (is_real_mouse_device(first_guess)) {
            fd = real_open(first_guess, O_RDONLY | O_NONBLOCK, 0);
            if (fd < 0) {
                vlogf(TAG " %s passed the mouse check but open() failed "
                      "errno=%d (%s) -- scanning for another\n",
                      first_guess, errno, strerror(errno));
            } else {
                vlogf(TAG " opening mouse: %s  rate=%.2f\n", first_guess, rate);
            }
        } else {
            vlogf(TAG " %s is not (or is no longer) a connected mouse -- "
                  "scanning for one\n", first_guess);
        }
        free(first_guess);
    }

    /* v12: this covers BOTH "the configured/detected device turned
     * out to be wrong" above AND "there was no device= at all and
     * autodetect_mouse() found nothing back in the constructor" (e.g.
     * the mouse simply isn't plugged in yet at boot) -- either way we
     * now wait here instead of the old behavior of giving up for the
     * rest of the process's life. */
    if (fd < 0) fd = acquire_mouse_fd();
    if (fd < 0) {
        /* Only reachable if input_running was cleared (shutdown)
         * before any mouse ever turned up. */
        return NULL;
    }

    /* v14: try to adopt an orphaned virtual device left by a prior
     * exec stage before creating a new one. vendor/product match the
     * values in init_uinput() / init_wheel_uinput() below. If no
     * orphan exists, find_orphan_uinput() returns -1 and we fall
     * through to the normal create path. */
    uinput_fd = find_orphan_uinput(0x1234, 0x5678);
    if (uinput_fd < 0) uinput_fd = init_uinput();
    if (uinput_fd < 0) vlogf(TAG " WARNING: uinput init failed\n");
    else               vlogf(TAG " uinput ready\n");

    wheel_uinput_fd = find_orphan_uinput(0x1234, 0x5679);
    if (wheel_uinput_fd < 0) wheel_uinput_fd = init_wheel_uinput();
    if (wheel_uinput_fd < 0) vlogf(TAG " WARNING: wheel uinput init failed\n");
    else                     vlogf(TAG " wheel uinput ready\n");

    struct input_event ev;
    while (input_running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    cursor_y -= (int)(ev.value * rate);
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_y > SCREEN_H - 1) cursor_y = SCREEN_H - 1;
                } else if (ev.code == REL_Y) {
                    cursor_x += (int)(ev.value * rate);
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_x > SCREEN_W - 1) cursor_x = SCREEN_W - 1;
                } else if (ev.code == REL_WHEEL || ev.code == REL_HWHEEL) {
                    /* v13: straight passthrough to the dedicated wheel
                     * device -- see init_wheel_uinput() above for why
                     * this doesn't touch cursor_x/y or the touch path
                     * at all. */
                    if (wheel_uinput_fd >= 0) {
                        send_wheel(wheel_uinput_fd, ev.value,
                                   ev.code == REL_HWHEEL);
                    }
                }
                if (touch_down && uinput_fd >= 0) {
                    /* v7: only forward this as a touch event if the
                     * reported position actually changes. While still
                     * inside tap-tolerance, get_touch_send_pos() keeps
                     * returning the frozen press_x/press_y -- re-sending
                     * BTN_TOUCH=1 at that same unchanged coordinate on
                     * every single jitter-driven EV_REL (mice emit
                     * these constantly, even "motionless") is NOT what
                     * a finger does. A real touch reports down once and
                     * is then silent until it moves or lifts. Spamming
                     * redundant down-assertions at ~the same point is
                     * exactly the kind of repeated key-down stream that
                     * a touch/gesture stack can read as repeated
                     * presses -- i.e. our "two mice" symptom -- even
                     * though BTN_TOUCH's value never actually toggles.
                     * So: compute the position, and only call
                     * send_touch() if it differs from the last position
                     * we actually sent. */
                    int sx, sy;
                    get_touch_send_pos(&sx, &sy);
                    if (sx != last_sent_x || sy != last_sent_y) {
                        send_touch(uinput_fd, sx, sy, 1);
                        last_sent_x = sx; last_sent_y = sy;
                    }
                }
            } else if (ev.type == EV_KEY &&
                       (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE)) {
                int was_down = touch_down;
                touch_down = ev.value;
                if (uinput_fd >= 0) {
                    if (touch_down && !was_down) {
                        /* fresh press: anchor the tap-tolerance origin
                         * here, reset the flag for this new press */
                        int cx = cursor_x + CLICK_OFFSET_X;
                        int cy = cursor_y + CLICK_OFFSET_Y;
                        if (cx > SCREEN_W - 1) cx = SCREEN_W - 1;
                        if (cy > SCREEN_H - 1) cy = SCREEN_H - 1;
                        press_x = cx; press_y = cy;
                        past_tap_tolerance = 0;
                        last_sent_x = cx; last_sent_y = cy;
                        send_touch(uinput_fd, cx, cy, 1);
                    } else {
                        int sx, sy;
                        get_touch_send_pos(&sx, &sy);
                        last_sent_x = sx; last_sent_y = sy;
                        send_touch(uinput_fd, sx, sy, touch_down);
                    }
                }
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            /* v12: device-lost / hot-swap path.
             *
             * EAGAIN/EWOULDBLOCK on this O_NONBLOCK fd just means "no
             * event waiting right now" -- completely normal, happens
             * on basically every iteration, not handled here at all.
             * Anything else (in practice this is ENODEV) means the
             * underlying device itself is gone: once an evdev node's
             * device is unregistered, every subsequent read() on an
             * fd still open to it returns this same error forever,
             * and the /dev/input/eventN path disappears at the same
             * moment too -- so there is nothing to recover by
             * retrying this fd or re-opening the same path. This is
             * exactly "user swapped mice" (old one's node dies) or
             * "system renumbered on reboot" surfacing at runtime
             * instead of at startup.
             *
             * Drop the dead fd and block in acquire_mouse_fd() --
             * same scan-and-verify logic as the very first open --
             * until a replacement (the same mouse replugged, or a
             * different one) shows up. uinput device and cursor
             * position are left exactly as they are; only the mouse
             * input fd changes underneath them. */
            vlogf(TAG " mouse device lost (read errno=%d, %s) -- "
                  "scanning for a replacement\n", errno, strerror(errno));
            close(fd);
            fd = -1;

            /* Don't leave a touch visually stuck "down" on screen
             * just because the mouse vanished mid-press. */
            if (touch_down && uinput_fd >= 0) {
                send_touch(uinput_fd, last_sent_x, last_sent_y, 0);
            }
            touch_down = 0;
            past_tap_tolerance = 0;

            fd = acquire_mouse_fd();
            /* If input_running was cleared while we were waiting, fd
             * is still -1 here; the while(input_running) check at the
             * top of the next iteration exits the loop before this fd
             * is ever read from again. */
        }
        usleep(1000);

        /* v7: the old "periodic re-assertion while held" block that
         * used to live here is removed. A real finger does not get
         * artificially kept alive by a timer re-sending BTN_TOUCH=1
         * every ~16ms -- it reports down once and the touch stack
         * holds that state until a genuine new SYN_REPORT changes it
         * (move or release). Re-asserting on a timer, decoupled from
         * any real input, was a second independent source of
         * redundant down-events stacking on top of the EV_REL spam
         * (now also fixed above) -- together these are what produced
         * the "two mice on top of each other" double-click/cancel
         * behavior: a single physical tap was reaching MPC as a burst
         * of repeated press assertions rather than one clean down/up
         * pair, exactly unlike a real finger tap. If a specific
         * long-press/gesture recognizer turns out to still need a
         * heartbeat, that should be reintroduced narrowly (and still
         * gated through last_sent_x/y, sending only genuinely new
         * SYN_REPORTs) rather than unconditionally on a fixed tick. */

        /* v5: independent cursor-plane keepalive.
         * splice_cursor_props() only ever runs when MPC itself calls
         * drmModeAtomicCommit() -- on a view that redraws rarely (or
         * not at all once settled), the cursor plane's position would
         * stay frozen at whatever it was on MPC's last commit, which
         * looks exactly like "blinking / not visible depending on the
         * view". This fires do_cursor_commit() on a steady ~60Hz
         * cadence regardless of MPC's own redraw activity, so cursor
         * movement stays smooth and present even on a static screen.
         * Safe to run concurrently with MPC's own hooked commit --
         * both paths serialize through cursor_state_mutex. */
        {
            static long cursor_tick = 0;
            cursor_tick++;
            if (cursor_tick % 16 == 0 && cursor_fb_ready && cursor_drm_fd >= 0) {
                do_cursor_commit(cursor_drm_fd);
            }
        }
    }

    if (uinput_fd >= 0) { ioctl(uinput_fd, UI_DEV_DESTROY); close(uinput_fd); }
    if (wheel_uinput_fd >= 0) { ioctl(wheel_uinput_fd, UI_DEV_DESTROY); close(wheel_uinput_fd); }
    if (fd >= 0) close(fd);
    return NULL;
}

/* ----------------------------------------------------------------
 * The actual hook: drmModeAtomicCommit().
 *
 * We do NOT call real_drmModeAtomicAddProperty() to splice into MPC's
 * req object -- req's internal item list is private libdrm state we'd
 * rather not poke at across versions. Instead we build our OWN small
 * atomic request containing only the plane-35 properties, and submit
 * it as a separate, immediately-following ioctl on the SAME fd, using
 * DRM_MODE_ATOMIC_NONBLOCK (no page-flip event wait) so it can't stall
 * behind MPC's own commit. Because it's the same fd MPC already holds
 * as master, this is authorized exactly like MPC's own commit is --
 * the master check is per-fd/per-file, not per-call.
 * ---------------------------------------------------------------- */
/* ----------------------------------------------------------------
 * Splice our cursor-plane properties into MPC's own atomic request
 * via the public drmModeAtomicAddProperty() API, so they land as
 * part of the SAME atomic transaction as MPC's flip. This is the
 * preferred path -- it eliminates the window between two separate
 * commits where the previous approach (a second, independent ioctl
 * issued right after MPC's commit) could let the display latch a
 * frame that includes MPC's update but not yet ours, or vice versa,
 * producing the single-frame flicker we were chasing.
 *
 * Returns 1 if properties were successfully added to req (caller
 * should NOT also do a separate commit), 0 if this path isn't
 * available (caller should fall back to the separate-commit path).
 * ---------------------------------------------------------------- */
static int splice_cursor_props(drmModeAtomicReqPtr req, int fd)
{
    if (!real_drmModeAtomicAddProperty)
        real_drmModeAtomicAddProperty = dlsym(RTLD_NEXT, "drmModeAtomicAddProperty");
    if (!real_drmModeAtomicAddProperty || !req)
        return 0;

    pthread_mutex_lock(&cursor_state_mutex);

    if (!ensure_cursor_fb(fd)) {
        pthread_mutex_unlock(&cursor_state_mutex);
        return 0;
    }

    int cx = cursor_x, cy = cursor_y;

    /* Alternate which of the two identical buffers we present, so
     * every commit is a genuine fb_id change. */
    cursor_fb_index ^= 1;
    uint32_t this_fb_id = cursor_fb_ids[cursor_fb_index];
    uint32_t local_zpos_prop = zpos_prop_id;
    uint64_t local_zpos_val  = zpos_max_value;

    pthread_mutex_unlock(&cursor_state_mutex);

    struct cursor_prop { uint32_t prop_id; uint64_t value; };
    struct cursor_prop props[11]; /* 10 base props + optional zpos */
    int n = 0;
    props[n++] = (struct cursor_prop){ PROP_CRTC_ID, CRTC_ID };
    props[n++] = (struct cursor_prop){ PROP_FB_ID,   this_fb_id };
    props[n++] = (struct cursor_prop){ PROP_CRTC_X,  (uint64_t)(int64_t)cx };
    props[n++] = (struct cursor_prop){ PROP_CRTC_Y,  (uint64_t)(int64_t)cy };
    props[n++] = (struct cursor_prop){ PROP_CRTC_W,  CURSOR_W };
    props[n++] = (struct cursor_prop){ PROP_CRTC_H,  CURSOR_H };
    props[n++] = (struct cursor_prop){ PROP_SRC_X,   0 };
    props[n++] = (struct cursor_prop){ PROP_SRC_Y,   0 };
    props[n++] = (struct cursor_prop){ PROP_SRC_W,   (uint64_t)CURSOR_W << 16 };
    props[n++] = (struct cursor_prop){ PROP_SRC_H,   (uint64_t)CURSOR_H << 16 };
    if (local_zpos_prop)
        props[n++] = (struct cursor_prop){ local_zpos_prop, local_zpos_val };

    int all_ok = 1;
    for (int i = 0; i < n; i++) {
        int r = real_drmModeAtomicAddProperty(req, PLANE_ID,
                                               props[i].prop_id,
                                               props[i].value);
        if (r < 0) {
            all_ok = 0;
            static int errcnt = 0;
            if (errcnt++ < 10)
                vlogf(TAG " drmModeAtomicAddProperty failed for prop_id=%u "
                      "errno=%d (%s)\n", props[i].prop_id, errno, strerror(errno));
        }
    }
    return all_ok;
}

static int do_cursor_commit(int fd)
{
    static long total_calls = 0, total_ok = 0, total_ebusy = 0, total_other_err = 0;
    total_calls++;

    pthread_mutex_lock(&cursor_state_mutex);

    if (!ensure_cursor_fb(fd)) {
        pthread_mutex_unlock(&cursor_state_mutex);
        return -1;
    }

    int cx = cursor_x, cy = cursor_y; /* snapshot, input thread may write concurrently */

    cursor_fb_index ^= 1;
    uint32_t this_fb_id = cursor_fb_ids[cursor_fb_index];
    uint32_t local_zpos_prop = zpos_prop_id;
    uint64_t local_zpos_val  = zpos_max_value;

    pthread_mutex_unlock(&cursor_state_mutex);

    struct cursor_prop2 { uint32_t obj_id; uint32_t prop_id; uint64_t value; };
    struct cursor_prop2 props[11];
    int n = 0;
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_CRTC_ID, CRTC_ID };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_FB_ID,   this_fb_id };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_CRTC_X,  (uint64_t)(int64_t)cx };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_CRTC_Y,  (uint64_t)(int64_t)cy };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_CRTC_W,  CURSOR_W };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_CRTC_H,  CURSOR_H };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_SRC_X,   0 };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_SRC_Y,   0 };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_SRC_W,   (uint64_t)CURSOR_W << 16 };
    props[n++] = (struct cursor_prop2){ PLANE_ID, PROP_SRC_H,   (uint64_t)CURSOR_H << 16 };
    if (local_zpos_prop)
        props[n++] = (struct cursor_prop2){ PLANE_ID, local_zpos_prop, local_zpos_val };

    uint32_t obj_ids[1]   = { PLANE_ID };
    uint32_t num_props[1] = { (uint32_t)n };
    uint32_t prop_ids[11];
    uint64_t prop_vals[11];
    for (int i = 0; i < n; i++) {
        prop_ids[i]  = props[i].prop_id;
        prop_vals[i] = props[i].value;
    }

    struct drm_mode_atomic atomic;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags           = DRM_MODE_ATOMIC_NONBLOCK;
    atomic.count_objs      = 1;
    atomic.objs_ptr        = (uintptr_t)obj_ids;
    atomic.count_props_ptr = (uintptr_t)num_props;
    atomic.props_ptr       = (uintptr_t)prop_ids;
    atomic.prop_values_ptr = (uintptr_t)prop_vals;

    int ret = ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    if (ret < 0) {
        if (errno == EBUSY) total_ebusy++;
        else total_other_err++;
        static int errcnt = 0;
        if (errcnt++ < 10)
            vlogf(TAG " cursor plane commit failed errno=%d (%s)\n", errno, strerror(errno));
    } else {
        total_ok++;
    }

    /* Periodic ratio report -- tells us if our commit is keeping pace
     * with MPC's own commit cadence, or falling behind/erroring out
     * at some rate that would explain visible flicker. */
    if (total_calls % 200 == 0) {
        vlogf(TAG " stats: calls=%ld ok=%ld ebusy=%ld other_err=%ld "
              "ok_ratio=%.1f%%\n",
              total_calls, total_ok, total_ebusy, total_other_err,
              100.0 * (double)total_ok / (double)total_calls);
    }

    return ret;
}

int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data)
{
    if (!real_drmModeAtomicCommit)
        real_drmModeAtomicCommit = dlsym(RTLD_NEXT, "drmModeAtomicCommit");

    static long mpc_commit_count = 0;
    mpc_commit_count++;

    /* v10: this flag-breakdown bookkeeping exists purely to feed the
     * log line below -- it ran unconditionally on every single MPC
     * commit (i.e. every rendered frame, indefinitely) even though the
     * result was only ever read once per 200 commits. Skip the
     * counters entirely when logging is off. */
    if (verbose_logging) {
        /* Track how MPC's own commit flags break down -- if a large
         * fraction are TEST_ONLY (validation passes that never actually
         * flip the display) or otherwise don't carry NONBLOCK/flip
         * semantics, our spliced cursor properties would be validated
         * but never actually presented on those calls, which would look
         * exactly like "every other commit doesn't show the cursor". */
        static long flag_test_only = 0, flag_nonblock = 0, flag_allow_modeset = 0, flag_other = 0;
        if (flags & DRM_MODE_ATOMIC_TEST_ONLY) flag_test_only++;
        if (flags & DRM_MODE_ATOMIC_NONBLOCK) flag_nonblock++;
        if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) flag_allow_modeset++;
        if (!(flags & (DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET)))
            flag_other++;

        if (mpc_commit_count <= 20 || mpc_commit_count % 200 == 0) {
            vlogf(TAG " mpc commit #%ld flags=0x%x (test_only_total=%ld "
                  "nonblock_total=%ld modeset_total=%ld other_total=%ld)\n",
                  mpc_commit_count, flags, flag_test_only, flag_nonblock,
                  flag_allow_modeset, flag_other);
        }
    }

    /* Preferred path: splice our cursor-plane properties into MPC's
     * own request BEFORE it commits, so they land in the same atomic
     * transaction as MPC's flip. No race window, no separate ioctl. */
    /* Never splice onto TEST_ONLY commits. These are validation-only
     * passes that never reach the display, but if we still toggled
     * our alternating fb_index on them, we'd silently burn through
     * our two buffer slots out of sync with real frames -- which
     * would produce exactly the half-rate blink we're chasing if
     * MPC issues one TEST_ONLY validation pass per real commit.
     *
     * v8: also gated on is_singleton_owner -- if a second, stale
     * instance of this library is alive in another process, it must
     * not also be poking the cursor plane (or running do_cursor_commit
     * below) on every commit. We still let MPC's own commit go through
     * untouched; we just don't add our own competing writes to it. */
    int spliced = 0;
    if (is_singleton_owner && !(flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
        spliced = splice_cursor_props(req, fd);
    }

    int ret = real_drmModeAtomicCommit(fd, req, flags, user_data);

    static int logged_once = 0;
    if (!logged_once) {
        logged_once = 1;
        vlogf(TAG " observed MPC drmModeAtomicCommit(fd=%d) ret=%d "
              "spliced=%d -- cursor plane injection active\n",
              fd, ret, spliced);
    }
    if (mpc_commit_count % 200 == 0) {
        vlogf(TAG " mpc_commit_count=%ld spliced_this_call=%d\n",
              mpc_commit_count, spliced);
    }

    /* Fallback: only issue our own separate commit if splicing into
     * MPC's request wasn't possible (e.g. symbol not resolved). This
     * reintroduces the race window the splice path avoids, but it's
     * better than no cursor at all. Also gated on is_singleton_owner
     * for the same reason as the splice path above. */
    if (is_singleton_owner && !spliced) {
        do_cursor_commit(fd);
    }

    return ret;
}

/* ----------------------------------------------------------------
 * Constructor
 * ---------------------------------------------------------------- */
static int acquire_singleton_lock(void)
{
    /* v9: O_CLOEXEC is load-bearing, not cosmetic.
     *
     * The v8 log showed the SAME pid logging "loaded" three times with
     * an identical build timestamp -- that only happens if the process
     * called execve() on itself a few times during startup (a launcher
     * stub re-exec'ing into the real MPC binary, same PID, fresh
     * address space + constructors each time). exec() does NOT close
     * ordinary file descriptors by default, so the very first exec
     * stage's lock fd silently survived into every later stage of that
     * same process -- each of which then opened its OWN new fd to the
     * same lock file and found it already held, by itself, from a
     * stage that no longer exists and can never release it. The real,
     * final, long-running instance was permanently locked out by its
     * own earlier exec stage, which is exactly why the mouse went
     * completely dead rather than just double-firing.
     *
     * O_CLOEXEC makes the kernel close this fd automatically at the
     * moment of exec, before the next stage's constructor even runs --
     * so each exec stage gets a clean shot at the lock. Protection
     * against genuinely separate, concurrently-running processes is
     * unaffected: that case never involved exec(), so this flag
     * changes nothing about it. */
    int fd = real_open(LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        vlogf(TAG " WARNING: could not open lock file %s errno=%d (%s) -- "
              "proceeding WITHOUT a singleton guarantee\n",
              LOCK_FILE, errno, strerror(errno));
        return 0; /* fail open: behave like pre-v8, rather than refusing
                   * to run at all just because /tmp is unwritable */
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        vlogf(TAG " another instance already holds %s (errno=%d, %s) -- "
              "this process will NOT start the input thread, uinput "
              "device, or cursor-plane splicing, to avoid double-firing "
              "touches alongside the live instance\n",
              LOCK_FILE, errno, strerror(errno));
        close(fd);
        return 0;
    }
    /* Deliberately leak fd for the life of THIS image: the kernel
     * drops this flock() automatically on exit, on crash, AND now on
     * exec (thanks to O_CLOEXEC) -- so there is no cleanup step and no
     * way for this to become a stale lock that blocks a legitimate
     * future launch or a later stage of the same process. */
    return 1;
}

__attribute__((constructor))
static void preload_init(void)
{
    real_open = dlsym(RTLD_NEXT, "open");
    if (!real_open) return;

    /* v10: read config FIRST, before opening the log fd or logging
     * anything at all. verbose_logging must be known before the very
     * first vlogf() call, or the "loaded" / lock-acquisition lines
     * would always print regardless of the configured setting. */
    read_config();

    /* v8: O_APPEND instead of O_TRUNC. If a second instance ever does
     * start up (e.g. the lock-file mechanism below somehow isn't
     * available), it will no longer truncate/clobber the first
     * instance's still-open log fd mid-write -- that torn-write
     * corruption (a stray "total=8)" fragment) is what first exposed
     * the duplicate-instance bug in the v7 log. */
    /* v9: O_CLOEXEC here too -- not load-bearing like the lock fd
     * above (each exec stage reopens the log by path in O_APPEND mode
     * regardless, so logging keeps working either way), but there's no
     * reason to leak an orphaned fd into every later stage of an exec
     * chain when we can just not. */
    /* v10: only bother opening the log file at all if logging is on. */
    if (verbose_logging) {
        log_fd = real_open("/tmp/force_cursor_drm.log",
                           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }

    vlogf(TAG " loaded built=" __DATE__ " " __TIME__ " pid=%d\n", getpid());

    is_singleton_owner = acquire_singleton_lock();
    if (!is_singleton_owner) {
        vlogf(TAG " exiting init early: not the singleton owner\n");
        return;
    }

    /* v12: mouse_dev here (config "device=", or whatever
     * autodetect_mouse() finds right this instant) is only ever a
     * first guess -- it's fine for it to be NULL (no mouse plugged in
     * yet) or for autodetect_mouse() to find nothing at this exact
     * moment. input_monitor() now owns retrying this for as long as
     * the process runs, both for "no mouse yet at boot" and for
     * runtime hot-swap if one disappears later, so the old behavior
     * of giving up here and never starting the thread at all is
     * gone -- a mouse plugged in five minutes after boot now works
     * without needing the addon (or MPC) restarted. */
    if (!mouse_dev) mouse_dev = autodetect_mouse();
    vlogf(TAG " mouse=%s rate=%.2f\n", mouse_dev ? mouse_dev : "(none yet)", rate);
    input_running = 1;
    pthread_create(&input_thread, NULL, input_monitor, NULL);
}
