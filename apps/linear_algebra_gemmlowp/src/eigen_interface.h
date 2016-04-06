#ifndef EIGEN_INTERFACE_H
#define EIGEN_INTERFACE_H

#include <Eigen/Eigen>

// Gemmlowp interface for Eigen

namespace eigen {

namespace {

// Default storage ordering is column-major.
void init_matrix_buffer(const int M, const int N, const int *A, const int lda, buffer_t *buff) {
    memset((void*)buff, 0, sizeof(buffer_t));
    Map<MatrixXi>(data, 2, 2);
}

} // anonymous namespace

typedef Eigen::Matrix<uint8_t, Eigen::Dynamic, 1> EigenVector;
typedef Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> EigenMatrix;
typedef Eigen::Matrix<int32_t, Eigen::Dynamic, Eigen::Dynamic> EigenMatrix32i;

inline int eigen_igemm(bool transA, bool transB, bool transC, const EigenMatrix &A,
                       int32_t a_offset, const EigenMatrix &B, int32_t b_offset,
                       const EigenMatrix &C, int32_t c_offset, int32_t c_mult_int,
                       int32_t c_shift) {
    EigenMatrix32i A_int = A.cast<int32_t>();
    EigenMatrix32i B_int = B.cast<int32_t>();
    EigenMatrix32i C_int = C.cast<int32_t>();
    if (transpose_a) {
        A_int = A_int.transpose();
    }
    if (transpose_b) {
        B_int = B_int.transpose();
    }
    C_int = (A_int + a_offset) * (B_int + b_offset) + c_offset;
    C_int *= c_mult_int;

    for (int y = 0; y < C_int.cols(); ++y) {
        for (int x = 0; x < C_int.rows(); ++x) {
            C_int(x, y) = C_int(x, y) >> c_shift;
        }
    }

    C = C_int.cast<uint8_t>();

    if (transpose_c) {
        C = C.transpose();
    }

    return 0;
}

void eigen_igemm(bool transpose_a, bool transpose_b, bool transpose_c,
                 int M, int N, int K, const uint8_t *A,
                 int32_t a_offset, int lda, const uint8_t *B,
                 int32_t b_offset, int ldb, uint8_t *C,
                 int32_t c_offset, int32_t c_mult_int,
                 int32_t c_shift, int ldc) {
    EigenMatrix matrix_A, matrix_B, matrix_C;
    if (!transpose_A) {
        init_matrix_buffer(M, K, A, lda, &matrix_A);
    } else {
        init_matrix_buffer(K, M, A, lda, &matrix_A);
    }

    if (!transpose_B) {
        init_matrix_buffer(K, N, B, ldb, &matrix_B);
    } else {
        init_matrix_buffer(N, K, B, ldb, &matrix_B);
    }

    if (!transpose_C) {
        init_matrix_buffer(M, N, C, ldc, &matrix_C);
    } else {
        init_matrix_buffer(N, M, C, ldc, &matrix_C);
    }

    eigen_igemm(transpose_A, transpose_B, transpose_c, &matrix_A,
                a_offset, &matrix_B, b_offset, &matrix_C, c_offset,
                c_mult_int, c_shift));
}




} // namespace eigen

#endif  // EIGEN_INTERFACE_H
