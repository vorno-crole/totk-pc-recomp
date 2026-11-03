// macOS (Intel) implementation of platform.h: pthreads, os_sync_wait_on_address locks and condition variables, POSIX
// files, Mach calls for memory and the load counters, posix_spawn for child processes. It follows linux.c section by
// section; docs/macos.md says what differs and why. Needs macOS 14.4 for os_sync_wait_on_address.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <crt_externs.h>
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <SDL3/SDL_clipboard.h>

#include "platform.h"

extern char **environ;

// ---- threads

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    volatile int32_t done;  // fn returned, for plat_thread_join_ns
    char name[64];          // the kernel keeps 63 characters
};

// A signal stack for the calling thread, so the fault and crash handlers still run when its own stack is exhausted.
// Never freed: threads that end are few, and a stack freed while a handler might run on it would be worse.
static void alt_stack(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    size_t size = 64 << 10;   // a signal frame with the AVX-512 state takes several KiB
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {.ss_sp = p, .ss_size = size};
    sigaltstack(&ss, NULL);
}

static void *thread_main(void *p) {
    PlatThread *t = p;
    alt_stack();
    if (t->name[0]) pthread_setname_np(t->name);
    t->fn(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    // The stack is mapped lazily, as Windows reserves it: only the pages a thread touches cost memory.
    pthread_attr_setstacksize(&a, stack ? stack : PLAT_STACK_DEFAULT);
    int e = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}

// macOS has no affinity to query or set (only hints between threads of one task), so the callers' "unknown" and
// "failed" paths apply.
int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    *all = *first = 0;
    return 0;
}

int plat_process_affinity(uint64_t mask) {
    (void)mask;
    return 0;
}

// Only the calling thread: plat_thread_id is not a Mach port, and no caller needs another thread's time on this OS.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    if (tid != plat_thread_id()) return 0;
    thread_basic_info_data_t b;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    mach_port_t me = mach_thread_self();
    kern_return_t kr = thread_info(me, THREAD_BASIC_INFO, (thread_info_t)&b, &n);
    mach_port_deallocate(mach_task_self(), me);
    if (kr != KERN_SUCCESS) return 0;
    return ((uint64_t)b.user_time.seconds + b.system_time.seconds) * 1000000000u +
           ((uint64_t)b.user_time.microseconds + b.system_time.microseconds) * 1000u;
}

// ---- thread interruption: Mach's thread_suspend and thread_get_state / thread_set_state, the same model as Windows'
// SuspendThread and Get/SetThreadContext. A signal handshake as on Linux does not work here: delivery to a running
// thread often took longer than the handshake's 200 us, so most stops failed (docs/macos.md). A stop always succeeds; a
// thread blocked in the kernel reports the user address it will return to, which kernel.c's checks then turn down.

struct PlatIntr {
    thread_act_t port;
    x86_thread_state64_t st;   // the state plat_intr_stop read, which plat_intr_resume writes back redirected
};

PlatIntr *plat_intr_self(void) {
    PlatIntr *t = calloc(1, sizeof *t);
    if (t) t->port = mach_thread_self();
    return t;
}

// Called by the thread itself as it ends, or by another once the thread no longer runs.
void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    mach_port_deallocate(mach_task_self(), t->port);
    free(t);
}

// A suspended thread still finishes what it was doing in the kernel. One stopped on a write fault in game code may set
// up the call of the fault's signal handler after plat_intr_stop read its state: rip in _sigtramp, the handler's
// arguments in the registers, rsp on the signal stack. Reading the state again for the redirect took that state, and
// kern_ap_stub then went back into game code with it and without the handler: game data was corrupted, and a jump
// landed on a stack address (tests/intr_fault_macos.c failed within seconds). So the redirect writes back the state
// the stop read, with rip changed. If the kernel set the handler up in between, that call is dropped: the stub runs
// with the registers of the fault and returns to the faulting instruction, which faults again and is handled then.
// The dropped call leaves the kernel's "on the signal stack" mark set; fault_handler clears it (uc_onstack).
int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    if (thread_suspend(t->port) != KERN_SUCCESS) return 0;
    mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(t->port, x86_THREAD_STATE64, (thread_state_t)&t->st, &n) != KERN_SUCCESS) {
        thread_resume(t->port);
        return 0;
    }
    *rip = t->st.__rip;
    *rsp = t->st.__rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    int ok = 1;
    if (to) {
        x86_thread_state64_t st = t->st;
        st.__rip = to;
        ok = thread_set_state(t->port, x86_THREAD_STATE64, (thread_state_t)&st, x86_THREAD_STATE64_COUNT) == KERN_SUCCESS;
    }
    thread_resume(t->port);
    return ok;
}

// ---- fibers: a stack switch of our own. swapcontext saves and restores the signal mask with a system call on
// every switch, and the game switches fibers often. fiber_swap saves the callee-saved registers, the MXCSR and the
// x87 control word on the current stack, stores the stack pointer in *save and loads the other one. A new fiber's
// stack is laid out as fiber_swap leaves one, returning into fiber_entry, which calls fn(arg) (r12 and r13).
struct PlatFiber {
    void *sp;
    void *stack;   // mapped stack (NULL for a thread's own context)
    size_t size;
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
__asm__(".text\n"
        ".globl _plat_fiber_swap\n"
        ".p2align 4\n"
        "_plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".globl _plat_fiber_entry\n"
        ".p2align 4\n"
        "_plat_fiber_entry:\n"
        "    movq %r13, %rdi\n"
        "    callq *%r12\n"
        "    ud2\n");

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    // Reserved, not committed; the lowest page stays inaccessible, so an overflow faults instead of corrupting.
    void *m = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        free(f);
        return NULL;
    }
    mprotect(m, 4096, PROT_NONE);
    f->stack = m, f->size = stack;
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
    uint64_t *sp = top - 10;   // fpu state (2 words), r15, r14, r13, r12, rbx, rbp, return address, padding
    sp[0] = 0x037f;            // x87 control word: the default
    sp[1] = 0x1f80;            // MXCSR: the default
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = 0;
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;   // on this thread; the switch back into from sets it on whichever thread that is
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

