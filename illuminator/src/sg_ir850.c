/*
 * sg_ir850.c — Savgood SG-IR850-8M illuminator controller implementation.
 * See sg_ir850.h and docs/ILLUMINATOR.md.
 */
#define _POSIX_C_SOURCE 200809L  /* clock_gettime, nanosleep, O_CLOEXEC */
#define _DEFAULT_SOURCE          /* cfmakeraw, CRTSCTS                  */

#include "sg_ir850.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#define FRAME_LEN   7
#define SYNC_BYTE   0xFFu

/* ---- angle table (from SG-IR850-8M protocol spec, section 7) -------------- *
 * motor position -> beam coverage angle (degrees). The published table is
 * monotonic with a steady +0x10 (16) position step per ~1 deg above ~6 deg;
 * a handful of rows in the high 60s deg range were OCR-garbled in the PDF, so
 * those are reconstructed from that exact cadence (endpoints 0x06FE = 70 deg
 * and 0x06A4 = 64 deg are clean and anchor it). Interpolated for set_fov. */
typedef struct { uint16_t pos; double deg; } angle_entry_t;

static const angle_entry_t ANGLE_TABLE[] = {
    {0x0064,  1.96}, {0x00C4,  2.95}, {0x00F4,  4.09}, {0x0124,  4.91},
    {0x0154,  6.00}, {0x0184,  7.00}, {0x01C4,  8.00}, {0x01F4,  9.00},
    {0x0224, 10.00}, {0x0244, 11.00}, {0x0264, 12.00}, {0x0294, 13.00},
    {0x02B4, 14.00}, {0x02E4, 15.00}, {0x0304, 16.00}, {0x0334, 17.00},
    {0x0354, 18.00}, {0x0374, 19.00}, {0x0394, 20.00}, {0x03B4, 21.00},
    {0x03C4, 22.00}, {0x03D4, 23.00}, {0x03F4, 24.00}, {0x0414, 25.00},
    {0x0434, 26.00}, {0x0444, 27.00}, {0x0464, 28.00}, {0x0474, 29.00},
    {0x0484, 30.00}, {0x0494, 31.00}, {0x04A4, 32.00}, {0x04B4, 33.00},
    {0x04C4, 34.00}, {0x04D4, 35.00}, {0x04E4, 36.00}, {0x04F4, 37.00},
    {0x0504, 38.00}, {0x0514, 39.00}, {0x0524, 40.00}, {0x0534, 41.00},
    {0x0544, 42.00}, {0x0554, 43.00}, {0x0564, 44.00}, {0x0574, 45.00},
    {0x0584, 46.00}, {0x0594, 47.00}, {0x05A4, 48.00}, {0x05B4, 49.00},
    {0x05C4, 50.00}, {0x05D4, 51.00}, {0x05E4, 52.00}, {0x05F4, 53.00},
    {0x0604, 54.00}, {0x0614, 55.00}, {0x0624, 56.00}, {0x0634, 57.00},
    {0x0644, 58.00}, {0x0654, 59.00}, {0x0664, 60.00}, {0x0674, 61.00},
    {0x0684, 62.00}, {0x0694, 63.00}, {0x06A4, 64.00}, {0x06B4, 65.00},
    {0x06C4, 66.00}, {0x06D4, 67.00}, {0x06E4, 68.00}, {0x06F4, 69.00},
    {0x06FE, 70.00},
};
static const int ANGLE_TABLE_N =
    (int)(sizeof(ANGLE_TABLE) / sizeof(ANGLE_TABLE[0]));

uint16_t sg_angle_to_position(double degrees)
{
    if (degrees <= ANGLE_TABLE[0].deg)
        return ANGLE_TABLE[0].pos;
    if (degrees >= ANGLE_TABLE[ANGLE_TABLE_N - 1].deg)
        return ANGLE_TABLE[ANGLE_TABLE_N - 1].pos;
    for (int i = 1; i < ANGLE_TABLE_N; i++) {
        if (degrees <= ANGLE_TABLE[i].deg) {
            const angle_entry_t *a = &ANGLE_TABLE[i - 1];
            const angle_entry_t *b = &ANGLE_TABLE[i];
            double t = (degrees - a->deg) / (b->deg - a->deg);
            double pos = (double)a->pos + t * ((double)b->pos - (double)a->pos);
            return (uint16_t)(pos + 0.5);
        }
    }
    return ANGLE_TABLE[ANGLE_TABLE_N - 1].pos;
}

