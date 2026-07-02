#define _GNU_SOURCE
#include "serial.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
/* termios2 lives in <asm/termbits.h>; do NOT also include <termios.h> —
 * the two clash. We drive the port entirely through TCGETS2/TCSETS2. */
#include <asm/termbits.h>

int serial_open(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) { perror("radar: open serial"); return -1; }

    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) < 0) { perror("radar: TCGETS2"); close(fd); return -1; }

    /* Raw 8N1, no flow control, receiver on, ignore modem lines. */
    tio.c_cflag &= ~(CBAUD | PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD | BOTHER;
    tio.c_ispeed = baud;
    tio.c_ospeed = baud;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR |
                     ISTRIP | INPCK | BRKINT | IGNBRK | PARMRK);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG | IEXTEN);
    tio.c_cc[VMIN]  = 0;    /* return whatever is available... */
    tio.c_cc[VTIME] = 1;    /* ...within 100 ms — keeps the loop responsive */

    if (ioctl(fd, TCSETS2, &tio) < 0) { perror("radar: TCSETS2"); close(fd); return -1; }
    ioctl(fd, TCFLSH, TCIOFLUSH);   /* drop any stale bytes from a prior run */
    return fd;
}
