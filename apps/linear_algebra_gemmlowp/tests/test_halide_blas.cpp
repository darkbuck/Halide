#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <cblas.h>
#include <halide_blas.h>
#include "Halide.h"

#define RUN_TEST(method)                                                   \
    std::cout << std::setw(30) << ("Testing " #method ": ") << std::flush; \
    if (test_##method(N)) {                                                \
        std::cout << "PASSED\n";                                           \
    }                                                                      \

#define L3_TEST(method, cblas_code, hblas_code) \
    bool test_##method(int N) {                 \
        Scalar alpha = random_scalar();         \
        Scalar beta = random_scalar();          \
        Matrix eA(random_matrix(N));            \
        Matrix eB(random_matrix(N));            \
        Matrix eC(random_matrix(N));            \
        Matrix aA(eA), aB(eB), aC(eC);          \
                                                \
        {                                       \
            Scalar *A = &(eA[0]);               \
            Scalar *B = &(eB[0]);               \
            Scalar *C = &(eC[0]);               \
            cblas_code;                         \
        }                                       \
                                                \
        {                                       \
            Scalar *A = &(aA[0]);               \
            Scalar *B = &(aB[0]);               \
            Scalar *C = &(aC[0]);               \
            hblas_code;                         \
        }                                       \
                                                \
        return compareMatrices(N, eC, aC);      \
    }

void cblas_igemm(const bool transpose_a, const bool transpose_b,
                 const int M, const int N, const int K, const int alpha,
                 const int *A, const int lda, const int *B, const int ldb,
                 const int beta, int *C,
                 const int ldc) {

    std::vector<float> A_float(M*K);
    std::vector<float> B_float(K*N);
    std::vector<float> C_float(M*N);
    for (int i=0; i<M*K; ++i) {
        A_float[i] = static_cast<float>(A[i]);
    }
    for (int i=0; i<K*N; ++i) {
        B_float[i] = static_cast<float>(B[i]);
    }

    if (transpose_a && transpose_b) {
        cblas_sgemm(CblasColMajor, CblasTrans, CblasTrans, M, N, K, alpha,
                    &(A_float[0]), lda, &(B_float[0]), ldb, beta, &(C_float[0]), ldc);
    } else if (transpose_a) {
        cblas_sgemm(CblasColMajor, CblasTrans, CblasNoTrans, M, N, K, alpha,
                    &(A_float[0]), lda, &(B_float[0]), ldb, beta, &(C_float[0]), ldc);
    } else if (transpose_b) {
        cblas_sgemm(CblasColMajor, CblasNoTrans, CblasTrans, M, N, K, alpha,
                    &(A_float[0]), lda, &(B_float[0]), ldb, beta, &(C_float[0]), ldc);
    } else {
        cblas_sgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, M, N, K, alpha,
                    &(A_float[0]), lda, &(B_float[0]), ldb, beta, &(C_float[0]), ldc);
    }

    for (int i=0; i<M*N; ++i) {
        C[i] = static_cast<int>(C_float[i]);
    }
}

struct BLASTest {
    typedef uint8_t Scalar;
    typedef std::vector<uint8_t> Vector;
    typedef std::vector<uint8_t> Matrix;

    std::random_device rand_dev;
    std::default_random_engine rand_eng;

    BLASTestBase() : rand_eng(rand_dev()) {}

    Scalar random_scalar() {
        std::uniform_int_distribution<uint8_t> uniform_dist(1, 10);
        return uniform_dist(rand_eng);
    }

    Vector random_vector(int N) {
        Vector buff(N);
        for (int i=0; i<N; ++i) {
            buff[i] = random_scalar();
        }
        return buff;
    }

    Matrix random_matrix(int N) {
        Matrix buff(N * N);
        for (int i=0; i<N*N; ++i) {
            buff[i] = random_scalar();
        }
        return buff;
    }

    bool compareScalars(Scalar x, Scalar y, Scalar epsilon = 4 * std::numeric_limits<Scalar>::epsilon()) {
        if (x == y) {
            return true;
        } else {
            const Scalar min_normal = std::numeric_limits<Scalar>::min();

            Scalar ax = std::abs(x);
            Scalar ay = std::abs(y);
            Scalar diff = std::abs(x - y);

            bool equal = false;
            if (x == 0.0 || y == 0.0 || diff < min_normal) {
                equal = diff < (epsilon * min_normal);
            } else {
                equal = diff / (ax + ay) < epsilon;
            }

            if (!equal) {
                std::cerr << "FAIL! expected = " << x << ", actual = " << y << "\n";
            }

            return equal;
        }
    }

    bool compareVectors(int N, const Vector &x, const Vector &y,
                        Scalar epsilon = 16 * std::numeric_limits<Scalar>::epsilon()) {
        bool equal = true;
        for (int i = 0; i < N; ++i) {
            if (!compareScalars(x[i], y[i], epsilon)) {
                std::cerr << "Vectors differ at index: " << i << "\n";
                equal = false;
                break;
            }
        }
        return equal;
    }

    bool compareMatrices(int N, const Matrix &A, const Matrix &B,
                         Scalar epsilon = 16 * std::numeric_limits<Scalar>::epsilon()) {
        bool equal = true;
        for (int i = 0; i < N*N; ++i) {
            if (!compareScalars(A[i], B[i], epsilon)) {
                std::cerr << "Matrices differ at coords: (" << i%N << ", " << i/N << ")\n";
                equal = false;
                break;
            }
        }
        return equal;
    }

    void run_tests(int N) {
        RUN_TEST(igemm_notrans);
        RUN_TEST(igemm_transA);
        RUN_TEST(igemm_transB);
        RUN_TEST(igemm_transAB);
    }

    L3_TEST(igemm_notrans,
            cblas_igemm(false, false, N, N, N, alpha, A, N, B, N, beta, C, N),
            hblas_igemm(HblasColMajor, HblasNoTrans, HblasNoTrans, N, N, N, alpha, A, N, B, N, beta, C, N));
    L3_TEST(igemm_transA,
            cblas_igemm(true, false, N, N, N, alpha, A, N, B, N, beta, C, N),
            hblas_igemm(HblasColMajor, HblasTrans, HblasNoTrans, N, N, N, alpha, A, N, B, N, beta, C, N));
    L3_TEST(igemm_transB,
            cblas_igemm(false, true, N, N, N, alpha, A, N, B, N, beta, C, N),
            hblas_igemm(HblasColMajor, HblasNoTrans, HblasTrans, N, N, N, alpha, A, N, B, N, beta, C, N));
    L3_TEST(igemm_transAB,
            cblas_igemm(true, true, N, N, N, alpha, A, N, B, N, beta, C, N),
            hblas_igemm(HblasColMajor, HblasTrans, HblasTrans, N, N, N, alpha, A, N, B, N, beta, C, N));
};

int main(int argc, char *argv[]) {
    BLASTest test;

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            int size = std::stoi(argv[i]);
            std::cout << "Testing halide_blas with N = " << size << ":\n";
            test.run_tests(size);
        }
    } else {
        int size = 64 * 7;
        std::cout << "Testing halide_blas with N = " << size << ":\n";
        test.run_tests(size);
    }
}
