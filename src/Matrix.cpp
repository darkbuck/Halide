#include "Matrix.h"

namespace Halide {

bool is_int(Expr i) {
    return i.type().is_int() || i.type().is_uint());
}

bool is_positive_int(Expr i) {
    return Internal::is_positive_const(i) && is_int(i);
}

MatrixRef::MatrixRef(Matrix M, Expr i, Expr j) : mat(M), row(i), col(j) {
    internal_assert(row.defined() && is_int(row));
    internal_assert(col.defined() && is_int(col));
}

void MatrixRef::operator=(Expr x) {
    if (mat.is_large) {
        mat.func(row, col) = x;
    } else {
        const int i = mat.small_offset(row, col);
        mat.coeffs[i] = expr;
    }
}

void MatrixRef::operator+=(Expr x) {
    if (mat.is_large) {
        mat.func(row, col) += x;
    } else {
        const int i = mat.small_offset(row, col);
        mat.coeffs[i] = mat.coeffs[i] + expr;
    }
}

void MatrixRef::operator-=(Expr x) {
    if (mat.is_large) {
        mat.func(row, col) -= x;
    } else {
        const int i = mat.small_offset(row, col);
        mat.coeffs[i] = mat.coeffs[i] - expr;
    }
}

void MatrixRef::operator*=(Expr x) {
    if (mat.is_large) {
        mat.func(row, col) *= x;
    } else {
        const int i = mat.small_offset(row, col);
        mat.coeffs[i] = mat.coeffs[i] * expr;
    }
}

void MatrixRef::operator/=(Expr x) {
    if (mat.is_large) {
        mat.func(row, col) -= x;
    } else {
        const int i = mat.small_offset(row, col);
        mat.coeffs[i] = mat.coeffs[i] - expr;
    }
}

void MatrixRef::operator=(const FuncRefVar &e) {
    internal_assert(e.size() == 1);
    (*this) = Expr(e);
}

void MatrixRef::operator=(const FuncRefExpr &e) {
    internal_assert(e.size() == 1);
    (*this) = Expr(e);
}

MatrixRef::operator Expr() const {
    if (mat.is_large) {
        return mat.func(row, col);
    } else {
        const int i = mat.small_offset(row, col);
        return mat.coeffs[i];
    }
}

int Matrix::small_offset(Expr row, Expr col) {
    if (!is_large) {
        internal_assert(is_positive_int(i));
        internal_assert(is_positive_int(j));
        internal_assert(is_positive_int(nrows));
        internal_assert(is_positive_int(ncols));

        const int i = *as_const_int(row);
        const int j = *as_const_int(col);
        const int m = *as_const_int(nrows);

        return i + j * m;
    }

    return -1;
}

Matrix::Matrix() : is_large(false), nrows(0), ncols(0) {}

Matrix::Matrix(Expr m, Expr n, Type t) : is_large(true), nrows(m), ncols(n) {
    internal_assert(nrows.defined() && is_int(nrows));
    internal_assert(ncols.defined() && is_int(ncols));

    if (is_positive_int(nrows) && is_positive_int(ncols)) {
        const int nr = *Internal::as_const_int(nrows);
        const int nc = *Internal::as_const_int(ncols);

        if (nr <= 4 && nc <= 4) {
            is_large = false;
            coeffs.resize(nr * nc, Halide::undef(t));
            return;
        }
    }

    x = Var("x");
    y = Var("y");

    func(x, y) = Halide::undef(t);
    func.bound(x, 0, nrows)
                .bound(y, 0, ncols);
}

Matrix::Matrix(Expr m, Expr n, const std::vector<Expr>& c) : is_large(false), nrows(m), ncols(n) {
    internal_assert(is_positive_int(nrows));
    internal_assert(is_positive_int(ncols));

    const int nr = *Internal::as_const_int(nrows);
    const int nc = *Internal::as_const_int(ncols);

    internal_assert(nr <= 4 && nc <= 4);
    internal_assert(nr * nc == c.size());
    Type t = c[0].type();
    coeffs.resize(nr * nc, Halide::undef(t));
    for (int i = 0; i < c.size(); ++i) {
        internal_assert(c[i].type() = t);
        coeffs[i] = c[i];
    }
}

