#include "itch/mmap_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace ob::itch {

MmapFile::MmapFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path);
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::system_error(e, std::generic_category(), "fstat " + path);
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
        ::close(fd);
        return;  // empty file: valid, empty span
    }
    addr_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // the mapping keeps its own reference
    if (addr_ == MAP_FAILED) {
        addr_ = nullptr;
        throw std::system_error(errno, std::generic_category(), "mmap " + path);
    }
    // Advisory only — failure is not an error.
    ::madvise(addr_, size_, MADV_SEQUENTIAL);
}

MmapFile::~MmapFile() {
    if (addr_ != nullptr) {
        ::munmap(addr_, size_);
    }
}

MmapFile::MmapFile(MmapFile&& other) noexcept
    : addr_(std::exchange(other.addr_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MmapFile& MmapFile::operator=(MmapFile&& other) noexcept {
    if (this != &other) {
        if (addr_ != nullptr) {
            ::munmap(addr_, size_);
        }
        addr_ = std::exchange(other.addr_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

}  // namespace ob::itch
