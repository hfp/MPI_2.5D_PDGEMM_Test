#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "mpi.h"
#include "mkl.h"

#define VERIFY

#include "utils.c"

int main(int argc, char **argv) 
{
    int root = 0;
    MPI_Status status; 

    MPI_Init(&argc, &argv);
    
    int my_rank, nproc, c, nproc_ij;
    get_nproc_ij_c(argc, argv, &nproc, &my_rank, &c, &nproc_ij);

    int n = get_problem_size(argc, argv, nproc_ij, my_rank);
    int n_local = n / nproc_ij;
    int local_bs = n_local * n_local;
    int ntest = 10;
    if (argc >= 4) ntest = atoi(argv[3]);
    if (ntest < 1 || ntest > 50) ntest = 10;
    if (my_rank == 0) 
    {
        printf("Test settings:\n");
        printf("  * Process grid : %d * %d * %d (c = %d)\n", nproc_ij, nproc_ij, c, c);
        printf("  * Matrix size  : Global N = %d, local N = %d\n", n, n_local);
        printf("  * Repeat tests : %d\n", ntest);
        printf("\n");
    }

    //prepare for the cartesian topology
    MPI_Comm cart_comm;
    int dims[3] = {nproc_ij, nproc_ij, c};
    int periods[3] = {1, 1, 0};
    int coords[3];
    int reorder = 0;
    MPI_Cart_create(MPI_COMM_WORLD, 3, dims, periods, reorder, &cart_comm);
    MPI_Comm_rank(cart_comm, &my_rank);
    MPI_Cart_coords(cart_comm, my_rank, 3, coords);
    
    int i, j, k, my_row, my_col, my_plane;
    i = my_row   = coords[0]; 
    j = my_col   = coords[1]; 
    k = my_plane = coords[2]; 

    // Split the cart_comm into row_comm, col_comm, dup_comm, and plane_comm
    // row_comm: P(i, *, k), col_comm: P(*, j, k), dup_comm: P(i, j, *), plane_comm: P(*, *, k)
    MPI_Comm row_comm, col_comm, dup_comm, plane_comm;
    int remain_i[3]  = {1, 0, 0};
    int remain_j[3]  = {0, 1, 0};
    int remain_k[3]  = {0, 0, 1};
    int remain_ij[3] = {1, 1, 0};
    MPI_Cart_sub(cart_comm, remain_j,  &row_comm);
    MPI_Cart_sub(cart_comm, remain_i,  &col_comm);
    MPI_Cart_sub(cart_comm, remain_k,  &dup_comm);
    MPI_Cart_sub(cart_comm, remain_ij, &plane_comm);

    int plane;
    MPI_Comm_rank(dup_comm, &plane);
    
    double *M, *N, *P;
    if (0 == my_rank) initial_matrix(&M, &N, &P, n, n, n);

    double *matbuf = (double *) mkl_malloc(6 * local_bs * sizeof(double), 16);
    double *A  = matbuf + local_bs * 0;
    double *B  = matbuf + local_bs * 1;
    double *C  = matbuf + local_bs * 2;
    double *A0 = matbuf + local_bs * 3;
    double *B0 = matbuf + local_bs * 4;
    double *C0 = matbuf + local_bs * 5;

    // Scatter and Gather used data types
    MPI_Datatype subarrtype;
    int *sendcounts = (int *) malloc(nproc_ij * nproc_ij * sizeof(int));
    int *displs = (int *) malloc(nproc_ij * nproc_ij * sizeof(int));
    init_subarrtype(root, my_rank, n, nproc_ij, n_local, &subarrtype, sendcounts, displs);

    // Distribute A & B on plane 0
    scatter_data(
        root, my_rank, my_plane, n, nproc_ij, local_bs,
        plane_comm, sendcounts, displs, subarrtype,
        A, B, M, N
    );
    MPI_Barrier(cart_comm);
    
    double st0, et0, st1, et1;
    double comm_t = 0.0, dgemm_t = 0.0, total_t = 0.0;
    
    double *sendA, *recvA, *sendB, *recvB, *tmpptr;
    
    for (int itest = 0; itest < ntest; itest++)
    {
        MPI_Barrier(MPI_COMM_WORLD);
        
        st0 = MPI_Wtime();
        
        // 1. Replicate input matrix on each plane
        st1 = MPI_Wtime();
        MPI_Bcast(A, local_bs, MPI_DOUBLE, 0, dup_comm);
        MPI_Bcast(B, local_bs, MPI_DOUBLE, 0, dup_comm);
        
        // 2. Initial circular shift on A and B
        int dst, src, shift, datatag;
        MPI_Status sta0, sta1;
        MPI_Request req0, req1, req2;
        sendA = A;  recvA = A0;
        sendB = B;  recvB = B0;
        shift = k * (nproc_ij / c);
        int dstA = (j - i + shift + c * nproc_ij) % nproc_ij;
        int dstB = (i - j + shift + c * nproc_ij) % nproc_ij;
        MPI_Isend(sendA, local_bs, MPI_DOUBLE, dstA, 0, row_comm, &req2);
        MPI_Isend(sendB, local_bs, MPI_DOUBLE, dstB, 1, col_comm, &req2);
        MPI_Irecv(recvA, local_bs, MPI_DOUBLE, MPI_ANY_SOURCE, 0, row_comm, &req0);
        MPI_Irecv(recvB, local_bs, MPI_DOUBLE, MPI_ANY_SOURCE, 1, col_comm, &req1);
        MPI_Wait(&req0, &sta0);
        MPI_Wait(&req1, &sta1);
        
        et1 = MPI_Wtime();
        comm_t += et1 - st1;
        
        // 3. Initial local DGEMM
        st1 = MPI_Wtime();
        cblas_dgemm(
            CblasRowMajor, CblasNoTrans, CblasNoTrans,
            n_local, n_local, n_local,
            1, recvA, n_local, recvB, n_local, 0, C0, n_local
        );
        et1 = MPI_Wtime();
        dgemm_t += et1 - st1;
        
        tmpptr = sendA; sendA = recvA; recvA = tmpptr;
        tmpptr = sendB; sendB = recvB; recvB = tmpptr;

        // 4. Do nproc_ij / c steps of Cannon's algorithm
        int j_dst = (j + 1) % nproc_ij;
        int i_dst = (i + 1) % nproc_ij; 
        int j_src = (j - 1 + nproc_ij) % nproc_ij;
        int i_src = (i - 1 + nproc_ij) % nproc_ij; 
        for (int stage = 1; stage < nproc_ij / c; stage++) 
        {
            datatag = stage + 1;
            
            st1 = MPI_Wtime();
            
            // (1) Send A block to the right and receive a new from the left
            MPI_Isend(sendA, local_bs, MPI_DOUBLE, j_dst, datatag, row_comm, &req2);
            MPI_Irecv(recvA, local_bs, MPI_DOUBLE, j_src, datatag, row_comm, &req0);

            // (2) Send B block to the below and receive a new from the up
            MPI_Isend(sendB, local_bs, MPI_DOUBLE, i_dst, datatag, col_comm, &req2);
            MPI_Irecv(recvB, local_bs, MPI_DOUBLE, i_src, datatag, col_comm, &req1);
            
            MPI_Wait(&req0, &sta0);
            MPI_Wait(&req1, &sta1);
            
            et1 = MPI_Wtime();
            comm_t += et1 - st1;
            
            // (3) Local DGEMM
            st1 = MPI_Wtime();
            cblas_dgemm(
                CblasRowMajor, CblasNoTrans, CblasNoTrans,
                n_local, n_local, n_local,
                1, recvA, n_local, recvB, n_local, 1, C0, n_local
            );
            et1 = MPI_Wtime();
            dgemm_t += et1 - st1;
            
            tmpptr = sendA; sendA = recvA; recvA = tmpptr;
            tmpptr = sendB; sendB = recvB; recvB = tmpptr;
        }

        // 5. Reduce sum the result to processes on plane 0
        st1 = MPI_Wtime();
        MPI_Reduce(C0, C, local_bs, MPI_DOUBLE, MPI_SUM, 0, dup_comm);
        et1 = MPI_Wtime();
        comm_t += et1 - st1;
        
        et0 = MPI_Wtime();
        total_t += et0 - st0;
        
        #ifdef VERIFY
        if (itest == 0)
        {
            gather_result(
                root, my_rank, my_plane, n, nproc_ij, local_bs,
                plane_comm, sendcounts, displs, subarrtype,
                C, M, N, P
            );
        }
        #endif
    }
    
    double GFlop = 2.0 * (double) n * (double) n * (double) n * (double) ntest / 1000000000.0;
    
    double avg_comm_t, avg_dgemm_t, avg_total_t;
    
    if (my_plane == 0) 
    {
        MPI_Reduce(&comm_t,  &avg_comm_t,  1, MPI_DOUBLE, MPI_SUM, root, plane_comm);
        MPI_Reduce(&dgemm_t, &avg_dgemm_t, 1, MPI_DOUBLE, MPI_SUM, root, plane_comm);
        MPI_Reduce(&total_t, &avg_total_t, 1, MPI_DOUBLE, MPI_SUM, root, plane_comm);
        avg_comm_t  /= (double) (nproc_ij * nproc_ij);
        avg_dgemm_t /= (double) (nproc_ij * nproc_ij);
        avg_total_t /= (double) (nproc_ij * nproc_ij);
        
        if (my_rank == root) 
        {
            printf("PDGEMM 2.5D algorithm %d runs average timing:\n", ntest);
            printf("  * Communication = %.2lf (s)\n", avg_comm_t);
            printf("  * Local DGEMM   = %.2lf (s), %.2lf GFlops\n", avg_dgemm_t, GFlop / avg_dgemm_t);
            printf("  * Overall       = %.2lf (s), %.2lf GFlops\n", avg_total_t, GFlop / avg_total_t);
        }
    }
    
    free(sendcounts);
    free(displs);
    mkl_free(matbuf);
    
    MPI_Type_free(&subarrtype);
    MPI_Comm_free(&row_comm);
    MPI_Comm_free(&col_comm);
    MPI_Comm_free(&dup_comm);
    MPI_Comm_free(&plane_comm);
    MPI_Finalize();
    
    return 0;
}