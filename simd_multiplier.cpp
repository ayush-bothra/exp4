#include "multiplier.hpp"
#include "matmul_kernel.inc"

void SimdMultiplier::multiply(const Matrix& a, const Matrix& b, Matrix& c) const {
    ikj_kernel(a.data(), b.data(), c.data(), a.size());
}
