/* sse_client.h - generic SSE consumer (two instances: radar + EO tracker).
 * Connects, skips response headers, splits events on \n\n, hands each
 * "data: {...}" payload to the callback. Reconnects with 200 ms -> 2 s
 * exponential backoff; reports connect transitions so the daemon can open
 * the core's re-bind grace on an upstream restart.
 */
#ifndef FUS_SSE_CLIENT_H
#define FUS_SSE_CLIENT_H

#include <stdint.h>

typedef void (*SsePayloadCb)(const char *json, void *user);
typedef void (*SseDropCb)(void *user);          /* connection lost/reset */

typedef struct SseClient SseClient;

SseClient *sse_client_start(const char *host, int port,
                            SsePayloadCb cb, SseDropCb drop_cb, void *user);
int  sse_client_connected(const SseClient *c, uint64_t stale_ns);
void sse_client_stop(SseClient *c);   /* threads are detached; marks stopping */

#endif
