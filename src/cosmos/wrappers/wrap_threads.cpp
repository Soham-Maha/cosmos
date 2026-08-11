#include "cosmos/cosmos.hpp"
#include <pthread.h>
#include <sched.h>

// Linker-wrapping passthrough stubs for the POSIX threading/sync surface
// (see docs/design.md §3 "Threads / Sync"). Under the sim build these route
// pthread / sched calls into libcosmos; __real_* aliases resolve to the real
// libpthread functions at final-link time via --wrap. The cooperative
// single-threaded fiber scheduler is layered on later; for now they pass
// through unchanged.

extern "C" {

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg);
int __real_pthread_join(pthread_t thread, void **retval);
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __real_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int __real_pthread_cond_signal(pthread_cond_t *cond);
int __real_sched_yield(void);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    return __real_pthread_create(thread, attr, start_routine, arg);
}

int __wrap_pthread_join(pthread_t thread, void **retval) {
    return __real_pthread_join(thread, retval);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) { return __real_pthread_mutex_lock(mutex); }

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    return __real_pthread_mutex_unlock(mutex);
}

int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    return __real_pthread_cond_wait(cond, mutex);
}

int __wrap_pthread_cond_signal(pthread_cond_t *cond) { return __real_pthread_cond_signal(cond); }

int __wrap_sched_yield(void) { return __real_sched_yield(); }

} // extern "C"
