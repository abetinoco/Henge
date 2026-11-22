#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

static int fail_alloc;
static void *queue_alloc(size_t size) { return fail_alloc ? NULL : malloc(size); }
#define HV_NETQ_ALLOC queue_alloc
#include "net_queue.h"

static HvNetQueue q;

static void drain(unsigned count, uint8_t value) {
    for (unsigned i = 0; i < count; i++) {
        HvNetFrame peek = hv_netq_peek(&q), f = hv_netq_pop(&q);
        assert(peek.data == f.data && f.data && f.data[0] == value);
        free(f.data);
    }
    assert(!hv_netq_pop(&q).data && q.bytes == 0);
}

static void limits(void) {
    uint8_t one = 42;
    assert(hv_netq_init(&q, 0) == 0);
    for (unsigned i = 0; i < 512; i++) assert(hv_netq_push(&q, &one, 1));
    assert(!hv_netq_push(&q, &one, 1) && q.dropped == 1 && q.bytes == 512);
    drain(512, one);
    hv_netq_destroy(&q);

    assert(hv_netq_init(&q, 1) == 0);
    for (unsigned i = 0; i < HV_NETQ_MAX_FRAMES; i++) assert(hv_netq_push(&q, &one, 1));
    assert(!hv_netq_push(&q, &one, 1) && q.dropped == 1);
    drain(HV_NETQ_MAX_FRAMES, one);
    hv_netq_destroy(&q);

    assert(hv_netq_init(&q, 1) == 0);
    uint8_t *large = malloc(65536);
    assert(large);
    memset(large, one, 65536);
    for (unsigned i = 0; i < 256; i++) assert(hv_netq_push(&q, large, 65536));
    assert(q.bytes == HV_NETQ_BYTES && q.peak_bytes == HV_NETQ_BYTES);
    assert(!hv_netq_push(&q, &one, 1) && q.dropped == 1);
    HvNetFrame first = hv_netq_pop(&q);
    free(first.data);
    assert(hv_netq_push(&q, large, 65536)); /* byte budget restored when consumed */
    drain(256, one);
    free(large);
    hv_netq_destroy(&q);
}

static void allocation_and_wrap(void) {
    uint8_t one = 9;
    assert(hv_netq_init(&q, 1) == 0);
    fail_alloc = 1;
    assert(!hv_netq_push(&q, &one, 1));
    assert(q.alloc_failed == 1 && q.dropped == 1 && q.bytes == 0 && q.head == q.tail);
    fail_alloc = 0;
    q.head = q.tail = UINT32_MAX - 7u;
    for (unsigned i = 0; i < 32; i++) assert(hv_netq_push(&q, &one, 1));
    drain(32, one);
    assert(hv_netq_push(&q, &one, 1));
    hv_netq_destroy(&q); /* releases a pending frame */
    assert(q.bytes == 0 && q.head == 0 && q.tail == 0);
    assert(hv_netq_init(&q, 0) == 0 && q.frame_limit == 512);
    hv_netq_destroy(&q);
}

#define MESSAGES 100000u
static void *produce(void *unused) {
    (void)unused;
    for (uint32_t i = 0; i < MESSAGES; i++)
        while (!hv_netq_push(&q, (uint8_t *)&i, sizeof i)) sched_yield();
    return NULL;
}

static void concurrent_fifo(void) {
    assert(hv_netq_init(&q, 1) == 0);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, produce, NULL) == 0);
    for (uint32_t i = 0; i < MESSAGES; i++) {
        HvNetFrame f;
        while (!(f = hv_netq_pop(&q)).data) sched_yield();
        uint32_t got;
        memcpy(&got, f.data, sizeof got);
        assert(f.len == sizeof got && got == i);
        free(f.data);
    }
    pthread_join(producer, NULL);
    assert(q.bytes == 0 && q.head == q.tail);
    hv_netq_destroy(&q);
}

static void peek_backpressure(void) {
    uint8_t first = 11, next = 22, rejected = 33;
    assert(hv_netq_init(&q, 0) == 0);
    assert(hv_netq_push(&q, &first, 1));
    HvNetFrame held = hv_netq_peek(&q);
    for (unsigned i = 1; i < HV_NETQ_LEGACY_FRAMES; i++) assert(hv_netq_push(&q, &next, 1));
    assert(!hv_netq_push(&q, &rejected, 1));
    assert(hv_netq_peek(&q).data == held.data && held.data[0] == first);
    HvNetFrame popped = hv_netq_pop(&q);
    assert(popped.data == held.data && popped.len == held.len);
    free(popped.data);
    assert(hv_netq_peek(&q).data[0] == next && q.bytes == HV_NETQ_LEGACY_FRAMES - 1);
    drain(HV_NETQ_LEGACY_FRAMES - 1, next);
    hv_netq_destroy(&q);
}

typedef struct Message { uint32_t producer, sequence; } Message;
static void *produce_tagged(void *arg) {
    uint32_t producer = *(uint32_t *)arg;
    for (uint32_t i = 0; i < MESSAGES; i++) {
        Message m = {producer, i};
        while (!hv_netq_push(&q, (uint8_t *)&m, sizeof m)) sched_yield();
    }
    return NULL;
}

static void multiple_producers(void) {
    assert(hv_netq_init(&q, 1) == 0);
    uint32_t ids[2] = {0, 1}, expected[2] = {0, 0};
    pthread_t producers[2];
    for (int i = 0; i < 2; i++) assert(pthread_create(&producers[i], NULL, produce_tagged, &ids[i]) == 0);
    for (uint32_t i = 0; i < 2 * MESSAGES; i++) {
        HvNetFrame f;
        while (!(f = hv_netq_pop(&q)).data) sched_yield();
        Message m;
        assert(f.len == sizeof m);
        memcpy(&m, f.data, sizeof m);
        assert(m.producer < 2 && m.sequence == expected[m.producer]++);
        free(f.data);
    }
    for (int i = 0; i < 2; i++) {
        pthread_join(producers[i], NULL);
        assert(expected[i] == MESSAGES);
    }
    assert(q.bytes == 0 && q.head == q.tail && !hv_netq_pop(&q).data);
    hv_netq_destroy(&q);
}

int main(void) {
    limits();
    allocation_and_wrap();
    concurrent_fifo();
    peek_backpressure();
    multiple_producers();
    puts("net queue: limits, allocation failure, wrap, teardown, peek and concurrent producers passed");
    return 0;
}
