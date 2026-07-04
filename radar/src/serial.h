/* POSIX serial open with arbitrary baud (termios2/BOTHER) for the
 * AWR2944PEVM. The data UART runs at a non-standard 3,125,000 baud, which
 * cfsetspeed() can't express — hence termios2. CLI UART is 115200. */
#ifndef AIRPOC_SERIAL_H
#define AIRPOC_SERIAL_H

/* Open `path` raw, 8N1, no flow control, at `baud` (any value). Returns a
 * blocking-with-short-timeout fd (VMIN=0, VTIME=1) or -1 on error. */
int serial_open(const char *path, int baud);

#endif /* AIRPOC_SERIAL_H */
