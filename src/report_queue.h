/* GPL-2.0-only. Caller serializes producer and consumer with usb_mutex.
 * Preserve short PS presses: keeping only the latest report loses down/up
 * transitions that both arrive between two polling ticks. */
#ifndef NPP_REPORT_QUEUE_H
#define NPP_REPORT_QUEUE_H
#include <stdint.h>
#include <string.h>
#define NPP_REPORT_QUEUE_SIZE 16
typedef struct {
    uint8_t data[NPP_REPORT_QUEUE_SIZE][80];
    uint8_t len[NPP_REPORT_QUEUE_SIZE];
    unsigned head, count;
    int overflow;
} npp_report_queue_t;

static inline int npp_report_push(npp_report_queue_t *q, const uint8_t *p, int n)
{
    unsigned tail;
    if (!p || n <= 0 || n > 80) return -1;
    if (q->count == NPP_REPORT_QUEUE_SIZE) {
        q->overflow = 1;
        return -1;
    }
    tail = (q->head + q->count) % NPP_REPORT_QUEUE_SIZE;
    memcpy(q->data[tail], p, (unsigned)n);
    q->len[tail] = (uint8_t)n;
    q->count++;
    return 0;
}

static inline int npp_report_pop(npp_report_queue_t *q, uint8_t *out)
{
    int n;
    if (!q->count) return 0;
    n = q->len[q->head];
    memcpy(out, q->data[q->head], (unsigned)n);
    q->head = (q->head + 1) % NPP_REPORT_QUEUE_SIZE;
    q->count--;
    return n;
}
#endif
