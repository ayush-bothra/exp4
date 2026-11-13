#include "workload.hpp"

#include <random>
#include <thread>

Pipeline::Pipeline(const Multiplier* mult, DiskWriter* disk, std::size_t n, std::uint64_t seed)
    : mult_(mult), disk_(disk), a_(n), b_(n), c_(n) {
    std::mt19937_64 rng(seed);
    a_.fill_random(rng);
    b_.fill_random(rng);
    c_.fill_random(rng);  // gives an I/O-only job real data to write
}

JobTiming Pipeline::run() {
    JobTiming t;
    if (mult_ != nullptr) {
        const auto t0 = Clock::now();
        mult_->multiply(a_, b_, c_);
        t.compute_s = seconds(Clock::now() - t0);
    }
    if (disk_ != nullptr) {
        const DiskWriter::Timing w = disk_->write(c_.data(), c_.bytes());
        t.write_s = w.write_s;
        t.sync_s = w.sync_s;
    }
    return t;
}

ArrivalSchedule::ArrivalSchedule(double lambda, std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::exponential_distribution<double> gap(lambda);
    times_.reserve(count);
    double t = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        t += gap(rng);
        times_.push_back(t);
    }
}

std::vector<JobRecord> Server::run(const ArrivalSchedule& schedule) {
    std::vector<JobRecord> records;
    records.reserve(schedule.size());

    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const auto due = t0 + std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<double>(schedule.at(i)));
        if (Clock::now() < due) std::this_thread::sleep_until(due);  // idle: queue is empty

        JobRecord r;
        r.arrival = schedule.at(i);
        r.start = seconds(Clock::now() - t0);
        r.t = pipeline_.run();
        r.finish = seconds(Clock::now() - t0);
        records.push_back(r);
    }
    return records;
}
