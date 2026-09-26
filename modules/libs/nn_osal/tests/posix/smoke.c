/* SPDX-License-Identifier: Apache-2.0 */
/* POSIX-backend smoke test: exercises every surface a Linux nn app needs.
 * Build/run:  make -C tests/posix   (any Linux host — this IS the port test) */
#include <nn_osal/thread.h>
#include <nn_osal/sync.h>
#include <nn_osal/time.h>
#include <nn_osal/log.h>
#include <nn_osal/storage.h>
#include <nn_osal/socket.h>
#include <nn_osal/gpio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

NN_OSAL_LOG_MODULE(smoke);

static nn_osal_sem_t   s_sem;
static nn_osal_mutex_t s_mtx;
static nn_osal_thread_t s_thr;
static NN_OSAL_THREAD_STACK_DEFINE(s_stack, 4096);
static int s_worker_ran;

static void worker(void *a, void *b, void *c)
{
    (void)b; (void)c;
    nn_osal_mutex_lock(&s_mtx, NN_OSAL_WAIT_FOREVER);
    s_worker_ran = *(int *)a;
    nn_osal_mutex_unlock(&s_mtx);
    nn_osal_sem_give(&s_sem);
}

static int s_loaded;
static int kv_cb(const char *sub, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (strcmp(sub, "k1") == 0 && len == 5 && memcmp(data, "hello", 5) == 0)
        s_loaded = 1;
    return 0;
}

int main(void)
{
    /* time */
    int64_t t0 = nn_osal_uptime_ms();
    nn_osal_sleep_ms(50);
    int64_t dt = nn_osal_uptime_ms() - t0;
    assert(dt >= 45 && dt < 500);
    NN_LOG_INF("time ok (slept %lld ms)", (long long)dt);

    /* thread + sem + mutex */
    assert(nn_osal_sem_init(&s_sem, 0, 1) == 0);
    assert(nn_osal_mutex_init(&s_mtx) == 0);
    assert(nn_osal_sem_take(&s_sem, 100) == -EAGAIN);      /* timeout path */
    int arg = 42;
    assert(nn_osal_thread_create(&s_thr, s_stack, sizeof s_stack,
                                 worker, &arg, NULL, NULL, 5, "smoke-w") == 0);
    assert(nn_osal_sem_take(&s_sem, 2000) == 0);
    assert(s_worker_ran == 42);
    NN_LOG_INF("thread/sem/mutex ok");

    /* storage kv: save → load_all round-trip in an isolated dir */
    char tmpl[] = "/tmp/nn_osal_smoke_XXXXXX";
    setenv("NN_OSAL_KV_DIR", mkdtemp(tmpl), 1);
    assert(nn_osal_kv_init() == 0);
    assert(nn_osal_kv_register("smoke", kv_cb, NULL) == 0);
    assert(nn_osal_kv_save("smoke/k1", "hello", 5) == 0);
    assert(nn_osal_kv_load_all() == 0 && s_loaded == 1);
    assert(nn_osal_kv_delete("smoke/k1") == 0);
    NN_LOG_INF("storage kv ok (%s)", tmpl);

    /* sockets: v6 loopback UDP round-trip */
    nn_osal_socket_t rx = nn_osal_socket(0, NN_OSAL_SOCK_DGRAM, 0);
    nn_osal_socket_t tx = nn_osal_socket(0, NN_OSAL_SOCK_DGRAM, 0);
    assert(rx >= 0 && tx >= 0);
    nn_osal_sockaddr_in6_t a = { .port = 47654 };
    assert(nn_osal_inet_pton6("::1", a.addr) == 0);
    assert(nn_osal_bind(rx, &a) == 0);
    assert(nn_osal_sendto(tx, "ping", 4, 0, &a) == 4);
    nn_osal_pollfd_t pf = { .sock = rx, .events = NN_OSAL_POLLIN };
    assert(nn_osal_poll(&pf, 1, 1000) == 1);
    char buf[8]; nn_osal_sockaddr_in6_t src;
    assert(nn_osal_recvfrom(rx, buf, sizeof buf, 0, &src) == 4);
    assert(memcmp(buf, "ping", 4) == 0);
    char astr[46];
    assert(nn_osal_inet_ntop6(src.addr, astr, sizeof astr) == 0);
    nn_osal_close(rx); nn_osal_close(tx);
    NN_LOG_INF("socket ok (from %s)", astr);

    /* gpio registry */
    nn_osal_gpio_pin_t pin;
    assert(nn_osal_gpio_from_raw(&pin, NULL, 7, 0) == 0);
    assert(nn_osal_gpio_register("smoke-led", &pin) == 0);
    assert(nn_osal_gpio_get("smoke-led") == &pin);
    assert(nn_osal_gpio_set_logical(&pin, 1) == 0);
    assert(nn_osal_gpio_toggle(&pin) == 0);
    assert(nn_osal_gpio_get_logical(&pin) == 0);
    NN_LOG_INF("gpio ok");

    printf("POSIX backend smoke: ALL OK\n");
    return 0;
}