// No timed join on this OS: the thread says when fn returned, and only then is the join sure not to block for long.
int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
        if (plat_now_ns() >= end) return 0;
        plat_sleep_ms(1);
    }
    plat_thread_join(t);
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) { pthread_detach(t->t); }

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    (void)t, (void)mask;
    return 0;
}

uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

uint32_t plat_cpu_usable(void) { return plat_cpu_count(); }

// Windows' levels map to Mach priorities around the default of 31. Unlike Linux's nice values, a thread can always
// return to normal, so every change applies.
static int mach_prio(int prio) {
    return prio <= PLAT_PRIO_IDLE ? 15 : prio == PLAT_PRIO_LOWEST ? 25 : prio == PLAT_PRIO_BELOW_NORMAL ? 28
         : prio == PLAT_PRIO_NORMAL ? 31 : prio == PLAT_PRIO_ABOVE_NORMAL ? 33 : prio == PLAT_PRIO_HIGHEST ? 37 : 47;
}

static void set_prio(pthread_t t, int prio) {
    struct sched_param p = {.sched_priority = mach_prio(prio)};
    pthread_setschedparam(t, SCHED_OTHER, &p);
}

void plat_thread_priority(int prio) { set_prio(pthread_self(), prio); }

void plat_thread_set_priority(PlatThread *t, int prio) { set_prio(t->t, prio); }

int plat_thread_priority_get(void) {
    struct sched_param p;
    int policy;
    if (pthread_getschedparam(pthread_self(), &policy, &p)) return PLAT_PRIO_NORMAL;
    int v = p.sched_priority;
    return v <= 15 ? PLAT_PRIO_IDLE : v <= 25 ? PLAT_PRIO_LOWEST : v <= 28 ? PLAT_PRIO_BELOW_NORMAL
         : v <= 31 ? PLAT_PRIO_NORMAL : v <= 33 ? PLAT_PRIO_ABOVE_NORMAL : v <= 37 ? PLAT_PRIO_HIGHEST
         : PLAT_PRIO_TIME_CRITICAL;
}

void plat_yield(void) { sched_yield(); }

// ---- time

// CLOCK_UPTIME_RAW: read from the commpage without a system call, and it does not jump when the clock is corrected.
uint64_t plat_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000u; }

void plat_timer_1ms(void) {}   // timed waits are already that fine here

// The sleep ends PLAT_SPIN_NS early and the rest is spent yielding, so the deadline is met closely, as on Windows.
#define PLAT_SPIN_NS 100000u

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) {
        uint64_t left = ns - PLAT_SPIN_NS;
        struct timespec ts = {(time_t)(left / 1000000000u), (long)(left % 1000000000u)};
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
    }
    while (plat_now_ns() < end) sched_yield();
}

void plat_wait_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void plat_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

// ---- locks: a reader-writer lock in one 32-bit word, zero when free.
// bit 31: a writer holds it; bit 30: some thread sleeps on the word; bits 0-29: readers holding it.
// A thread that cannot take the lock sets the sleeper bit and waits on the futex; every release that finds the bit
// clears it and wakes all sleepers, which retry (and set it again if they still cannot get in). Readers do not enter
// while the sleeper bit is set, so a waiting writer is not starved by a stream of readers.

#define LK_W 0x80000000u
#define LK_S 0x40000000u
#define LK_R 0x3fffffffu
#define LK_SPINS 64

// The same words as linux.c's futexes, through the public wait-on-address call. The wait returns at once when *w is no
// longer val; ns 0 waits without a limit. 0 on a timeout.
static int addr_wait(volatile uint32_t *w, uint32_t val, uint64_t ns) {
    int r;
    if (ns) r = os_sync_wait_on_address_with_timeout((void *)w, val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                     OS_CLOCK_MACH_ABSOLUTE_TIME, ns);
    else r = os_sync_wait_on_address((void *)w, val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    return !(r < 0 && errno == ETIMEDOUT);
}

static void addr_wake_one(volatile uint32_t *w) { os_sync_wake_by_address_any((void *)w, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE); }
static void addr_wake_all(volatile uint32_t *w) { os_sync_wake_by_address_all((void *)w, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE); }

static inline volatile uint32_t *lk(PlatMutex *m) { return (volatile uint32_t *)&m->p; }

// Sets the sleeper bit on s and sleeps while the word still equals the result.
static void lk_sleep(volatile uint32_t *w, uint32_t s) {
    if (!(s & LK_S) && !__atomic_compare_exchange_n(w, &s, s | LK_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    addr_wait(w, s | LK_S, 0);
}

static void lk_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~LK_S, __ATOMIC_RELAXED);
    addr_wake_all(w);
}

void plat_mutex_init(PlatMutex *m) { m->p = 0; }

int plat_mutex_trylock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_R)))
        if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_R))) {
            if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_fetch_and(w, ~LK_W, __ATOMIC_RELEASE);
    if (s & LK_S) lk_wake(w);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_S)))
        if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_S))) {
            if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_sub_fetch(w, 1, __ATOMIC_RELEASE);
    if (!(s & LK_R) && (s & LK_S)) lk_wake(w);
}

// ---- condition variables: a sequence number in the first 32 bits. A waiter reads it before releasing the lock and
// sleeps only while it is unchanged, so a signal between the release and the sleep is not lost.

static inline volatile uint32_t *cv(PlatCond *c) { return (volatile uint32_t *)&c->p; }

void plat_cond_init(PlatCond *c) { c->p = 0; }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    addr_wait(cv(c), seq, 0);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    int woken = ns ? addr_wait(cv(c), seq, ns) : 0;   // ns 0 is a poll here, as on the other hosts
    plat_mutex_lock(m);
    return woken;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    addr_wake_one(cv(c));
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    addr_wake_all(cv(c));
}

// ---- files

