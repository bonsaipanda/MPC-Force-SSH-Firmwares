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
#include <errno.h>
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
#include <alsa/asoundlib.h>
#define MULTIPLIER 1.5

// Cursor state
#include "mouse_cursor.h"  // External cursor design (64x64 RGBA)
static uint32_t cursor_bo = 0;
static int cursor_initialized = 0;
static int saved_fd = -1;
static uint32_t saved_crtc = 0;
static int cursor_x = 50; // Bottom left corner
static int cursor_y = 1230; // Bottom left corner
static pthread_t input_thread;
static int input_running = 0;
static int uinput_fd = -1;        // Single device for single touch (cursor clicks)
static int touchscreen_fd = -1;   // Real touchscreen device for MT gestures
static int keyboard_fd = -1;      // Virtual keyboard for button->key mappings
static volatile int gesture_in_progress = 0;
static volatile int left_button_pressed = 0;  // Track left button state for dragging
char* device = NULL;

// MIDI sequencer state
static snd_seq_t *midi_seq = NULL;
static int midi_port = -1;

// Button to key/MIDI mappings (max 16 mappings)
#define MAX_BUTTON_MAPPINGS 16
#define MAPPING_TYPE_KEY 0
#define MAPPING_TYPE_MIDI_CC 1

struct button_mapping {
    int button_code;
    int type;           // MAPPING_TYPE_KEY or MAPPING_TYPE_MIDI_CC
    int value;          // key_code for keyboard, CC number for MIDI
};
static struct button_mapping button_mappings[MAX_BUTTON_MAPPINGS];
static int num_button_mappings = 0;

static float rate = 1.0f;
static int read_params_file(const char* path, char** device, float* multiplier);

// Button name to code mapping
struct name_code_pair {
    const char* name;
    int code;
};

static const struct name_code_pair button_names[] = {
    {"BTN_LEFT", BTN_LEFT},
    {"BTN_RIGHT", BTN_RIGHT},
    {"BTN_MIDDLE", BTN_MIDDLE},
    {"BTN_SIDE", BTN_SIDE},
    {"BTN_EXTRA", BTN_EXTRA},
    {"BTN_FORWARD", BTN_FORWARD},
    {"BTN_BACK", BTN_BACK},
    {"BTN_TASK", BTN_TASK},
    {NULL, 0}
};

static const struct name_code_pair key_names[] = {
    {"KEY_ESC", KEY_ESC},
    {"KEY_SPACE", KEY_SPACE},
    {"KEY_ENTER", KEY_ENTER},
    {"KEY_TAB", KEY_TAB},
    {"KEY_BACKSPACE", KEY_BACKSPACE},
    {"KEY_LEFTSHIFT", KEY_LEFTSHIFT},
    {"KEY_RIGHTSHIFT", KEY_RIGHTSHIFT},
    {"KEY_LEFTCTRL", KEY_LEFTCTRL},
    {"KEY_RIGHTCTRL", KEY_RIGHTCTRL},
    {"KEY_LEFTALT", KEY_LEFTALT},
    {"KEY_RIGHTALT", KEY_RIGHTALT},
    {"KEY_UP", KEY_UP},
    {"KEY_DOWN", KEY_DOWN},
    {"KEY_LEFT", KEY_LEFT},
    {"KEY_RIGHT", KEY_RIGHT},
    {"KEY_PAGEUP", KEY_PAGEUP},
    {"KEY_PAGEDOWN", KEY_PAGEDOWN},
    {"KEY_HOME", KEY_HOME},
    {"KEY_END", KEY_END},
    {"KEY_DELETE", KEY_DELETE},
    {"KEY_INSERT", KEY_INSERT},
    {"KEY_F1", KEY_F1},
    {"KEY_F2", KEY_F2},
    {"KEY_F3", KEY_F3},
    {"KEY_F4", KEY_F4},
    {"KEY_F5", KEY_F5},
    {"KEY_F6", KEY_F6},
    {"KEY_F7", KEY_F7},
    {"KEY_F8", KEY_F8},
    {"KEY_F9", KEY_F9},
    {"KEY_F10", KEY_F10},
    {"KEY_F11", KEY_F11},
    {"KEY_F12", KEY_F12},
    {"KEY_MENU", KEY_MENU},
    {NULL, 0}
};

