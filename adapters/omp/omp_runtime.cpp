// OpenMP runtime for the unmodified-benchmark strategy (adapters/omp).
#include "omp.h"

#include <time.h>

namespace {
    // Host time source, isolated so a target port can swap this for a hal
    // timer without touching the omp_get_wtime API surface below.
    double wtime_seconds(void) {
        return (double)clock() / (double)CLOCKS_PER_SEC;
    }
}

extern "C" int omp_get_num_threads(void) {
    return 1;
}

extern "C" int omp_get_thread_num(void) {
    return 0;
}

extern "C" int omp_get_max_threads(void) {
    return 1;
}

extern "C" int omp_get_num_procs(void) {
    return 1;
}

extern "C" void omp_set_num_threads(int n) {
    (void)n;
}

extern "C" double omp_get_wtime(void) {
    return wtime_seconds();
}
