#include "suffice/encoded_ranges.hpp"
#include "suffice/row_range.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace suffice {
namespace {

std::uint64_t low_mask(unsigned bits) noexcept {
    return bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
}

std::uint64_t plane_word(std::span<const std::uint8_t> plane, std::size_t word) noexcept {
    const auto offset = word * 8;
    if constexpr (std::endian::native == std::endian::little) {
        if (plane.size() - offset >= 8) {
            std::uint64_t result;
            std::memcpy(&result, plane.data() + offset, sizeof(result));
            return result;
        }
    }
    const auto bytes = std::min<std::size_t>(8, plane.size() - offset);
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < bytes; ++i) result |= std::uint64_t{plane[offset + i]} << (8 * i);
    return result;
}

void write_bits(std::span<std::uint64_t> out, std::uint64_t first,
                 std::uint64_t value, unsigned count) noexcept {
    value &= low_mask(count);
    const auto word = static_cast<std::size_t>(first / 64);
    const auto shift = static_cast<unsigned>(first % 64);
    out[word] |= value << shift;
    if (shift != 0 && count > 64 - shift) out[word + 1] |= value >> (64 - shift);
}

std::uint64_t line_bytes(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.empty()) return 0;
    const auto offset = reinterpret_cast<std::uintptr_t>(bytes.data()) % 64;
    return ((offset + bytes.size() + 63) / 64) * 64;
}

} // namespace

EncodedRangeExecutor::EncodedRangeExecutor(const PackedDataset& dataset,
                                           std::uint32_t channel,
                                           std::span<const Bounds> accepted,
                                           unsigned first_stage_bits)
    : dataset_(dataset), channel_(channel), first_stage_bits_(first_stage_bits) {
    if (channel >= dataset.info().channels) throw std::invalid_argument("encoded range channel is outside dataset");
    if (accepted.size() > ranges_.size()) throw std::invalid_argument("encoded range executor supports at most eight ranges");
    if (first_stage_bits == 0 || first_stage_bits > 32) throw std::invalid_argument("first stage bits must be in [1, 32]");

    constexpr i64 minimum = std::numeric_limits<std::int32_t>::min();
    constexpr i64 maximum = std::numeric_limits<std::int32_t>::max();
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        const auto range = accepted[i];
        if (range.lo > range.hi || range.lo < minimum || range.hi > maximum) {
            throw std::invalid_argument("encoded acceptance ranges must be nonempty int32 intervals");
        }
        if (i != 0 && accepted[i - 1].hi >= range.lo) {
            throw std::invalid_argument("encoded acceptance ranges must be sorted and disjoint");
        }
    }

    const auto domain = dataset.info().ranges[channel];
    std::array<Bounds, 8> normalized{};
    for (const auto range : accepted) {
        Bounds clipped{std::max<i64>(range.lo, domain.min), std::min<i64>(range.hi, domain.max)};
        if (clipped.lo > clipped.hi) continue;
        if (range_count_ != 0 && normalized[range_count_ - 1].hi + 1 == clipped.lo) {
            normalized[range_count_ - 1].hi = clipped.hi;
        } else {
            normalized[range_count_++] = clipped;
        }
    }
    if (range_count_ != 0) {
        // Outside the verified observed domain the accepted classification is
        // immaterial. Extending its edges removes unnecessary comparisons.
        if (normalized[0].lo == domain.min) normalized[0].lo = minimum;
        if (normalized[range_count_ - 1].hi == domain.max) normalized[range_count_ - 1].hi = maximum;
    }
    for (std::size_t i = 0; i < range_count_; ++i) {
        ranges_[i] = {static_cast<std::uint32_t>(normalized[i].lo + 2147483648LL),
                      static_cast<std::uint32_t>(normalized[i].hi + 2147483648LL)};
    }
    accepts_all_ = range_count_ == 1 && ranges_[0].lower == 0 &&
                   ranges_[0].upper == std::numeric_limits<std::uint32_t>::max();
    words_per_block_ = (static_cast<std::size_t>(dataset.info().block_rows) + 63) / 64;
    if (range_count_ == 0 || accepts_all_ || dataset.info().rows == 0) return;

    // The biased sign plane already is the exact nonnegative-membership mask.
    // Its complement is the negative-membership mask. Every supported block
    // begins on a byte boundary, including 32-row blocks inside a mask word.
    if constexpr (std::endian::native == std::endian::little) {
        if (range_count_ == 1) {
            sign_projection_ =
                (ranges_[0].lower == 0x80000000U && ranges_[0].upper == 0xffffffffU) ||
                (ranges_[0].lower == 0 && ranges_[0].upper == 0x7fffffffU);
            invert_projection_ = ranges_[0].lower == 0;
            if (sign_projection_) return;
        }
    }

    const auto comparator_words = range_count_ * words_per_block_;
    equal_lower_.resize(comparator_words);
    greater_lower_.resize(comparator_words);
    equal_upper_.resize(comparator_words);
    less_upper_.resize(comparator_words);
    unknown_.resize(words_per_block_);
    accepted_.resize(words_per_block_);
}

