#ifndef HV_NET_QUEUE_H
#define HV_NET_QUEUE_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HV_NETQ_LEGACY_FRAMES 512u
#define HV_NETQ_MAX_FRAMES 16384u
#define HV_NETQ_BYTES (16u << 20)
#ifndef HV_NETQ_ALLOC
#define HV_NETQ_ALLOC malloc
#endif

typedef struct HvNetFrame {
    uint32_t len;
    uint8_t *data;
} HvNetFrame;

typedef struct HvNetQueue {
    HvNetFrame f[HV_NETQ_MAX_FRAMES];
    uint32_t head, tail, frame_limit;
    uint64_t bytes, byte_limit, peak_bytes, dropped, alloc_failed;
    pthread_mutex_t mu;
} HvNetQueue;

/* Setup/teardown run with all producers and the sole consumer stopped. Default capacity is upstream's. */
static inline int hv_netq_init(HvNetQueue *q, int extended) {
    memset(q, 0, sizeof *q);
    q->frame_limit = extended ? HV_NETQ_MAX_FRAMES : HV_NETQ_LEGACY_FRAMES;
    q->byte_limit = extended ? HV_NETQ_BYTES : UINT64_MAX;
    return pthread_mutex_init(&q->mu, NULL);
}

static inline int hv_netq_push(HvNetQueue *q, const uint8_t *data, uint32_t len) {
    pthread_mutex_lock(&q->mu);
    int ok = q->tail - q->head < q->frame_limit && len <= q->byte_limit - q->bytes;
    if (ok) {
        uint8_t *copy = HV_NETQ_ALLOC(len);
        if (!copy) {
            q->alloc_failed++;
            ok = 0;
        } else {
            memcpy(copy, data, len);
            q->f[q->tail++ % HV_NETQ_MAX_FRAMES] = (HvNetFrame){len, copy};
            q->bytes += len;
            if (q->bytes > q->peak_bytes) q->peak_bytes = q->bytes;
        }
    }
    if (!ok) q->dropped++;
    pthread_mutex_unlock(&q->mu);
    return ok;
}

/* Only one consumer: a peek stays valid until that consumer removes it. Producers only append. */
static inline HvNetFrame hv_netq_peek(HvNetQueue *q) {
    pthread_mutex_lock(&q->mu);
    HvNetFrame f = {0, NULL};
    if (q->head != q->tail) f = q->f[q->head % HV_NETQ_MAX_FRAMES];
    pthread_mutex_unlock(&q->mu);
    return f;
}

static inline HvNetFrame hv_netq_pop(HvNetQueue *q) {
    pthread_mutex_lock(&q->mu);
    HvNetFrame f = {0, NULL};
    if (q->head != q->tail) {
        uint32_t slot = q->head++ % HV_NETQ_MAX_FRAMES;
        f = q->f[slot];
        q->f[slot] = (HvNetFrame){0, NULL};
        q->bytes -= f.len;
    }
    pthread_mutex_unlock(&q->mu);
    return f;
}

static inline void hv_netq_destroy(HvNetQueue *q) {
    HvNetFrame f;
    while ((f = hv_netq_pop(q)).data) free(f.data);
    pthread_mutex_destroy(&q->mu);
    memset(q, 0, sizeof *q);
}

#endif
