/* Push a mmw_demo .cfg to the radar CLI UART. Port of the ground bench's
 * radar/cfg_sender.py: one command per line, drain stale bytes, write,
 * read until "Done"/"Error"/prompt, pause, repeat. The stock firmware
 * streams nothing until it sees a full profile ending in sensorStart. */
#ifndef AIRPOC_CFG_PUSH_H
#define AIRPOC_CFG_PUSH_H

/* Send every non-comment line of `cfg_path` over the open CLI fd. Returns
 * 0 if all lines were acked without "Error", -1 otherwise. Logs each
 * line's response to stderr. Does not close the fd. */
int cfg_push(int cli_fd, const char *cfg_path);

#endif /* AIRPOC_CFG_PUSH_H */
