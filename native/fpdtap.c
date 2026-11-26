// fpdtap: side-fingerprint fixes for the Redmi K40S (munch) HyperOS 4 port.
//
// 1. Double-tap. The vendor Goodix HAL sends a double-tap as GF key 6 through /dev/goodix_fp. The port
//    kernel's gf_spi passes it on as raw linux keycode 6, which uinput-goodix does not register, so it is
//    dropped (dmesg: "gf_kernel_key_input: received key event[6], key=6, value=N"). HyperOS
//    (MiuiFingerPrintTapListener) wants BTN_C (KEYCODE_BUTTON_C) from vendor 0x0666 / product 0x0888.
//    We follow /dev/kmsg and replay those lines as BTN_C on our own uinput device.
//
// 2. Swipe to zoom in the camera. libgf_hal has a working navigation mode for the Milan sensor, but
//    nothing ever calls gf_hal_navigate(). While com.android.camera is the top app and the screen is on,
//    we call gf_hal_navigate(dev, XY) inside the HAL process with ptrace. Swipes then arrive as
//    KEY_UP/KEY_DOWN on the HAL's uinput_nav device; we grab it and send KEY_ZOOMIN/KEY_ZOOMOUT, which
//    MiuiCamera handles natively. Navigation mode cannot be switched off again (navigate(NONE) and cancel
//    both leave the sensor without tap detection), so on leaving the camera we restart the HAL service and
//    restore the double-tap key status by writing the HAL's key-status variable through /proc/<pid>/mem.
//
// Config files in /data/adb/fpdtap/: swipe_zoom_off (disable 2), zoom_steps (default 5, 0.1x each),
// swipe_invert (swipe up = zoom out).
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "FpDoubleTap"
// logd tends to prune root's logs on this ROM, so every line also goes to the kernel log (dmesg).
#include <stdarg.h>
static void logx(int prio, const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    __android_log_write(prio, TAG, b);
    static int kw = -2;
    if (kw == -2) kw = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kw >= 0) dprintf(kw, "<%d>fpdtap: %s\n", prio == ANDROID_LOG_ERROR ? 3 : 6, b);
}
#define LOGI(...) logx(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGE(...) logx(ANDROID_LOG_ERROR, __VA_ARGS__)

#define CFG_DIR "/data/adb/fpdtap"
#define KMSG_MATCH "gf_kernel_key_input: received key event[6]"
#define HAL_NAME "android.hardware.biometrics.fingerprint@2.1-service"
#define CAMERA_PKG "com.android.camera"
#define TOP_APP "/dev/cpuset/top-app/cgroup.procs"
#define BACKLIGHT "/sys/class/backlight/panel0-backlight/brightness"

// libgf_hal.so (vendor V816.0.9.0) offsets and their first 8 bytes, checked before every call.
#define OFF_NAVIGATE 0x1c2f8     // int gf_hal_navigate(gf_fingerprint_device*, gf_nav_mode_t)
static const uint8_t PROLOGUE[8] = {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9};
#define OFF_KEY_STATUS 0x50f50   // int written by gf_hal_common_set_key_status (2 = double-tap mode)
#define OFF_SET_KEY_STATUS 0x22be4  // gf_hal_common_set_key_status: adrp x8; str w0,[x8,#0xf50]; ret
static const uint8_t SET_KEY_CODE[8] = {0x68, 0x01, 0x00, 0xd0, 0x00, 0x51, 0x0f, 0xb9};
#define GOODIX_DEV_GOT 0x6868    // fingerprint.goodix.so: GOT slot -> &dev
#define NAV_MODE_XY 3

static volatile sig_atomic_t stop_req;
static int tap_fd = -1, zoom_fd = -1;

