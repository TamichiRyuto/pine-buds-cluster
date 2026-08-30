// Minimal MPI API subset for host-side testing and target bootstrap.
// Freestanding C++98: no exceptions, no RTTI, no STL, no heap.
#ifndef PINEBUDS_ADAPTERS_MPI_H
#define PINEBUDS_ADAPTERS_MPI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int MPI_Comm;
typedef int MPI_Datatype;

#define MPI_SUCCESS 0
#define MPI_ERR_COUNT 2
#define MPI_ERR_OTHER 15
#define MPI_ERR_INTERN 16
#define MPI_COMM_WORLD ((MPI_Comm)0)
#define MPI_FLOAT ((MPI_Datatype)0)

typedef struct MPI_Status {
    int MPI_SOURCE;
    int MPI_TAG;
} MPI_Status;

int MPI_Init(int *argc, char ***argv);
int MPI_Comm_rank(MPI_Comm comm, int *rank);
int MPI_Comm_size(MPI_Comm comm, int *size);
int MPI_Finalize(void);
int MPI_Initialized(int *flag);
int MPI_Finalized(int *flag);
int MPI_Send(const void *buf, int count, MPI_Datatype datatype, int dest,
             int tag, MPI_Comm comm);
int MPI_Recv(void *buf, int count, MPI_Datatype datatype, int source,
             int tag, MPI_Comm comm, MPI_Status *status);
int MPI_Barrier(MPI_Comm comm);
double MPI_Wtime(void);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_H */
