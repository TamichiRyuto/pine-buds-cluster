// Bootstrap seam for the OpenMP runtime. Not part of the OpenMP API surface.
#ifndef PINEBUDS_ADAPTERS_OMP_ADAPTER_H
#define PINEBUDS_ADAPTERS_OMP_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Worker port seam (design doc docs/design-ibrt-transport.md §15.2 item 2). NULL = sequential. */
typedef struct omp_port {
    int  (*worker_count)(void);                          /* extra workers available: 0 or 1 */
    int  (*worker_start)(void (*fn)(void *), void *data); /* run fn(data) on the worker; 0 on success */
    void (*worker_join)(void);                           /* block until the worker finished fn */
    int  (*self_is_worker)(void);                        /* 1 when the caller runs on the worker */
} omp_port;

void omp_set_port(const omp_port *port);

/* libgomp-ABI entry GCC emits for `#pragma omp parallel` (-fopenmp). */
void GOMP_parallel(void (*fn)(void *), void *data, unsigned num_threads, unsigned flags);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_OMP_ADAPTER_H */
