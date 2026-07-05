/* disk.c — data-volume presence and free-space guard */
#include "recorder.h"
#include <sys/statvfs.h>
#include <sys/stat.h>

double disk_free_gb(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) return 0;
    return (double)v.f_bavail * v.f_frsize / 1e9;
}

double disk_total_gb(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) return 0;
    return (double)v.f_blocks * v.f_frsize / 1e9;
}

/* The recordings root must be a real mounted data volume, not a directory on
 * the rootfs: require the root's parent mount (/data) to differ from "/". */
int disk_present(const char *path)
{
    struct stat a, b;
    if (stat(path, &a) != 0) return 0;
    if (stat("/", &b) != 0) return 0;
    return a.st_dev != b.st_dev;
}
