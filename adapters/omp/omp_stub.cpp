// Stage-1 OpenMP stub: sequential execution, single implicit thread.
#include "omp.h"

extern "C" int omp_get_num_threads(void) {
    return 1;
}

extern "C" int omp_get_thread_num(void) {
    return 0;
}