int plat_file_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    if (path[0] == '/') return snprintf(out, cap, "%s", path) < (int)cap;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    return snprintf(out, cap, "%s/%s", cwd, path) < (int)cap;
}

void plat_file_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir(tmp, 0755);
            *p = c;
        }
    }
    mkdir(tmp, 0755);
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        if (buf && got != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

// O_APPEND makes each write land whole at the end of the file, from any thread.
PlatAppend *plat_append_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    ssize_t k;
    do k = write(fd, data, n);
    while (k < 0 && errno == EINTR);
    return k == (ssize_t)n;
}

// Flushes the directory holding path, so a rename into it survives a crash.
static void sync_dir_of(const char *path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int plat_file_replace(const char *from, const char *to) {
    if (rename(from, to)) return 0;
    sync_dir_of(to);
    return 1;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    // A rename replaces a read-only file here (only the folder's permission counts) but not on Windows: a file the
    // player made read-only stays as it is on both.
    if (access(path, W_OK) && errno != ENOENT) return 0;
    char tmp[PATH_MAX + 48];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && !rename(tmp, path)) {
        sync_dir_of(path);
        return 1;
    }
    unlink(tmp);
    return 0;
}

int plat_file_flush(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !fsync(fileno(f)); }

void plat_file_delete(const char *path) { unlink(path); }

int plat_file_truncate(const char *path, uint64_t size) { return truncate(path, (off_t)size) == 0; }

int64_t plat_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_size;
}

// The owner's write permission stands for Windows' read-only attribute.
int plat_file_set_readonly(const char *path, int on) {
    struct stat st;
    if (stat(path, &st)) return 0;
    mode_t m = on ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(path, m & 07777) == 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < el || strcmp(e->d_name + n - el, ext) || e->d_name[0] == '.') continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

// ---- virtual memory. Reservations are PROT_NONE mappings; commit is mprotect read-write, decommit a fresh PROT_NONE
// mapping over the range (its pages read zero when committed again). A byte per guest page records the committed
// state (pstate, mapped lazily: only the pages touched cost memory).
//
// Replacing pages detaches them from a GPU import: Metal keeps reading the old ones (docs/macos.md), so a pool has to
// be released before its pages are decommitted, which Windows needs as well.
//
// Write watch: macOS has neither GetWriteWatch nor userfaultfd, so pages are write-protected and the first write to one
// faults. A bit per page (ww_bits) records the written pages. For a watched page in state PS_RW, the rules are:
//   - bit set: the page may be really writable, and it will be reported;
//   - bit clear: the page is really read-only, so the next write faults and sets the bit.
// A commit, and a change back to read-write (plat_mem_protect, a thawed region), leave the page really read-only with
// its bit clear, so only real writes are reported, as on Windows and Linux. The price is one fault for the first write
// of each page, on top of the zero fill. A decommit clears the bits.
// ww_spin serialises the fault path with a reset, which clears a bit and protects its page; without it a fault that had
// set its bit could make the page writable just after the reset protected it, and that page's writes would be lost. The
// fault path takes it in a signal handler, so nothing may fault or wait while holding it.

#define PG 4096ull
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };   // PS_NONE.. are committed: PLAT_PROT_* + PS_NONE
static uint8_t *pstate;
static volatile uint64_t *ww_bits;
static struct { uint64_t lo, hi; } ww_ranges[8];
static volatile int32_t ww_nranges;
static volatile int32_t ww_spin;
static PlatMutex vm_lock = PLAT_MUTEX_INIT;

static void fault_install(void);

static void ps_set(uint64_t addr, uint64_t n, uint8_t v) {
    if (addr < PS_LIMIT) memset(pstate + addr / PG, v, (size_t)((n + PG - 1) / PG));
}