// Parse code from string (name, hex, or decimal)
static int parse_code(const char* str, const struct name_code_pair* table)
{
    // Try hex format (0xNNNN)
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        return (int)strtol(str, NULL, 16);
    }

    // Try plain decimal number
    char* endptr;
    long val = strtol(str, &endptr, 10);
    if (endptr != str && *endptr == '\0' && val >= 0 && val <= 0xFFFF) {
        return (int)val;
    }

    // Try name lookup
    for (int i = 0; table[i].name != NULL; i++) {
        if (strcasecmp(str, table[i].name) == 0) {
            return table[i].code;
        }
    }

    return -1;  // Not found
}

// Initialize bright visible cursor
static void init_cursor(int fd, uint32_t crtcId)
{
    fprintf(stdout, "-------MockbaMod Mouse Cursor --------\n");
    if (read_params_file("/dev/shm/.mouseCursor", &device, &rate) != 0) {
        fprintf(stdout, "*** MockbaMod Mouse Cursor: Failed to read device.txt file *****\n");

        return;
    } else {
        fprintf(stdout, "-------MockbaMod Mouse Cursor --------\n\n    Device: %s\n    Speed Multiplier:%f\n", device, rate);
    }
    if (cursor_initialized)
        return;

    // Cursor data is loaded from mouse_cursor.h (64x64 RGBA with transparency)
    // No need to generate it here - just use the pre-defined cursor_data array

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

// Initialize uinput device for single-touch events (cursor clicks only)
static int init_uinput()
{
    struct uinput_user_dev uidev;
    int fd;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    // Enable event types
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);

    // Enable keys
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    // Enable single-touch axes only
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    // Setup device info
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Virtual Mouse Touch");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x0000;
    uidev.id.product = 0x0000;
    uidev.id.version = 0;

    // Set axis ranges
    uidev.absmin[ABS_X] = 0;
    uidev.absmax[ABS_X] = 799;
    uidev.absmin[ABS_Y] = 0;
    uidev.absmax[ABS_Y] = 1279;

    // Write device
    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        close(fd);
        return -1;
    }

    // Create device
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// Open real touchscreen device for injecting MT gestures
static int open_touchscreen_device()
{
    int fd = open("/dev/input/event0", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stdout, "[TOUCHSCREEN] Failed to open /dev/input/event0 for writing (errno=%d)\n", errno);
        return -1;
    }
    fprintf(stdout, "[TOUCHSCREEN] Opened /dev/input/event0 for MT gesture injection\n");
    return fd;
}

// Open physical keyboard device for monitoring hardware button codes
static int open_keyboard_monitor()
{
    // Try event3 first (Amit's Input Provider - virtual keyboard that might receive hardware buttons)
    // Then try event1 (gpio-keys - physical GPIO buttons)
    const char* kbd_paths[] = {"/dev/input/event3", "/dev/input/event1", NULL};

    for (int i = 0; kbd_paths[i] != NULL; i++) {
        int fd = open(kbd_paths[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fprintf(stdout, "[KBD_MONITOR] Opened %s for hardware button monitoring\n", kbd_paths[i]);
            fprintf(stdout, "[KBD_MONITOR] Press any hardware buttons (MENU, SHIFT, etc.) to see their key codes\n");
            fflush(stdout);
            return fd;
        }
    }

    fprintf(stdout, "[KBD_MONITOR] Could not open keyboard device for monitoring\n");
    return -1;
}

// Open event3 (Amit's Input Provider) for key injection
// MPC already listens to this device, so our keys will be recognized
static int init_uinput_keyboard()
{
    // Instead of creating a new virtual keyboard (which MPC won't listen to),
    // open the existing "Amit's Input Provider" keyboard device that MPC already monitors.
    // This device is created by the midiloop addon and MPC opens it at startup.
    int fd = open("/dev/input/event3", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stdout, "[KEYBOARD] Failed to open /dev/input/event3 (Amit's Input Provider) (errno=%d)\n", errno);
        fflush(stdout);
        return -1;
    }

    fprintf(stdout, "[KEYBOARD] Opened /dev/input/event3 (Amit's Input Provider) for keyboard injection\n");
    fprintf(stdout, "[KEYBOARD] MPC is already listening to this device, so button mappings will work!\n");
    fflush(stdout);
    return fd;
}

