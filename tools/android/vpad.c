/* vpad.c - a virtual copy of the AYANEO Pocket EVO's built-in controller (uinput), for testing the
   VS. Super Mario Bros. port on the device. Same keys, axes, ranges and USB ids as "AYANEO Controller".
   usage: vpad FIFO [-t] [-x]  (-t: no analog trigger axes, L2 / R2 as plain buttons only;
                                -x: an Xbox 360 pad instead (xpad layout, 045e:028e), which Steam
                                Input takes over on a Steam Deck the way it does a real one)
   commands, one per line, written to FIFO:
     down BTN | up BTN | press BTN [ms] | chord BTN1 BTN2 [gap_ms] [hold_ms] | wait ms | done PATH | quit
   buttons: a b x y l1 r1 l2 r2 (key + axis) l2a r2a (axis only) l2k r2k (key only)
            select start mode l3 r3 up down left right */
#include <linux/uinput.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

static int fd;
static int no_trigger_axes;
static int xbox;

static void emit(int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) perror("write");
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }
static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

struct btn { const char *name; int key; int abs; int absval; };
static const struct btn btns[] = {
    { "a", 304, -1, 0 }, { "b", 305, -1, 0 }, { "x", 307, -1, 0 }, { "y", 308, -1, 0 },
    { "l1", 310, -1, 0 }, { "r1", 311, -1, 0 },
    { "l2", 312, ABS_BRAKE, 255 }, { "r2", 313, ABS_GAS, 255 },
    { "l2a", -1, ABS_BRAKE, 255 }, { "r2a", -1, ABS_GAS, 255 },
    { "l2k", 312, -1, 0 }, { "r2k", 313, -1, 0 },
    { "select", 314, -1, 0 }, { "start", 315, -1, 0 }, { "mode", 316, -1, 0 },
    { "l3", 317, -1, 0 }, { "r3", 318, -1, 0 },
    { "up", -1, ABS_HAT0Y, -1 }, { "down", -1, ABS_HAT0Y, 1 },
    { "left", -1, ABS_HAT0X, -1 }, { "right", -1, ABS_HAT0X, 1 },
};

static const struct btn *find(const char *n) {
    for (size_t i = 0; i < sizeof btns / sizeof btns[0]; i++) if (!strcmp(btns[i].name, n)) return &btns[i];
    return NULL;
}

static void set(const struct btn *b, int on) {
    int trig_l = b->abs == ABS_BRAKE || b->key == 312, trig_r = b->abs == ABS_GAS || b->key == 313;
    if (xbox && (trig_l || trig_r)) {   /* xpad: the triggers are the ABS_Z / ABS_RZ axes, no buttons */
        emit(EV_ABS, trig_l ? ABS_Z : ABS_RZ, on ? 255 : 0);
        return;
    }
    if (b->key >= 0) emit(EV_KEY, b->key, on);
    if (b->abs >= 0 && !(no_trigger_axes && (b->abs == ABS_GAS || b->abs == ABS_BRAKE))) emit(EV_ABS, b->abs, on ? b->absval : 0);
}

static void abs_setup(int code, int min, int max, int fuzz, int flat) {
    struct uinput_abs_setup s;
    memset(&s, 0, sizeof s);
    s.code = (unsigned short)code;
    s.absinfo.minimum = min; s.absinfo.maximum = max; s.absinfo.fuzz = fuzz; s.absinfo.flat = flat;
    if (ioctl(fd, UI_ABS_SETUP, &s) < 0) perror("UI_ABS_SETUP");
}

