#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <eigen_interface.h>
#include <halide_blas.h>
#include "Halide.h"

#define RUN_TEST(method)                                                   \
    std::cout << std::setw(30) << ("Testing " #method ": ") << std::flush; \
    if (test_##method(N)) {                                                \
        std::cout << "PASSED\n";                                           \
    }                                                                      \

#define L3_TEST(method, eigen_code, hblas_code)   \
    bool test_##method(int N) {                   \
        uint8_t a_offset = random_uint8_t();      \
        uint8_t b_offset = random_uint8_t();      \
        uint8_t c_offset = random_uint8_t();      \
        uint8_t c_mult_int = random_uint8_t();    \
        uint8_t c_shift = random_uint8_t();       \
        Matrix eA(random_matrix(N));              \
        Matrix eB(random_matrix(N));              \
        Matrix eC(random_matrix(N));              \
        Matrix aA(eA), aB(eB), aC(eC);            \
                                                  \
        {                                         \
            uint8_t *A = &(eA[0]);                \
            uint8_t *B = &(eB[0]);                \
            uint8_t *C = &(eC[0]);                \
            eigen_code;                           \
        }                                         \
                                                  \
        {                                         \
            uint8_t *A = &(aA[0]);                \
            uint8_t *B = &(aB[0]);                \
            uint8_t *C = &(aC[0]);                \
            hblas_code;                           \
        }                                         \
                                                  \
        return compareMatrices(N, eC, aC);        \
    }

struct BLASTest {
    typedef std::vector<uint8_t> Vector;
    typedef std::vector<uint8_t> Matrix;

    std::random_device rand_dev;
    std::default_random_engine rand_eng;

    BLASTest() : rand_eng(rand_dev()) {}

    uint8_t random_uint8_t() {
        std::uniform_int_distribution<uint8_t> uniform_dist(1, 10);
        return uniform_dist(rand_eng);
    }

    Vector random_vector(int N) {
        Vector buff(N);
        for (int i=0; i<N; ++i) {
            buff[i] = random_uint8_t();
        }
        return buff;
    }

    Matrix random_matrix(int N) {
        Matrix buff(N * N);
        for (int i=0; i<N*N; ++i) {
            buff[i] = random_uint8_t();
        }
        return buff;
    }

    bool compare_scalar(uint8_t x, uint8_t y) {
        if (x == y) {
            return true;
        } else {
            std::cerr << "FAIL! expected = " << x << ", actual = " << y << "\n";
            return false;
        }
    }

    bool compareVectors(int N, const Vector &x, const Vector &y) {
        bool equal = true;
        for (int i = 0; i < N; ++i) {
            if (!compare_scalar(x[i], y[i])) {
                std::cerr << "Vectors differ at index: " << i << "\n";
                equal = false;
                break;
            }
        }
        return equal;
    }

    bool compareMatrices(int N, const Matrix &A, const Matrix &B) {
        printf("Matrix A\n");
        for (int i = 0; i < N*N; ++i) {
            if (i == N) {
                printf("\n");
            }
            printf("%d  ", A[i]);
        }

        printf("\nMatrix B\n");
        for (int i = 0; i < N*N; ++i) {
            if (i == N) {
                printf("\n");
            }
            printf("%d  ", B[i]);
        }

        bool equal = true;
        for (int i = 0; i < N*N; ++i) {
            if (!compare_scalar(A[i], B[i])) {
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
        RUN_TEST(igemm_transC);
        RUN_TEST(igemm_transAC);
        RUN_TEST(igemm_transBC);
        RUN_TEST(igemm_transABC);
    }

    L3_TEST(igemm_notrans,
            eigen::eigen_igemm(false, false, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(false, false, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transA,
            eigen::eigen_igemm(true, false, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(true, false, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transB,
            eigen::eigen_igemm(false, true, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(false, true, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transAB,
            eigen::eigen_igemm(true, true, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(true, true, false, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transC,
            eigen::eigen_igemm(false, false, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(false, false, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transAC,
            eigen::eigen_igemm(true, false, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(true, false, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transBC,
            eigen::eigen_igemm(false, true, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(false, true, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
    L3_TEST(igemm_transABC,
            eigen::eigen_igemm(true, true, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N),
            hblas_igemm(true, true, true, N, N, N, A, a_offset, N, B, b_offset, N, C, c_offset, c_mult_int, c_shift, N));
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
        int size = 32;
        std::cout << "Testing halide_blas with N = " << size << ":\n";
        test.run_tests(size);
    }
}