Matrix::Matrix(Expr m, Expr n, Func f) : is_large(true), nrows(m), ncols(n) {
    internal_assert(is_int(nrows));
    internal_assert(is_int(ncols));
    internal_assert(f.outputs() == 1);

    if (f.dimensions() == 1) {
        internal_assert(is_one(ncols) || is_one(nrows));

        if (is_one(ncols)) {
            if (is_positive_int(nrows)) {
                const int nr = *Internal::as_const_int(nrows);

                if (nr <= 4) {
                    is_large = false;
                    coeffs.resize(nr);

                    for (int i = 0; i < nr; ++i) {
                        coeffs[i] = f(i);
                    }

                    return;
                }
            }

            x = f.args()[0];
            y = Var("y");
            func(x, y) = Halide::undef(f.output_types()[0]);
            func(x, 0) = f(x);
            func.bound(x, 0, nrows)
                .bound(y, 0, 1);
        } else {  // is_one(nrows)
            if (is_positive_int(ncols)) {
                const int nc = *Internal::as_const_int(ncols);

                if (nc <= 4) {
                    is_large = false;
                    coeffs.resize(nc);

                    for (int i = 0; i < nc; ++i) {
                        coeffs[i] = f(i);
                    }

                    return;
                }
            }

            x = Var("y");
            y = f.args()[0];
            func(x, y) = Halide::undef(f.output_types()[0]);
            func(0, y) = f(y);
            func.bound(x, 0, 1)
                .bound(y, 0, ncols);
        }
    } else {
        internal_assert(f.dimensions() == 2);

        if (is_positive_int(nrows) && is_positive_int(ncols)) {
            const int nr = *Internal::as_const_int(nrows);
            const int nc = *Internal::as_const_int(ncols);

            if (nr <= 4 && nc <= 4) {
                is_large = false;
                coeffs.resize(nr*nc);

                for (int j = 0; j < nc; ++j) {
                    for (int i = 0; i < nr; ++i) {
                        const int idx = small_offset(i, j);
                        coeffs[idx] = f(i, j);
                    }
                }

                return;
            }
        }

        x = f.args()[0];
        y = f.args()[1];
        func = f;
        func.bound(x, 0, nrows)
            .bound(y, 0, ncols);
    }
}

Type Matrix::type() const {
    if (is_large) {
        return func.output_types()[0];
    } else {
        return coeffs[0].type();
    }
}

Expr Matrix::num_rows() const {
    return nrows;
}

Expr Matrix::num_cols() const {
    return ncols;
}

Matrix Matrix::row(Expr i) const {
    if (is_positive_int(ncols)) {
        const int n = *Internal::as_const_int(ncols);
        if (n <= 4) {
            std::vector<Expr> row_coeffs(n);
            for (int j = 0; j < n; ++j) {
                row_coeffs[j] = (*this)(i, j);
                return Matrix(1, ncols, row_coeffs);
            }
        }
    }

    Func row_func("matrix_row");
    row_func(y) = func(i, y);
    return Matrix(1, ncols, row_func);
}

Matrix Matrix::col(Expr j) const {
    if (is_positive_int(nrows)) {
        const int m = *Internal::as_const_int(nrows);
        if (m <= 4) {
            std::vector<Expr> col_coeffs(n);
            for (int i = 0; i < m; ++i) {
                col_coeffs[i] = (*this)(i, j);
                return Matrix(nrows, 1, col_coeffs);
            }
        }
    }

    Func col_func("matrix_col");
    col_func(x) = func(x, j);
    return Matrix(nrows, 1, col_func);
}

Matrix Matrix::block(Expr min_i, Expr max_i, Expr min_j, Expr max_j) const {
    Expr block_nrows = simplify(max_i - min_i + 1);
    Expr block_ncols = simplify(max_j - min_j + 1);

    if (is_positive_int(block_nrows) && is_positive_int(block_ncols)) {
        const int m = *Internal::as_const_int(block_nrows);
        const int n = *Internal::as_const_int(block_ncols);

        if (m <= 4 && n <= 4) {
            std::vector<Expr> block_coeffs(m * n);
            for (int j = 0; j < n; ++n) {
                for (int i = 0; i < m; ++i) {
                    const int idx = i + j * m;
                    block_coeffs[idx] = (*this)(i, j);
                    return Matrix(m, n, block_coeffs);
                }
            }
        }
    }

    Func block_func("matrix_block");
    block_func(x, y) = Halide::select(min_i <= x && x <= max_i &&
                                      min_j <= y && y <= max_j, func(x, y),
                                      Halide::undef(func.output_types()[0]));
    block_func
            .bound(x, min_i, block_nrows)
            .bound(y, min_j, block_ncols);
}

