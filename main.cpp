// Workload characterization + queueing experiment. See Makefile for build, analyze.py for plots.
//
//   ./workload n=256 jobs=2000 rhos=0.3,0.5,0.7,0.8,0.9 file=/path/on/real/disk/workload.dat
//
// Phase A (characterize): each workload run back-to-back, which saturates one resource and
//   gives the lab's metric table.
// Phase B (queue_sweep): the four CPU x disk-pattern pipelines driven by Poisson arrivals at
//   several utilizations, compared against M/M/1 and M/G/1 predictions.

#include "common.hpp"
#include "disk.hpp"
#include "measure.hpp"
#include "multiplier.hpp"
#include "workload.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

enum class Pattern { Sequential, Random };

struct Options {
    std::size_t n = 256;           // matrix size; must be a multiple of 64 (page-aligned slots)
    std::size_t jobs = 2000;       // jobs kept per queue run
    std::size_t warmup = 200;      // jobs discarded at the start of every run
    std::size_t char_jobs = 300;   // jobs per characterization run
    std::size_t calib_jobs = 200;  // jobs used to measure E[S] before each sweep
    std::size_t file_mib = 4096;   // size of the output file
    int reps = 3;
    std::uint64_t seed = 1;
    std::vector<double> rhos{0.3, 0.5, 0.7, 0.8, 0.9};
    std::string file = "workload.dat";
    std::string out = "results";
    bool allow_tmpfs = false;      // only for smoke tests
    std::string phase = "both";    // char | queue | both
    double char_secs = 0;          // >0: run each characterization workload for this long
    double idle_secs = 0;          // >0: start with an idle interval (monitoring baseline)
    double gap_secs = 0;           // pause between characterization workloads

    static Options parse(int argc, char** argv) {
        Options o;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const std::size_t eq = arg.find('=');
            if (eq == std::string::npos) throw std::invalid_argument("expected key=value: " + arg);
            const std::string k = arg.substr(0, eq), v = arg.substr(eq + 1);
            if (k == "n") o.n = std::stoul(v);
            else if (k == "jobs") o.jobs = std::stoul(v);
            else if (k == "warmup") o.warmup = std::stoul(v);
            else if (k == "char_jobs") o.char_jobs = std::stoul(v);
            else if (k == "calib_jobs") o.calib_jobs = std::stoul(v);
            else if (k == "file_mib") o.file_mib = std::stoul(v);
            else if (k == "reps") o.reps = std::stoi(v);
            else if (k == "seed") o.seed = std::stoull(v);
            else if (k == "file") o.file = v;
            else if (k == "out") o.out = v;
            else if (k == "allow_tmpfs") o.allow_tmpfs = (v == "1");
            else if (k == "phase") o.phase = v;
            else if (k == "char_secs") o.char_secs = std::stod(v);
            else if (k == "idle_secs") o.idle_secs = std::stod(v);
            else if (k == "gap_secs") o.gap_secs = std::stod(v);
            else if (k == "rhos") {
                o.rhos.clear();
                std::stringstream ss(v);
                for (std::string item; std::getline(ss, item, ',');) o.rhos.push_back(std::stod(item));
            } else {
                throw std::invalid_argument("unknown option: " + k);
            }
        }
        if (o.n == 0 || o.n % 64 != 0) throw std::invalid_argument("n must be a multiple of 64");
        for (double r : o.rhos)
            if (!(r > 0.0 && r < 1.0)) throw std::invalid_argument("each rho must be in (0,1)");
        if (o.jobs < 100) throw std::invalid_argument("jobs must be at least 100");
        if (o.phase != "char" && o.phase != "queue" && o.phase != "both")
            throw std::invalid_argument("phase must be char, queue or both");
        return o;
    }
};

class Experiment {
public:
    explicit Experiment(Options o)
        : opt_(std::move(o)), slot_bytes_(opt_.n * opt_.n * sizeof(double)) {
        std::filesystem::create_directories(opt_.out);
    }

    void run() {
        self_check();
        if (opt_.phase != "queue") characterize();
        if (opt_.phase != "char") queue_sweep();
    }

private:
    struct Config {
        std::string name;
        const Multiplier* mult;
        Pattern pattern;
    };

    static std::string pattern_name(Pattern p) { return p == Pattern::Sequential ? "seq" : "rand"; }

