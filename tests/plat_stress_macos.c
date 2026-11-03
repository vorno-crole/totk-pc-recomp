// Stress test of runtime/platform/macos.c without game files: locks, condition variables, fibers, thread interruption
// and the write watch with threads writing while another syncs a copy of the memory. It found the two faults in
// docs/macos.md ("Thread interruption" and the fault handler's installation). Build it against the runtime's platform
// objects and SDL3, the clipboard's only user:
//   clang -std=gnu11 -O2 -march=x86-64-v3 -mmacosx-version-min=14.4 -Iruntime -I/usr/local/include tests/plat_stress_macos.c \
//       <macos.c.o> <common.c.o> -L/usr/local/lib -lSDL3 -fuse-ld=lld -o plat_stress
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/platform.h"
#define CK(c)                                                                                                          \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
static int fails;
// locks
static PlatMutex m = PLAT_MUTEX_INIT;
static PlatCond cv = PLAT_COND_INIT;
static volatile uint64_t cnt;
static volatile int rd_in;
static void lock_w(void *a) {
    for (int i = 0; i < 300000; i++) {
        plat_mutex_lock(&m);
        cnt++;
        plat_mutex_unlock(&m);
    }
}
static void lock_r(void *a) {
    for (int i = 0; i < 100000; i++) {
        plat_mutex_lock_shared(&m);
        plat_inc32(&rd_in);
        (void)cnt;
        plat_dec32(&rd_in);
        plat_mutex_unlock_shared(&m);
    }
}
static int ready;
static void cv_t(void *a) {
    plat_mutex_lock(&m);
    ready = 1;
    plat_cond_broadcast(&cv);
    plat_mutex_unlock(&m);
}
// fibers
static PlatFiber *f_main, *f_a;
static int f_n;
static void fib(void *a) {
    for (;;) {
        f_n += *(int *)a;
        plat_fiber_switch(f_main);
    }
}
// interrupt
static PlatIntr *it;
static volatile int it_ready, it_stop;
static volatile uint64_t it_spin;
static volatile int redirected;
void redirect_target(void);
__asm__(".text\n_redirect_target:\n pushq %rax\n movl $1,%eax\n movl %eax,_redirected(%rip)\n popq %rax\n jmp "
        "*_redirect_back(%rip)\n");
