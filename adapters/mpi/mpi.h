// Minimal MPI API subset for host-side testing and target bootstrap.
// Freestanding C++98: no exceptions, no RTTI, no STL, no heap.
#ifndef PINEBUDS_ADAPTERS_MPI_H
#define PINEBUDS_ADAPTERS_MPI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;

#define MPI_SUCCESS 0
#define MPI_ERR_COUNT 2
#define MPI_ERR_OTHER 15
#define MPI_ERR_INTERN 16
#define MPI_COMM_WORLD ((MPI_Comm)0)
#define MPI_FLOAT ((MPI_Datatype)0)
#define MPI_SUM ((MPI_Op)0)

typedef struct MPI_Status {
    int MPI_SOURCE;
    int MPI_TAG;
} MPI_Status;

typedef int MPI_Request;
#define MPI_REQUEST_NULL ((MPI_Request)0)

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
int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                   MPI_Datatype datatype, MPI_Op op, MPI_Comm comm);
int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest,
              int tag, MPI_Comm comm, MPI_Request *request);
int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source,
              int tag, MPI_Comm comm, MPI_Request *request);
int MPI_Wait(MPI_Request *request, MPI_Status *status);
int MPI_Waitall(int count, MPI_Request *requests, MPI_Status *statuses);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_H */
