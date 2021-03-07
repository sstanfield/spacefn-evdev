/*
 * spacefn-evdev.c
 * James Laird-Wah (abrasive) 2018
 * This code is in the public domain.
 */

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/dir.h>
#include <sys/param.h>
 
#include <unistd.h>

#define KEY_MASK   0x000000FF
#define FLAG_MASK  0x00000F00
#define SHIFT_FLAG 0x00000100

// Key mapping
unsigned int key_map(unsigned int code) {
    switch (code) {
        case KEY_1:    // my magical escape button
            exit(0);

        // HJKL for colemak
        case KEY_H:
            return KEY_LEFT;
        case KEY_Y:
            return KEY_DOWN;
        case KEY_N:
            return KEY_UP;
        case KEY_U:
            return KEY_RIGHT;

        case KEY_Z:
            return KEY_0;
        case KEY_X:
            return KEY_1;
        case KEY_C:
            return KEY_2;
        case KEY_V:
            return KEY_3;
        case KEY_CAPSLOCK:
            return KEY_SLASH;
        case KEY_A:
            return KEY_MINUS;
        case KEY_S:
            return KEY_4;
        case KEY_D:
            return KEY_5;
        case KEY_F:
            return KEY_6;
        case KEY_TAB:
            return KEY_GRAVE;
        case KEY_Q:
            return KEY_EQUAL;
        case KEY_W:
            return KEY_7;
        case KEY_E:
            return KEY_8;
        case KEY_R:
            return KEY_9;
        case KEY_L:
            return SHIFT_FLAG | KEY_9;
        case KEY_SEMICOLON:
            return SHIFT_FLAG | KEY_0;

        case KEY_B:
            return KEY_PAGEDOWN;
        case KEY_T:
            return KEY_PAGEUP;

        case KEY_COMMA:
            return KEY_HOME;
        case KEY_DOT:
            return KEY_END;
    }
    return 0;
}

// Global device handles
struct libevdev *idev;
struct libevdev_uinput *odev;
int fd;

// Ordered unique key buffer
#define MAX_BUFFER 8
unsigned int buffer[MAX_BUFFER];
unsigned int n_buffer = 0;

static int buffer_remove(unsigned int code) {
    for (int i=0; i<n_buffer; i++)
        if (buffer[i] == code) {
            memcpy(&buffer[i], &buffer[i+1], (n_buffer - i - 1) * sizeof(*buffer));
            n_buffer--;
            return 1;
        }
    return 0;
}

static int buffer_append(unsigned int code) {
    if (n_buffer >= MAX_BUFFER)
        return 1;
    buffer[n_buffer++] = code;
    return 0;
}

// Key I/O functions
#define V_RELEASE 0
#define V_PRESS 1
#define V_REPEAT 2
static void send_key(unsigned int code, int value) {
    if (code & SHIFT_FLAG) {
        libevdev_uinput_write_event(odev, EV_KEY, KEY_RIGHTSHIFT, value);
    }

    libevdev_uinput_write_event(odev, EV_KEY, code & KEY_MASK, value);
    libevdev_uinput_write_event(odev, EV_SYN, SYN_REPORT, 0);
}

// Useful for debugging.
static void print_event(struct input_event *ev) {
    printf("Event: %s %s %d\n",
           libevdev_event_type_get_name(ev->type),
           libevdev_event_code_get_name(ev->type, ev->code),
           ev->value);
}

static int read_one_key(struct input_event *ev) {
    int err = libevdev_next_event(idev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, ev);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        exit(99);
    }

    if (ev->type != EV_KEY) {
        libevdev_uinput_write_event(odev, ev->type, ev->code, ev->value);
        return -1;
    }

    return 0;
}

enum {
    IDLE,
    DECIDE,
    SHIFT,
} state = IDLE;

static void state_idle(void) {
    struct input_event ev;
    for (;;) {
        while (read_one_key(&ev));

        if (ev.code == KEY_SPACE && ev.value == V_PRESS) {
            state = DECIDE;
            return;
        }

        send_key(ev.code, ev.value);
    }
}

static void state_decide(void) {
    n_buffer = 0;
    struct input_event ev;

    for (;;) {
	    while (read_one_key(&ev));

        if (ev.value == V_PRESS) {
            unsigned int code = key_map(ev.code);
            if (code) {
                buffer_append(code);
                send_key(code, ev.value);
            } else {
                send_key(ev.code, ev.value);
            }
            state = SHIFT;
            return;
        }

        if (ev.code == KEY_SPACE && ev.value == V_RELEASE) {
            send_key(KEY_SPACE, V_PRESS);
            send_key(KEY_SPACE, V_RELEASE);
            state = IDLE;
            return;
        }

        if (ev.value == V_RELEASE) {
            send_key(ev.code, ev.value);
            continue;
        }
    }
}

