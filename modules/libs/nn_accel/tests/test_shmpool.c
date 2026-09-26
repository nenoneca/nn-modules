/* Two PROCESSES share a pool: the child maps a pool it never allocated,
 * reads a frame the parent wrote (no copy through the socket), and writes
 * a marker back into the same pages.  Also checks backpressure. */
#include <nn_accel/shmpool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOTS 4
#define SLOT_BYTES (64 * 1024)

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) { perror("socketpair"); return 1; }

    nn_pool_t pool;
    if (nn_pool_create(&pool, SLOTS, SLOT_BYTES, 640, 640, 640 * 3, 1)) {
        fprintf(stderr, "create failed\n"); return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {                       /* ── consumer ── */
        close(sv[0]);
        char msg[64]; int fd = -1;
        if (nn_recv_fd(sv[1], msg, sizeof msg, &fd) < 0 || fd < 0) _exit(2);
        nn_pool_t cp;
        if (nn_pool_map(&cp, fd)) _exit(3);
        /* geometry travelled inside the mapping, not the message */
        if (cp.hdr->width != 640 || cp.hdr->slots != SLOTS) _exit(4);
        char req[64];
        for (int i = 0; i < 3; i++) {
            if (nn_recv_fd(sv[1], req, sizeof req, NULL) < 0) _exit(5);
            unsigned slot = (unsigned)atoi(req);
            unsigned char *px = nn_pool_slot_ptr(&cp, slot);
            if (!px) _exit(6);
            if (px[0] != (unsigned char)(0xA0 + i) || px[SLOT_BYTES - 1] != 0x5A)
                _exit(7);                 /* wrong pixels => not shared */
            px[1] = 0xEE;                 /* write back through the mapping */
            nn_pool_release(&cp, slot);
            if (nn_send_fd(sv[1], "ok", 3, -1) < 0) _exit(8);
        }
        _exit(0);
    }

    close(sv[1]);
    char hello[32]; snprintf(hello, sizeof hello, "pool");
    if (nn_send_fd(sv[0], hello, strlen(hello) + 1, pool.fd) < 0) return 1;

    int fails = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t slot;
        if (nn_pool_acquire(&pool, &slot)) { printf("FAIL acquire\n"); fails++; break; }
        unsigned char *px = nn_pool_slot_ptr(&pool, slot);
        memset(px, 0, SLOT_BYTES);
        px[0] = (unsigned char)(0xA0 + i);
        px[SLOT_BYTES - 1] = 0x5A;
        nn_pool_publish(&pool, slot, (uint64_t)i);
        char m[16]; snprintf(m, sizeof m, "%u", slot);
        nn_send_fd(sv[0], m, strlen(m) + 1, -1);
        char ack[8];
        if (nn_recv_fd(sv[0], ack, sizeof ack, NULL) < 0) { fails++; break; }
        if (px[1] != 0xEE) { printf("FAIL consumer write not visible\n"); fails++; }
        if (pool.hdr->state[slot] != NN_SLOT_FREE) {
            printf("FAIL slot not released\n"); fails++;
        }
    }

    /* backpressure: fill every slot, the next acquire must refuse */
    uint32_t s;
    int got = 0;
    while (nn_pool_acquire(&pool, &s) == 0) { got++; if (got > SLOTS) break; }
    if (got != SLOTS) { printf("FAIL acquired %d of %d\n", got, SLOTS); fails++; }
    if (nn_pool_acquire(&pool, &s) != -EAGAIN) {
        printf("FAIL no backpressure when full\n"); fails++;
    }

    int st = 0; waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAIL consumer exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        fails++;
    }
    nn_pool_close(&pool);
    printf("shmpool: %s\n", fails ? "FAIL" : "PASS (zero-copy across processes)");
    return fails ? 1 : 0;
}