std::uint64_t EncodedRangeExecutor::scratch_bytes() const noexcept {
    return sizeof(*this) + (equal_lower_.capacity() + greater_lower_.capacity() +
           equal_upper_.capacity() + less_upper_.capacity() + unknown_.capacity() +
           accepted_.capacity()) * sizeof(std::uint64_t);
}

ExecutionStats EncodedRangeExecutor::run(std::span<std::uint64_t> output_mask) {
    return run_range(0,dataset_.info().rows,output_mask);
}

ExecutionStats EncodedRangeExecutor::run_range(std::uint64_t first_row,std::uint64_t row_count,
                                               std::span<std::uint64_t> output_mask) {
    const auto end_row=range_detail::validate(dataset_.info().rows,first_row,row_count,output_mask);
    const auto rows=row_count;
    ExecutionStats stats;
    stats.rows = rows;
    stats.output_bytes = output_mask.size_bytes();
    stats.peak_scratch_bytes = scratch_bytes();
    stats.kernel = "bitplane-preimage";

    if (rows == 0 || range_count_ == 0 || accepts_all_) {
        if (accepts_all_ && rows != 0) {
            std::fill(output_mask.begin(), output_mask.end(), ~std::uint64_t{0});
            const auto tail = static_cast<unsigned>(rows % 64);
            if (tail != 0) output_mask.back() &= low_mask(tail);
            stats.accepted = rows;
        }
        stats.resolved_by_bits[0] = rows;
        stats.bulk_resolutions = rows != 0 ? 1 : 0;
        return stats;
    }

    const auto block_rows=dataset_.info().block_rows;
    const auto first_block=first_row/block_rows,last_block=(end_row-1)/block_rows;
    if (sign_projection_) {
        stats.kernel = "bitplane-preimage-projection";
        for (auto block=first_block;block<=last_block;++block) {
            const auto plane = dataset_.plane_view(block, channel_, 0, 1);
            const auto block_first=block*block_rows;
            const auto begin=std::max(first_row,block_first);
            const auto end=std::min(end_row,block_first+dataset_.block_length(block));
            range_detail::copy(output_mask,begin-first_row,plane,begin-block_first,end-begin);
            const auto byte_begin=static_cast<std::size_t>((begin-block_first)/8);
            const auto byte_end=static_cast<std::size_t>((end-block_first+7)/8);
            const auto accessed=plane.subspan(byte_begin,byte_end-byte_begin);
            stats.requested_payload_bytes += accessed.size();
            stats.estimated_cacheline_bytes += line_bytes(accessed);
            ++stats.loaded_planes;
            ++stats.region_loads;
        }
        if (invert_projection_) {
            for (auto& word : output_mask) word = ~word;
        }
        const auto tail = static_cast<unsigned>(rows % 64);
        if (tail != 0) output_mask.back() &= low_mask(tail);
        stats.accepted = mask_popcount(output_mask);
        stats.resolved_by_bits[1] = rows;
        return stats;
    }

    for (auto block=first_block;block<=last_block;++block) {
        const auto block_first=block*block_rows;
        const auto length = dataset_.block_length(block);
        const auto begin=std::max(first_row,block_first)-block_first;
        const auto end=std::min(end_row,block_first+length)-block_first;
        const auto first_word=static_cast<std::size_t>(begin/64),end_word=static_cast<std::size_t>((end+63)/64);
        for (auto word=first_word;word<end_word;++word) {
            const auto lo=static_cast<unsigned>(std::max<std::uint64_t>(begin,word*64)-word*64);
            const auto hi=static_cast<unsigned>(std::min<std::uint64_t>(end,word*64+64)-word*64);
            const auto valid = low_mask(hi)&~low_mask(lo);
            unknown_[word] = valid;
            accepted_[word] = 0;
            for (std::size_t range = 0; range < range_count_; ++range) {
                const auto index = range * words_per_block_ + word;
                equal_lower_[index] = valid;
                greater_lower_[index] = 0;
                equal_upper_[index] = valid;
                less_upper_[index] = 0;
            }
        }
        std::uint64_t remaining = end-begin;
        for (unsigned bit = 0; bit < 32 && remaining != 0; ++bit) {
            const auto plane = dataset_.plane_view(block, channel_, bit, 1);
            const auto accessed=plane.subspan(first_word*8,std::min(plane.size(),end_word*8)-first_word*8);
            stats.requested_payload_bytes += accessed.size();
            stats.estimated_cacheline_bytes += line_bytes(accessed);
            ++stats.loaded_planes;
            const auto weight = std::uint32_t{1} << (31 - bit);
            const auto suffix_mask = static_cast<std::uint32_t>(low_mask(31 - bit));
            std::uint64_t next_remaining = 0;
            for (auto word=first_word;word<end_word;++word) {
                const auto active = unknown_[word];
                if (active == 0) continue;
                const auto x = plane_word(plane, word);
                std::uint64_t certain_union = 0;
                std::uint64_t possible_union = 0;
                for (std::size_t range = 0; range < range_count_; ++range) {
                    const auto index = range * words_per_block_ + word;
                    const auto lower = ranges_[range].lower;
                    const auto upper = ranges_[range].upper;
                    std::uint64_t certain_lower, possible_lower;
                    if (lower == 0) {
                        certain_lower = possible_lower = active;
                    } else {
                        auto equal = equal_lower_[index] & active;
                        auto greater = greater_lower_[index] & active;
                        if ((lower & weight) != 0) {
                            equal &= x;
                        } else {
                            greater |= equal & x;
                            equal &= ~x;
                        }
                        equal_lower_[index] = equal;
                        greater_lower_[index] = greater;
                        certain_lower = greater | (((lower & suffix_mask) == 0) ? equal : 0);
                        possible_lower = greater | equal;
                    }
                    std::uint64_t certain_upper, possible_upper;
                    if (upper == std::numeric_limits<std::uint32_t>::max()) {
                        certain_upper = possible_upper = active;
                    } else {
                        auto equal = equal_upper_[index] & active;
                        auto less = less_upper_[index] & active;
                        if ((upper & weight) != 0) {
                            less |= equal & ~x;
                            equal &= x;
                        } else {
                            equal &= ~x;
                        }
                        equal_upper_[index] = equal;
                        less_upper_[index] = less;
                        certain_upper = less | (((upper & suffix_mask) == suffix_mask) ? equal : 0);
                        possible_upper = less | equal;
                    }
                    certain_union |= certain_lower & certain_upper;
                    possible_union |= possible_lower & possible_upper;
                }
                accepted_[word] |= certain_union;
                unknown_[word] = possible_union & ~certain_union;
                next_remaining += static_cast<std::uint64_t>(std::popcount(unknown_[word]));
            }
            stats.resolved_by_bits[bit + 1] += remaining - next_remaining;
            remaining = next_remaining;
            if (bit + 1 == first_stage_bits_) stats.refined_rows += remaining;
        }
        if (remaining != 0) throw std::logic_error("encoded membership unresolved after all 32 planes");
        for (auto word=first_word;word<end_word;++word) {
            const auto lo=std::max<std::uint64_t>(begin,word*64);
            const auto hi=std::min<std::uint64_t>(end,word*64+64);
            write_bits(output_mask,block_first+lo-first_row,accepted_[word]>>(lo-word*64),static_cast<unsigned>(hi-lo));
            stats.accepted += static_cast<std::uint64_t>(std::popcount(accepted_[word]));
        }
        ++stats.region_loads;
    }
    return stats;
}

} // namespace suffice
