/**
 * @file force_cursor.c
 * Decription: Mouse Cursor Hack Implementation for Akai Force.
 *
 * * Credits @no3z (Discord)
 * MockbaMid Addon Adaptation: Amit Talwar (@locrian) (Discord)
 * Date: January 2026
 *
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
#define MULTIPLIER 1.5

// Cursor state
static uint32_t cursor_data[64 * 64] = { 0 };
static uint32_t cursor_bo = 0;
static int cursor_initialized = 0;
static int saved_fd = -1;
static uint32_t saved_crtc = 0;
static int cursor_x = 50; // Bottom left corner
static int cursor_y = 1230; // Bottom left corner
static pthread_t input_thread;
static int input_running = 0;
static int uinput_fd = -1;
char* device = NULL;

static float rate = 1.0f;
static int read_params_file(const char* path, char** device, float* multiplier);

// Initialize bright visible cursor
static void init_cursor(int fd, uint32_t crtcId)
{
    fprintf(stdout, "-------MockbaMod Mouse Cursor credits @no3z (Discord) --------\n");
    if (read_params_file("/dev/shm/.mouseCursor", &device, &rate) != 0) {
        fprintf(stdout, "*** MockbaMod Mouse Cursor: Failed to read device.txt file *****\n");

        return;
    } else {
        fprintf(stdout, "-------MockbaMod Mouse Cursor --------\n\n    Device: %s\n    Speed Multiplier:%f\n", device, rate);
    }
    if (cursor_initialized)
        return;

    // Create bright white 24x24 circle
    memset(cursor_data, 0, sizeof(cursor_data));
    int center = 12;
    int radius = 12;
    for (int y = 0; y < 24; y++) {
        for (int x = 0; x < 24; x++) {
            int dx = x - center;
            int dy = y - center;
            if (dx * dx + dy * dy <= radius * radius) {
                cursor_data[y * 64 + x] = 0xFFFFFFFF;
            }
        }
    }

    // Create DRM buffer
    struct drm_mode_create_dumb create_req = { 0 };
    create_req.width = 64;
    create_req.height = 64;
    create_req.bpp = 32;

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) == 0) {
        cursor_bo = create_req.handle;
        saved_fd = fd;
        saved_crtc = crtcId;

        struct drm_mode_map_dumb map_req = { 0 };
        map_req.handle = cursor_bo;

        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) == 0) {
            void* ptr = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_req.offset);
            if (ptr != MAP_FAILED) {
                memcpy(ptr, cursor_data, sizeof(cursor_data));
                munmap(ptr, create_req.size);
                cursor_initialized = 1;
            }
        }
    }
}

// Initialize uinput device for touch events

static int init_uinput()
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    // Enable touch events
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    // Set up absolute position ranges (portrait: 800x1280)
    struct uinput_abs_setup abs_x = {
        .code = ABS_X,
        .absinfo = { .minimum = 0, .maximum = 799 },
    };
    struct uinput_abs_setup abs_y = {
        .code = ABS_Y,
        .absinfo = { .minimum = 0, .maximum = 1279 },
    };
    ioctl(fd, UI_ABS_SETUP, &abs_x);
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    // Create device
    struct uinput_setup setup = { 0 };
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "Virtual Mouse Touch");
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// Send touch event
static void send_touch_event(int fd, int x, int y, int pressed)
{
    struct input_event ev[4];
    memset(ev, 0, sizeof(ev));

    // Position
    ev[0].type = EV_ABS;
    ev[0].code = ABS_X;
    ev[0].value = x;

    ev[1].type = EV_ABS;
    ev[1].code = ABS_Y;
    ev[1].value = y;

    // Touch state
    ev[2].type = EV_KEY;
    ev[2].code = BTN_TOUCH;
    ev[2].value = pressed;

    // Sync
    ev[3].type = EV_SYN;
    ev[3].code = SYN_REPORT;
    ev[3].value = 0;

    write(fd, ev, sizeof(ev));
}

// Input monitoring thread
static void* input_monitor(void* arg)
{

    fprintf(stdout, "--------- opening device %s\n", device);
    int fd = open(device, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stdout, "----------- ERROR opening device %s for Mouse Events\n", device);
        free(device);

        return NULL;
    }
    free(device);

    // Initialize uinput for touch injection
    uinput_fd = init_uinput();

    struct input_event ev;
    int (*real_drmModeMoveCursor)(int, uint32_t, int, int) = dlsym(RTLD_NEXT, "drmModeMoveCursor");

    while (input_running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) {
            if (ev.type == EV_REL) {
                // Swap X and Y for portrait display (800x1280), invert Y
                if (ev.code == REL_X) {
                    cursor_y -= (ev.value * rate); // Mouse X -> Screen Y (inverted)
                    if (cursor_y < 0)
                        cursor_y = 0;
                    if (cursor_y > 1279)
                        cursor_y = 1279; // Portrait height
                } else if (ev.code == REL_Y) {
                    cursor_x += (ev.value * rate); // Mouse Y -> Screen X
                    if (cursor_x < 0)
                        cursor_x = 0;
                    if (cursor_x > 799)
                        cursor_x = 799; // Portrait width
                }
            } else if (ev.type == EV_KEY) {
                // Mouse button events
                if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    if (uinput_fd >= 0) {
                        send_touch_event(uinput_fd, cursor_x, cursor_y, ev.value);
                    }
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                // Move cursor after sync
                if (saved_fd >= 0 && real_drmModeMoveCursor) {
                    real_drmModeMoveCursor(saved_fd, saved_crtc, cursor_x, cursor_y);
                }
            }
        }
        usleep(1000); // 1ms sleep
    }

    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    close(fd);
    return NULL;
}

// Hook drmModeSetCursor2
int drmModeSetCursor2(int fd, uint32_t crtcId, uint32_t bo_handle,
    uint32_t width, uint32_t height,
    int32_t hot_x, int32_t hot_y)
{
    static int (*real_drmModeSetCursor2)(int, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t) = NULL;

    if (!real_drmModeSetCursor2) {
        real_drmModeSetCursor2 = dlsym(RTLD_NEXT, "drmModeSetCursor2");
    }

    if (bo_handle == 0) {
        if (!cursor_initialized) {
            init_cursor(fd, crtcId);

            // Start input monitor thread
            if (!input_running) {
                input_running = 1;
                pthread_create(&input_thread, NULL, input_monitor, NULL);
            }
        }

        if (cursor_bo != 0 && real_drmModeSetCursor2) {
            int ret = real_drmModeSetCursor2(fd, crtcId, cursor_bo, 64, 64, 0, 0);

            // Initially position the cursor
            int (*real_drmModeMoveCursor)(int, uint32_t, int, int) = dlsym(RTLD_NEXT, "drmModeMoveCursor");
            if (real_drmModeMoveCursor) {
                real_drmModeMoveCursor(fd, crtcId, cursor_x, cursor_y);
            }

            return ret;
        }
        return 0;
    }

    // Pass through other cursor sets
    if (real_drmModeSetCursor2) {
        return real_drmModeSetCursor2(fd, crtcId, bo_handle, width, height, hot_x, hot_y);
    }
    return 0;
}

// Hook drmModeSetCursor
int drmModeSetCursor(int fd, uint32_t crtcId, uint32_t bo_handle,
    uint32_t width, uint32_t height)
{
    return drmModeSetCursor2(fd, crtcId, bo_handle, width, height, 0, 0);
}

// Hook drmModeMoveCursor
int drmModeMoveCursor(int fd, uint32_t crtcId, int x, int y)
{
    static int (*real_drmModeMoveCursor)(int, uint32_t, int, int) = NULL;
    static int count = 0;

    if (!real_drmModeMoveCursor) {
        real_drmModeMoveCursor = dlsym(RTLD_NEXT, "drmModeMoveCursor");
    }

    if (count < 3) {
        fprintf(stderr, "[CURSOR_PATCH] MPC moved cursor to %d,%d\n", x, y);
        count++;
    }

    if (real_drmModeMoveCursor) {
        return real_drmModeMoveCursor(fd, crtcId, x, y);
    }
    return 0;
}

static int read_params_file(const char* path, char** out_str, float* out_val)
{
    FILE* fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[256];
    /* ----- first line (string) ----- */
    if (!fgets(line, sizeof line, fp)) { /* no first line? */
        fclose(fp);
        return -1;
    }
    size_t len = strcspn(line, "\r\n"); /* strip newline */
    *out_str = malloc(len + 1);
    if (!*out_str) {
        fclose(fp);
        return -1;
    }
    memcpy(*out_str, line, len);
    (*out_str)[len] = '\0';

    /* ----- second line (float) ----- */
    if (!fgets(line, sizeof line, fp)) {
        *out_val = 1.0f; /* default if missing */
    } else {
        char* endptr;
        float val = strtof(line, &endptr);
        if (endptr == line || *out_val < 0.1f || val > 5.0f)
            val = 1.0f; /* default on parse error or out of range */
        *out_val = val;
    }

    fclose(fp);
    return 0;
}