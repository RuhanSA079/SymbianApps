/*
 * ns_thread.h: worker threads and one process-wide mutex for NetSurf's C
 * code on Symbian (native RThread/RMutex; threads share the process heap).
 */
#ifndef NS_THREAD_H
#define NS_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the mutex; call once from the main thread. 0 on success. */
int ns_mutex_init(void);
void ns_mutex_lock(void);
void ns_mutex_unlock(void);

/* Run fn(arg) on a new thread with the given stack size. The thread ends
 * when fn returns. 0 on success, a negative Symbian error otherwise. */
int ns_thread_start(void (*fn)(void *arg), void *arg, int stack_size);

#ifdef __cplusplus
}
#endif

#endif