// Initialize ALSA sequencer for MIDI CC sending
static int init_midi_sequencer()
{
    int err;

    // Open ALSA sequencer
    err = snd_seq_open(&midi_seq, "default", SND_SEQ_OPEN_OUTPUT, 0);
    if (err < 0) {
        fprintf(stdout, "[MIDI] Failed to open ALSA sequencer: %s\n", snd_strerror(err));
        fflush(stdout);
        return -1;
    }

    // Set client name
    snd_seq_set_client_name(midi_seq, "MouseButtonMIDI");

    // Create output port
    midi_port = snd_seq_create_simple_port(midi_seq, "Output",
                                            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                            SND_SEQ_PORT_TYPE_APPLICATION);
    if (midi_port < 0) {
        fprintf(stdout, "[MIDI] Failed to create MIDI port\n");
        fflush(stdout);
        snd_seq_close(midi_seq);
        midi_seq = NULL;
        return -1;
    }

    fprintf(stdout, "[MIDI] ALSA sequencer initialized, client port: %d\n", midi_port);
    fflush(stdout);
    return 0;
}

// Send MIDI CC message to Mockba Automation In (129:0)
static void send_midi_cc(int cc_number, int value, int pressed)
{
    if (!midi_seq || midi_port < 0) {
        fprintf(stdout, "[MIDI] Sequencer not initialized!\n");
        fflush(stdout);
        return;
    }

    // Only send on button press (not release)
    if (!pressed) {
        return;
    }

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);

    // Set event type to controller (CC)
    snd_seq_ev_set_controller(&ev, 0, cc_number, value);

    // Set source port
    snd_seq_ev_set_source(&ev, midi_port);

    // Set destination: 129:0 (Mockba Automation In)
    snd_seq_ev_set_dest(&ev, 129, 0);

    // Send event directly
    snd_seq_ev_set_direct(&ev);

    // Send the event
    if (snd_seq_event_output(midi_seq, &ev) < 0) {
        fprintf(stdout, "[MIDI] Error sending CC %d\n", cc_number);
        fflush(stdout);
    } else {
        fprintf(stdout, "[MIDI] Sent CC %d (value=%d) to 129:0\n", cc_number, value);
        fflush(stdout);
    }

    snd_seq_drain_output(midi_seq);
}

// Send keyboard key event
static void send_key_event(int fd, int key_code, int pressed)
{
    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));

    // Key press/release
    ev[0].type = EV_KEY;
    ev[0].code = key_code;
    ev[0].value = pressed;

    // Sync
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    ev[1].value = 0;

    write(fd, ev, sizeof(ev));
}

