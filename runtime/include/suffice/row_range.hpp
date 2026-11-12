#pragma once

#include "suffice/runtime.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace suffice::range_detail {

// Validate with subtraction before forming first + count. An empty range at
// the dataset end is valid. The returned end is therefore representable.
inline std::uint64_t validate(std::uint64_t dataset_rows, std::uint64_t first,
                              std::uint64_t count, std::span<std::uint64_t> mask) {
    if (first > dataset_rows || count > dataset_rows - first)
        throw std::invalid_argument("execution row range is outside dataset");
    if (mask.size() != mask_words(count))
        throw std::invalid_argument("output mask size does not match row range");
    std::fill(mask.begin(), mask.end(), std::uint64_t{0});
    return first + count;
}

inline std::uint64_t low_mask(unsigned count) noexcept {
    return count >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << count) - 1);
}

inline void write(std::span<std::uint64_t> mask, std::uint64_t first,
                  std::uint64_t bits, unsigned count) noexcept {
    if (!count) return;
    bits &= low_mask(count);
    const auto word = static_cast<std::size_t>(first / 64);
    const auto shift = static_cast<unsigned>(first % 64);
    mask[word] |= bits << shift;
    if (shift && count > 64 - shift) mask[word + 1] |= bits >> (64 - shift);
}

// Read at most 64 valid bits from a little-endian bitplane. An unaligned read
// may touch nine bytes, but never goes beyond its source span. Callers establish
// first + count <= the valid logical length of the plane.
inline std::uint64_t read(std::span<const std::uint8_t> plane,
                          std::uint64_t first, unsigned count) noexcept {
    if (!count) return 0;
    const auto offset = static_cast<std::size_t>(first / 8);
    const auto shift = static_cast<unsigned>(first % 8);
    const auto bytes = static_cast<std::size_t>((count + shift + 7) / 8);
    const auto low_bytes = std::min<std::size_t>(8, bytes);
    std::uint64_t value = 0;
    if constexpr (std::endian::native == std::endian::little)
        std::memcpy(&value, plane.data() + offset, low_bytes);
    else
        for (std::size_t i = 0; i < low_bytes; ++i)
            value |= std::uint64_t{plane[offset + i]} << (8 * i);
    value >>= shift;
    if (bytes == 9) value |= std::uint64_t{plane[offset + 8]} << (64 - shift);
    return value & low_mask(count);
}

// Append a selected plane interval into a zeroed local output mask. Whole-byte
// transfers retain the original projection memcpy path, including block32.
inline void copy(std::span<std::uint64_t> mask, std::uint64_t target_first,
                  std::span<const std::uint8_t> plane, std::uint64_t source_first,
                  std::uint64_t count) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        if (target_first % 8 == 0 && source_first % 8 == 0) {
            const auto bytes = static_cast<std::size_t>(count / 8);
            if (bytes) std::memcpy(reinterpret_cast<std::uint8_t*>(mask.data()) + target_first / 8,
                                   plane.data() + source_first / 8, bytes);
            const auto bits = static_cast<std::uint64_t>(bytes) * 8;
            target_first += bits; source_first += bits; count -= bits;
        }
    }
    while (count) {
        const auto take = static_cast<unsigned>(std::min<std::uint64_t>(64, count));
        write(mask, target_first, read(plane, source_first, take), take);
        target_first += take; source_first += take; count -= take;
    }
}

} // namespace suffice::range_detail
