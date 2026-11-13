#pragma once
#include "common.hpp"
#include "disk.hpp"
#include "matrix.hpp"
#include "multiplier.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct JobTiming {
    double compute_s = 0.0;
    double write_s = 0.0;
    double sync_s = 0.0;

    double io_s() const { return write_s + sync_s; }
    double total_s() const { return compute_s + write_s + sync_s; }

    JobTiming& operator+=(const JobTiming& o) {
        compute_s += o.compute_s;
        write_s += o.write_s;
        sync_s += o.sync_s;
        return *this;
    }
};

// One unit of work: optionally multiply, then optionally write the product to disk.
// A null Multiplier gives an I/O-only job, a null DiskWriter gives a CPU-only job.
// Pointers are non-owning; the caller keeps both objects alive.
class Pipeline {
public:
    Pipeline(const Multiplier* mult, DiskWriter* disk, std::size_t n, std::uint64_t seed);
    JobTiming run();

private:
    const Multiplier* mult_;
    DiskWriter* disk_;
    Matrix a_, b_, c_;
};

// Poisson arrivals: exponential gaps with mean 1/lambda. Times are seconds from the run start.
class ArrivalSchedule {
public:
    ArrivalSchedule(double lambda, std::size_t count, std::uint64_t seed);
    std::size_t size() const { return times_.size(); }
    double at(std::size_t i) const { return times_[i]; }

private:
    std::vector<double> times_;
};

struct JobRecord {
    double arrival = 0.0;  // scheduled arrival, not the moment the worker noticed it
    double start = 0.0;
    double finish = 0.0;
    JobTiming t;

    double wait() const { return start - arrival; }
    double service() const { return finish - start; }
    double response() const { return finish - arrival; }
};

// Single FIFO server. It replays the schedule: a job starts at max(arrival, previous finish).
// Waiting is measured from the scheduled arrival, so a slow job cannot hide the delay it causes
// to later ones.
class Server {
public:
    explicit Server(Pipeline& pipeline) : pipeline_(pipeline) {}
    std::vector<JobRecord> run(const ArrivalSchedule& schedule);

private:
    Pipeline& pipeline_;
};