// ---------- small helpers ----------
static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static int file_exists(const char *n) {
    char p[128];
    snprintf(p, sizeof(p), CFG_DIR "/%s", n);
    return access(p, F_OK) == 0;
}
static int cfg_int(const char *n, int def) {
    char p[128], b[16] = "";
    snprintf(p, sizeof(p), CFG_DIR "/%s", n);
    FILE *f = fopen(p, "r");
    if (!f) return def;
    int v = fgets(b, sizeof(b), f) ? atoi(b) : def;
    fclose(f);
    return v > 0 ? v : def;
}
static int read_cmdline(int pid, char *buf, size_t n) {
    char p[48];
    snprintf(p, sizeof(p), "/proc/%d/cmdline", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    return 0;
}
static int find_pid(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    char cmd[256];
    while ((e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;
        if (!read_cmdline(pid, cmd, sizeof(cmd))) {
            const char *b = strrchr(cmd, '/');
            if (!strcmp(b ? b + 1 : cmd, name)) { found = pid; break; }
        }
    }
    closedir(d);
    return found;
}

// ---------- uinput ----------
static int make_uinput(const char *name, int vendor, int product, const int *keys, int nkeys) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < nkeys; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_HOST;
    us.id.vendor = vendor;
    us.id.product = product;
    us.id.version = 1;
    snprintf(us.name, sizeof(us.name), "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    return fd;
}
static void emit(int fd, int code, int val) {
    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY; ev[0].code = code; ev[0].value = val;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    if (write(fd, ev, sizeof(ev)) != sizeof(ev)) LOGE("uinput write: %s", strerror(errno));
}

// ---------- ptrace call into the HAL ----------
static uint64_t map_base(int pid, const char *lib) {
    char p[48], line[512];
    snprintf(p, sizeof(p), "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    uint64_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t s, off;
        char path[256] = "";
        if (sscanf(line, "%" SCNx64 "-%*" SCNx64 " %*s %" SCNx64 " %*s %*s %255s", &s, &off, path) < 2) continue;
        const char *b = strrchr(path, '/');
        if (b && !strcmp(b + 1, lib) && off == 0) { base = s; break; }
    }
    fclose(f);
    return base;
}
static int rd_mem(int pid, uint64_t a, void *buf, size_t n) {
    char p[48];
    snprintf(p, sizeof(p), "/proc/%d/mem", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ok = pread64(fd, buf, n, (off64_t)a) == (ssize_t)n;
    close(fd);
    return ok ? 0 : -1;
}
// Set the HAL's key status by writing the variable directly: no ptrace stop, no HAL mutex involved.
static int write_key_status(int pid, int32_t v) {
    uint64_t hal = map_base(pid, "libgf_hal.so");
    uint8_t code[8];
    if (!hal || rd_mem(pid, hal + OFF_SET_KEY_STATUS, code, 8) || memcmp(code, SET_KEY_CODE, 8)) {
        LOGE("set_key_status code mismatch, not writing");
        return -1;
    }
    char p[48];
    snprintf(p, sizeof(p), "/proc/%d/mem", pid);
    int fd = open(p, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int32_t back = -1;
    int ok = pwrite64(fd, &v, 4, (off64_t)(hal + OFF_KEY_STATUS)) == 4 &&
             pread64(fd, &back, 4, (off64_t)(hal + OFF_KEY_STATUS)) == 4 && back == v;
    close(fd);
    return ok ? 0 : -1;
}

static uint64_t hal_dev(int pid) {
    uint64_t glue = map_base(pid, "fingerprint.goodix.so"), slot = 0, dev = 0;
    if (!glue || rd_mem(pid, glue + GOODIX_DEV_GOT, &slot, 8) || !slot || rd_mem(pid, slot, &dev, 8)) return 0;
    return dev;
}
static int regset(int pid, int req, int type, void *p, size_t n) {
    struct iovec io = {p, n};
    return (int)ptrace(req, pid, (void *)(long)type, &io);
}
// Is the main thread blocked in its hwbinder ioctl? Only then is it outside the HAL mutex; hijacking it
// anywhere else can deadlock the HAL. Returns the binder fd, or -1.
static int binder_wait_fd(int pid) {
    char p[64], b[128] = "", link[64] = "";
    snprintf(p, sizeof(p), "/proc/%d/syscall", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    long nr = -1;
    unsigned long long a0 = 0;
    if (sscanf(b, "%ld 0x%llx", &nr, &a0) != 2 || nr != 29 /* __NR_ioctl */) return -1;
    snprintf(p, sizeof(p), "/proc/%d/fd/%llu", pid, a0);
    n = readlink(p, link, sizeof(link) - 1);
    if (n > 0) link[n] = 0;
    return strstr(link, "binder") ? (int)a0 : -1;
}

// After the stop the interrupted ioctl has either been rewound already (pc on the svc, x0 = fd) or not yet
// (pc after the svc, x0 = -ERESTARTSYS/-EINTR). Normalise to the rewound form so restoring re-issues it.
static int rewind_ioctl(int pid, struct user_pt_regs *r, int fd) {
    uint32_t at = 0, before = 0;
    if (r->regs[8] != 29) return -1;
    rd_mem(pid, r->pc, &at, 4);
    rd_mem(pid, r->pc - 4, &before, 4);
    if (at == 0xd4000001 && (int)r->regs[0] == fd) return 0;
    int64_t x0 = (int64_t)r->regs[0];
    if (before == 0xd4000001 && (x0 == -512 || x0 == -513 || x0 == -514 || x0 == -516 || x0 == -4)) {
        r->pc -= 4;
        r->regs[0] = (uint64_t)fd;
        return 0;
    }
    logx(ANDROID_LOG_INFO, "HAL busy: pc=%llx x0=%lld", (unsigned long long)r->pc, (long long)x0);
    return -1;
}

// Run func(a0, a1) on the HAL main thread. 0 = done (*ret set), 1 = HAL busy (retry later), -1 = error.
static int hal_call(int pid, uint64_t off, uint64_t a0, uint64_t a1, int64_t *ret) {
    uint64_t hal = map_base(pid, "libgf_hal.so");
    uint8_t code[8];
    if (!hal || rd_mem(pid, hal + off, code, 8) || memcmp(code, PROLOGUE, 8)) {
        LOGE("libgf_hal+%" PRIx64 " not found or prologue mismatch", off);
        return -1;
    }
    int bfd = binder_wait_fd(pid);
    if (bfd < 0) return 1;
    if (ptrace(PTRACE_SEIZE, pid, 0, 0)) { LOGE("seize: %s", strerror(errno)); return -1; }
    int st, rc = -1;
    if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) || waitpid(pid, &st, __WALL) < 0) goto out;
    struct user_pt_regs saved, r;
    struct user_fpsimd_state fp;
    if (regset(pid, PTRACE_GETREGSET, NT_PRSTATUS, &saved, sizeof(saved)) ||
        regset(pid, PTRACE_GETREGSET, NT_PRFPREG, &fp, sizeof(fp))) goto out;
    if (rewind_ioctl(pid, &saved, bfd)) { rc = 1; goto out; }
    r = saved;
    r.regs[0] = a0;
    r.regs[1] = a1;
    r.regs[30] = 0;  // return to NULL: the SIGSEGV stop tells us the call finished
    r.sp = (saved.sp - 0x400) & ~0xfULL;
    r.pc = hal + off;
    regset(pid, PTRACE_SETREGSET, NT_PRSTATUS, &r, sizeof(r));
    ptrace(PTRACE_CONT, pid, 0, 0);
    for (;;) {
        long t0 = now_ms();
        int w;
        while ((w = waitpid(pid, &st, __WALL | WNOHANG)) == 0 && now_ms() - t0 < 3000) usleep(5000);
        if (w == 0) {  // stuck inside the HAL: kill it, init restarts it, the framework reconnects
            LOGE("HAL call timed out, restarting HAL");
            kill(pid, SIGKILL);
            waitpid(pid, &st, __WALL);
            return -1;
        }
        if (w < 0 || !WIFSTOPPED(st)) { LOGE("HAL lost during call"); return -1; }
        regset(pid, PTRACE_GETREGSET, NT_PRSTATUS, &r, sizeof(r));
        if (WSTOPSIG(st) == SIGSEGV) {
            if (r.pc == 0) { *ret = (int32_t)r.regs[0]; rc = 0; }
            else LOGE("fault inside HAL call at %llx", (unsigned long long)r.pc);
            break;
        }
        ptrace(PTRACE_CONT, pid, 0, 0);  // unrelated stop: continue without the signal
    }
    regset(pid, PTRACE_SETREGSET, NT_PRSTATUS, &saved, sizeof(saved));
    regset(pid, PTRACE_SETREGSET, NT_PRFPREG, &fp, sizeof(fp));
out:
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return rc;
}

// ---------- state checks ----------
static int screen_on(void) {
    char b[16] = "";
    int fd = open(BACKLIGHT, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    return n > 0 && atoi(b) > 0;
}
static int camera_on_top(void) {
    FILE *f = fopen(TOP_APP, "r");
    if (!f) return 0;
    int pid, hit = 0;
    char cmd[64];
    while (!hit && fscanf(f, "%d", &pid) == 1)
        if (!read_cmdline(pid, cmd, sizeof(cmd)) && !strcmp(cmd, CAMERA_PKG)) hit = 1;
    fclose(f);
    return hit;
}
static int open_input_by_name(const char *want) {
    char p[32], name[64];
    for (int i = 0; i < 32; i++) {
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, want)) return fd;
        close(fd);
    }
    return -1;
}
static int double_tap_status(void) {
    // Same rule as PowerFingerprintServiceStubImpl: anything but empty/"none" -> 2.
    FILE *p = popen("/system/bin/settings get system fingerprint_double_tap 2>/dev/null", "r");
    char b[64] = "";
    if (p) { if (!fgets(b, sizeof(b), p)) b[0] = 0; pclose(p); }
    b[strcspn(b, "\r\n")] = 0;
    return (!b[0] || !strcmp(b, "none") || !strcmp(b, "null")) ? 0 : 2;
}

// ---------- swipe-zoom state machine ----------
static int nav_on, nav_pid = -1, nav_fd = -1;
static long nav_fail_until;

static void enter_nav(void) {
    int pid = find_pid(HAL_NAME);
    uint64_t dev = pid > 0 ? hal_dev(pid) : 0;
    int64_t ret = -1;
    int rc = dev ? hal_call(pid, OFF_NAVIGATE, dev, NAV_MODE_XY, &ret) : -1;
    if (rc == 1) return;  // HAL busy: try again on the next tick
    if (rc || ret) {
        LOGE("enter nav failed (pid=%d rc=%d ret=%" PRId64 ")", pid, rc, ret);
        nav_fail_until = now_ms() + 10000;
        return;
    }
    nav_on = 1;
    nav_pid = pid;
    nav_fd = open_input_by_name("uinput_nav");
    if (nav_fd >= 0 && ioctl(nav_fd, EVIOCGRAB, 1)) LOGE("grab uinput_nav: %s", strerror(errno));
    LOGI("camera: swipe-zoom on (hal %d)", pid);
}

static void leave_nav(void) {
    if (nav_fd >= 0) { close(nav_fd); nav_fd = -1; }
    nav_on = 0;
    // The sensor stays in navigation mode until the HAL restarts; init brings it back at once.
    if (nav_pid > 0 && find_pid(HAL_NAME) == nav_pid) kill(nav_pid, SIGTERM);
    int pid = -1;
    uint64_t dev = 0;
    for (long t0 = now_ms(); now_ms() - t0 < 10000; usleep(100 * 1000)) {
        pid = find_pid(HAL_NAME);
        if (pid > 0 && pid != nav_pid && (dev = hal_dev(pid))) break;
    }
    if (!dev) { LOGE("HAL did not come back"); nav_pid = -1; return; }
    int st = double_tap_status();  // takes ~0.5 s (app_process), also lets the HAL finish starting
    usleep(1000 * 1000);
    if (write_key_status(pid, st)) LOGE("restore key status failed");
    LOGI("camera left: hal restarted (%d -> %d), key status %d", nav_pid, pid, st);
    nav_pid = -1;
}

static void handle_nav_input(void) {
    struct input_event ev[16];
    ssize_t n = read(nav_fd, ev, sizeof(ev));
    if (n < 0 && errno == ENODEV) { close(nav_fd); nav_fd = -1; return; }
    for (int i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
        if (ev[i].type != EV_KEY || ev[i].value != 1) continue;  // taps arrive as volume keys: dropped
        int dir = ev[i].code == KEY_UP ? 1 : ev[i].code == KEY_DOWN ? -1 : 0;
        if (!dir) continue;
        if (file_exists("swipe_invert")) dir = -dir;
        int key = dir > 0 ? KEY_ZOOMIN : KEY_ZOOMOUT, steps = cfg_int("zoom_steps", 5);
        for (int s = 0; s < steps; s++) {
            emit(zoom_fd, key, 1);
            emit(zoom_fd, key, 0);
            usleep(25 * 1000);
        }
    }
}

static void on_term(int sig) { (void)sig; stop_req = 1; }

int main(void) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);

    int tap_keys[] = {BTN_C}, zoom_keys[] = {KEY_ZOOMIN, KEY_ZOOMOUT};
    // Named uinput-goodix so /system/usr/keylayout/uinput-goodix.kl maps 306 -> BUTTON_C.
    tap_fd = make_uinput("uinput-goodix", 0x0666, 0x0888, tap_keys, 1);
    zoom_fd = make_uinput("fpdtap-zoom", 0, 0, zoom_keys, 2);  // Generic.kl: 418/419 -> ZOOM_IN/OUT
    if (tap_fd < 0 || zoom_fd < 0) { LOGE("uinput: %s", strerror(errno)); return 1; }

    int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd < 0) { LOGE("kmsg: %s", strerror(errno)); return 1; }
    lseek(kfd, 0, SEEK_END);
    LOGI("started");

    int tap_down = 0;
    long last_check = 0;
    char buf[1024];
    while (!stop_req) {
        struct pollfd pf[2] = {{kfd, POLLIN, 0}, {nav_fd, POLLIN, 0}};
        int r = poll(pf, nav_fd >= 0 ? 2 : 1, 500);
        if (r < 0 && errno != EINTR) { LOGE("poll: %s", strerror(errno)); sleep(1); }

        if (r > 0 && (pf[0].revents & POLLIN)) {
            for (;;) {  // drain kmsg, one record per read
                ssize_t n = read(kfd, buf, sizeof(buf) - 1);
                if (n < 0) { if (errno == EPIPE) continue; break; }
                buf[n] = 0;
                if (!strstr(buf, KMSG_MATCH)) continue;
                char *v = strstr(buf, "value=");
                if (!v) continue;
                int val = atoi(v + 6) ? 1 : 0;
                if (val == tap_down) continue;
                tap_down = val;
                emit(tap_fd, BTN_C, val);
                if (val) LOGI("double tap");
            }
        }
        if (r > 0 && nav_fd >= 0 && (pf[1].revents & (POLLIN | POLLERR | POLLHUP))) handle_nav_input();

        long t = now_ms();
        if (t - last_check < 400) continue;
        last_check = t;
        int want = t >= nav_fail_until && !file_exists("swipe_zoom_off") && screen_on() && camera_on_top();
        if (nav_on && find_pid(HAL_NAME) != nav_pid) {  // HAL restarted behind our back: mode is gone
            if (nav_fd >= 0) { close(nav_fd); nav_fd = -1; }
            nav_on = 0;
            nav_pid = -1;
        }
        if (want && !nav_on) enter_nav();
        else if (!want && nav_on) leave_nav();
    }
    if (nav_on) leave_nav();
    ioctl(tap_fd, UI_DEV_DESTROY);
    ioctl(zoom_fd, UI_DEV_DESTROY);
    return 0;
}
