#include "keys.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

int vremote_open(const int* codes, int ncodes)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -errno;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0)
        goto fail;
    for (int i = 0; i < ncodes; i++) {
        if (codes[i] > 0 && codes[i] < KEY_MAX)
            ioctl(fd, UI_SET_KEYBIT, codes[i]);
    }

    // Legacy setup API: works on the older kernels webOS 4.x ships.
    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof(dev));
    strncpy(dev.name, VREMOTE_NAME, UINPUT_MAX_NAME_SIZE - 1);
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x1234;
    dev.id.product = 0x5678;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof(dev)) != sizeof(dev))
        goto fail;
    if (ioctl(fd, UI_DEV_CREATE) < 0)
        goto fail;

    // Give udev / the input stack a moment to pick the device up.
    usleep(300 * 1000);
    return fd;

fail : {
    int e = errno;
    close(fd);
    return -e;
}
}

static int emit(int fd, int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

int vremote_press(int fd, int code)
{
    if (fd < 0)
        return -1;
    int r = 0;
    r |= emit(fd, EV_KEY, code, 1);
    r |= emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(40 * 1000);
    r |= emit(fd, EV_KEY, code, 0);
    r |= emit(fd, EV_SYN, SYN_REPORT, 0);
    return r;
}

void vremote_close(int fd)
{
    if (fd < 0)
        return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
