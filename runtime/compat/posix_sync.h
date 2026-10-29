/* The Darwin locking, address-wait and clock calls the POSIX runtime uses (os_unfair_lock, os_sync_wait_on_address,
 * clock_gettime_nsec_np), given on Linux with futexes, so win32_posix.c and os/core_model.c keep one form.
 *
 * On macOS this only includes the system headers. On Linux:
 * - os_unfair_lock is a three-state futex lock (0 free, 1 taken, 2 taken with waiters; Drepper, "Futexes Are Tricky",
 *   mutex 2). Like os_unfair_lock it is valid when zero-filled and is not recursive.
 * - os_sync_wait_on_address waits on the low 32 bits of a 4- or 8-byte word: the runtime's words are sequence numbers
 *   and flags that change in their low bits, and x86-64 is little-endian. A change only in the high half is not seen;
 *   none of the runtime's waits depends on one.
 * - CLOCK_UPTIME_RAW is CLOCK_MONOTONIC_RAW (both stop nothing on suspend that the runtime measures). */
#ifndef RECOMP_POSIX_SYNC_H
#define RECOMP_POSIX_SYNC_H

#ifdef __APPLE__
#include <os/lock.h>
#include <os/os_sync_wait_on_address.h>
#else
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/syscall.h>
#include <linux/futex.h>

typedef struct { volatile uint32_t v; } os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT {0}

static inline long recomp_futex(volatile void *addr, int op, uint32_t val, const struct timespec *ts) {
    return syscall(SYS_futex, addr, op, val, ts, NULL, 0);
}
static inline int os_unfair_lock_trylock(os_unfair_lock *l) {
    uint32_t free_ = 0;
    return __atomic_compare_exchange_n(&l->v, &free_, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static inline void os_unfair_lock_lock(os_unfair_lock *l) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        recomp_futex(&l->v, FUTEX_WAIT_PRIVATE, 2, NULL);
        c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    }
}
static inline void os_unfair_lock_unlock(os_unfair_lock *l) {
    if (__atomic_exchange_n(&l->v, 0, __ATOMIC_RELEASE) == 2) recomp_futex(&l->v, FUTEX_WAKE_PRIVATE, 1, NULL);
}

#define OS_SYNC_WAIT_ON_ADDRESS_NONE 0
#define OS_SYNC_WAKE_BY_ADDRESS_NONE 0
#define OS_CLOCK_MACH_ABSOLUTE_TIME 0
/* As on macOS: >= 0 when woken (or the value differed), -1 with errno ETIMEDOUT / EINTR otherwise. */
static inline int os_sync_wait_on_address_with_timeout(void *addr, uint64_t value, size_t size, int flags, int clock, uint64_t ns) {
    (void)size; (void)flags; (void)clock;
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)}, *t = ns == UINT64_MAX ? NULL : &ts;
    if (recomp_futex(addr, FUTEX_WAIT_PRIVATE, (uint32_t)value, t) == 0) return 0;
    if (errno == EAGAIN) return 0;                   /* the word had already changed */
    return -1;
}
static inline int os_sync_wait_on_address(void *addr, uint64_t value, size_t size, int flags) {
    return os_sync_wait_on_address_with_timeout(addr, value, size, flags, 0, UINT64_MAX);
}
static inline int os_sync_wake_by_address_any(void *addr, size_t size, int flags) {
    (void)size; (void)flags;
    return (int)recomp_futex(addr, FUTEX_WAKE_PRIVATE, 1, NULL);
}
static inline int os_sync_wake_by_address_all(void *addr, size_t size, int flags) {
    (void)size; (void)flags;
    return (int)recomp_futex(addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
}

#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
static inline uint64_t clock_gettime_nsec_np(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
#endif
