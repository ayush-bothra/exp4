#include "disk.hpp"
#include "common.hpp"

#include <algorithm>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr long kTmpfsMagic = 0x01021994;

void write_fully(int fd, const char* p, std::size_t len, off_t off) {
    while (len > 0) {
        const ssize_t w = ::pwrite(fd, p, len, off);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw_errno("pwrite");
        }
        p += w;
        off += w;
        len -= static_cast<std::size_t>(w);
    }
}

}  // namespace

DiskWriter::DiskWriter(const std::string& path, std::size_t slot_bytes, std::size_t file_bytes,
                       std::unique_ptr<OffsetPolicy> policy, bool allow_tmpfs)
    : slot_bytes_(slot_bytes), slots_(file_bytes / slot_bytes), policy_(std::move(policy)) {
    if (slots_ == 0) throw std::invalid_argument("file is smaller than one slot");

    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throw_errno("open " + path);

    try {
        struct statfs fs {};
        if (::fstatfs(fd_, &fs) != 0) throw_errno("fstatfs");
        if (!allow_tmpfs && fs.f_type == kTmpfsMagic) {
            throw std::runtime_error(path + " is on tmpfs (RAM), so there would be no disk I/O. "
                                            "Choose a path on a real disk.");
        }
        prefill();
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}

DiskWriter::~DiskWriter() {
    if (fd_ >= 0) ::close(fd_);
}

// Fully allocate the file with real zeros so later writes overwrite existing blocks. Without
// this, sequential writes would also pay for file growth (metadata) and random writes would
// not, and the two patterns would differ for the wrong reason.
void DiskWriter::prefill() {
    const std::size_t want = slots_ * slot_bytes_;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) throw_errno("fstat");

    if (static_cast<std::size_t>(st.st_size) < want) {
        const std::vector<char> zeros(1 << 20, 0);
        for (std::size_t off = 0; off < want; off += zeros.size()) {
            write_fully(fd_, zeros.data(), std::min(zeros.size(), want - off),
                        static_cast<off_t>(off));
        }
    }
    if (::fdatasync(fd_) != 0) throw_errno("fdatasync");
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);  // start from a cold page cache
}

DiskWriter::Timing DiskWriter::write(const void* buf, std::size_t len) {
    if (len > slot_bytes_) throw std::invalid_argument("buffer larger than one slot");
    const off_t off = static_cast<off_t>(policy_->next_slot() * slot_bytes_);

    const auto t0 = Clock::now();
    write_fully(fd_, static_cast<const char*>(buf), len, off);
    const auto t1 = Clock::now();
    if (::fdatasync(fd_) != 0) throw_errno("fdatasync");
    const auto t2 = Clock::now();

    return {seconds(t1 - t0), seconds(t2 - t1)};
}
