// Stage-1 stub of omp.h for the unmodified-benchmark strategy: everything
// runs sequentially and pragmas are ignored. Stage 2 replaces this with a
// real GOMP runtime on cp_accel.
#ifndef PINEBUDS_ADAPTERS_OMP_H
#define PINEBUDS_ADAPTERS_OMP_H

#ifdef __cplusplus
extern "C" {
#endif

int omp_get_num_threads(void);
int omp_get_thread_num(void);
int omp_get_max_threads(void);
int omp_get_num_procs(void);
void omp_set_num_threads(int n);
double omp_get_wtime(void);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_OMP_H */
