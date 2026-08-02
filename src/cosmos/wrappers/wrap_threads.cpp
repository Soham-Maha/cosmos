#include "cosmos/cosmos.hpp"
#include <pthread.h>

extern "C" {

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start_routine)(void*), void* arg) {
    (void)thread;
    (void)attr;
    (void)start_routine;
    (void)arg;
    return 0;
}

int __wrap_pthread_join(pthread_t thread, void** retval) {
    (void)thread;
    (void)retval;
    return 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    (void)mutex;
    return 0;
}

} // extern "C"
