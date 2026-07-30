#pragma once

// Read-only memory-mapped file with MADV_SEQUENTIAL hint (DESIGN §4): the
// parser is a cursor over this mapping — no reads, no copies. First pass is
// page-fault/IO bound (report warm-cache numbers, §8).

#include <cstddef>
#include <span>
#include <string>

namespace ob::itch {

class MmapFile {
public:
    explicit MmapFile(const std::string& path);  // throws std::system_error
    ~MmapFile();

    MmapFile(MmapFile&& other) noexcept;
    MmapFile& operator=(MmapFile&& other) noexcept;
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(addr_), size_};
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    void* addr_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace ob::itch