uint64_t redirect_back;
static void it_main(void *a) {
    it = plat_intr_self();
    it_ready = 1;
    while (!it_stop)
        it_spin++;
}
// write watch
#define BASE 0x6000000000ull
#define N (1ull << 26)
static volatile int ww_stop;
static void writer(void *a) {
    uint64_t id = (uintptr_t)a;
    volatile uint32_t *p = (volatile uint32_t *)BASE;
    uint64_t x = id * 2654435761u + 1;
    uint64_t seq = 1;
    while (!ww_stop) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t pg = (x >> 33) % (N / 4096);
        p[(pg * 4096) / 4 + id] = (uint32_t)seq++;
    }
}
#define PR_N (4u << 20)
static PlatFile *pr_file;
static volatile uint64_t pr_reads, pr_fail;
static void reader(void *a) {
    while (!ww_stop) {
        if (plat_file_pread(pr_file, (void *)(uintptr_t)BASE, PR_N, 0) != PR_N) pr_fail++;
        pr_reads++;
    }
}
int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    PlatThread *t[8];
    for (int i = 0; i < 4; i++)
        t[i] = plat_thread_new(lock_w, 0, "w");
    for (int i = 4; i < 8; i++)
        t[i] = plat_thread_new(lock_r, 0, "r");
    for (int i = 0; i < 8; i++)
        plat_thread_join(t[i]);
    CK(cnt == 1200000);
    printf("locks ok %llu\n", cnt);
    plat_mutex_lock(&m);
    uint64_t t0 = plat_now_ns();
    int r = plat_cond_wait_ns(&cv, &m, 20000000);
    uint64_t dt = plat_now_ns() - t0;
    CK(r == 0);
    CK(dt >= 19000000 && dt < 60000000);
    printf("cond timeout %d after %.2f ms\n", r, dt / 1e6);
    plat_thread_spawn(cv_t, 0, "cv", 0);
    while (!ready)
        plat_cond_wait(&cv, &m);
    plat_mutex_unlock(&m);
    printf("cond wake ok\n");
    PlatThread *j = plat_thread_new(cv_t, 0, "j");
    CK(plat_thread_join_ns(j, 1000000000) == 1);
    f_main = plat_fiber_self();
    int inc = 3;
    f_a = plat_fiber_new(1 << 16, fib, &inc);
    for (int i = 0; i < 1000; i++)
        plat_fiber_switch(f_a);
    CK(f_n == 3000);
    plat_fiber_free(f_a);
    printf("fibers %d\n", f_n);
    PlatThread *th = plat_thread_new(it_main, 0, "it");
    while (!it_ready)
        plat_pause();
    int stops = 0, red = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t rip, rsp;
        if (plat_intr_stop(it, &rip, &rsp)) {
            stops++;
            if (i % 10 == 0) {
                redirect_back = rip;
                redirected = 0;
                plat_intr_resume(it, (uint64_t)redirect_target);
                while (!redirected)
                    plat_pause();
                red++;
            } else
                plat_intr_resume(it, 0);
        }
    }
    it_stop = 1;
    plat_thread_join(th);
    CK(stops > 1900);
    CK(red > 190);
    printf("interrupt stops %d redirects %d\n", stops, red);
    CK(plat_mem_reserve(BASE, N, 1));
    CK(plat_mem_reserve(BASE, 4096, 1) == 0);
    CK(plat_mem_commit(BASE, N));
    uint64_t *pg = malloc(sizeof(uint64_t) * (N / 4096));
    uint32_t *shadow = calloc(1, N);
    PlatThread *w[3];
    for (int i = 0; i < 3; i++)
        w[i] = plat_thread_new(writer, (void *)(uintptr_t)i, "wr");
    uint64_t rounds = 0, total = 0;
    t0 = plat_now_ns();
    while (plat_now_ns() - t0 < 3000000000ull) {
        uint64_t n = plat_mem_written(BASE, N, pg, N / 4096, 1);
        CK(n != UINT64_MAX);
        for (uint64_t i = 0; i < n; i++)
            memcpy((char *)shadow + (pg[i] - BASE), (void *)pg[i], 4096);
        total += n;
        rounds++;
        plat_sleep_ms(2);
    }
    ww_stop = 1;
    for (int i = 0; i < 3; i++)
        plat_thread_join(w[i]);
    uint64_t n = plat_mem_written(BASE, N, pg, N / 4096, 1);
    for (uint64_t i = 0; i < n; i++)
        memcpy((char *)shadow + (pg[i] - BASE), (void *)pg[i], 4096);
    CK(memcmp(shadow, (void *)BASE, N) == 0);
    size_t bad = 0;
    for (size_t i = 0; i < N / 4; i++)
        if (shadow[i] != ((uint32_t *)BASE)[i])
            bad++;
    printf("write watch: %llu rounds, %llu pages copied, %zu words differ from memory after the last sync\n", rounds,
           total, bad);
    // File reads into watched memory while the main thread resets the watch: a reset between plat_file_pread's touch
    // and the kernel's write fails the read with EFAULT unless the read retries (the RomFS crash in docs/macos.md).
    const char *path = "plat_stress_pread.bin";
    uint8_t *src = malloc(PR_N);
    for (size_t i = 0; i < PR_N; i++)
        src[i] = (uint8_t)(i * 131 + 7);
    CK(plat_file_write(path, src, PR_N));
    pr_file = plat_file_open(path);
    CK(pr_file != NULL);
    ww_stop = 0;
    PlatThread *rd = plat_thread_new(reader, 0, "rd");
    t0 = plat_now_ns();
    while (plat_now_ns() - t0 < 3000000000ull) {
        CK(plat_mem_written(BASE, N, pg, N / 4096, 1) != UINT64_MAX);
        plat_sleep_ms(2);
    }
    ww_stop = 1;
    plat_thread_join(rd);
    CK(pr_fail == 0);
    CK(memcmp((void *)(uintptr_t)BASE, src, PR_N) == 0);
    printf("pread under resets: %llu reads of %u KiB, %llu failed\n", pr_reads, PR_N >> 10, pr_fail);
    plat_file_close(pr_file);
    plat_file_delete(path);
    free(src);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
