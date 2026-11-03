// Stress test of runtime/platform/macos.c: thread interruption (plat_intr_stop, plat_intr_resume) of a thread whose
// writes keep faulting on the write watch, as kernel.c's asynchronous preemption does to game code. A writer runs an
// assembly loop that stores a sequence number to random watched pages; the main thread resets the watch and, between
// resets, stops the writer and redirects it to a stub that saves every register, scrambles them, yields the CPU,
// restores them and jumps back, as kernel.c's kern_ap_stub does. At the end the memory must hold what a replay of the
// loop's random sequence writes. Build it like tests/plat_stress_macos.c:
// Arguments: seconds (10), stops per watch reset (20) and microseconds between stops (20).
//   clang -std=gnu11 -O2 -march=x86-64-v3 -mmacosx-version-min=14.4 -Iruntime -I/usr/local/include \
//       tests/intr_fault_macos.c runtime/platform/macos.c runtime/platform/common.c -L/usr/local/lib -lSDL3 \
//       -framework IOKit -framework CoreFoundation -fuse-ld=lld -o intr_fault
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/platform.h"

#define BASE 0x6000000000ull
#define PAGES 4096ull   // 16 MiB, a power of two
#define A_MUL 6364136223846793005ull
#define A_ADD 1442695040888963407ull

static volatile int stop;
static volatile uint64_t seq_end;
volatile uint64_t stub_runs;   // the stub counts its runs (assembly names it)
uint64_t back;                 // where the stub jumps back to (one writer)

// rdi: base, rsi: &stop. Returns the last sequence number stored. Lives between ww_loop and ww_loop_end.
uint64_t ww_loop(uint64_t base, volatile int *stopp);
void ww_loop_end(void);
__asm__(".text\n.p2align 4\n.globl _ww_loop\n_ww_loop:\n"
        "    movq $1, %r8\n"                       // rng state
        "    xorq %r9, %r9\n"                      // sequence
        "    movabsq $6364136223846793005, %r10\n"
        "    movabsq $1442695040888963407, %r11\n"
        "1:  imulq %r10, %r8\n"
        "    addq %r11, %r8\n"
        "    incq %r9\n"
        "    movq %r8, %rax\n"
        "    shrq $33, %rax\n"
        "    andq $4095, %rax\n"
        "    shlq $12, %rax\n"
        "    movq %r9, %rcx\n"
        "    andq $511, %rcx\n"                    // a word of the page, by sequence
        "    leaq (%rdi,%rax), %rdx\n"
        "    movq %r9, (%rdx,%rcx,8)\n"
        "    cmpl $0, (%rsi)\n"
        "    je 1b\n"
        "    movq %r9, %rax\n"
        "    ret\n"
        ".globl _ww_loop_end\n_ww_loop_end:\n    ret\n");

// The redirect target: as kern_ap_stub, below the red zone, flags first, every integer register, scrambled while away.
void ww_stub(void);
__asm__(".text\n.p2align 4\n.globl _ww_stub\n_ww_stub:\n"
        "    leaq -136(%rsp), %rsp\n"
        "    pushfq\n"
        "    pushq %rax\n pushq %rcx\n pushq %rdx\n pushq %rsi\n pushq %rdi\n pushq %r8\n pushq %r9\n pushq %r10\n"
        "    pushq %r11\n pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-16, %rsp\n"
        "    movq _back(%rip), %rax\n"
        "    movq %rax, 88(%rbp)\n"
        "    movq $-1, %rcx\n movq $-1, %rdx\n movq $-1, %rsi\n movq $-1, %r8\n movq $-1, %r9\n movq $-1, %r10\n"
        "    movq $-1, %r11\n"
        "    lock incq _stub_runs(%rip)\n"
        "    call _sched_yield\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n popq %r11\n popq %r10\n popq %r9\n popq %r8\n popq %rdi\n popq %rsi\n popq %rdx\n popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 136(%rsp), %rsp\n"
        "    jmpq *-136(%rsp)\n");

static PlatIntr *wr;
static volatile int wr_ready;
static void writer(void *a) {
    (void)a;
    wr = plat_intr_self();
    wr_ready = 1;
    seq_end = ww_loop(BASE, &stop);
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 10;
    int every = argc > 2 ? atoi(argv[2]) : 20;   // stops per watch reset: the writer faults now and then, not always
    int gap_us = argc > 3 ? atoi(argv[3]) : 20;  // between stops, so the writer runs
    setvbuf(stdout, 0, _IONBF, 0);
    if (!plat_mem_reserve(BASE, PAGES * 4096, 1) || !plat_mem_commit(BASE, PAGES * 4096)) {
        printf("FAIL: reserve\n");
        return 1;
    }
    uint64_t *pg = malloc(sizeof(uint64_t) * PAGES);
    PlatThread *t = plat_thread_new(writer, NULL, "writer");
    while (!wr_ready) plat_pause();
    uint64_t t0 = plat_now_ns(), stops = 0, sent = 0, outside = 0, resets = 0;
    uintptr_t lo = (uintptr_t)ww_loop, hi = (uintptr_t)ww_loop_end;
    while (plat_now_ns() - t0 < (uint64_t)(secs * 1e9)) {
        if (stops % (uint64_t)every == 0) {
            plat_mem_written(BASE, PAGES * 4096, pg, PAGES, 1);
            resets++;
        }
        uint64_t rip, rsp;
        if (plat_intr_stop(wr, &rip, &rsp)) {
            stops++;
            uint64_t to = 0;
            if (rip >= lo && rip < hi && stub_runs == sent) {   // the stub of the last redirect has run
                back = rip;
                to = (uint64_t)(uintptr_t)ww_stub;
                sent++;
            } else outside++;
            plat_intr_resume(wr, to);
        }
        for (uint64_t g = plat_now_ns(); plat_now_ns() - g < (uint64_t)gap_us * 1000;) plat_pause();
    }
    stop = 1;
    plat_thread_join(t);
    // replay
    uint64_t *want = calloc(PAGES * 512, 8), x = 1;
    for (uint64_t s = 1; s <= seq_end; s++) {
        x = x * A_MUL + A_ADD;
        want[((x >> 33) & 4095) * 512 + (s & 511)] = s;
    }
    uint64_t bad = 0, first = 0;
    const uint64_t *got = (const uint64_t *)(uintptr_t)BASE;
    for (uint64_t i = 0; i < PAGES * 512; i++)
        if (got[i] != want[i] && !bad++) first = i;
    printf("%llu stores, %llu resets, %llu stops (%llu outside the loop), %llu redirects, %llu stub runs; %llu words "
           "wrong%s\n", (unsigned long long)seq_end, (unsigned long long)resets, (unsigned long long)stops,
           (unsigned long long)outside, (unsigned long long)sent, (unsigned long long)stub_runs,
           (unsigned long long)bad, bad ? "" : "");
    if (bad)
        printf("first wrong word %llu: %#llx, want %#llx\n", (unsigned long long)first,
               (unsigned long long)got[first], (unsigned long long)want[first]);
    printf("%s\n", bad || stub_runs != sent ? "FAILED" : "passed");
    return bad != 0;
}
