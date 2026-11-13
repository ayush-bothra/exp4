#include "measure.hpp"

#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

double tv_s(const timeval& t) { return static_cast<double>(t.tv_sec) + t.tv_usec * 1e-6; }

std::string num(double v) {
    std::ostringstream s;
    s << std::setprecision(10) << v;
    return s.str();
}

void read_io_counters(unsigned long long& wchar, unsigned long long& write_bytes) {
    wchar = write_bytes = 0;
    std::ifstream f("/proc/self/io");
    std::string key;
    unsigned long long v;
    while (f >> key >> v) {
        if (key == "wchar:") wchar = v;
        else if (key == "write_bytes:") write_bytes = v;
    }
}

double read_loadavg1() {
    std::ifstream f("/proc/loadavg");
    double v = 0;
    f >> v;
    return v;
}

void emit(std::ofstream& out, const std::vector<std::string>& cells) {
    for (std::size_t i = 0; i < cells.size(); ++i) out << (i ? "," : "") << cells[i];
    out << '\n';
}

}  // namespace

// ---- Row / CsvFile ----------------------------------------------------------------------

Row& Row::add(const std::string& name, double value) { return add(name, num(value)); }

Row& Row::add(const std::string& name, const std::string& value) {
    names_.push_back(name);
    values_.push_back(value);
    return *this;
}

CsvFile::CsvFile(const std::string& path) {
    std::error_code ec;
    need_header_ = !(std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0);
    out_.open(path, std::ios::app);
    if (!out_) throw std::runtime_error("cannot open " + path);
}

void CsvFile::write(const Row& row) {
    if (need_header_) {
        emit(out_, row.names());
        need_header_ = false;
    }
    emit(out_, row.values());
    out_.flush();
}

// ---- ResourceMonitor --------------------------------------------------------------------

void ResourceMonitor::start() {
    getrusage(RUSAGE_SELF, &ru0_);
    read_io_counters(wchar0_, write_bytes0_);
    t0_ = Clock::now();
}

ResourceMonitor::Usage ResourceMonitor::stop() const {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    unsigned long long wchar, write_bytes;
    read_io_counters(wchar, write_bytes);

    Usage u;
    u.wall_s = seconds(Clock::now() - t0_);
    u.user_s = tv_s(ru.ru_utime) - tv_s(ru0_.ru_utime);
    u.sys_s = tv_s(ru.ru_stime) - tv_s(ru0_.ru_stime);
    u.cpu_pct = u.wall_s > 0 ? 100.0 * (u.user_s + u.sys_s) / u.wall_s : 0.0;
    u.loadavg1 = read_loadavg1();
    u.max_rss_kb = ru.ru_maxrss;
    u.vol_ctx = ru.ru_nvcsw - ru0_.ru_nvcsw;
    u.invol_ctx = ru.ru_nivcsw - ru0_.ru_nivcsw;
    u.minor_faults = ru.ru_minflt - ru0_.ru_minflt;
    u.major_faults = ru.ru_majflt - ru0_.ru_majflt;
    u.wchar = wchar - wchar0_;
    u.write_bytes = write_bytes - write_bytes0_;
    return u;
}

void ResourceMonitor::Usage::add_to(Row& row) const {
    row.add("wall_s", wall_s)
        .add("user_s", user_s)
        .add("sys_s", sys_s)
        .add("cpu_pct", cpu_pct)
        .add("loadavg1", loadavg1)
        .add("max_rss_kb", static_cast<double>(max_rss_kb))
        .add("vol_ctx", static_cast<double>(vol_ctx))
        .add("invol_ctx", static_cast<double>(invol_ctx))
        .add("minor_faults", static_cast<double>(minor_faults))
        .add("major_faults", static_cast<double>(major_faults))
        .add("wchar", static_cast<double>(wchar))
        .add("write_bytes", static_cast<double>(write_bytes));
}

// ---- QueueStats -------------------------------------------------------------------------

QueueStats::QueueStats(const std::vector<JobRecord>& records, std::size_t warmup) {
    if (records.size() < warmup + 3) throw std::invalid_argument("too few jobs for statistics");
    const auto first = records.begin() + static_cast<std::ptrdiff_t>(warmup);
    const std::vector<JobRecord> kept(first, records.end());
    n_ = kept.size();
    const double n = static_cast<double>(n_);

    double sum_s = 0, sum_s2 = 0, sum_w = 0, sum_t = 0, sum_c = 0, sum_wr = 0, sum_sy = 0;
    for (const JobRecord& r : kept) {
        const double s = r.service();
        sum_s += s;
        sum_s2 += s * s;
        sum_w += r.wait();
        sum_t += r.response();
        sum_c += r.t.compute_s;
        sum_wr += r.t.write_s;
        sum_sy += r.t.sync_s;
    }
    mean_s_ = sum_s / n;
    es2_ = sum_s2 / n;
    cv2_ = (es2_ - mean_s_ * mean_s_) / (mean_s_ * mean_s_);
    mean_wait_ = sum_w / n;
    mean_resp_ = sum_t / n;
    mean_compute_ = sum_c / n;
    mean_write_ = sum_wr / n;
    mean_sync_ = sum_sy / n;

    // Realized arrival rate and utilization, from what actually happened in this run.
    lambda_ = (n - 1.0) / (kept.back().arrival - kept.front().arrival);
    rho_ = lambda_ * mean_s_;
    util_busy_ = sum_s / (kept.back().finish - kept.front().arrival);
    little_l_ = lambda_ * mean_resp_;

    // Lag-1 autocorrelation of service times. Both models assume independent service times.
    double num_ac = 0, den_ac = 0;
    for (std::size_t i = 0; i < n_; ++i) {
        const double d = kept[i].service() - mean_s_;
        den_ac += d * d;
        if (i + 1 < n_) num_ac += d * (kept[i + 1].service() - mean_s_);
    }
    lag1_ = den_ac > 0 ? num_ac / den_ac : 0.0;

    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (rho_ < 1.0) {
        wq_mm1_ = rho_ * mean_s_ / (1.0 - rho_);
        wq_mg1_ = lambda_ * es2_ / (2.0 * (1.0 - rho_));
        l_mm1_ = rho_ / (1.0 - rho_);
        l_mg1_ = lambda_ * (wq_mg1_ + mean_s_);
    } else {
        wq_mm1_ = wq_mg1_ = l_mm1_ = l_mg1_ = nan;  // unstable queue: no steady state
    }
}

void QueueStats::add_to(Row& row) const {
    row.add("jobs", static_cast<double>(n_))
        .add("lambda", lambda_)
        .add("rho", rho_)
        .add("util_busy", util_busy_)
        .add("mean_S", mean_s_)
        .add("E_S2", es2_)
        .add("cv2_S", cv2_)
        .add("s_lag1", lag1_)
        .add("mean_compute_s", mean_compute_)
        .add("mean_write_s", mean_write_)
        .add("mean_sync_s", mean_sync_)
        .add("wq_meas", mean_wait_)
        .add("wq_mm1", wq_mm1_)
        .add("wq_mg1", wq_mg1_)
        .add("resp_meas", mean_resp_)
        .add("L_meas", little_l_)
        .add("L_mm1", l_mm1_)
        .add("L_mg1", l_mg1_);
}
