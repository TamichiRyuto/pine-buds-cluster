// OpenMP runtime for the unmodified-benchmark strategy (adapters/omp).
//
// Stage 2 (docs/design-ibrt-transport.md §15): GCC's -fopenmp outlines
// `#pragma omp parallel for` into fn(data) plus a call to GOMP_parallel;
// this file provides that entry with the libgomp ABI, forking a team of at
// most two (primary + one worker) through the omp_port seam. Without a
// port everything is sequential (Stage 1). gnu++98, freestanding.
//
// Placement: on the target the worker is the second Cortex-M4F, which
// cannot execute from flash, so the two queries the outlined body calls
// (omp_get_num_threads / omp_get_thread_num) live in .cp_text_sram.
#include "omp.h"
#include "omp_adapter.h"

#include <time.h>

#ifdef PINEBUDS_TARGET
#include "hal_location.h"
#define OMP_CP_TEXT CP_TEXT_SRAM_LOC
#else
#define OMP_CP_TEXT
#endif

namespace {
    // Host time source, isolated so a target port can swap this for a hal
    // timer without touching the omp_get_wtime API surface below.
    double wtime_seconds(void) {
        return (double)clock() / (double)CLOCKS_PER_SEC;
    }

    const omp_port *g_port = 0;

    // Size of the innermost active team: 1 outside any region or in a
    // nested one, 2 while a forked region is running.
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

extern "C" OMP_CP_TEXT int omp_get_num_threads(void) {
    return g_team_size;
}

extern "C" OMP_CP_TEXT int omp_get_thread_num(void) {
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

    // Team size: the num_threads clause wins over nthreads-var, both capped
    // by the port capacity. A nested region (default nested=false) is
    // always a team of one and never touches the port.
    int team = 1;
    if (g_depth == 0) {
        int requested =
            (num_threads > 0) ? (int)num_threads : omp_get_max_threads();
        team = min_int(requested, capacity());
    }

    int saved_team_size = g_team_size;
    ++g_depth;

    // Publish the team before starting the worker: it may query
    // omp_get_num_threads() before the primary has run its own share.
    g_team_size = team;
    int forked = (team >= 2) && (g_port->worker_start(fn, data) == 0);
    if (!forked) {
        g_team_size = 1;  // worker refused (or team of one): primary does it all
    }
    fn(data);
    if (forked) {
        g_port->worker_join();
    }

    g_team_size = saved_team_size;
    --g_depth;
}
