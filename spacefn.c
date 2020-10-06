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
#include <sys/dir.h>
#include <unistd.h>

// Key mapping {{{1
unsigned int key_map(unsigned int code) {
    switch (code) {
        case KEY_BRIGHTNESSDOWN:    // my magical escape button
            exit(0);

        case KEY_J:
            return KEY_LEFT;
        case KEY_K:
            return KEY_DOWN;
        case KEY_L:
            return KEY_UP;
        case KEY_SEMICOLON:
            return KEY_RIGHT;

        case KEY_M:
            return KEY_HOME;
        case KEY_COMMA:
            return KEY_PAGEDOWN;
        case KEY_DOT:
            return KEY_PAGEUP;
        case KEY_SLASH:
            return KEY_END;

        case KEY_B:
            return KEY_SPACE;
    }
    return 0;
}

// Blacklist keys for which I have a mapping, to try and train myself out of using them
int blacklist(unsigned int code) {
    switch (code) {
        case KEY_UP:
        case KEY_DOWN:
        case KEY_RIGHT:
        case KEY_LEFT:
        case KEY_HOME:
        case KEY_END:
        case KEY_PAGEUP:
        case KEY_PAGEDOWN:
            return 1;
    }
    return 0;
}


// Global device handles {{{1
struct libevdev *idev;
struct libevdev_uinput *odev;
int fd;

// Ordered unique key buffer {{{1
#define MAX_BUFFER 8
unsigned int buffer[MAX_BUFFER];
unsigned int n_buffer = 0;

static int buffer_contains(unsigned int code) {
    for (int i=0; i<n_buffer; i++)
        if (buffer[i] == code)
            return 1;

    return 0;
}

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

// Key I/O functions {{{1
// output {{{2
#define V_RELEASE 0
#define V_PRESS 1
#define V_REPEAT 2
static void send_key(unsigned int code, int value) {
    libevdev_uinput_write_event(odev, EV_KEY, code, value);
    libevdev_uinput_write_event(odev, EV_SYN, SYN_REPORT, 0);
}

// Useful for debugging.
static void print_event(struct input_event *ev) {
    printf("Event: %s %s %d\n",
           libevdev_event_type_get_name(ev->type),
           libevdev_event_code_get_name(ev->type, ev->code),
           ev->value);
}

// input {{{2
static int read_one_key(struct input_event *ev) {
    int err = libevdev_next_event(idev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, ev);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        exit(1);
    }

    if (ev->type != EV_KEY) {
        libevdev_uinput_write_event(odev, ev->type, ev->code, ev->value);
        return -1;
    }

    if (blacklist(ev->code))
        return -1;

    return 0;
}

// State functions {{{1
enum {
    IDLE,
    DECIDE,
    SHIFT,
} state = IDLE;

static void state_idle(void) {  // {{{2
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

// Change buffer from decide state raw keys to shift state mapped keys.
// Just clearing the buffer on decide -> shift change can lead to presses without
// a release sometimes and that can lock a laptop trackpad.
void fix_buffer() {
    unsigned int tbuffer[MAX_BUFFER];
    int moves = 0;
    for (int i=0; i<n_buffer; i++) {
        unsigned int code = key_map(buffer[i]);
        if (!code) {
            code = buffer[i];
        } else {
            tbuffer[moves++] = code;
        }
        send_key(code, V_PRESS);
    }
    n_buffer = moves;
    if (n_buffer > 0) memcpy(buffer, tbuffer, n_buffer * sizeof(*buffer));
}

static void state_decide(void) {    // {{{2
    n_buffer = 0;
    struct input_event ev;
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    fd_set set;
    FD_ZERO(&set);

    while (timeout.tv_usec >= 0) {
        FD_SET(fd, &set);
        int nfds = select(fd+1, &set, NULL, NULL, &timeout);
        if (!nfds)
            break;

        while (read_one_key(&ev));

        if (ev.value == V_PRESS) {
            buffer_append(ev.code);
            continue;
        }

        if (ev.code == KEY_SPACE && ev.value == V_RELEASE) {
            send_key(KEY_SPACE, V_PRESS);
            send_key(KEY_SPACE, V_RELEASE);
            // These weren't mapped, so send the actual presses and clear the buffer.
            for (int i=0; i<n_buffer; i++)
                send_key(buffer[i], V_PRESS);
            n_buffer = 0;
            state = IDLE;
            return;
        }

        if (ev.value == V_RELEASE && !buffer_contains(ev.code)) {
            send_key(ev.code, ev.value);
            continue;
        }

        if (ev.value == V_RELEASE && buffer_remove(ev.code)) {
            unsigned int code = key_map(ev.code);
            if (code) {
                send_key(code, V_PRESS);
                send_key(code, V_RELEASE);
            } else {
                send_key(ev.code, V_PRESS);
                send_key(ev.code, V_RELEASE);
            }
            state = SHIFT;
            fix_buffer();
            return;
        }
    }

    printf("timed out\n");
    fix_buffer();
    state = SHIFT;
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
        printf("state %d\n", state);
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

int main(int argc, char **argv) {   // {{{1
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
    // Not sure who to solve this properly, the enter release will come from
    // spacefn without it and X seems to not connect the press and release in 
    // this case (different logical keyboards).
    sleep(1);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open input");
        return 1;
    }

    int err = libevdev_new_from_fd(fd, &idev);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        return 1;
    }
    printf("Input device name: \"%s\"\n", libevdev_get_name(idev));
    printf("Input device ID: bus %#x vendor %#x product %#x\n",
       libevdev_get_id_bustype(idev),
       libevdev_get_id_vendor(idev),
       libevdev_get_id_product(idev));
    if (!is_keeb(idev)) {
        fprintf(stderr, "This device does not look like a keyboard\n");
        return 1;
    }
    printf("Location: %s\n", libevdev_get_phys(idev));
    if (libevdev_get_uniq(idev)) printf("Identity: %s\n", libevdev_get_uniq(idev));

    int uifd = open("/dev/uinput", O_RDWR);
    if (uifd < 0) {
        perror("open /dev/uinput");
        return 1;
    }

    err = libevdev_uinput_create_from_device(idev, uifd, &odev);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        return 1;
    }

    err = libevdev_grab(idev, LIBEVDEV_GRAB);
    if (err) {
        fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
        return 1;
    }

    run_state_machine();
}

