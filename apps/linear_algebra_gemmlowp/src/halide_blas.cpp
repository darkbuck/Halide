#include <string.h>
#include <iostream>
#include "halide_blas.h"

#define assert_no_error(func)                                       \
  if (func != 0) {                                                  \
    std::cerr << "ERROR! Halide kernel returned non-zero value.\n"; \
  }                                                                 \

namespace {

void init_matrix_buffer(const int M, const int N, const int *A, const int lda, buffer_t *buff) {
    memset((void*)buff, 0, sizeof(buffer_t));
    buff->host = (uint8_t*)const_cast<int*>(A);
    buff->extent[0] = M;
    buff->extent[1] = N;
    buff->stride[0] = 1;
    buff->stride[1] = lda;
    buff->elem_size = sizeof(int);
}

}

#ifdef __cplusplus
extern "C" {
#endif

//////////
// gemm //
//////////

void hblas_igemm(const enum HBLAS_ORDER Order, const enum HBLAS_TRANSPOSE TransA,
                 const enum HBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const int alpha, const int *A,
                 const int lda, const int *B, const int ldb,
                 const int beta, int *C, const int ldc) {
    bool tA = false, tB = false;
    switch (TransA) {
    case HblasNoTrans:
        tA = false; break;
    case HblasConjTrans:
    case HblasTrans:
        tA = true; break;
    };

    switch (TransB) {
    case HblasNoTrans:
        tB = false; break;
    case HblasConjTrans:
    case HblasTrans:
        tB = true; break;
    };

    buffer_t buff_A, buff_B, buff_C;
    if (!tA) {
        init_matrix_buffer(M, K, A, lda, &buff_A);
    } else {
        init_matrix_buffer(K, M, A, lda, &buff_A);
    }

    if (!tB) {
        init_matrix_buffer(K, N, B, ldb, &buff_B);
    } else {
        init_matrix_buffer(N, K, B, ldb, &buff_B);
    }

    init_matrix_buffer(M, N, C, ldc, &buff_C);

    assert_no_error(halide_igemm(tA, tB, alpha, &buff_A, &buff_B, beta, &buff_C));
}


#ifdef __cplusplus
}
#endif