static int ww_watched(uint64_t addr) {
    int n = __atomic_load_n(&ww_nranges, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (addr >= ww_ranges[i].lo && addr < ww_ranges[i].hi) return 1;
    return 0;
}

static void ww_lock(void) {
    while (__atomic_exchange_n(&ww_spin, 1, __ATOMIC_ACQUIRE)) plat_pause();
}

static void ww_unlock(void) { __atomic_store_n(&ww_spin, 0, __ATOMIC_RELEASE); }

// Clears the bits of the pages of [addr, addr + n).
static void ww_clear(uint64_t addr, uint64_t n) {
    uint64_t p = addr / PG, end = (addr + n + PG - 1) / PG;
    while (p < end) {
        uint64_t stop = (p | 63) + 1 < end ? (p | 63) + 1 : end;
        uint64_t mask = (stop - p == 64 ? ~0ull : (1ull << (stop - p)) - 1) << (p & 63);
        __atomic_fetch_and(&ww_bits[p >> 6], ~mask, __ATOMIC_RELAXED);
        p = stop;
    }
}

// The fault path: a write hit a page whose bit is clear. 1 when it was that and the write should run again.
static int ww_fault(uint64_t addr) {
    if (!ww_bits || addr >= PS_LIMIT || !ww_watched(addr)) return 0;
    uint64_t page = addr & ~(PG - 1);
    ww_lock();
    int ours = pstate[page / PG] == PS_RW;
    if (ours) {
        __atomic_fetch_or(&ww_bits[page / PG >> 6], 1ull << (page / PG & 63), __ATOMIC_RELAXED);
        mprotect((void *)(uintptr_t)page, PG, PROT_READ | PROT_WRITE);
    }
    ww_unlock();
    return ours;
}

// A kernel write into a page the tracker holds read-only fails with EFAULT instead of faulting. An atomic OR of zero
// takes that fault from user space without changing the byte, so the page is written and writable before the read.
static void ww_touch(uint64_t addr, uint64_t n) {
    if (!ww_bits || !n || addr >= PS_LIMIT) return;
    for (uint64_t p = addr & ~(PG - 1); p < addr + n && p < PS_LIMIT; p += PG)
        if (pstate[p / PG] == PS_RW && ww_watched(p)) __atomic_fetch_or((volatile uint8_t *)(uintptr_t)p, 0, __ATOMIC_RELAXED);
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    plat_mutex_lock(&vm_lock);
    if (!pstate) {
        void *p = mmap(NULL, PS_LIMIT / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        pstate = p == MAP_FAILED ? NULL : p;
    }
    if (watch && !ww_bits) {
        fault_install();
        void *p = mmap(NULL, PS_LIMIT / PG / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) __atomic_store_n(&ww_bits, (volatile uint64_t *)p, __ATOMIC_RELEASE);
    }
    int ok = 0;
    if (pstate && addr + n <= PS_LIMIT) {
        // VM_FLAGS_FIXED fails on a taken address, where mmap's MAP_FIXED would replace what is there.
        mach_vm_address_t at = addr;
        if (mach_vm_allocate(mach_task_self(), &at, n, VM_FLAGS_FIXED) == KERN_SUCCESS) {
            mprotect((void *)(uintptr_t)addr, n, PROT_NONE);
            ok = 1;
        }
    }
    if (ok) {
        ps_set(addr, n, PS_RESERVED);
        int k = ww_nranges;
        if (watch && ww_bits && k < (int)(sizeof ww_ranges / sizeof *ww_ranges)) {
            ww_ranges[k].lo = addr, ww_ranges[k].hi = addr + n;
            __atomic_store_n(&ww_nranges, k + 1, __ATOMIC_RELEASE);
        } else if (watch) {
            fprintf(stderr, "platform: write watch over %#llx+%#llx not available\n", (unsigned long long)addr,
                    (unsigned long long)n);
        }
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

void *plat_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    int real = ww_watched(addr) ? PROT_READ : PROT_READ | PROT_WRITE;   // the first write faults and is recorded
    ww_lock();
    int ok = !mprotect((void *)(uintptr_t)addr, n, real);
    if (ok) ps_set(addr, n, PS_RW);
    ww_unlock();
    return ok;
}

int plat_mem_decommit(uint64_t addr, uint64_t n) {
    ww_lock();
    void *p = mmap((void *)(uintptr_t)addr, n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
    int ok = p == (void *)(uintptr_t)addr;
    if (ok) {
        ps_set(addr, n, PS_RESERVED);
        if (ww_bits && addr < PS_LIMIT) ww_clear(addr, n);
    }
    ww_unlock();
    return ok;
}

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    int real = prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE : prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
    if (prot == PLAT_PROT_RW && ww_watched(addr)) real = PROT_READ;   // the next write faults and is recorded
    ww_lock();
    int ok = !mprotect((void *)(uintptr_t)addr, n, real);
    if (ok) ps_set(addr, n, (uint8_t)(PS_NONE + prot));
    ww_unlock();
    return ok;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (!pstate || addr >= PS_LIMIT) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < PS_LIMIT ? limit : PS_LIMIT, i = addr / PG;
    uint8_t v = pstate[i];
    while ((i + 1) * PG < lim && pstate[i + 1] == v) i++;
    *end = (i + 1) * PG < lim ? (i + 1) * PG : lim;
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    if (!ww_bits || !ww_watched(addr)) return UINT64_MAX;
    uint64_t got = 0, run = 0, run_end = 0;   // [run, run_end): pages to protect, joined to save system calls
    uint64_t p = addr / PG, end = (addr + n + PG - 1) / PG;
    ww_lock();
    while (p < end && got < max) {
        uint64_t w = __atomic_load_n(&ww_bits[p >> 6], __ATOMIC_RELAXED) >> (p & 63);
        if (!w) {
            p = (p | 63) + 1;
            continue;
        }
        p += (uint64_t)__builtin_ctzll(w);
        if (p >= end) break;
        uint64_t a = p * PG;
        pages[got++] = a;
        if (reset) {
            __atomic_fetch_and(&ww_bits[p >> 6], ~(1ull << (p & 63)), __ATOMIC_RELAXED);
            if (pstate[p] == PS_RW) {
                if (a != run_end) {
                    if (run_end > run) mprotect((void *)(uintptr_t)run, run_end - run, PROT_READ);
                    run = a;
                }
                run_end = a + PG;
            }
        }
        p++;
    }
    if (run_end > run) mprotect((void *)(uintptr_t)run, run_end - run, PROT_READ);
    ww_unlock();
    return got;
}

// One handler for the synchronous faults. A write to a protected page arrives as SIGBUS or SIGSEGV with the write bit in
// the page-fault error code, and goes to the write watch and then to the write-fault hook (memory.c thaws a frozen
// region); a fault in this executable's code then goes to the crash hook (main.c); anything else gets the default
// action. Runs on the thread's signal stack.
static int (*fault_hook)(uint64_t addr);
static void (*crash_fn)(const PlatCrash *c);

// 1 when the page at addr now allows the access: another thread changed its protection between the fault and this
// check (a commit, a thaw), and the access may simply run again.
static int access_ok_now(uint64_t addr, int write) {
    mach_vm_address_t a = addr & ~(PG - 1);
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) !=
        KERN_SUCCESS || a > addr)
        return 0;
    return (info.protection & (write ? VM_PROT_WRITE : VM_PROT_READ)) != 0;
}

// Retries of the same address in a row: an access that keeps faulting on a page that allows it ends as a crash.
static volatile uint64_t retry_at;
static volatile int32_t retry_n;

// The kernel keeps a "on the signal stack" mark that sigreturn sets from uc_onstack. A handler call that
// plat_intr_resume dropped never returned, so the mark stayed set and the next signal went on the interrupted stack;
// the interrupted code is on the signal stack only when its rsp is inside it.
static void onstack_fix(ucontext_t *u) {
    stack_t ss;
    if (sigaltstack(NULL, &ss) || (ss.ss_flags & SS_DISABLE)) return;
    uint64_t sp = u->uc_mcontext->__ss.__rsp, lo = (uint64_t)(uintptr_t)ss.ss_sp;
    u->uc_onstack = sp >= lo && sp - lo < ss.ss_size;
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    onstack_fix(u);
    uint32_t err = u->uc_mcontext->__es.__err;
    int mem = sig == SIGSEGV || sig == SIGBUS;
    uint64_t at = (uint64_t)(uintptr_t)si->si_addr;
    if (mem && (err & 2) && (ww_fault(at) || (fault_hook && fault_hook(at)))) return;
    if (mem && access_ok_now(at, err & 2)) {
        if (__atomic_exchange_n(&retry_at, at, __ATOMIC_RELAXED) != at) __atomic_store_n(&retry_n, 0, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&retry_n, 1, __ATOMIC_RELAXED) < 1000) return;
    }
    // A fault nobody handles ends the process here, whichever code it is in. Returning with the default action set
    // instead would leave it set for the whole process when the access then succeeds (a page another thread made
    // accessible in between), and the next write-watch fault on any thread would end the game without a word.
    uintptr_t rip = (uintptr_t)u->uc_mcontext->__ss.__rip;
    Dl_info info, self;
    int ours = dladdr((void *)plat_crash_hook, &self);
    if (mem) {
        char line[512];
        uint8_t st = at < PS_LIMIT && pstate ? pstate[at / PG] : 0xff;
        int watched = ww_bits && at < PS_LIMIT && ww_watched(at);
        int bit = watched ? (int)(ww_bits[at / PG >> 6] >> (at / PG & 63) & 1) : -1;
        int k = snprintf(line, sizeof line, "platform: unhandled %s fault at %#llx (err %#x, page state %u, watched %d, "
                         "written bit %d) by code in %s\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV", (unsigned long long)at,
                         err, st, watched, bit, dladdr((void *)rip, &info) && info.dli_fname ? info.dli_fname : "?");
        if (k > 0) write_all(2, line, (size_t)k < sizeof line ? (size_t)k : sizeof line - 1);
    }
    if (crash_fn && ours) {
        PlatCrash c = {sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : "arithmetic exception",
                       (uint32_t)sig, rip, rip - (uintptr_t)self.dli_fbase, -1, 0};
        if (mem) c.access = (err & 16) ? 8 : (err & 2) ? 1 : 0, c.addr = at;
        crash_fn(&c);   // logs and exits
    }
    signal(sig, SIG_DFL);   // returns into the faulting instruction, which faults again with the default action
}

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

