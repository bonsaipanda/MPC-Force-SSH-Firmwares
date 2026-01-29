/**
 * @file force_cursor.c
 * Decription: Mouse Cursor Hack Implementation for Akai Force.
 *
 * Credits @no3z (Discord)
 * MockbaMid Addon Adaptation: Amit Talwar (@locrian) (Discord)
 * Cursor image addition: Jukka Korhonen (@ThatBonsaipanda)
 * Auto-detect mouse modification: ChatGPT
 * Date: January 2026
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Offset from cursor top-left to the "tip" in screen coordinates
#define CLICK_OFFSET_X 0
#define CLICK_OFFSET_Y 27

// Cursor state
static uint32_t cursor_bo = 0;
static int cursor_initialized = 0;
static int saved_fd = -1;
static uint32_t saved_crtc = 0;
static int cursor_x = 799;
static int cursor_y = 1279;
static pthread_t input_thread;
static int input_running = 0;
static int uinput_fd = -1;
static char* device = NULL;
static int touch_down = 0;

// Config file (speed only)
static const char* CONF_FILE_PATH = "/etc/force_cursor.conf";

// External cursor image
#include "mouse_cursor_offset.h"

static float rate = 2.0f;

/* ------------------------------------------------------------
 * Auto-detect mouse device from /proc/bus/input/devices
 * ------------------------------------------------------------ */
static char* auto_detect_mouse_device(void)
{
    FILE* fp = fopen("/proc/bus/input/devices", "r");
    if (!fp)
        return NULL;

    char line[256];
    char event_name[32] = {0};
    int has_rel = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "H: Handlers=", 12) == 0) {
            char* ev = strstr(line, "event");
            if (ev)
                sscanf(ev, "%31s", event_name);
        }

        if (strncmp(line, "B: REL=", 7) == 0) {
            has_rel = 1;
        }

        if (line[0] == '\n') {
            if (has_rel && event_name[0]) {
                fclose(fp);
                char* path = malloc(64);
                snprintf(path, 64, "/dev/input/%s", event_name);
                return path;
            }
            event_name[0] = 0;
            has_rel = 0;
        }
    }

    fclose(fp);
    return NULL;
}

/* ------------------------------------------------------------
 * Read speed multiplier from config file
 * ------------------------------------------------------------ */
static int read_params_file(const char* path, float* out_val)
{
    FILE* fp = fopen(path, "r");
    if (!fp) {
        *out_val = 2.0f;
        return -1;
    }

    char line[128];
    if (!fgets(line, sizeof(line), fp)) {
        *out_val = 2.0f;
    } else {
        char* endptr;
        float val = strtof(line, &endptr);
        if (endptr == line || val < 0.1f || val > 5.0f)
            val = 2.0f;
        *out_val = val;
    }

    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------
 * Initialize cursor
 * ------------------------------------------------------------ */
static void init_cursor(int fd, uint32_t crtcId)
{
    fprintf(stdout, "-------MockbaMod Mouse Cursor credits @no3z (Discord) --------\n");

    read_params_file(CONF_FILE_PATH, &rate);

    device = auto_detect_mouse_device();
    if (!device) {
        fprintf(stdout, "*** MockbaMod Mouse Cursor: No mouse device found ***\n");
        return;
    }

    fprintf(stdout,
        "-------MockbaMod Mouse Cursor --------\n\n"
        "    Device: %s\n"
        "    Speed Multiplier: %.2f\n",
        device, rate);

    if (cursor_initialized)
        return;

    struct drm_mode_create_dumb create_req = {0};
    create_req.width = 64;
    create_req.height = 64;
    create_req.bpp = 32;

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) == 0) {
        cursor_bo = create_req.handle;
        saved_fd = fd;
        saved_crtc = crtcId;

        struct drm_mode_map_dumb map_req = {0};
        map_req.handle = cursor_bo;

        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) == 0) {
            void* ptr = mmap(0, create_req.size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, map_req.offset);
            if (ptr != MAP_FAILED) {
                memcpy(ptr, cursor_data, sizeof(cursor_data));
                munmap(ptr, create_req.size);
                cursor_initialized = 1;
            }
        }
    }
}