    std::unique_ptr<DiskWriter> make_writer(Pattern p) const {
        const std::size_t file_bytes = opt_.file_mib * 1024 * 1024;
        const std::size_t slots = file_bytes / slot_bytes_;
        std::unique_ptr<OffsetPolicy> policy;
        if (p == Pattern::Sequential) policy = std::make_unique<SequentialOffsets>(slots);
        else policy = std::make_unique<RandomOffsets>(slots, opt_.seed);
        return std::make_unique<DiskWriter>(opt_.file, slot_bytes_, file_bytes, std::move(policy),
                                            opt_.allow_tmpfs);
    }

    // Both multipliers run the same source; they must agree numerically or the comparison is void.
    void self_check() {
        std::mt19937_64 rng(opt_.seed);
        const std::size_t n = 64;
        Matrix a(n), b(n), c_simd(n), c_scalar(n);
        a.fill_random(rng);
        b.fill_random(rng);
        simd_.multiply(a, b, c_simd);
        scalar_.multiply(a, b, c_scalar);
        double worst = 0.0;
        for (std::size_t i = 0; i < n * n; ++i)
            worst = std::max(worst, std::abs(c_simd.data()[i] - c_scalar.data()[i]));
        if (worst > 1e-9) throw std::runtime_error("SIMD and scalar results differ");
        std::cout << "self-check ok (max difference " << worst << ")\n";
    }

    // Wall-clock start/end of every measured interval, for slicing vmstat/iostat/top logs.
    void mark(const std::string& phase, const std::string& label, double rho, int rep, double t0,
              double t1) const {
        // Epoch seconds need ~13 significant digits, more than Row's default number format keeps.
        char a[32], b[32];
        std::snprintf(a, sizeof a, "%.3f", t0);
        std::snprintf(b, sizeof b, "%.3f", t1);
        Row row;
        row.add("phase", phase).add("label", label).add("rho", rho).add("rep", rep)
            .add("t_start", std::string(a)).add("t_end", std::string(b));
        CsvFile(opt_.out + "/markers.csv").write(row);
    }

    // ---- Phase A ------------------------------------------------------------------------
    void characterize() {
        struct Case {
            std::string name;
            const Multiplier* mult;  // null: no compute stage
            bool use_disk;
            Pattern pattern;
        };
        const std::vector<Case> cases = {
            {"cpu_simd", &simd_, false, Pattern::Sequential},
            {"cpu_scalar", &scalar_, false, Pattern::Sequential},
            {"io_seq", nullptr, true, Pattern::Sequential},
            {"io_rand", nullptr, true, Pattern::Random},
            {"pipe_simd_seq", &simd_, true, Pattern::Sequential},
            {"pipe_simd_rand", &simd_, true, Pattern::Random},
            {"pipe_scalar_seq", &scalar_, true, Pattern::Sequential},
            {"pipe_scalar_rand", &scalar_, true, Pattern::Random},
        };

        CsvFile csv(opt_.out + "/characterization.csv");

        if (opt_.idle_secs > 0) {
            std::printf("[characterize] idle for %.0f s\n", opt_.idle_secs);
            const double t0 = epoch_s();
            std::this_thread::sleep_for(std::chrono::duration<double>(opt_.idle_secs));
            mark("char", "idle", 0, 0, t0, epoch_s());
        }

        for (const Case& c : cases) {
            std::unique_ptr<DiskWriter> disk;
            if (c.use_disk) disk = make_writer(c.pattern);
            Pipeline pipe(c.mult, disk.get(), opt_.n, opt_.seed);

            for (std::size_t i = 0; i < opt_.warmup; ++i) pipe.run();

            JobTiming sum;
            std::size_t done = 0;
            ResourceMonitor mon;
            const double t0 = epoch_s();
            mon.start();
            const auto begin = Clock::now();
            while (opt_.char_secs > 0 ? seconds(Clock::now() - begin) < opt_.char_secs
                                      : done < opt_.char_jobs) {
                sum += pipe.run();
                ++done;
            }
            const ResourceMonitor::Usage u = mon.stop();
            mark("char", c.name, 0, 0, t0, epoch_s());

            const double jobs = static_cast<double>(done);
            Row row;
            row.add("workload", c.name)
                .add("jobs", jobs)
                .add("mean_compute_ms", 1e3 * sum.compute_s / jobs)
                .add("mean_write_ms", 1e3 * sum.write_s / jobs)
                .add("mean_sync_ms", 1e3 * sum.sync_s / jobs)
                .add("io_fraction", sum.io_s() / sum.total_s())
                .add("jobs_per_s", jobs / u.wall_s);
            u.add_to(row);
            csv.write(row);
            std::printf("[characterize] %-17s cpu %5.1f%%  io_fraction %.2f  %.1f jobs/s\n",
                        c.name.c_str(), u.cpu_pct, sum.io_s() / sum.total_s(), jobs / u.wall_s);
            if (opt_.gap_secs > 0)
                std::this_thread::sleep_for(std::chrono::duration<double>(opt_.gap_secs));
        }
    }