void plat_reexec(void) {
    char exe[PATH_MAX];
    fflush(stdout);
    fflush(stderr);
    if (plat_exe_path(exe, sizeof exe)) execv(exe, *_NSGetArgv());
    fprintf(stderr, "FATAL: re-running the process failed (%s)\n", strerror(errno));
    _exit(70);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), addr, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    uint8_t b;
    uint64_t end = addr + (n ? n : 1);
    for (uint64_t a = addr; a < end; a = (a | 4095) + 1)
        if (!plat_mem_read(a, &b, 1)) return 0;
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// A PlatFile is the descriptor + 1, so descriptor 0 is not NULL.
PlatFile *plat_file_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) fcntl(fd, F_NOCACHE, 1);   // macOS's O_DIRECT
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

static int fd_of(PlatFile *f) { return (int)(intptr_t)f - 1; }

void plat_file_close(PlatFile *f) {
    if (f) close(fd_of(f));
}

// The write watch's reset may protect a page again between ww_touch and the read (the render thread resets watched
// ranges every frame), and the kernel then fails the read with EFAULT. So the read goes in parts of PREAD_PART bytes,
// each touched just before it and read again after an EFAULT; only a part that keeps failing is a real read-only page.
#define PREAD_PART (256u << 10)
#define PREAD_TRIES 64

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    uint8_t *p = buf;
    uint64_t done = 0;
    while (done < n) {
        uint32_t part = n - done < PREAD_PART ? (uint32_t)(n - done) : PREAD_PART;
        ssize_t k;
        for (int tries = 0;; tries++) {
            ww_touch((uint64_t)(uintptr_t)(p + done), part);
            do k = pread(fd_of(f), p + done, part, (off_t)(off + done));
            while (k < 0 && errno == EINTR);
            if (k >= 0 || errno != EFAULT || tries == PREAD_TRIES) break;
        }
        if (k < 0) return errno == EFAULT ? -2 : -1;
        done += (uint64_t)k;
        if ((uint32_t)k < part) break;   // the end of the file
    }
    return (int64_t)done;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    struct stat st;
    if (fstat(fd_of(f), &st)) return 0;
    *out = (PlatFileId){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                        (uint64_t)st.st_mtimespec.tv_sec * 1000000000u + (uint64_t)st.st_mtimespec.tv_nsec,
                        (uint64_t)st.st_ctimespec.tv_sec * 1000000000u + (uint64_t)st.st_ctimespec.tv_nsec};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    struct stat st;
    if (fstat(fd_of(f), &st) || !st.st_size) return NULL;
    void *v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd_of(f), 0);
    return v == MAP_FAILED ? NULL : v;
}

int plat_fs_os_error(void) { return errno; }

static int fs_err(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return PLAT_FS_MISSING;
    return S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? fs_err(errno) : PLAT_FS_OK; }

// Deleting needs write access to the directory holding the entry, which a read-only directory (plat_file_set_readonly,
// or one copied from another emulator's save folder) lacks; on Windows the read-only attribute of a directory does not
// stop deleting what is in it. After EACCES the parent gets its owner's write bit back and the delete runs once more.
static int parent_writable(const char *path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return 0;
    char *cut = strrchr(dir, '/');
    if (!cut) return 0;
    *cut = 0;
    if (!dir[0]) return 0;
    struct stat st;
    return !stat(dir, &st) && !(st.st_mode & S_IWUSR) && chmod(dir, (st.st_mode | S_IWUSR) & 07777) == 0;
}

static int remove_entry(const char *path, int dir) {
    int r = dir ? rmdir(path) : unlink(path);
    if (r && errno == EACCES && parent_writable(path)) r = dir ? rmdir(path) : unlink(path);
    return r ? fs_err(errno) : PLAT_FS_OK;
}

