#pragma once
#include "common.hpp"
#include "workload.hpp"

#include <fstream>
#include <string>
#include <sys/resource.h>
#include <vector>

// One CSV row built from named columns, so headers can never drift from the values.
class Row {
public:
    Row& add(const std::string& name, double value);
    Row& add(const std::string& name, const std::string& value);
    const std::vector<std::string>& names() const { return names_; }
    const std::vector<std::string>& values() const { return values_; }

private:
    std::vector<std::string> names_, values_;
};

// Appends rows to a file; writes the header only when the file is new or empty.
class CsvFile {
public:
    explicit CsvFile(const std::string& path);
    void write(const Row& row);

private:
    std::ofstream out_;
    bool need_header_;
};

// The lab's system metrics for one measured interval: CPU %, load average, memory,
// execution time, and disk activity.
class ResourceMonitor {
public:
    struct Usage {
        double wall_s = 0, user_s = 0, sys_s = 0, cpu_pct = 0;
        double loadavg1 = 0;  // 1-minute average, sampled at stop(); lags on short runs
        long max_rss_kb = 0;  // peak resident set of the process (excludes page cache)
        long vol_ctx = 0, invol_ctx = 0, minor_faults = 0, major_faults = 0;
        unsigned long long wchar = 0;        // bytes passed to write(), i.e. into the page cache
        unsigned long long write_bytes = 0;  // bytes the kernel sent to storage for this process
        void add_to(Row& row) const;
    };

    void start();
    Usage stop() const;

private:
    Clock::time_point t0_;
    rusage ru0_{};
    unsigned long long wchar0_ = 0, write_bytes0_ = 0;
};

// Measured and predicted queue statistics for one run. Warm-up jobs are discarded.
// Theory: M/M/1 (exponential service) and M/G/1 via Pollaczek-Khinchine (any service
// distribution, using the measured E[S] and E[S^2]).
class QueueStats {
public:
    QueueStats(const std::vector<JobRecord>& records, std::size_t warmup);
    double rho() const { return rho_; }
    double mean_service() const { return mean_s_; }
    void add_to(Row& row) const;

private:
    std::size_t n_ = 0;
    double lambda_ = 0, mean_s_ = 0, es2_ = 0, cv2_ = 0, rho_ = 0, util_busy_ = 0;
    double mean_wait_ = 0, mean_resp_ = 0, little_l_ = 0, lag1_ = 0;
    double mean_compute_ = 0, mean_write_ = 0, mean_sync_ = 0;
    double wq_mm1_ = 0, wq_mg1_ = 0, l_mm1_ = 0, l_mg1_ = 0;
};