    // ---- Phase B ------------------------------------------------------------------------
    void queue_sweep() {
        std::vector<Config> configs = {
            {"simd_seq", &simd_, Pattern::Sequential},
            {"simd_rand", &simd_, Pattern::Random},
            {"scalar_seq", &scalar_, Pattern::Sequential},
            {"scalar_rand", &scalar_, Pattern::Random},
        };
        CsvFile summary(opt_.out + "/queue_summary.csv");
        std::mt19937_64 rng(opt_.seed);

        for (int rep = 0; rep < opt_.reps; ++rep) {
            std::shuffle(configs.begin(), configs.end(), rng);  // randomize order against drift
            for (const Config& cfg : configs) sweep_one(cfg, rep, summary);
        }
    }

    void sweep_one(const Config& cfg, int rep, CsvFile& summary) {
        std::unique_ptr<DiskWriter> disk = make_writer(cfg.pattern);
        Pipeline pipe(cfg.mult, disk.get(), opt_.n, opt_.seed);

        for (std::size_t i = 0; i < opt_.warmup; ++i) pipe.run();

        // Calibration: back-to-back jobs give E[S], which converts a target rho into a rate.
        JobTiming sum;
        const auto c0 = Clock::now();
        for (std::size_t i = 0; i < opt_.calib_jobs; ++i) sum += pipe.run();
        const double calib = static_cast<double>(opt_.calib_jobs);
        const double mean_s = seconds(Clock::now() - c0) / calib;
        std::printf("[rep %d] %-12s E[S] = %.3f ms  (compute %.3f, write %.3f, sync %.3f)\n", rep,
                    cfg.name.c_str(), 1e3 * mean_s, 1e3 * sum.compute_s / calib,
                    1e3 * sum.write_s / calib, 1e3 * sum.sync_s / calib);

        for (double rho : opt_.rhos) {
            const double lambda = rho / mean_s;
            const ArrivalSchedule schedule(lambda, opt_.jobs + opt_.warmup, opt_.seed + run_id_++);

            Server server(pipe);
            ResourceMonitor mon;
            const double t0 = epoch_s();
            mon.start();
            const std::vector<JobRecord> records = server.run(schedule);
            const ResourceMonitor::Usage u = mon.stop();
            mark("queue", cfg.name, rho, rep, t0, epoch_s());

            const QueueStats stats(records, opt_.warmup);

            Row row;
            row.add("config", cfg.name)
                .add("mult", cfg.mult->name())
                .add("pattern", pattern_name(cfg.pattern))
                .add("rep", static_cast<double>(rep))
                .add("rho_target", rho)
                .add("lambda_target", lambda);
            stats.add_to(row);
            u.add_to(row);
            summary.write(row);

            write_jobs(cfg.name, rho, rep, records);
            std::printf("    rho target %.2f  measured %.3f  mean S %.3f ms\n", rho, stats.rho(),
                        1e3 * stats.mean_service());
        }
    }

    void write_jobs(const std::string& name, double rho, int rep,
                    const std::vector<JobRecord>& records) const {
        char buf[64];
        std::snprintf(buf, sizeof buf, "_rho%.2f_rep%d.csv", rho, rep);
        CsvFile csv(opt_.out + "/jobs_" + name + buf);
        for (std::size_t i = opt_.warmup; i < records.size(); ++i) {
            const JobRecord& r = records[i];
            Row row;
            row.add("arrival", r.arrival)
                .add("start", r.start)
                .add("finish", r.finish)
                .add("compute", r.t.compute_s)
                .add("write", r.t.write_s)
                .add("sync", r.t.sync_s);
            csv.write(row);
        }
    }

    Options opt_;
    std::size_t slot_bytes_;
    std::uint64_t run_id_ = 0;
    SimdMultiplier simd_;
    ScalarMultiplier scalar_;
};

int main(int argc, char** argv) {
    try {
        Experiment(Options::parse(argc, argv)).run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