static int create(void) {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); return 0; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int k = 304; k <= 318; k++)
        if (!xbox || (k != 306 && k != 309 && k != 312 && k != 313)) ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    if (xbox) {   /* xpad: sticks X/Y and RX/RY, triggers Z / RZ 0..255 */
        int sticks[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
        for (int i = 0; i < 4; i++) { ioctl(fd, UI_SET_ABSBIT, sticks[i]); abs_setup(sticks[i], -32768, 32767, 16, 128); }
        ioctl(fd, UI_SET_ABSBIT, ABS_Z); abs_setup(ABS_Z, 0, 255, 0, 0);
        ioctl(fd, UI_SET_ABSBIT, ABS_RZ); abs_setup(ABS_RZ, 0, 255, 0, 0);
    } else {
        int sticks[] = { ABS_X, ABS_Y, ABS_Z, ABS_RZ };
        for (int i = 0; i < 4; i++) { ioctl(fd, UI_SET_ABSBIT, sticks[i]); abs_setup(sticks[i], -32768, 32767, 255, 4095); }
    }
    if (!xbox && !no_trigger_axes) {
        ioctl(fd, UI_SET_ABSBIT, ABS_GAS); abs_setup(ABS_GAS, 0, 255, 0, 15);
        ioctl(fd, UI_SET_ABSBIT, ABS_BRAKE); abs_setup(ABS_BRAKE, 0, 255, 0, 15);
    }
    ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X); abs_setup(ABS_HAT0X, -1, 1, 0, 0);
    ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y); abs_setup(ABS_HAT0Y, -1, 1, 0, 0);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    if (xbox) {
        us.id.vendor = 0x045e; us.id.product = 0x028e; us.id.version = 0x0114;
        snprintf(us.name, sizeof us.name, "Microsoft X-Box 360 pad");
    } else {
        us.id.vendor = 0x4001; us.id.product = 0x0428; us.id.version = 0x0111;
        snprintf(us.name, sizeof us.name, "AYANEO Controller");
    }
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); return 0; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 0; }
    return 1;
}

static int run(char *line) {
    char cmd[32] = "", a1[256] = "", a2[64] = "";
    int n1 = -1, n2 = -1;
    int got = sscanf(line, "%31s %255s %63s %d %d", cmd, a1, a2, &n1, &n2);
    if (got < 1) return 1;
    if (!strcmp(cmd, "quit")) return 0;
    if (!strcmp(cmd, "wait")) { msleep(atoi(a1)); return 1; }
    if (!strcmp(cmd, "done")) { FILE *f = fopen(a1, "w"); if (f) fclose(f); return 1; }
    const struct btn *b = find(a1);
    if (!b) { fprintf(stderr, "vpad: unknown button '%s'\n", a1); return 1; }
    if (!strcmp(cmd, "down")) { set(b, 1); syn(); }
    else if (!strcmp(cmd, "up")) { set(b, 0); syn(); }
    else if (!strcmp(cmd, "press")) {
        int ms = got >= 3 ? atoi(a2) : 120;
        set(b, 1); syn(); msleep(ms > 0 ? ms : 120); set(b, 0); syn();
    } else if (!strcmp(cmd, "chord")) {
        const struct btn *c = find(a2);
        if (!c) { fprintf(stderr, "vpad: unknown button '%s'\n", a2); return 1; }
        int gap = got >= 4 ? n1 : 0, hold = got >= 5 ? n2 : 200;
        set(b, 1); syn();
        if (gap > 0) msleep(gap);
        set(c, 1); syn(); msleep(hold);
        set(b, 0); set(c, 0); syn();
    } else fprintf(stderr, "vpad: unknown command '%s'\n", cmd);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vpad FIFO [-t]\n"); return 2; }
    for (int a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "-t")) no_trigger_axes = 1;
        else if (!strcmp(argv[a], "-x")) xbox = 1;
    }
    if (!create()) return 1;
    printf("vpad: created%s\n", no_trigger_axes ? " (digital triggers only)" : "");
    fflush(stdout);
    for (;;) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { perror(argv[1]); return 1; }
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (!run(line)) { fclose(f); ioctl(fd, UI_DEV_DESTROY); close(fd); printf("vpad: removed\n"); return 0; }
        }
        fclose(f);
    }
}
