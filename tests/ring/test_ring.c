#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "epoch/ring.h"
#include "test.h"

#define STRESS_BYTES 2000000u

static void test_init(void)
{
    struct epoch_ring r;
    uint8_t buf[8];
    CHECK(!epoch_ring_init(&r, buf, 0));
    CHECK(!epoch_ring_init(&r, buf, 1));
    CHECK(!epoch_ring_init(&r, buf, 6));
    CHECK(!epoch_ring_init(&r, NULL, 8));
    CHECK(!epoch_ring_init(&r, buf, 0xC0000000u));
    CHECK(epoch_ring_init(&r, buf, 2));
    CHECK(epoch_ring_init(&r, buf, 8));
    CHECK(atomic_is_lock_free(&r.head) && atomic_is_lock_free(&r.tail) && atomic_is_lock_free(&r.dropped));
}

static void test_full_and_order(void)
{
    struct epoch_ring r;
    uint8_t buf[4], b = 0;
    CHECK(epoch_ring_init(&r, buf, 4));
    CHECK(!epoch_ring_get(&r, &b));
    for (uint8_t i = 0; i < 4u; i++)
        CHECK(epoch_ring_put(&r, i));
    CHECK(!epoch_ring_put(&r, 9)); /* the newest byte is dropped */
    CHECK(epoch_ring_dropped(&r) == 1u);
    for (uint8_t i = 0; i < 4u; i++)
        CHECK(epoch_ring_get(&r, &b) && b == i);
    CHECK(!epoch_ring_get(&r, &b));
}

static void test_index_wrap(void)
{
    struct epoch_ring r;
    uint8_t buf[4], b = 0;
    CHECK(epoch_ring_init(&r, buf, 4));
    atomic_store(&r.head, UINT32_MAX - 1u);
    atomic_store(&r.tail, UINT32_MAX - 1u);
    for (uint8_t i = 0; i < 4u; i++)
        CHECK(epoch_ring_put(&r, (uint8_t)(10u + i)));
    CHECK(!epoch_ring_put(&r, 0));
    CHECK(atomic_load(&r.head) == 2u);
    for (uint8_t i = 0; i < 4u; i++)
        CHECK(epoch_ring_get(&r, &b) && b == 10u + i);
    CHECK(!epoch_ring_get(&r, &b));
}

struct stress {
    struct epoch_ring r;
    uint8_t buf[64];
    uint32_t accepted;
    uint32_t attempts;
    uint32_t received;
    uint32_t errors;
};

static uint32_t xorshift(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* A put that fails is retried with the same value, so the consumer must see 0, 1, 2, ... modulo 256 with no gap, whatever the drops. */
static void *producer(void *arg)
{
    struct stress *s = arg;
    uint32_t rng = 0x12345678u;
    while (s->accepted < STRESS_BYTES) {
        s->attempts++;
        if (epoch_ring_put(&s->r, (uint8_t)s->accepted))
            s->accepted++;
        if ((xorshift(&rng) & 7u) == 0u)
            sched_yield(); /* randomized preemption point */
    }
    return NULL;
}

static void *consumer(void *arg)
{
    struct stress *s = arg;
    uint32_t rng = 0x9ABCDEF1u;
    uint8_t b = 0;
    while (s->received < STRESS_BYTES) {
        if (epoch_ring_get(&s->r, &b)) {
            if (b != (uint8_t)s->received)
                s->errors++;
            s->received++;
        }
        if ((xorshift(&rng) & 7u) == 0u)
            sched_yield();
    }
    return NULL;
}

static void test_stress(void)
{
    static struct stress s;
    memset(&s, 0, sizeof s);
    CHECK(epoch_ring_init(&s.r, s.buf, sizeof s.buf));
    pthread_t p, c;
    CHECK(pthread_create(&c, NULL, consumer, &s) == 0);
    CHECK(pthread_create(&p, NULL, producer, &s) == 0);
    CHECK(pthread_join(p, NULL) == 0);
    CHECK(pthread_join(c, NULL) == 0);
    CHECK(s.errors == 0u);
    CHECK(s.received == STRESS_BYTES);
    CHECK(epoch_ring_dropped(&s.r) == s.attempts - s.accepted);
    printf("ring stress: %u bytes, %u dropped puts\n", (unsigned)s.received, (unsigned)epoch_ring_dropped(&s.r));
}

int main(void)
{
    test_init();
    test_full_and_order();
    test_index_wrap();
    test_stress();
    return TEST_DONE("ring");
}