int plat_fs_rmdir(const char *path) {
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) return remove_entry(path, 0);
    return remove_entry(path, 1);
}

// A read-only file goes as any other: deleting asks for write access to the directory only.
int plat_fs_unlink(const char *path) { return remove_entry(path, 0); }

// RENAME_EXCL keeps Windows' rule that a rename never replaces; a file system without it gets a check first, which
// another process could race but the save directories are ours.
int plat_fs_rename(const char *from, const char *to, int durable) {
    if (renamex_np(from, to, RENAME_EXCL)) {
        if (errno != EINVAL && errno != ENOTSUP) return fs_err(errno);
        if (plat_fs_kind(to) != PLAT_FS_MISSING) return PLAT_FS_EXISTS;
        if (rename(from, to)) return fs_err(errno);
    }
    if (durable) {
        sync_dir_of(from);
        sync_dir_of(to);
    }
    return PLAT_FS_OK;
}

int plat_fs_link(const char *existing, const char *path) { return link(existing, path) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_links(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (int)st.st_nlink;
}

void plat_fs_sync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

// Open files never stop a rename here.
void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    (void)dir;
    if (cap) *out = 0;
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return fs_err(errno);
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        int kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
        if (!cb(e->d_name, kind, kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    struct stat st;
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
        close(fd);
        fd = -1, errno = ENOENT;
    }
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_fs_create(const char *path, int *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pwrite(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k;
}

int plat_file_set_size(PlatFile *f, uint64_t size) { return ftruncate(fd_of(f), (off_t)size) ? fs_err(errno) : 0; }

int plat_file_fsync(PlatFile *f) { return fsync(fd_of(f)) == 0; }

int plat_temp_dir(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    size_t n = strlen(t);
    while (n > 1 && t[n - 1] == '/') n--;
    return snprintf(out, cap, "%.*s", (int)n, t) < (int)cap;
}

uintptr_t plat_exe_base(void) {
    Dl_info d;
    return dladdr((void *)plat_exe_base, &d) ? (uintptr_t)d.dli_fbase : 0;
}

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    Dl_info d;
    if (!dladdr((void *)addr, &d) || !d.dli_fname) return 0;
    const char *n = strrchr(d.dli_fname, '/');
    *base = (uintptr_t)d.dli_fbase;
    return snprintf(name, cap, "%s", n ? n + 1 : d.dli_fname) < (int)cap;
}

int plat_exe_path(char *out, size_t cap) {
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    char real[PATH_MAX];
    if (_NSGetExecutablePath(raw, &n) || !realpath(raw, real)) return 0;
    return snprintf(out, cap, "%s", real) < (int)cap;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// Cached per thread: plat_rmutex_lock asks on every acquire, hundreds of thousands of times a second in play. The low
// 32 bits of the system-wide thread id stay unique for the life of a game. noinline: a fiber may resume on another
// thread (platform.h), and a caller that inlined this could keep the address of tid it computed before a
// plat_fiber_switch; as a call it reads the slot of the thread it runs on.
__attribute__((noinline)) uint32_t plat_thread_id(void) {
    static __thread uint32_t tid;
    if (!tid) {
        uint64_t id = 0;
        pthread_threadid_np(NULL, &id);
        tid = (uint32_t)id;
    }
    return tid;
}

// The first set variable of names ("ru_RU.UTF-8") as a BCP-47 tag ("ru-RU") into tag; "" for C and POSIX.
static void locale_tag(const char *const *names, int n, char *tag, size_t cap) {
    const char *v = NULL;
    for (int i = 0; i < n && !(v && *v); i++) v = getenv(names[i]);
    size_t k = 0;
    if (v && strcmp(v, "C") && strncmp(v, "C.", 2) && strcmp(v, "POSIX"))
        for (; v[k] && v[k] != '.' && v[k] != '@' && k < cap - 1; k++) tag[k] = v[k] == '_' ? '-' : v[k];
    tag[k] = 0;
}

const char *plat_ui_language(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_TIME", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

// The system's own CJK fonts.
int plat_font_path(const char *lang, char *out, size_t cap) {
    const char *f = !strcmp(lang, "ja") ? "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc"
                  : !strcmp(lang, "ko") ? "/System/Library/Fonts/AppleSDGothicNeo.ttc"
                  : !strcmp(lang, "zh-Hans") || !strcmp(lang, "zh-Hant") ? "/System/Library/Fonts/Hiragino Sans GB.ttc" : NULL;
    return f && plat_file_exists(f) && snprintf(out, cap, "%s", f) < (int)cap;
}

// "macOS 15.4 (Intel)", or "(Rosetta)" when this x86-64 build runs translated on Apple silicon.
int plat_os_version(char *out, size_t cap) {
    char v[32] = "";
    size_t n = sizeof v;
    sysctlbyname("kern.osproductversion", v, &n, NULL, 0);
    int translated = 0;
    n = sizeof translated;
    if (sysctlbyname("sysctl.proc_translated", &translated, &n, NULL, 0)) translated = 0;
    return snprintf(out, cap, "macOS %s (%s)", v, translated ? "Rosetta" : "Intel") < (int)cap;
}

int plat_is_wine(void) { return 0; }

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    if (fstat(2, &st) || !S_ISREG(st.st_mode)) return 0;
    char path[PATH_MAX];
    if (fcntl(2, F_GETPATH, path)) return 0;
    return snprintf(out, cap, "%s", path) < (int)cap;
}

// ---- process

void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

int plat_setenv(const char *name, const char *value) {
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

uint64_t plat_process_cpu_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t plat_process_uptime_ns(void) {
    struct kinfo_proc p;
    size_t n = sizeof p;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &p, &n, NULL, 0)) return 0;
    uint64_t at = (uint64_t)p.kp_proc.p_starttime.tv_sec * 1000000000u + (uint64_t)p.kp_proc.p_starttime.tv_usec * 1000u;
    uint64_t now = plat_unix_time_ns();
    return now > at ? now - at : 0;
}

uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_r(&t, out);
}

// The range of the Windows CRT's localtime_s and gmtime_s (1970 to 3000), which glibc exceeds: the game's time
// service reports times outside it as invalid on both hosts.
#define PLAT_TIME_MAX 32535215999ll

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
    tzset();
    return timezone;
}

