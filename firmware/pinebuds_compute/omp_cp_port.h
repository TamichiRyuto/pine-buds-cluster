// OpenMP worker port on the second Cortex-M4F (cp_accel), design doc §15.2
// item 3. Target-only (like mpi_ibrt_glue); host tests cover the runtime
// through tests/omp_thread_port.h instead.
#ifndef OMP_CP_PORT_H
#define OMP_CP_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Opens CP_TASK_HW on the CP (once, kept open) and installs the omp port.
// Returns 0 on success; on failure the runtime stays sequential.
int omp_cp_port_init(void);

// Worker invocations that actually ran on the CP (self_is_worker() == 1
// at the time fn was called). Device-check evidence, §15.4.
unsigned omp_cp_port_worker_runs(void);

// Fast-timer ticks of the last forked region, measured on the MCU:
// primary_share = event posted -> primary finished its own rows;
// extra_wait_for_cp = how much longer join waited for the CP after that.
void omp_cp_port_last_ticks(unsigned int *primary_share,
                            unsigned int *extra_wait_for_cp);

#ifdef __cplusplus
}
#endif

#endif  // OMP_CP_PORT_H