Matrix Matrix::transpose() const {
    if (is_large) {
        Func mat_trans("matrix_trans");
        mat_trans(x, y) = func(y, x);
        return Matrix(ncols, nrows, mat_trans);
    } else {
        std::vector<Expr> coeff_trans(nrows * ncols);
        for (int j = 0; j < ncols; ++j) {
            for (int i = 0; i < nrows; ++i) {
                const int idx = small_offset(i, j);
                const int idx_t = small_offset(j, i);
                coeff_trans[idx_t] = coeffs[idx];
            }
        }
        return Matrix(ncols, nrows, coeff_trans);
    }
}

MatrixRef Matrix::operator[] (Expr i) {
    internal_assert(is_one(nrows) || is_one(ncols));

    if (is_one(nrows)) {
        return MatrixRef(*this, 0, i);
    } else /*if (is_one(ncols))*/ {
        return MatrixRef(*this, i, 0);
    }
}

MatrixRef Matrix::operator() (Expr i, Expr j) {
    return MatrixRef(*this, i, j);
}

Matrix operator+(Matrix a, Matrix b) {
    internal_assert(a.num_rows() == b.num_rows());
    internal_assert(a.num_cols() == b.num_cols());

    if (a.is_large) {
        Var x("x"), y("y");

        Func sum("matrix_sum");
        sum(x, y) = a.func(x, y) + b.func(x, y);
        return Matrix(a.nrows, a.ncols, sum);
    } else {
        std::vector<Expr> sum(a.coeffs);
        for (int i = 0; i < sum.size(); ++i) {
            sum[i] += b.coeffs[i];
        }

        return Matrix(a.nrows, a.ncols, sum);
    }
}

Matrix operator-(Matrix a, Matrix b) {
    internal_assert(a.num_rows() == b.num_rows());
    internal_assert(a.num_cols() == b.num_cols());

    if (a.is_large) {
        Var x("x"), y("y");

        Func diff("matrix_diff");
        diff(x, y) = a.func(x, y) - b.func(x, y);
        return Matrix(a.nrows, a.ncols, diff);
    } else {
        std::vector<Expr> diff(a.coeffs);
        for (int i = 0; i < diff.size(); ++i) {
            diff[i] -= b.coeffs[i];
        }

        return Matrix(a.nrows, a.ncols, diff);
    }
}

Matrix operator*(Expr a, Matrix b) {
    if (b.is_large) {
        Var x("x"), y("y");

        Func scale("matrix_scale");
        scale(x, y) = a * b.func(x, y);
        return Matrix(b.nrows, b.ncols, scale);
    } else {
        std::vector<Expr> scale(b.coeffs());
        for (int i = 0; i < scale.size(); ++i) {
            scale[i] *= a;
        }

        return Matrix(a.nrows, a.ncols, scale);
    }
}

Matrix operator*(Matrix b, Expr a) {
    if (b.is_large) {
        Var x("x"), y("y");

        Func scale("matrix_scale");
        scale(x, y) = a * b.func(x, y);
        return Matrix(b.nrows, b.ncols, scale);
    } else {
        std::vector<Expr> scale(b.coeffs());
        for (int i = 0; i < scale.size(); ++i) {
            scale[i] *= a;
        }

        return Matrix(a.nrows, a.ncols, scale);
    }
}

Matrix operator*(Matrix a, Matrix b) {
    internal_assert(a.num_cols() == b.num_rows());

    Expr prod_nrows = a.num_rows();
    Expr prod_ncols = a.num_cols();

    if (is_positive_const(prod_nrows) && is_positive_const(prod_ncols)) {
        const int m = *as_const_int(prod_nrows);
        const int n = *as_const_int(prod_ncols);

        if (m <= 4 && n <= 4) {
            // Product will be a small matrix.
            std::vector<Expr> prod(m * n);

            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < m; ++i) {
                    const int idx = i + j * m;
                    if (a.is_large) {
                        RDom k(0, a.num_rows(), "k");
                        prod[idx] = sum(a.func(i, k) * b.func(k, j));
                    } else {
                        const int p = *as_const_int(a.ncols);
                        prod[idx] = cast(type(), 0);
                        for (int k = 0; k < p; ++k) {
                            prod[idx] += a(i, k) * b(k, j);
                        }
                    }
                }
            }

            return Matrix(prod_nrows, prod_ncols, prod);
        }
    }

    Func prod("matrix_prod");
    RDom z(0, a.nrows, "z");
    prod(x, y) = sum(a.func(x, z) * b.func(z, y));
    return Matrix(prod_nrows, prod_ncols, prod);
}


}