static int pipe_cloexec(int p[2]) {
    if (pipe(p)) return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    char **argv = *_NSGetArgv();
    char exe[PATH_MAX];
    if (!plat_exe_path(exe, sizeof exe)) return -1;
    int p[2];
    if (pipe_cloexec(p)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    setenv(env_name, env_value, 1);
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    unsetenv(env_name);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) {
        close(p[0]);
        return -1;
    }
    size_t len = 0;
    char sink[512];
    ssize_t k;
    while (len + 1 < cap && (k = read(p[0], out + len, cap - 1 - len)) != 0)
        if (k > 0) len += (size_t)k;
        else if (errno != EINTR) break;
    if (cap) out[len] = 0;
    while ((k = read(p[0], sink, sizeof sink)) > 0 || (k < 0 && errno == EINTR)) {}
    close(p[0]);
    int status = 0;
    uint64_t end = plat_now_ns() + (uint64_t)timeout_ms * 1000000u;
    while (waitpid(pid, &status, timeout_ms ? WNOHANG : 0) == 0) {
        if (plat_now_ns() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        plat_sleep_ms(10);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int plat_random(void *out, size_t n) {
    arc4random_buf(out, n);
    return 1;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) { return SDL_SetClipboardText(utf8); }

static void reap_main(void *arg) {
    waitpid((pid_t)(intptr_t)arg, NULL, 0);
}

// open shows the folder in Finder; it runs detached, so a slow answer does not hold up the caller, and a
// thread of its own reaps it so it does not stay a zombie.
int plat_open_folder(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return 0;
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
    char *argv[] = {"open", (char *)path, NULL};
    pid_t pid;
    int e = posix_spawnp(&pid, "open", NULL, &a, argv, environ);
    posix_spawnattr_destroy(&a);
    if (e) return 0;
    plat_thread_spawn(reap_main, (void *)(intptr_t)pid, "reap", 64 << 10);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load: Mach host and task statistics. The GPU and the CPU temperature have no public source here, so they
// stay -1, which the overlay reads as unknown.

struct PlatLoad {
    uint64_t busy, total;   // the previous call's ticks; total 0 before the first
    uint64_t proc_ns;
    io_service_t gpu;       // the GPU's IOAccelerator, whose PerformanceStatistics the driver keeps current; 0: none
};

// MoltenVK's deviceLUID is the Metal device's registryID, big-endian (MoltenVK's MVKDevice.mm,
// MVKPhysicalDevice::populateDeviceIDProperties), which is the registry entry ID of its IOAccelerator. Without a LUID,
// or with no accelerator of that ID, the first accelerator that has statistics.
static io_service_t accelerator_of(const uint8_t *luid) {
    uint64_t want = 0;
    if (luid)
        for (int i = 0; i < 8; i++) want = want << 8 | luid[i];
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return 0;
    io_service_t found = 0, first = 0;
    for (io_service_t s; (s = IOIteratorNext(it));) {
        uint64_t id = 0;
        CFTypeRef st = IORegistryEntryCreateCFProperty(s, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        int has = st != NULL;
        if (st) CFRelease(st);
        if (has && want && IORegistryEntryGetRegistryEntryID(s, &id) == KERN_SUCCESS && id == want && !found) {
            found = s;
            continue;
        }
        if (has && !first) {
            first = s;
            continue;
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (found && first) IOObjectRelease(first);
    return found ? found : first;
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus) {
    (void)pci_bus;
    PlatLoad *l = calloc(1, sizeof(PlatLoad));
    if (l) l->gpu = accelerator_of(gpu_luid);
    return l;
}

void plat_load_close(PlatLoad *l) {
    if (l && l->gpu) IOObjectRelease(l->gpu);
    free(l);
}

// One number of the GPU's PerformanceStatistics; -1 when the driver does not give it.
static double gpu_stat(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = CFDictionaryGetValue(d, key);
    double x;
    return v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberDoubleType, &x) ? x : -1;
}

// The statistics AMD's macOS drivers keep ("Device Utilization %", the load Activity Monitor shows), read fresh on
// each call; NULL without them.
static CFDictionaryRef gpu_stats(PlatLoad *l) {
    if (!l || !l->gpu) return NULL;
    CFTypeRef st = IORegistryEntryCreateCFProperty(l->gpu, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
    if (st && CFGetTypeID(st) != CFDictionaryGetTypeID()) CFRelease(st), st = NULL;
    return st;
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    host_cpu_load_info_data_t h;
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&h, &n) != KERN_SUCCESS) return;
    uint64_t busy = (uint64_t)h.cpu_ticks[CPU_STATE_USER] + h.cpu_ticks[CPU_STATE_SYSTEM] + h.cpu_ticks[CPU_STATE_NICE];
    uint64_t total = busy + h.cpu_ticks[CPU_STATE_IDLE], pns = plat_process_cpu_ns();
    if (l->total && total > l->total) {
        uint64_t dt = total - l->total;
        *total_pct = 100.0f * (float)(busy - l->busy) / (float)dt;
        // the process time is in ns, the machine's in ticks summed over every CPU
        double dt_ns = (double)dt * 1e9 / (double)sysconf(_SC_CLK_TCK);
        *proc_pct = (float)(100.0 * (double)(pns - l->proc_ns) / dt_ns);
    }
    l->busy = busy, l->total = total, l->proc_ns = pns;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    CFDictionaryRef d = gpu_stats(l);
    if (!d) return;
    double load = gpu_stat(d, CFSTR("Device Utilization %")), used = gpu_stat(d, CFSTR("inUseVidMemoryBytes"));
    if (load >= 0) *load_pct = (float)load;
    if (used >= 0) *vram_used_mb = (float)(used / 1048576.0);
    CFRelease(d);
}

void plat_gpu_sensors(PlatLoad *l, float *temp_c, float *power_w, float *clock_mhz) {
    *temp_c = *power_w = *clock_mhz = -1;
    CFDictionaryRef d = gpu_stats(l);
    if (!d) return;
    *temp_c = (float)gpu_stat(d, CFSTR("Temperature(C)"));
    *power_w = (float)gpu_stat(d, CFSTR("Total Power(W)"));
    *clock_mhz = (float)gpu_stat(d, CFSTR("Core Clock(MHz)"));
    CFRelease(d);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    uint64_t total = 0;
    size_t n = sizeof total;
    vm_statistics64_data_t v;
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    if (!sysctlbyname("hw.memsize", &total, &n, NULL, 0) && total) {
        *total_mb = (float)(total >> 20);
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c) == KERN_SUCCESS) {
            uint64_t pg = (uint64_t)vm_kernel_page_size, free_b = ((uint64_t)v.free_count + v.inactive_count) * pg;
            *used_mb = (float)((total - free_b) >> 20);
        }
    }
    uint64_t resident, private_bytes;
    plat_process_memory(&resident, &private_bytes);
    if (resident) *proc_mb = (float)(resident >> 20);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    *resident = *private_bytes = 0;
    task_vm_info_data_t t;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&t, &n) != KERN_SUCCESS) return;
    *resident = t.resident_size;
    *private_bytes = t.phys_footprint;
}

// macOS compresses and swaps instead of a commit limit, so only the process's own share is known.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    *proc_mb = *used_mb = *limit_mb = -1;
    uint64_t resident, private_bytes;
    plat_process_memory(&resident, &private_bytes);
    if (private_bytes) *proc_mb = (double)(private_bytes >> 20);
}