double sg_position_to_angle(uint16_t pos)
{
    if (pos <= ANGLE_TABLE[0].pos)
        return ANGLE_TABLE[0].deg;
    if (pos >= ANGLE_TABLE[ANGLE_TABLE_N - 1].pos)
        return ANGLE_TABLE[ANGLE_TABLE_N - 1].deg;
    for (int i = 1; i < ANGLE_TABLE_N; i++) {
        if (pos <= ANGLE_TABLE[i].pos) {
            const angle_entry_t *a = &ANGLE_TABLE[i - 1];
            const angle_entry_t *b = &ANGLE_TABLE[i];
            double t = ((double)pos - a->pos) / ((double)b->pos - a->pos);
            return a->deg + t * (b->deg - a->deg);
        }
    }
    return ANGLE_TABLE[ANGLE_TABLE_N - 1].deg;
}

/* ---- low-level framing ---------------------------------------------------- */

/* checksum = (addr + b3 + b4 + b5 + b6) mod 0x100 (sync byte excluded). */
static uint8_t frame_sum(const uint8_t f[FRAME_LEN])
{
    return (uint8_t)(f[1] + f[2] + f[3] + f[4] + f[5]);
}

static void sleep_us(int us)
{
    if (us <= 0)
        return;
    struct timespec ts = { us / 1000000, (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

/* Build + send one command frame (b3..b6 are the four payload bytes). */
static int send_frame(sg_ir850_t *dev, uint8_t b3, uint8_t b4,
                      uint8_t b5, uint8_t b6)
{
    if (!dev || dev->fd < 0)
        return -EBADF;

    uint8_t f[FRAME_LEN];
    f[0] = SYNC_BYTE;
    f[1] = dev->addr;
    f[2] = b3;
    f[3] = b4;
    f[4] = b5;
    f[5] = b6;
    f[6] = frame_sum(f);

    size_t off = 0;
    while (off < FRAME_LEN) {
        ssize_t n = write(dev->fd, f + off, FRAME_LEN - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        off += (size_t)n;
    }
    tcdrain(dev->fd);            /* block until physically transmitted        */
    sleep_us(dev->tx_gap_us);    /* spec: 1-2 ms between commands             */
    return 0;
}

/* Read exactly FRAME_LEN bytes of a valid response, resyncing on SYNC_BYTE.
 * Returns 0 and fills out[7] on success; -ETIMEDOUT if nothing valid arrives
 * within timeout_ms. */
static int read_frame(sg_ir850_t *dev, uint8_t out[FRAME_LEN], int timeout_ms)
{
    uint8_t buf[FRAME_LEN];
    int have = 0;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    long add_ns = (long)(timeout_ms % 1000) * 1000000L;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += add_ns;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long rem_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                      (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (rem_ms <= 0)
            return -ETIMEDOUT;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(dev->fd, &rfds);
        struct timeval tv = { rem_ms / 1000, (rem_ms % 1000) * 1000 };

        int r = select(dev->fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (r == 0)
            return -ETIMEDOUT;

        uint8_t b;
        ssize_t n = read(dev->fd, &b, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            continue;

        if (have == 0) {
            if (b != SYNC_BYTE)     /* wait for frame start                   */
                continue;
            buf[have++] = b;
        } else {
            buf[have++] = b;
            if (have == FRAME_LEN) {
                if (frame_sum(buf) == buf[6]) {
                    memcpy(out, buf, FRAME_LEN);
                    return 0;
                }
                /* bad checksum: resync — keep any embedded SYNC byte. */
                have = 0;
                for (int i = 1; i < FRAME_LEN; i++) {
                    if (buf[i] == SYNC_BYTE) {
                        int rest = FRAME_LEN - i;
                        memmove(buf, buf + i, (size_t)rest);
                        have = rest;
                        break;
                    }
                }
            }
        }
    }
}

/* Send a query (b3..b6) and read back the matching response frame. */
static int query(sg_ir850_t *dev, uint8_t b3, uint8_t b4,
                 uint8_t expect_b3, uint8_t expect_b4,
                 uint8_t resp[FRAME_LEN])
{
    int rc = send_frame(dev, b3, b4, 0x00, 0x00);
    if (rc)
        return rc;

    /* Read frames until the instruction echo matches (spec streams version
     * frames every 2 ms; for simple queries the first match is the answer). */
    for (int tries = 0; tries < 8; tries++) {
        uint8_t f[FRAME_LEN];
        rc = read_frame(dev, f, 200);
        if (rc)
            return rc;
        if (f[2] == expect_b3 && f[3] == expect_b4) {
            memcpy(resp, f, FRAME_LEN);
            return 0;
        }
    }
    return -ETIMEDOUT;
}

/* ---- serial open/close ---------------------------------------------------- */

int sg_open(sg_ir850_t *dev, const char *port, uint8_t addr)
{
    if (!dev || !port)
        return -EINVAL;

    dev->fd = -1;
    dev->addr = addr ? addr : SG_IR850_DEFAULT_ADDR;
    dev->tx_gap_us = 2000;       /* 2 ms, per spec advice                     */

    int fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);

    tio.c_cflag |= (CLOCAL | CREAD);      /* local line, enable receiver       */
    tio.c_cflag &= ~CSTOPB;               /* 1 stop bit                        */
    tio.c_cflag &= ~PARENB;               /* no parity                         */
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;                   /* 8 data bits                       */
    tio.c_cflag &= ~CRTSCTS;              /* no HW flow control                */
    tio.c_iflag &= ~(IXON | IXOFF | IXANY); /* no SW flow control             */
    tio.c_cc[VMIN] = 0;                   /* non-blocking-ish; select() gates  */
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    tcflush(fd, TCIOFLUSH);

    dev->fd = fd;
    return 0;
}

void sg_close(sg_ir850_t *dev)
{
    if (dev && dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

/* ---- control -------------------------------------------------------------- */

int sg_power(sg_ir850_t *dev, bool on)
{
    return send_frame(dev, 0x01, 0x01, on ? 0x01 : 0x00, 0x00);
}

int sg_set_power(sg_ir850_t *dev, uint8_t level)
{
    return send_frame(dev, 0x01, 0x03, level, 0x00);
}

int sg_power_step(sg_ir850_t *dev, bool increase)
{
    /* increase = data 0x00, decrease = data 0x01 (spec section 4.2) */
    return send_frame(dev, 0x01, 0x02, increase ? 0x00 : 0x01, 0x00);
}

int sg_zoom_to_position(sg_ir850_t *dev, uint16_t pos)
{
    if (pos < SG_POS_MIN)
        pos = SG_POS_MIN;
    if (pos > SG_POS_MAX)
        pos = SG_POS_MAX;
    return send_frame(dev, 0x01, 0x05,
                      (uint8_t)(pos >> 8), (uint8_t)(pos & 0xFF));
}

int sg_zoom_step(sg_ir850_t *dev, bool tele, uint8_t steps)
{
    /* TELE (narrow) = data1 0x00, WIDE (flood) = data1 0x01; data2 = steps. */
    return send_frame(dev, 0x01, 0x04, tele ? 0x00 : 0x01, steps);
}

int sg_set_fov_deg(sg_ir850_t *dev, double degrees)
{
    return sg_zoom_to_position(dev, sg_angle_to_position(degrees));
}

int sg_zoom_reset(sg_ir850_t *dev)
{
    return send_frame(dev, 0x01, 0x06, 0x00, 0x00);
}

/* ---- queries -------------------------------------------------------------- */

int sg_query_power(sg_ir850_t *dev, int *on)
{
    uint8_t r[FRAME_LEN];
    int rc = query(dev, 0x02, 0x01, 0x02, 0x01, r);
    if (rc)
        return rc;
    if (on)
        *on = r[4] ? 1 : 0;
    return 0;
}

int sg_query_current(sg_ir850_t *dev, int *level)
{
    uint8_t r[FRAME_LEN];
    int rc = query(dev, 0x02, 0x03, 0x02, 0x03, r);
    if (rc)
        return rc;
    if (level)
        *level = r[4];
    return 0;
}

int sg_query_position(sg_ir850_t *dev, uint16_t *pos)
{
    uint8_t r[FRAME_LEN];
    int rc = query(dev, 0x02, 0x05, 0x02, 0x05, r);
    if (rc)
        return rc;
    if (pos)
        *pos = (uint16_t)((r[4] << 8) | r[5]);
    return 0;
}

int sg_query_fan(sg_ir850_t *dev, int *on)
{
    uint8_t r[FRAME_LEN];
    int rc = query(dev, 0x02, 0x0F, 0x02, 0x0F, r);
    if (rc)
        return rc;
    if (on)
        *on = r[4] ? 1 : 0;
    return 0;
}
