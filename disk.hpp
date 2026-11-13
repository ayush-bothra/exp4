#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

// Chooses which slot of the output file the next job's result goes to.
class OffsetPolicy {
public:
    virtual ~OffsetPolicy() = default;
    virtual std::size_t next_slot() = 0;
    virtual std::string name() const = 0;
};

class SequentialOffsets : public OffsetPolicy {
public:
    explicit SequentialOffsets(std::size_t slots) : slots_(slots) {}
    std::size_t next_slot() override {
        const std::size_t s = next_;
        next_ = (next_ + 1) % slots_;
        return s;
    }
    std::string name() const override { return "seq"; }

private:
    std::size_t slots_;
    std::size_t next_ = 0;
};

class RandomOffsets : public OffsetPolicy {
public:
    RandomOffsets(std::size_t slots, std::uint64_t seed) : rng_(seed), dist_(0, slots - 1) {}
    std::size_t next_slot() override { return dist_(rng_); }
    std::string name() const override { return "rand"; }

private:
    std::mt19937_64 rng_;
    std::uniform_int_distribution<std::size_t> dist_;
};

// I/O-bound stage. Owns a pre-filled file divided into fixed-size slots. Each write() is one
// pwrite() of the whole result buffer into the policy's next slot, followed by fdatasync(),
// so the time includes the device and not just the page cache.
class DiskWriter {
public:
    struct Timing {
        double write_s;  // pwrite(): copy into the page cache
        double sync_s;   // fdatasync(): push to the device
    };

    DiskWriter(const std::string& path, std::size_t slot_bytes, std::size_t file_bytes,
               std::unique_ptr<OffsetPolicy> policy, bool allow_tmpfs);
    ~DiskWriter();
    DiskWriter(const DiskWriter&) = delete;
    DiskWriter& operator=(const DiskWriter&) = delete;

    Timing write(const void* buf, std::size_t len);

private:
    void prefill();

    int fd_ = -1;
    std::size_t slot_bytes_;
    std::size_t slots_;
    std::unique_ptr<OffsetPolicy> policy_;
};
