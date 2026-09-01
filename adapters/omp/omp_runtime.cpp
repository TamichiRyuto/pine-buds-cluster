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

    // OpenMP nthreads-var: 0 means unset (falls back to capacity).
    int g_nthreads_var = 0;

    // Nesting depth of GOMP_parallel calls currently in flight. libgomp's
    // default nested=false means any region opened while g_depth > 0 must
    // run as a team of one, inline, without touching the port at all.
    int g_depth = 0;

    int capacity(void) {
        return 1 + (g_port ? g_port->worker_count() : 0);
    }

    int min_int(int a, int b) {
        return a < b ? a : b;
    }
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
    if (g_nthreads_var == 0) {
        return capacity();
    }
    return min_int(g_nthreads_var, capacity());
}

extern "C" int omp_get_num_procs(void) {
    return capacity();
}

extern "C" void omp_set_num_threads(int n) {
    if (n >= 1) {
        g_nthreads_var = n;
    }
}

extern "C" double omp_get_wtime(void) {
    return wtime_seconds();
}

extern "C" void omp_set_port(const omp_port *port) {
    g_port = port;
}

extern "C" void GOMP_parallel(void (*fn)(void *), void *data,
                               unsigned num_threads, unsigned flags) {
    (void)flags;

    if (g_depth > 0) {
        // Nested region: default nested=false -> team of one, inline,
        // no port interaction. Save/restore the outer team state so the
        // caller sees its own team size and thread id again on return.
        int saved_team_size = g_team_size;
        g_team_size = 1;
        ++g_depth;
        fn(data);
        --g_depth;
        g_team_size = saved_team_size;
        return;
    }

    int requested = (num_threads > 0) ? (int)num_threads : omp_get_max_threads();
    int team = min_int(requested, capacity());

    ++g_depth;
    if (team >= 2) {
        g_team_size = 2;
        if (g_port->worker_start(fn, data) == 0) {
            fn(data);
            g_port->worker_join();
            g_team_size = 1;
            --g_depth;
            return;
        }
        g_team_size = 1;
        fn(data);
        --g_depth;
        return;
    }

    g_team_size = 1;
    fn(data);
    --g_depth;
}