static void state_shift(void) {
    struct input_event ev;
    for (;;) {
        while (read_one_key(&ev));

        if (ev.code == KEY_SPACE && ev.value == V_RELEASE) {
            for (int i=0; i<n_buffer; i++)
                send_key(buffer[i], V_RELEASE);
            state = IDLE;
            return;
        }
        if (ev.code == KEY_SPACE)
            continue;

        unsigned int code = key_map(ev.code);
        if (code) {
            if (ev.value == V_PRESS)
                buffer_append(code);
            else if (ev.value == V_RELEASE)
                buffer_remove(code);

            send_key(code, ev.value);
        } else {
            send_key(ev.code, ev.value);
        }

    }
}

static void run_state_machine(void) {
    for (;;) {
        //printf("state %d\n", state);
        switch (state) {
            case IDLE:
                state_idle();
                break;
            case DECIDE:
                state_decide();
                break;
            case SHIFT:
                state_shift();
                break;
        }
    }
}



int dev_select(const struct direct *entry) {
    if (entry->d_type == DT_CHR) return 1;
    else return 0;
}

int is_keeb(struct libevdev *idev) {
    if (libevdev_has_event_type(idev, EV_KEY) &&
        libevdev_has_event_type(idev, EV_SYN) &&
        libevdev_get_phys(idev) &&  // This will exclude virtual keyboards (like another spacefn instance).
        libevdev_has_event_code(idev, EV_KEY, KEY_SPACE) &&
        libevdev_has_event_code(idev, EV_KEY, KEY_A)) return 1;
    else return 0;
}

int main(int argc, char **argv) {
    int count,i;
    struct direct **files;

    if (argc < 2) {
        printf("usage: %s [--scan | /dev/input/...]\n", argv[0]);
        printf("    --scan: attempt to identify any keyboards in /dev/input and exit\n");
        return 1;
    }
    if (!strcmp(argv[1], "--scan")) {
        count = scandir("/dev/input", &files, dev_select, alphasort);
        for (i=0;i<count;i++) {
            char namebuf[100];
            namebuf[0] = 0;
            strncat(namebuf, "/dev/input/", 99);
            fd = open(strncat(namebuf, files[i]->d_name, 99), O_RDONLY);
            namebuf[11] = 0;
            if (fd < 0) {
                perror("open input");
                continue;
            }

            int err = libevdev_new_from_fd(fd, &idev);
            if (err) {
                continue;
            }
            if (is_keeb(idev)) {
                printf("\nFound keyboard %s\n", strncat(namebuf, files[i]->d_name, 99));
                namebuf[11] = 0;
                printf("Input device name: \"%s\"\n", libevdev_get_name(idev));
                printf("Input device ID: bus %#x vendor %#x product %#x\n",
                   libevdev_get_id_bustype(idev),
                   libevdev_get_id_vendor(idev),
                   libevdev_get_id_product(idev));
                printf("Location: %s\n", libevdev_get_phys(idev));
                if (libevdev_get_uniq(idev)) printf("Identity: %s\n", libevdev_get_uniq(idev));
            }
            libevdev_free(idev);
            close(fd);
            free(files[i]);
        }
        free(files);
        return 0;
    }

    // This sleep is a hack but it gives X time to read the Enter release event
    // when starting (unless someone holds it longer then a second) which keeps
    // several bad things from happening including "stuck" enter and possible locking
    // of a laptop trackpad until another key is pressed- and maybe longer).
    // Not sure how to solve this properly, the enter release will come from
    // spacefn without it and X seems to not connect the press and release in 
    // this case (different logical keyboards).
    sleep(1);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open input");
        return 2;
    }

    int err = libevdev_new_from_fd(fd, &idev);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        return 3;
    }
    printf("Input device name: \"%s\"\n", libevdev_get_name(idev));
    printf("Input device ID: bus %#x vendor %#x product %#x\n",
       libevdev_get_id_bustype(idev),
       libevdev_get_id_vendor(idev),
       libevdev_get_id_product(idev));
    if (!is_keeb(idev)) {
        fprintf(stderr, "This device does not look like a keyboard\n");
        return 4;
    }
    printf("Location: %s\n", libevdev_get_phys(idev));
    if (libevdev_get_uniq(idev)) printf("Identity: %s\n", libevdev_get_uniq(idev));

    int uifd = open("/dev/uinput", O_RDWR);
    if (uifd < 0) {
        perror("open /dev/uinput");
        return 5;
    }

    err = libevdev_uinput_create_from_device(idev, uifd, &odev);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        return 6;
    }

    err = libevdev_grab(idev, LIBEVDEV_GRAB);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        return 7;
    }

    run_state_machine();
}

