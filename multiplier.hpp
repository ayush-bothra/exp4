#pragma once
#include "matrix.hpp"
#include <string>

// CPU-bound stage. Both implementations run the same source (matmul_kernel.inc);
// they differ only in the compiler flags of their translation unit.
class Multiplier {
public:
    virtual ~Multiplier() = default;
    virtual void multiply(const Matrix& a, const Matrix& b, Matrix& c) const = 0;
    virtual std::string name() const = 0;
};

// Built with the auto-vectorizer on (-O3 -march=native).
class SimdMultiplier : public Multiplier {
public:
    void multiply(const Matrix& a, const Matrix& b, Matrix& c) const override;
    std::string name() const override { return "simd"; }
};

// Built with the auto-vectorizer off (see Makefile).
class ScalarMultiplier : public Multiplier {
public:
    void multiply(const Matrix& a, const Matrix& b, Matrix& c) const override;
    std::string name() const override { return "scalar"; }
};
