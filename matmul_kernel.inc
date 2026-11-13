// Shared kernel source, included by BOTH simd_multiplier.cpp and scalar_multiplier.cpp.
//
// It is `static` on purpose: each translation unit gets a private copy compiled with that
// unit's own flags. If this were `inline` or lived in a header, the linker would merge the
// copies and both classes would silently run the same machine code.
//
// Loop order is i-k-j: the inner loop walks b and c contiguously and has no reduction,
// so GCC can vectorize it at -O3 without -ffast-math.
#include <cstddef>

static void ikj_kernel(const double* __restrict a, const double* __restrict b,
                       double* __restrict c, std::size_t n) {
    for (std::size_t i = 0; i < n * n; ++i) c[i] = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < n; ++k) {
            const double aik = a[i * n + k];
            for (std::size_t j = 0; j < n; ++j) {
                c[i * n + j] += aik * b[k * n + j];
            }
        }
    }
}
