// Host-only pthread port for the MPI adapter: runs each rank on its own
// thread so blocking Recv and collectives get real concurrency, matching
// the target where each bud runs one rank. Test infrastructure — never
// compiled for target.
#ifndef MPI_THREAD_PORT_H
#define MPI_THREAD_PORT_H

#include <pthread.h>

#include "mpi_adapter.h"

namespace mpi_thread_port {

inline pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
inline pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
inline pthread_t g_thread_ids[2];
inline bool g_registered[2] = {false, false};

inline int self_rank() {
    for (int i = 0; i < 2; ++i) {
        if (g_registered[i] && pthread_equal(pthread_self(), g_thread_ids[i])) {
            return i;
        }
    }
    return 0;
}
inline void lock() { pthread_mutex_lock(&g_mtx); }
inline void unlock() { pthread_mutex_unlock(&g_mtx); }
inline void wait() { pthread_cond_wait(&g_cv, &g_mtx); }
inline void wake() { pthread_cond_broadcast(&g_cv); }

struct RankTask {
    int rank;
    void (*body)(int rank);
};

// Each thread registers its own id before running the body, so self_rank
// only ever looks up an entry the calling thread itself has written.
inline void* rank_trampoline(void* p) {
    RankTask* t = static_cast<RankTask*>(p);
    lock();
    g_thread_ids[t->rank] = pthread_self();
    g_registered[t->rank] = true;
    unlock();
    t->body(t->rank);
    return nullptr;
}

// Runs body(0) and body(1) concurrently with the pthread port installed.
// The port is uninstalled (back to sequential mode) before returning.
inline void run_two_ranks(void (*body)(int rank)) {
    mpi_adapter_port port;
    port.self_rank = &self_rank;
    port.lock = &lock;
    port.unlock = &unlock;
    port.wait = &wait;
    port.wake = &wake;
    mpi_adapter_set_port(&port);

    g_registered[0] = g_registered[1] = false;
    RankTask tasks[2] = {{0, body}, {1, body}};
    pthread_t th[2];
    pthread_create(&th[0], nullptr, &rank_trampoline, &tasks[0]);
    pthread_create(&th[1], nullptr, &rank_trampoline, &tasks[1]);
    pthread_join(th[0], nullptr);
    pthread_join(th[1], nullptr);
    mpi_adapter_set_port(nullptr);
}

}  // namespace mpi_thread_port

#endif  // MPI_THREAD_PORT_H
