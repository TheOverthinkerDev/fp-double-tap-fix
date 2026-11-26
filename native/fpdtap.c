// fpdtap: bridge Goodix side-fingerprint double-tap to HyperOS on the K40S (munch) port.
//
// The vendor HAL sends double-tap as GF key 6 through ioctl on /dev/goodix_fp. The port kernel's
// gf_spi passes it on as raw linux keycode 6, which the uinput-goodix device does not register, so
// the event is dropped. It still logs "gf_kernel_key_input: received key event[6], key=6, value=N".
//
// HyperOS (MiuiFingerPrintTapListener) expects KEYCODE_BUTTON_C (linux BTN_C, 306 in
// uinput-goodix.kl) from an input device with vendor 0x0666 / product 0x0888.
// This daemon follows /dev/kmsg and replays those lines as BTN_C on its own uinput device.
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "FpDoubleTap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define DEV_NAME "uinput-goodix"   // reuse /system/usr/keylayout/uinput-goodix.kl (306 -> BUTTON_C)
#define DEV_VENDOR 0x0666
#define DEV_PRODUCT 0x0888
#define MATCH "gf_kernel_key_input: received key event[6]"

static int ufd = -1;

static void emit(int type, int code, int val) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = val;
    if (write(ufd, &ev, sizeof(ev)) != sizeof(ev)) LOGE("write uinput: %s", strerror(errno));
}

static void key(int down) {
    emit(EV_KEY, BTN_C, down);
    emit(EV_SYN, SYN_REPORT, 0);
}

static int open_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_C);
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_HOST;
    us.id.vendor = DEV_VENDOR;
    us.id.product = DEV_PRODUCT;
    us.id.version = 1;
    snprintf(us.name, sizeof(us.name), "%s", DEV_NAME);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void on_term(int sig) {
    (void)sig;
    if (ufd >= 0) ioctl(ufd, UI_DEV_DESTROY);
    _exit(0);
}

int main(void) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    ufd = open_uinput();
    if (ufd < 0) { LOGE("uinput: %s", strerror(errno)); return 1; }

    int kfd = open("/dev/kmsg", O_RDONLY | O_CLOEXEC);
    if (kfd < 0) { LOGE("kmsg: %s", strerror(errno)); on_term(0); }
    lseek(kfd, 0, SEEK_END);  // only new records
    LOGI("started, vendor=%04x product=%04x", DEV_VENDOR, DEV_PRODUCT);

    int down = 0;
    char buf[1024];
    for (;;) {
        ssize_t n = read(kfd, buf, sizeof(buf) - 1);  // one record per read
        if (n < 0) {
            if (errno == EPIPE || errno == EINTR) continue;  // ring overrun / signal
            LOGE("read kmsg: %s", strerror(errno));
            sleep(1);
            continue;
        }
        buf[n] = 0;
        if (!strstr(buf, MATCH)) continue;
        char *v = strstr(buf, "value=");
        if (!v) continue;
        int val = atoi(v + 6) ? 1 : 0;
        if (val == down) continue;  // keep press/release paired
        down = val;
        key(val);
        if (val) LOGI("double tap");
    }
}