// Check if button has a mapping (returns pointer to mapping struct, or NULL)
static struct button_mapping* get_button_mapping(int button_code)
{
    for (int i = 0; i < num_button_mappings; i++) {
        if (button_mappings[i].button_code == button_code) {
            return &button_mappings[i];
        }
    }
    return NULL;  // No mapping
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

// Send a complete two-finger touch frame (both slots + one sync)
static void send_two_finger_frame(int fd, int x1, int y1, int tracking_id1, int x2, int y2, int tracking_id2, int send_btn_touch)
{
    struct input_event ev[16];
    memset(ev, 0, sizeof(ev));
    int idx = 0;

    // Slot 0
    ev[idx].type = EV_ABS;
    ev[idx].code = ABS_MT_SLOT;
    ev[idx].value = 0;
    idx++;

    ev[idx].type = EV_ABS;
    ev[idx].code = ABS_MT_TRACKING_ID;
    ev[idx].value = tracking_id1;
    idx++;

    if (tracking_id1 >= 0) {
        ev[idx].type = EV_ABS;
        ev[idx].code = ABS_MT_POSITION_X;
        ev[idx].value = x1;
        idx++;

        ev[idx].type = EV_ABS;
        ev[idx].code = ABS_MT_POSITION_Y;
        ev[idx].value = y1;
        idx++;
    }

    // Slot 1
    ev[idx].type = EV_ABS;
    ev[idx].code = ABS_MT_SLOT;
    ev[idx].value = 1;
    idx++;

    ev[idx].type = EV_ABS;
    ev[idx].code = ABS_MT_TRACKING_ID;
    ev[idx].value = tracking_id2;
    idx++;

    if (tracking_id2 >= 0) {
        ev[idx].type = EV_ABS;
        ev[idx].code = ABS_MT_POSITION_X;
        ev[idx].value = x2;
        idx++;

        ev[idx].type = EV_ABS;
        ev[idx].code = ABS_MT_POSITION_Y;
        ev[idx].value = y2;
        idx++;
    }

    // Add BTN_TOUCH (like real touchscreen)
    if (send_btn_touch) {
        ev[idx].type = EV_KEY;
        ev[idx].code = BTN_TOUCH;
        ev[idx].value = (tracking_id1 >= 0) ? 1 : 0;
        idx++;
    }

    // Add single-touch coordinates (first finger position, like real touchscreen)
    if (tracking_id1 >= 0) {
        ev[idx].type = EV_ABS;
        ev[idx].code = ABS_X;
        ev[idx].value = x1;
        idx++;

        ev[idx].type = EV_ABS;
        ev[idx].code = ABS_Y;
        ev[idx].value = y1;
        idx++;
    }

    // ONE sync for both fingers
    ev[idx].type = EV_SYN;
    ev[idx].code = SYN_REPORT;
    ev[idx].value = 0;
    idx++;

    write(fd, ev, idx * sizeof(struct input_event));
}

// Animate pinch gesture (zoom in or out)
static void animate_pinch_gesture(int fd, int center_x, int center_y, int zoom_in)
{
    // Prevent overlapping gestures
    if (gesture_in_progress) {
        return;
    }
    gesture_in_progress = 1;

    // Fast, small diagonal gesture (both horizontal and vertical)
    const int frames = 5;
    const int frame_delay_ms = 16;  // ~60fps

    // Diagonal spacing (both X and Y movement)
    const int min_spacing = 30;     // Fingers close (zoom in start)
    const int max_spacing = 100;    // Fingers apart (diagonal spread)

    // First frame: send BTN_TOUCH
    int first_frame = 1;

    for (int frame = 0; frame < frames; frame++) {
        int spacing;

        // Calculate spacing based on zoom direction
        float progress = (float)frame / (float)(frames - 1);

        if (zoom_in) {
            // Zoom in: fingers start close, move apart diagonally
            spacing = min_spacing + (int)((max_spacing - min_spacing) * progress);
        } else {
            // Zoom out: fingers start far, move together diagonally
            spacing = max_spacing - (int)((max_spacing - min_spacing) * progress);
        }

        // Calculate two touch points (diagonal spread - both X and Y)
        int x1 = center_x - spacing / 2;
        int y1 = center_y - spacing / 2;  // Bottom-left
        int x2 = center_x + spacing / 2;
        int y2 = center_y + spacing / 2;  // Top-right

        // Bounds checking - keep fingers on screen
        if (x1 < 0) x1 = 0;
        if (x1 > 799) x1 = 799;
        if (x2 < 0) x2 = 0;
        if (x2 > 799) x2 = 799;
        if (y1 < 0) y1 = 0;
        if (y1 > 1279) y1 = 1279;
        if (y2 < 0) y2 = 0;
        if (y2 > 1279) y2 = 1279;

        // Use sequential tracking IDs (like real touchscreen)
        static int base_tracking_id = 10;
        int tid1 = base_tracking_id;
        int tid2 = base_tracking_id + 1;

        // Debug output for first frame
        if (first_frame) {
            fprintf(stdout, "[GESTURE] Frame %d: finger1=(%d,%d) finger2=(%d,%d) spacing=%dpx tid=(%d,%d)\n",
                    frame, x1, y1, x2, y2, spacing, tid1, tid2);
            fflush(stdout);
        }

        // Send BOTH fingers in ONE frame (one sync)
        // Only send BTN_TOUCH on first frame
        send_two_finger_frame(fd, x1, y1, tid1, x2, y2, tid2, first_frame);
        first_frame = 0;

        usleep(frame_delay_ms * 1000);
    }

    // Release both touches in ONE frame with BTN_TOUCH=0
    send_two_finger_frame(fd, 0, 0, -1, 0, 0, -1, 1);

    // Increment tracking IDs for next gesture
    static int base_tracking_id = 10;
    base_tracking_id += 2;

    gesture_in_progress = 0;

    // Small delay before allowing next gesture
    usleep(30000); // 30ms
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

    // Initialize uinput for single-touch injection (cursor clicks)
    fprintf(stdout, "[INIT] Attempting to create uinput device...\n");
    fflush(stdout);
    uinput_fd = init_uinput();
    if (uinput_fd >= 0) {
        fprintf(stdout, "[INIT] SUCCESS: Virtual Touch device created (single-touch only), fd=%d\n", uinput_fd);
        fflush(stdout);
    } else {
        fprintf(stdout, "[INIT] FAILED: Could not create uinput device (errno=%d)\n", errno);
        fflush(stdout);
    }

    // Open real touchscreen for MT gesture injection
    fprintf(stdout, "[INIT] Attempting to open /dev/input/event0 for writing...\n");
    fflush(stdout);
    touchscreen_fd = open_touchscreen_device();
    if (touchscreen_fd >= 0) {
        fprintf(stdout, "[INIT] SUCCESS: Real touchscreen opened for MT gestures, fd=%d\n", touchscreen_fd);
        fflush(stdout);
    } else {
        fprintf(stdout, "[INIT] FAILED: Could not open touchscreen (errno=%d)\n", errno);
        fflush(stdout);
    }

    // Initialize devices for button mappings
    if (num_button_mappings > 0) {
        fprintf(stdout, "[INIT] Processing %d button mapping(s)...\n", num_button_mappings);
        fflush(stdout);

        // Check if we need keyboard device (for KEY mappings)
        int has_key_mappings = 0;
        int has_midi_mappings = 0;
        for (int i = 0; i < num_button_mappings; i++) {
            if (button_mappings[i].type == MAPPING_TYPE_KEY) {
                has_key_mappings = 1;
            } else if (button_mappings[i].type == MAPPING_TYPE_MIDI_CC) {
                has_midi_mappings = 1;
            }
        }

        // Initialize keyboard if needed
        if (has_key_mappings) {
            fprintf(stdout, "[INIT] Initializing keyboard device for KEY mappings...\n");
            fflush(stdout);
            keyboard_fd = init_uinput_keyboard();
            if (keyboard_fd >= 0) {
                fprintf(stdout, "[INIT] SUCCESS: Keyboard device ready, fd=%d\n", keyboard_fd);
                fflush(stdout);
            } else {
                fprintf(stdout, "[INIT] FAILED: Could not create keyboard device (errno=%d)\n", errno);
                fflush(stdout);
            }
        }

        // Initialize MIDI sequencer if needed
        if (has_midi_mappings) {
            fprintf(stdout, "[INIT] Initializing MIDI sequencer for MIDI_CC mappings...\n");
            fflush(stdout);
            if (init_midi_sequencer() == 0) {
                fprintf(stdout, "[INIT] SUCCESS: MIDI sequencer ready\n");
                fflush(stdout);
            } else {
                fprintf(stdout, "[INIT] FAILED: Could not initialize MIDI sequencer\n");
                fflush(stdout);
            }
        }
    } else {
        fprintf(stdout, "[INIT] No button mappings configured\n");
        fflush(stdout);
    }

    // Hardware button monitoring disabled (couldn't read from event1)
    // monitor_kbd_fd = open_keyboard_monitor();
    // if (monitor_kbd_fd >= 0) {
    //     fprintf(stdout, "[INIT] Hardware button monitoring ENABLED - press hardware buttons to see their codes\n");
    //     fflush(stdout);
    // }

    struct input_event ev;
    int (*real_drmModeMoveCursor)(int, uint32_t, int, int) = dlsym(RTLD_NEXT, "drmModeMoveCursor");

    while (input_running) {
        // Check for mouse events
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) {
            if (ev.type == EV_REL) {
                // Swap X and Y for portrait display (800x1280), invert Y
                int position_changed = 0;
                if (ev.code == REL_X) {
                    cursor_y -= (ev.value * rate); // Mouse X -> Screen Y (inverted)
                    if (cursor_y < 0)
                        cursor_y = 0;
                    if (cursor_y > 1279)
                        cursor_y = 1279; // Portrait height
                    position_changed = 1;
                } else if (ev.code == REL_Y) {
                    cursor_x += (ev.value * rate); // Mouse Y -> Screen X
                    if (cursor_x < 0)
                        cursor_x = 0;
                    if (cursor_x > 799)
                        cursor_x = 799; // Portrait width
                    position_changed = 1;
                } else if (ev.code == REL_WHEEL) {
                    // Mouse wheel -> pinch gesture (inject to real touchscreen)
                    fprintf(stdout, "[WHEEL] Detected wheel event: value=%d, touchscreen_fd=%d, gesture_in_progress=%d\n",
                            ev.value, touchscreen_fd, gesture_in_progress);
                    fflush(stdout);

                    if (touchscreen_fd >= 0 && !gesture_in_progress) {
                        if (ev.value > 0) {
                            // Scroll up = zoom in
                            fprintf(stdout, "[WHEEL] Injecting ZOOM IN gesture to /dev/input/event0 at (%d, %d)\n", cursor_x, cursor_y);
                            fflush(stdout);
                            animate_pinch_gesture(touchscreen_fd, cursor_x, cursor_y, 1);
                        } else if (ev.value < 0) {
                            // Scroll down = zoom out
                            fprintf(stdout, "[WHEEL] Injecting ZOOM OUT gesture to /dev/input/event0 at (%d, %d)\n", cursor_x, cursor_y);
                            fflush(stdout);
                            animate_pinch_gesture(touchscreen_fd, cursor_x, cursor_y, 0);
                        }
                    }
                }

                // If left button is pressed and cursor moved, send touch move event
                if (position_changed && left_button_pressed && uinput_fd >= 0) {
                    send_touch_event(uinput_fd, cursor_x, cursor_y, 1);
                }
            } else if (ev.type == EV_KEY) {
                // Mouse button events
                fprintf(stdout, "[DEBUG] EV_KEY: code=%d value=%d\n", ev.code, ev.value);
                fflush(stdout);

                // Check if button has a mapping
                struct button_mapping* mapping = get_button_mapping(ev.code);
                fprintf(stdout, "[DEBUG] Button %d: mapping=%p\n", ev.code, (void*)mapping);
                fflush(stdout);

                if (mapping != NULL) {
                    if (mapping->type == MAPPING_TYPE_KEY && keyboard_fd >= 0) {
                        // Send keyboard event
                        fprintf(stdout, "[BUTTON] Button %d -> Key %d (pressed=%d)\n", ev.code, mapping->value, ev.value);
                        fflush(stdout);
                        send_key_event(keyboard_fd, mapping->value, ev.value);
                    } else if (mapping->type == MAPPING_TYPE_MIDI_CC) {
                        // Send MIDI CC event
                        fprintf(stdout, "[BUTTON] Button %d -> MIDI CC %d (pressed=%d)\n", ev.code, mapping->value, ev.value);
                        fflush(stdout);
                        send_midi_cc(mapping->value, 127, ev.value);
                    }
                } else if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    // No mapping, send as touch event (default behavior)
                    fprintf(stdout, "[DEBUG] Sending as touch event\n");
                    fflush(stdout);
                    if (uinput_fd >= 0) {
                        send_touch_event(uinput_fd, cursor_x, cursor_y, ev.value);

                        // Track left button state for continuous drag
                        if (ev.code == BTN_LEFT) {
                            left_button_pressed = ev.value;
                            fprintf(stdout, "[DEBUG] Left button %s\n", ev.value ? "PRESSED" : "RELEASED");
                            fflush(stdout);
                        }
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
    if (touchscreen_fd >= 0) {
        close(touchscreen_fd);
    }
    if (keyboard_fd >= 0) {
        close(keyboard_fd);
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

    /* ----- additional lines (button mappings) ----- */
    num_button_mappings = 0;
    fprintf(stdout, "[CONFIG] Starting to parse button mappings...\n");
    fflush(stdout);
    while (fgets(line, sizeof line, fp) && num_button_mappings < MAX_BUTTON_MAPPINGS) {
        // Strip newline
        len = strcspn(line, "\r\n");
        line[len] = '\0';

        fprintf(stdout, "[CONFIG] Read line: '%s'\n", line);
        fflush(stdout);

        // Skip empty lines and comments
        if (len == 0 || line[0] == '#') {
            fprintf(stdout, "[CONFIG] Skipping (empty or comment)\n");
            fflush(stdout);
            continue;
        }

        // Parse BUTTON=KEY or BUTTON=MIDI_CC_XXX format
        char* eq = strchr(line, '=');
        if (!eq) {
            continue;  // Invalid format
        }

        *eq = '\0';  // Split at '='
        char* button_str = line;
        char* value_str = eq + 1;

        // Trim whitespace
        while (*button_str == ' ' || *button_str == '\t') button_str++;
        while (*value_str == ' ' || *value_str == '\t') value_str++;

        // Parse button code
        int button_code = parse_code(button_str, button_names);
        if (button_code < 0) {
            fprintf(stdout, "[CONFIG] Warning: Invalid button '%s' (skipped)\n", button_str);
            fflush(stdout);
            continue;
        }

        // Check if value is MIDI_CC_XXX format
        if (strncasecmp(value_str, "MIDI_CC_", 8) == 0) {
            // Parse MIDI CC number
            int cc_number = atoi(value_str + 8);
            if (cc_number >= 0 && cc_number <= 127) {
                button_mappings[num_button_mappings].button_code = button_code;
                button_mappings[num_button_mappings].type = MAPPING_TYPE_MIDI_CC;
                button_mappings[num_button_mappings].value = cc_number;
                fprintf(stdout, "[CONFIG] Button mapping: %s (%d) -> MIDI CC %d\n",
                        button_str, button_code, cc_number);
                fflush(stdout);
                num_button_mappings++;
            } else {
                fprintf(stdout, "[CONFIG] Warning: Invalid MIDI CC number '%s' (must be 0-127)\n",
                        value_str);
                fflush(stdout);
            }
        } else {
            // Parse as keyboard key
            int key_code = parse_code(value_str, key_names);
            if (key_code >= 0) {
                button_mappings[num_button_mappings].button_code = button_code;
                button_mappings[num_button_mappings].type = MAPPING_TYPE_KEY;
                button_mappings[num_button_mappings].value = key_code;
                fprintf(stdout, "[CONFIG] Button mapping: %s (%d) -> KEY %s (%d)\n",
                        button_str, button_code, value_str, key_code);
                fflush(stdout);
                num_button_mappings++;
            } else {
                fprintf(stdout, "[CONFIG] Warning: Invalid key '%s' (skipped)\n", value_str);
                fflush(stdout);
            }
        }
    }

    fclose(fp);
    return 0;
}
