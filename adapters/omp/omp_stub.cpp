// Stage-1 OpenMP stub: sequential execution, single implicit thread.
#include "omp.h"

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
