/* V4L2 mmap capture. Each DQBUF hands over one complete frame buffer (no byte
 * stream to desync). Geometry is probed from the driver so it tracks the sensor
 * ROI (1440x1088, bytesperline 2880). */
#include "pipeline.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

int cap_open(Capture *c, const char *dev, int nbufs)
{
    memset(c, 0, sizeof(*c));
    c->fd = open(dev, O_RDWR);
    if (c->fd < 0) { perror("cap: open"); return -1; }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (xioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) { perror("cap: G_FMT"); return -1; }
    c->width        = fmt.fmt.pix.width;
    c->height       = fmt.fmt.pix.height;
    c->bytesperline = fmt.fmt.pix.bytesperline;
    c->sizeimage    = fmt.fmt.pix.sizeimage;

    struct v4l2_requestbuffers req = {
        .count = nbufs, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
    };
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) { perror("cap: REQBUFS"); return -1; }
    c->nbufs  = req.count;
    c->bufs   = calloc(c->nbufs, sizeof(void *));
    c->buflen = calloc(c->nbufs, sizeof(size_t));

    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i
        };
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) { perror("cap: QUERYBUF"); return -1; }
        c->buflen[i] = b.length;
        c->bufs[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, c->fd, b.m.offset);
        if (c->bufs[i] == MAP_FAILED) { perror("cap: mmap"); return -1; }
        if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) { perror("cap: QBUF"); return -1; }
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0) { perror("cap: STREAMON"); return -1; }
    return 0;
}

const uint8_t *cap_dqbuf(Capture *c, int *index)
{
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0) { perror("cap: DQBUF"); return NULL; }
    *index = b.index;
    return (const uint8_t *)c->bufs[b.index];
}

void cap_requeue(Capture *c, int index)
{
    struct v4l2_buffer b = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = index
    };
    xioctl(c->fd, VIDIOC_QBUF, &b);
}

void cap_close(Capture *c)
{
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(c->fd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < c->nbufs; i++)
        if (c->bufs[i] && c->bufs[i] != MAP_FAILED) munmap(c->bufs[i], c->buflen[i]);
    free(c->bufs); free(c->buflen);
    if (c->fd > 0) close(c->fd);
}