/* ------------------------------------------------------------
 * Initialize uinput
 * ------------------------------------------------------------ */
static int init_uinput()
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    struct uinput_abs_setup abs_x = {
        .code = ABS_X,
        .absinfo = { .minimum = 0, .maximum = 799 }
    };
    struct uinput_abs_setup abs_y = {
        .code = ABS_Y,
        .absinfo = { .minimum = 0, .maximum = 1279 }
    };
    ioctl(fd, UI_ABS_SETUP, &abs_x);
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "Virtual Mouse Touch");
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------
 * Send touch event
 * ------------------------------------------------------------ */
static void send_touch_event(int fd, int x, int y, int pressed)
{
    struct input_event ev[4] = {0};

    ev[0].type = EV_ABS;
    ev[0].code = ABS_X;
    ev[0].value = x;

    ev[1].type = EV_ABS;
    ev[1].code = ABS_Y;
    ev[1].value = y;

    ev[2].type = EV_KEY;
    ev[2].code = BTN_TOUCH;
    ev[2].value = pressed;

    ev[3].type = EV_SYN;
    ev[3].code = SYN_REPORT;

    write(fd, ev, sizeof(ev));
}

/* ------------------------------------------------------------
 * Input thread
 * ------------------------------------------------------------ */
static void* input_monitor(void* arg)
{
    fprintf(stdout, "--------- opening device %s\n", device);
    int fd = open(device, O_RDONLY | O_NONBLOCK);
    free(device);

    if (fd < 0) {
        fprintf(stdout, "----------- ERROR opening mouse device\n");
        return NULL;
    }

    uinput_fd = init_uinput();

    struct input_event ev;
    int (*real_drmModeMoveCursor)(int, uint32_t, int, int) =
        dlsym(RTLD_NEXT, "drmModeMoveCursor");

    while (input_running) {
        if (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    cursor_y -= (ev.value * rate);
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_y > 1279) cursor_y = 1279;
                } else if (ev.code == REL_Y) {
                    cursor_x += (ev.value * rate);
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_x > 799) cursor_x = 799;
                }

                if (touch_down && uinput_fd >= 0) {
                    send_touch_event(
                        uinput_fd,
                        cursor_x + CLICK_OFFSET_X,
                        cursor_y + CLICK_OFFSET_Y,
                        1
                    );
                }
            } else if (ev.type == EV_KEY &&
                       (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE)) {
                touch_down = ev.value;
                if (uinput_fd >= 0) {
                    send_touch_event(
                        uinput_fd,
                        cursor_x + CLICK_OFFSET_X,
                        cursor_y + CLICK_OFFSET_Y,
                        touch_down
                    );
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if (saved_fd >= 0 && real_drmModeMoveCursor) {
                    real_drmModeMoveCursor(saved_fd, saved_crtc, cursor_x, cursor_y);
                }
            }
        }
        usleep(1000);
    }

    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------
 * DRM hooks
 * ------------------------------------------------------------ */
int drmModeSetCursor2(int fd, uint32_t crtcId, uint32_t bo_handle,
                      uint32_t width, uint32_t height,
                      int32_t hot_x, int32_t hot_y)
{
    static int (*real_fn)(int, uint32_t, uint32_t, uint32_t,
                          uint32_t, int32_t, int32_t) = NULL;

    if (!real_fn)
        real_fn = dlsym(RTLD_NEXT, "drmModeSetCursor2");

    if (bo_handle == 0) {
        if (!cursor_initialized) {
            init_cursor(fd, crtcId);
            if (!input_running) {
                input_running = 1;
                pthread_create(&input_thread, NULL, input_monitor, NULL);
            }
        }
        if (cursor_bo)
            return real_fn(fd, crtcId, cursor_bo, 64, 64, 0, 0);
        return 0;
    }

    return real_fn(fd, crtcId, bo_handle, width, height, hot_x, hot_y);
}

int drmModeSetCursor(int fd, uint32_t crtcId,
                     uint32_t bo_handle, uint32_t width, uint32_t height)
{
    return drmModeSetCursor2(fd, crtcId, bo_handle, width, height, 0, 0);
}
