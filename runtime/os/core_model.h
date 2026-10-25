/* The console core model on macOS (core_model.c; docs/macos-port.md "The console core model on macOS"). Windows keeps
 * the model of os_threads.c (host processors and priorities). */
#ifndef RECOMP_CORE_MODEL_H
#define RECOMP_CORE_MODEL_H
#ifndef _WIN32
#include <stdint.h>
#include <stdio.h>

typedef struct CmThread CmThread;

int cm_enabled(void);                                  /* SMO_OS_CONSOLE_CORES=0 turns it off */
CmThread *cm_new(int priority, uint64_t core_mask, const char *name);
void cm_enter(CmThread *t);                            /* the calling thread is t from now on: takes a core (may wait) */
void cm_leave(void);                                   /* the calling thread ends: gives its core back */
void cm_set_priority(CmThread *t, int priority);       /* nn::os priority: 0 highest .. 31 lowest */
void cm_set_mask(CmThread *t, uint64_t core_mask);     /* bits 0..2 */
void cm_set_name(CmThread *t, const char *name);
int cm_current_core(void);                             /* the core the calling thread holds, -1 if none */
void cm_yield(void);                                   /* nn::os::YieldThread */
void cm_block_begin(void);                             /* around a wait that blocks: the core goes to another thread */
void cm_block_end(void);
void cm_report(FILE *out);

/* The safe point: lifted code calls into the host here (hostrt.c host_import, call_indirect). The flag is the calling
 * thread's own, set by another thread that wants its core; for a thread outside the model it points at a zero. */
extern _Thread_local volatile int *cm_t_flag;
void cm_safe_point(void);
#define CM_SAFE_POINT() do { if (__builtin_expect(*cm_t_flag, 0)) cm_safe_point(); } while (0)

#else
#define CM_SAFE_POINT() do { } while (0)
#endif
#endif
