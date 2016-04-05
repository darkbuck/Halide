#ifndef HALIDE_BLAS_H
#define HALIDE_BLAS_H

#include <cmath>

#include "HalideRuntime.h"
#include "halide_igemm_notrans.h"
#include "halide_igemm_transA.h"
#include "halide_igemm_transB.h"
#include "halide_igemm_transAB.h"

inline int halide_igemm(bool transA, bool transB, int a, buffer_t *A, buffer_t *B, int b, buffer_t *C) {
    if (transA && transB) {
        return halide_igemm_transAB(a, A, B, b, C, C);
    } else if (transA) {
        return halide_igemm_transA(a, A, B, b, C, C);
    } else if (transB) {
        return halide_igemm_transB(a, A, B, b, C, C);
    } else {
        return halide_igemm_notrans(a, A, B, b, C, C);
    }
    return -1;
}

enum HBLAS_ORDER {HblasRowMajor=101, HblasColMajor=102};
enum HBLAS_TRANSPOSE {HblasNoTrans=111, HblasTrans=112, HblasConjTrans=113};
enum HBLAS_UPLO {HblasUpper=121, HblasLower=122};
enum HBLAS_DIAG {HblasNonUnit=131, HblasUnit=132};
enum HBLAS_SIDE {HblasLeft=141, HblasRight=142};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ===========================================================================
 * Prototypes for level 3 BLAS
 * ===========================================================================
 */

/*
 * Routines with standard 4 prefixes (S, D, C, Z)
 */
void hblas_igemm(const enum HBLAS_ORDER Order, const enum HBLAS_TRANSPOSE TransA,
                 const enum HBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const int alpha, const int *A,
                 const int lda, const int *B, const int ldb,
                 const int beta, int *C, const int ldc);

#ifdef __cplusplus
}
#endif

#endif  // HALIDE_BLAS_H
