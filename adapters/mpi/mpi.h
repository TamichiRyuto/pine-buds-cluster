// Minimal MPI API subset for host-side testing and target bootstrap.
// Freestanding C++98: no exceptions, no RTTI, no STL, no heap.
#ifndef PINEBUDS_ADAPTERS_MPI_H
#define PINEBUDS_ADAPTERS_MPI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int MPI_Comm;

#define MPI_SUCCESS 0
#define MPI_COMM_WORLD ((MPI_Comm)0)

int MPI_Init(int *argc, char ***argv);
int MPI_Comm_rank(MPI_Comm comm, int *rank);
int MPI_Comm_size(MPI_Comm comm, int *size);
int MPI_Finalize(void);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_H */
