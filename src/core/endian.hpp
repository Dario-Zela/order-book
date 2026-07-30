#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ob {

// std::byteswap is C++23; polyfill on builtins every supported compiler has.
template <typename T>
    requires std::is_unsigned_v<T>
constexpr T byteswap(T v) noexcept {
    if constexpr (sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return __builtin_bswap16(v);
    } else if constexpr (sizeof(T) == 4) {
        return __builtin_bswap32(v);
    } else {
        static_assert(sizeof(T) == 8);
        return __builtin_bswap64(v);
    }
}

// memcpy is the defined-behaviour way to type-pun (strict aliasing rules
// forbid reinterpret_cast-and-dereference); compilers fold this to a single
// load + bswap instruction.
template <typename T>
    requires std::is_unsigned_v<T>
T load_be(const void* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        v = byteswap(v);
    }
    return v;
}

// ITCH timestamps are 48-bit big-endian nanoseconds since midnight; no
// integral type is 6 bytes wide, so they get a dedicated loader.
inline std::uint64_t load_be48(const void* p) noexcept {
    const auto* b = static_cast<const unsigned char*>(p);
    return (std::uint64_t{b[0]} << 40) | (std::uint64_t{b[1]} << 32) |
           (std::uint64_t{b[2]} << 24) | (std::uint64_t{b[3]} << 16) |
           (std::uint64_t{b[4]} << 8) | std::uint64_t{b[5]};
}

}  // namespace ob
