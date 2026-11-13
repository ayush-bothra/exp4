#pragma once
#include <cstddef>
#include <random>
#include <vector>

// Square matrix of doubles, row-major. The product matrix doubles as the job's output buffer.
class Matrix {
public:
    explicit Matrix(std::size_t n) : n_(n), data_(n * n, 0.0) {}

    std::size_t size() const { return n_; }
    std::size_t bytes() const { return data_.size() * sizeof(double); }
    double* data() { return data_.data(); }
    const double* data() const { return data_.data(); }

    void fill_random(std::mt19937_64& rng) {
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        for (double& x : data_) x = dist(rng);
    }

private:
    std::size_t n_;
    std::vector<double> data_;
};
