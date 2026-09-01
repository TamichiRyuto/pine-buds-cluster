// Host-only pthread port for the OpenMP runtime: runs the worker half of a
// parallel region on its own thread, matching the target where the second
// Cortex-M4F (cp_accel) runs it. Test infrastructure — never compiled for
// target. Design doc §15.3 (R9).
#ifndef OMP_THREAD_PORT_H
#define OMP_THREAD_PORT_H

#include <pthread.h>

#include "omp_adapter.h"

namespace omp_thread_port {

inline pthread_t g_worker;
inline bool g_worker_running = false;
inline int g_starts = 0;  // how many times worker_start ran fn
inline int g_joins = 0;

struct WorkerTask {
    void (*fn)(void*);
    void* data;
};
inline WorkerTask g_task;

inline void* worker_trampoline(void* p) {
    WorkerTask* t = static_cast<WorkerTask*>(p);
    t->fn(t->data);
    return nullptr;
}

inline int worker_count() { return 1; }

inline int worker_start(void (*fn)(void*), void* data) {
    g_task.fn = fn;
    g_task.data = data;
    if (pthread_create(&g_worker, nullptr, &worker_trampoline, &g_task) != 0) {
        return -1;
    }
    g_worker_running = true;
    ++g_starts;
    return 0;
}

inline void worker_join() {
    if (g_worker_running) {
        pthread_join(g_worker, nullptr);
        g_worker_running = false;
    }
    ++g_joins;
}

inline int self_is_worker() {
    return g_worker_running && pthread_equal(pthread_self(), g_worker) ? 1 : 0;
}

// Installs the pthread port; omp_set_port(nullptr) uninstalls it.
inline void install() {
    static const omp_port port = {&worker_count, &worker_start, &worker_join,
                                  &self_is_worker};
    g_starts = 0;
    g_joins = 0;
    omp_set_port(&port);
}

}  // namespace omp_thread_port

#endif  // OMP_THREAD_PORT_H
