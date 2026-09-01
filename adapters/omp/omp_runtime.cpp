// OpenMP runtime for the unmodified-benchmark strategy (adapters/omp).
#include "omp.h"
#include "omp_adapter.h"

#include <time.h>

namespace {
    // Host time source, isolated so a target port can swap this for a hal
    // timer without touching the omp_get_wtime API surface below.
    double wtime_seconds(void) {
        return (double)clock() / (double)CLOCKS_PER_SEC;
    }

    const omp_port *g_port = 0;

    // File-local team state: 1 outside a parallel region, 2 while a team
    // of two is active (R3).
    int g_team_size = 1;
}

extern "C" int omp_get_num_threads(void) {
    return g_team_size;
}

extern "C" int omp_get_thread_num(void) {
    if (g_team_size == 2 && g_port && g_port->self_is_worker()) {
        return 1;
    }
    return 0;
}

extern "C" int omp_get_max_threads(void) {
    return 1 + (g_port ? g_port->worker_count() : 0);
}

extern "C" int omp_get_num_procs(void) {
    return 1 + (g_port ? g_port->worker_count() : 0);
}

extern "C" void omp_set_num_threads(int n) {
    (void)n;
}

extern "C" double omp_get_wtime(void) {
    return wtime_seconds();
}

extern "C" void omp_set_port(const omp_port *port) {
    g_port = port;
}

extern "C" void GOMP_parallel(void (*fn)(void *), void *data,
                               unsigned num_threads, unsigned flags) {
    (void)num_threads;
    (void)flags;

    if (g_port && g_port->worker_count() >= 1) {
        g_team_size = 2;
        g_port->worker_start(fn, data);
        fn(data);
        g_port->worker_join();
        g_team_size = 1;
        return;
    }

    fn(data);
}