// ---- child processes

struct PlatChild {
    pid_t pid;
    int in, out;   // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    if (err) *err = 0;
    int in[2], out[2];
    if (pipe_cloexec(in)) return NULL;
    if (pipe_cloexec(out)) {
        close(in[0]), close(in[1]);
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {(char *)exe, (char *)arg, NULL};
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (e) {
        close(in[1]), close(out[0]);
        if (err) *err = (uint32_t)e;
        return NULL;
    }
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in[1]), close(out[0]);
        return NULL;
    }
    c->pid = pid, c->in = in[1], c->out = out[0];
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    close(c->in);
    close(c->out);
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
    free(c);
}

// A child that died makes the write fail with EPIPE; SIGPIPE is ignored for the process (main.c), as Windows has none.
int plat_child_write(PlatChild *c, const void *data, size_t n) { return write_all(c->in, data, n); }

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    ssize_t k;
    do k = read(c->out, buf, cap);
    while (k < 0 && errno == EINTR);
    return (long)k;
}

// ---- restart
// TOTK_RESTART_WAIT names the process that called plat_restart, as its id and start time in microseconds (kinfo_proc's
// p_starttime): a process that has only reused the id is not waited for. Waiting keeps the new process off the old
// one's files (the log it moves at start, the saves) until the old one is gone. The new process gets no descriptor past
// stderr (POSIX_SPAWN_CLOEXEC_DEFAULT with 0-2 inherited), and the default signal mask and dispositions, whatever the
// calling thread had.
#define RESTART_WAIT "TOTK_RESTART_WAIT"

// The start time of process pid in microseconds, 0 when it is gone.
static unsigned long long process_start(int pid) {
    struct kinfo_proc kp;
    size_t n = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || n != sizeof kp) return 0;
    struct timeval t = kp.kp_proc.p_starttime;
    return (unsigned long long)t.tv_sec * 1000000ull + (unsigned long long)t.tv_usec;
}

// Fills the actions and attributes both plat_restart and plat_spawn start a child with.
static void spawn_setup(posix_spawn_file_actions_t *fa, posix_spawnattr_t *at, short flags) {
    posix_spawn_file_actions_init(fa);
    for (int fd = 0; fd < 3; fd++) posix_spawn_file_actions_addinherit_np(fa, fd);
    posix_spawnattr_init(at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(at, &none);
    posix_spawnattr_setsigdefault(at, &all);
    posix_spawnattr_setflags(at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT | flags);
}

int plat_restart(void) {
    char exe[PATH_MAX], wait[64];
    if (!*_NSGetArgc() || !plat_exe_path(exe, sizeof exe)) return 0;
    snprintf(wait, sizeof wait, "%d.%llu", (int)getpid(), process_start((int)getpid()));
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    spawn_setup(&fa, &at, 0);
    setenv(RESTART_WAIT, wait, 1);
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, &at, *_NSGetArgv(), environ);
    unsetenv(RESTART_WAIT);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return e == 0;
}

int plat_spawn(const char *const *argv, const char *cwd) {
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    spawn_setup(&fa, &at, POSIX_SPAWN_SETSID);
    posix_spawn_file_actions_addchdir_np(&fa, cwd);
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return e == 0;
}

int plat_restart_wait(uint32_t timeout_ms) {
    const char *v = getenv(RESTART_WAIT);
    if (!v) return 0;
    int pid = 0;
    unsigned long long start = 0;
    int ok = sscanf(v, "%d.%llu", &pid, &start) == 2 && pid > 0 && start;
    unsetenv(RESTART_WAIT);   // the processes this one starts do not wait again
    if (!ok) return 0;
    // The exit watch is registered first: a start time that still matches then proves it is that process, not a new one
    // with the id. ESRCH: it is already gone.
    int kq = kqueue();
    if (kq < 0) return 0;
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    int r = 0;
    if (kevent(kq, &ev, 1, NULL, 0, NULL) == 0 && process_start(pid) == start) {
        struct timespec t = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000};
        do r = kevent(kq, NULL, 0, &ev, 1, &t);
        while (r < 0 && errno == EINTR);
        r = r > 0 ? 1 : -1;
    }
    close(kq);
    return r;
}
