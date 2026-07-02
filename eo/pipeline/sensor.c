/* IMX296 sensor control over i2c: exposure (SHS1) + gain, atomically latched
 * with REGHOLD (CTRL08). Mirrors the validated bench-tool i2c AE. */
#include "pipeline.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

/* 16-bit register address, N data bytes, one I2C write transaction. */
static int i2c_w(Sensor *s, uint16_t reg, const uint8_t *data, int n)
{
    uint8_t buf[8];
    if (n + 2 > (int)sizeof(buf)) return -1;
    buf[0] = reg >> 8; buf[1] = reg & 0xff;
    memcpy(buf + 2, data, n);
    struct i2c_msg msg = { .addr = s->addr, .flags = 0, .len = n + 2, .buf = buf };
    struct i2c_rdwr_ioctl_data xfer = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(s->i2c_fd, I2C_RDWR, &xfer) < 0 ? -1 : 0;
}

/* Find the i2c bus whose device tree exposes the sensor at 0x1a (dir *-001a). */
static int find_bus(void)
{
    DIR *d = opendir("/sys/bus/i2c/devices");
    if (!d) return -1;
    struct dirent *e;
    int bus = -1;
    while ((e = readdir(d))) {
        const char *dash = strchr(e->d_name, '-');
        if (dash && strcmp(dash + 1, "001a") == 0) { bus = atoi(e->d_name); break; }
    }
    closedir(d);
    return bus;
}

int sensor_open(Sensor *s)
{
    s->addr = 0x1a;
    int bus = find_bus();
    if (bus < 0) { fprintf(stderr, "sensor: no i2c device *-001a found\n"); return -1; }
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    s->i2c_fd = open(path, O_RDWR);
    if (s->i2c_fd < 0) { perror("sensor: open i2c"); return -1; }
    return 0;
}

void sensor_close(Sensor *s)
{
    if (s->i2c_fd > 0) close(s->i2c_fd);
    s->i2c_fd = -1;
}

int sensor_apply(Sensor *s, int exp_lines, int gain)
{
    if (exp_lines < EO_MIN_EXP_LINES) exp_lines = EO_MIN_EXP_LINES;
    if (exp_lines > EO_MAX_EXP_LINES) exp_lines = EO_MAX_EXP_LINES;
    if (gain < EO_GAIN_MIN) gain = EO_GAIN_MIN;
    if (gain > EO_GAIN_MAX) gain = EO_GAIN_MAX;
    int shs1 = EO_VMAX - exp_lines;
    if (shs1 < EO_SHS1_MIN) shs1 = EO_SHS1_MIN;
    if (shs1 > EO_SHS1_MAX) shs1 = EO_SHS1_MAX;

    uint8_t on = 0x01, off = 0x00;
    uint8_t sh[3] = { shs1 & 0xff, (shs1 >> 8) & 0xff, (shs1 >> 16) & 0xff };
    uint8_t gn[2] = { gain & 0xff, (gain >> 8) & 0xff };

    /* REGHOLD on -> write SHS1 + GAIN -> REGHOLD off: all latch together at VSYNC,
     * so a frame never sees a half-written 24-bit SHS1 (the band-shift tear). */
    if (i2c_w(s, 0x3008, &on, 1))            return -1;   /* CTRL08 REGHOLD = 1 */
    if (i2c_w(s, 0x308d, sh, 3))             return -1;   /* SHS1 (24-bit LE)   */
    if (i2c_w(s, 0x3204, gn, 2))             return -1;   /* GAIN (16-bit LE)   */
    if (i2c_w(s, 0x3008, &off, 1))           return -1;   /* CTRL08 REGHOLD = 0 */
    return 0;
}
