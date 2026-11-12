#include "suffice/frontier_runtime.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define SUFFICE_FRONTIER_X86 1
#if defined(__GNUC__) || defined(__clang__)
#define SUFFICE_FRONTIER_AVX2 __attribute__((target("avx2")))
#else
#define SUFFICE_FRONTIER_AVX2
#endif
#else
#define SUFFICE_FRONTIER_X86 0
#endif

namespace suffice {
namespace {

std::uint64_t low_mask(unsigned count) noexcept {
    return count == 64 ? UINT64_MAX : ((std::uint64_t{1} << count) - 1);
}

bool valid_repair_options(FrontierRepairOptions options) noexcept {
    return (options.policy == FrontierRepairPolicy::SparseWords ||
            options.policy == FrontierRepairPolicy::Hybrid ||
            options.policy == FrontierRepairPolicy::DenseTiles) &&
           options.dense_threshold_percent >= 1 && options.dense_threshold_percent <= 100;
}

bool constant_root(const FrontierPlan& plan) noexcept {
    const auto op = plan.exact.nodes[plan.exact.root].op;
    return op == WordcodeOp::False || op == WordcodeOp::True;
}

#if SUFFICE_FRONTIER_X86
SUFFICE_FRONTIER_AVX2 std::size_t apply_vector(
    WordcodeOp op, const std::uint8_t* a, const std::uint8_t* b,
    std::uint64_t* output, std::size_t words) noexcept {
    std::size_t i = 0;
    if (op == WordcodeOp::And) {
        for (; i + 4 <= words; i += 4) {
            const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
            const auto y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i * 8));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + i), _mm256_and_si256(x, y));
        }
    } else if (op == WordcodeOp::Or) {
        for (; i + 4 <= words; i += 4) {
            const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
            const auto y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i * 8));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + i), _mm256_or_si256(x, y));
        }
    } else {
        const auto ones = _mm256_set1_epi64x(-1);
        for (; i + 4 <= words; i += 4) {
            const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + i), _mm256_xor_si256(x, ones));
        }
    }
    return i;
}
#endif

} // namespace

std::string_view frontier_repair_policy_name(FrontierRepairPolicy policy) noexcept {
    switch (policy) {
    case FrontierRepairPolicy::SparseWords: return "sparse";
    case FrontierRepairPolicy::Hybrid: return "hybrid";
    case FrontierRepairPolicy::DenseTiles: return "tile";
    }
    return "invalid";
}

FrontierRepairPolicy parse_frontier_repair_policy(std::string_view name) {
    if (name == "sparse") return FrontierRepairPolicy::SparseWords;
    if (name == "hybrid") return FrontierRepairPolicy::Hybrid;
    if (name == "tile") return FrontierRepairPolicy::DenseTiles;
    throw std::invalid_argument("frontier repair must be sparse, hybrid, or tile");
}

FrontierExecutor::FrontierExecutor(const PackedDataset& data, const FrontierPlan& plan,
                                   std::size_t tile_words, bool allow_simd,
                                   FrontierRepairOptions options)
    : data_(data), plan_(plan), tile_words_(tile_words), repair_(options) {
    // The public node arrays are checked against their private bounded compiler
    // witness. Structure alone would not prove the prefix inequalities.
    validate_frontier(plan);
    if (!tile_words || tile_words > 4096)
        throw std::invalid_argument("frontier tile_words must be in 1..4096");
    if (!valid_repair_options(options))
        throw std::invalid_argument("invalid frontier repair policy or density percent (expected 1..100)");
    for (const auto& load : plan.exact.loads) {
        if (load.channel >= data.info().channels)
            throw std::invalid_argument("frontier input channel is unavailable");
        const auto range = data.info().ranges[load.channel];
        if (load.range.lo > range.min || load.range.hi < range.max)
            throw std::invalid_argument("frontier source certificate does not cover dataset");
    }
#if SUFFICE_FRONTIER_X86
    use_simd_ = allow_simd && avx2_available() && std::endian::native == std::endian::little;
#else
    (void)allow_simd;
#endif
    // Certificates are still checked above even when every input is folded.
    if (constant_root(plan)) return;
    arena_.resize(std::size_t(plan.prefix.live_slot_count) * tile_words);
    views_.resize(plan.prefix.nodes.size());
    prefix_planes_.resize(plan.prefix.nodes.size());
    can_refine_ = plan.prefix.must_root != plan.prefix.may_root;
    // Identical retained roots are a structural proof that no repair is needed.
    if (!can_refine_) return;
    if (options.policy != FrontierRepairPolicy::SparseWords) {
        dense_executor_ = std::make_unique<WordcodeExecutor>(data, plan.exact, tile_words, allow_simd);
        dense_output_.resize(tile_words);
        dirty_words_.resize(tile_words);
    }
    if (options.policy == FrontierRepairPolicy::DenseTiles) return;
    exact_planes_.resize(plan.exact.nodes.size());
    exact_values_.resize(plan.exact.nodes.size());
    exact_to_prefix_.resize(plan.exact.nodes.size(), UINT32_MAX);
    std::vector<std::uint32_t> prefix_by_plane(plan.exact.loads.size() * 32, UINT32_MAX);
    for (std::size_t j = 0; j < plan.prefix.nodes.size(); ++j) {
        const auto& prefix = plan.prefix.nodes[j];
        if (prefix.op == WordcodeOp::Plane)
            prefix_by_plane[std::size_t(prefix.load_index) * 32 + prefix.bit] = static_cast<std::uint32_t>(j);
    }
    for (std::size_t i = 0; i < plan.exact.nodes.size(); ++i) {
        const auto& exact = plan.exact.nodes[i];
        if (exact.op != WordcodeOp::Plane) continue;
        exact_to_prefix_[i] = prefix_by_plane[std::size_t(exact.load_index) * 32 + exact.bit];
    }
}

std::uint64_t FrontierExecutor::required_scratch_bytes(const FrontierPlan& plan,
                                                       std::size_t tile_words,
                                                       FrontierRepairOptions options) noexcept {
    if (!plan.complete || !tile_words || tile_words > 4096 ||
        !valid_repair_options(options) || plan.exact.root >= plan.exact.nodes.size() ||
        plan.prefix.nodes.size() > max_wordcode_nodes || plan.exact.nodes.size() > max_wordcode_nodes ||
        plan.prefix.live_slot_count > max_wordcode_nodes) return 0;
    if (constant_root(plan)) return sizeof(FrontierExecutor);
    auto bytes = sizeof(FrontierExecutor) + std::uint64_t(plan.prefix.live_slot_count) * tile_words * 8 +
        std::uint64_t(plan.prefix.nodes.size()) * (sizeof(View) + sizeof(std::span<const std::uint8_t>));
    if (plan.prefix.must_root == plan.prefix.may_root) return bytes;
    if (options.policy != FrontierRepairPolicy::DenseTiles)
        bytes += std::uint64_t(plan.exact.nodes.size()) * (8 + 4 + sizeof(std::span<const std::uint8_t>));
    if (options.policy != FrontierRepairPolicy::SparseWords)
        bytes += WordcodeExecutor::required_scratch_bytes(plan.exact, tile_words) + tile_words * (8 + 4);
    return bytes;
}

std::uint64_t FrontierExecutor::scratch_bytes() const noexcept {
    return sizeof(*this) + (arena_.capacity() + exact_values_.capacity()) * sizeof(std::uint64_t) +
        views_.capacity() * sizeof(View) +
        (prefix_planes_.capacity() + exact_planes_.capacity()) * sizeof(std::span<const std::uint8_t>) +
        (exact_to_prefix_.capacity() + dirty_words_.capacity()) * sizeof(std::uint32_t) +
        dense_output_.capacity() * sizeof(std::uint64_t) +
        (dense_executor_ ? dense_executor_->scratch_bytes() : 0);
}

std::string FrontierExecutor::description() const {
    return "certified prefix batches; prefix_bits=" + std::to_string(plan_.prefix_bits) +
        "; tile_words=" + std::to_string(tile_words_) +
        "; repair=" + std::string(frontier_repair_policy_name(repair_.policy)) +
        "; dense_percent=" + std::to_string(repair_.dense_threshold_percent) +
        "; " + (use_simd_ ? std::string("AVX2 operators") : std::string("scalar operators"));
}

std::uint64_t FrontierExecutor::word(const View& view, std::size_t index) noexcept {
    if (view.words) return view.words[index];
    if (!view.bytes) return view.constant;
    const auto offset = index * 8;
    if (offset >= view.size) return 0;
    const auto available = std::min<std::size_t>(8, view.size - offset);
    std::uint64_t value = 0;
    if constexpr (std::endian::native == std::endian::little)
        std::memcpy(&value, view.bytes + offset, available);
    else for (std::size_t j = 0; j < available; ++j)
        value |= std::uint64_t(view.bytes[offset + j]) << (8 * j);
    if (view.shift) {
        value >>= view.shift;
        if (offset + 8 < view.size)
            value |= std::uint64_t(view.bytes[offset + 8]) << (64 - view.shift);
    }
    return value;
}

ExecutionStats FrontierExecutor::run(std::span<std::uint64_t> output) {
    return run_range(0, data_.info().rows, output);
}

ExecutionStats FrontierExecutor::run_range(std::uint64_t first, std::uint64_t count,
                                           std::span<std::uint64_t> output) {
    const auto rows = data_.info().rows;
    if (first > rows || count > rows - first)
        throw std::out_of_range("frontier row range is outside dataset");
    if (output.size() != mask_words(count))
        throw std::invalid_argument("frontier output must exactly match range mask size");
    std::fill(output.begin(), output.end(), std::uint64_t{0});
    ExecutionStats stats;
    stats.rows = count;
    stats.output_bytes = output.size_bytes();
    stats.peak_scratch_bytes = scratch_bytes();
    stats.kernel = "encoded-frontier-scalar";
    if (!count) return stats;
    const auto root_op = plan_.exact.nodes[plan_.exact.root].op;
    if (root_op == WordcodeOp::False || root_op == WordcodeOp::True) {
        if (root_op == WordcodeOp::True) {
            std::fill(output.begin(), output.end(), UINT64_MAX);
            if (count % 64) output.back() = low_mask(static_cast<unsigned>(count % 64));
            stats.accepted = count;
        }
        stats.resolved_by_bits[0] = count;
        stats.frontier_prefix_resolved_rows = count;
        stats.bulk_resolutions = 1;
        stats.kernel = "encoded-frontier-constant";
        return stats;
    }
    const auto block_rows = data_.info().block_rows;
    std::uint64_t produced = 0;
    while (produced < count) {
        const auto global = first + produced;
        const auto block = global / block_rows;
        const auto block_offset = static_cast<std::size_t>(global % block_rows);
        const auto segment = static_cast<std::size_t>(std::min<std::uint64_t>(
            count - produced, data_.block_length(block) - block_offset));
        const auto byte_count = (block_offset % 8 + segment + 7) / 8;
        ++stats.region_loads;
        for (std::size_t i = 0; i < plan_.prefix.nodes.size(); ++i) {
            const auto& n = plan_.prefix.nodes[i];
            if (n.op != WordcodeOp::Plane) continue;
            prefix_planes_[i] = data_.plane_view(block, plan_.prefix.loads[n.load_index].channel, n.bit, 1);
            ++stats.loaded_planes;
            stats.requested_payload_bytes += byte_count;
            const auto alignment = reinterpret_cast<std::uintptr_t>(prefix_planes_[i].data() + block_offset / 8) % 64;
            stats.estimated_cacheline_bytes += ((alignment + byte_count + 63) / 64) * 64;
        }
        for (std::size_t i = 0; i < exact_planes_.size(); ++i) {
            const auto& n = plan_.exact.nodes[i];
            if (n.op != WordcodeOp::Plane || exact_to_prefix_[i] != UINT32_MAX) continue;
            // Resolving the view reads metadata only. Payload is requested below
            // only for a word that actually contains an unknown output lane.
            exact_planes_[i] = data_.plane_view(block, plan_.exact.loads[n.load_index].channel, n.bit, 1);
        }
        for (std::size_t consumed = 0; consumed < segment;) {
            const auto take = std::min(segment - consumed, tile_words_ * 64);
            const auto local = block_offset + consumed;
            const auto words = (take + 63) / 64;
            const auto full_words = take / 64;
            for (std::size_t i = 0; i < plan_.prefix.nodes.size(); ++i) {
                const auto& n = plan_.prefix.nodes[i];
                auto& view = views_[i];
                if (n.op == WordcodeOp::False || n.op == WordcodeOp::True) {
                    view = {}; view.constant = n.op == WordcodeOp::True ? UINT64_MAX : 0;
                } else if (n.op == WordcodeOp::Plane) {
                    view = {};
                    view.bytes = prefix_planes_[i].data() + local / 8;
                    view.shift = static_cast<unsigned>(local % 8);
                    view.size = (view.shift + take + 7) / 8;
                } else {
                    // Both input descriptors are copied before their slots may
                    // be overwritten at a last use. Both roots remain live.
                    const auto a = views_[n.a];
                    const auto b = n.op == WordcodeOp::Not ? View{} : views_[n.b];
                    auto* destination = arena_.data() + std::size_t(n.slot) * tile_words_;
                    std::size_t done = 0;
#if SUFFICE_FRONTIER_X86
                    const auto* a_bytes = a.words ? reinterpret_cast<const std::uint8_t*>(a.words) : a.bytes;
                    const auto* b_bytes = b.words ? reinterpret_cast<const std::uint8_t*>(b.words) : b.bytes;
                    if (use_simd_ && !a.shift && !b.shift && a_bytes &&
                        (n.op == WordcodeOp::Not || b_bytes) && full_words >= 4) {
                        done = apply_vector(n.op, a_bytes, b_bytes, destination, full_words);
                        stats.kernel = "encoded-frontier-avx2";
                    }
#endif
                    for (; done < words; ++done) {
                        const auto x = word(a, done);
                        if (n.op == WordcodeOp::And) destination[done] = x & word(b, done);
                        else if (n.op == WordcodeOp::Or) destination[done] = x | word(b, done);
                        else destination[done] = ~x;
                    }
                    view = {}; view.words = destination; view.size = words * 8;
                }
            }
            const auto stitch = [&](std::size_t w, unsigned lanes, std::uint64_t answer) {
                const auto relative = produced + consumed + w * 64;
                const auto output_word = static_cast<std::size_t>(relative / 64);
                const auto shift = static_cast<unsigned>(relative % 64);
                output[output_word] |= answer << shift;
                if (shift && lanes > 64 - shift) output[output_word + 1] |= answer >> (64 - shift);
                stats.accepted += std::popcount(answer);
            };
            const auto repair_word = [&](std::size_t w, unsigned lanes) {
                ++stats.frontier_fallback_words;
                ++stats.frontier_sparse_words;
                stats.frontier_fallback_rows += lanes;
                for (std::size_t i = 0; i < plan_.exact.nodes.size(); ++i) {
                    const auto& n = plan_.exact.nodes[i];
                    auto& value = exact_values_[i];
                    switch (n.op) {
                    case WordcodeOp::False: value = 0; break;
                    case WordcodeOp::True: value = UINT64_MAX; break;
                    case WordcodeOp::Plane:
                        if (exact_to_prefix_[i] != UINT32_MAX) {
                            value = word(views_[exact_to_prefix_[i]], w);
                            // The view is reused; its bytes can be reread.
                            ++stats.cache_reuses;
                        } else {
                            const auto pos = local + w * 64;
                            const auto bytes = (pos % 8 + lanes + 7) / 8;
                            const View view{exact_planes_[i].data() + pos / 8, nullptr,
                                            bytes, static_cast<unsigned>(pos % 8), 0};
                            value = word(view, 0);
                            ++stats.loaded_planes;
                            stats.requested_payload_bytes += bytes;
                            const auto alignment = reinterpret_cast<std::uintptr_t>(view.bytes) % 64;
                            stats.estimated_cacheline_bytes += ((alignment + bytes + 63) / 64) * 64;
                        }
                        break;
                    case WordcodeOp::And: value = exact_values_[n.a] & exact_values_[n.b]; break;
                    case WordcodeOp::Or: value = exact_values_[n.a] | exact_values_[n.b]; break;
                    case WordcodeOp::Not: value = ~exact_values_[n.a]; break;
                    }
                }
                return exact_values_[plan_.exact.root];
            };
            std::size_t dirty_count = 0;
            for (std::size_t w = 0; w < words; ++w) {
                const auto lanes = static_cast<unsigned>(std::min<std::size_t>(64, take - w * 64));
                const auto valid = low_mask(lanes);
                const auto must = word(views_[plan_.prefix.must_root], w) & valid;
                const auto may = word(views_[plan_.prefix.may_root], w) & valid;
                const auto unknown = may & ~must;
                const auto unresolved = static_cast<unsigned>(std::popcount(unknown));
                stats.frontier_prefix_resolved_rows += lanes - unresolved;
                stats.refined_rows += unresolved;
                // Depth is the deepest plane required by the applicable
                // compiled stage, not its configured upper bound. Sign-only
                // predicates may finish after one bit even with a 12-bit cap.
                stats.resolved_by_bits[plan_.prefix.maximum_depth] += lanes - unresolved;
                stats.resolved_by_bits[std::max(plan_.prefix.maximum_depth,
                                               plan_.exact.maximum_depth)] += unresolved;
                auto answer = must;
                if (!unknown) ++stats.bulk_resolutions;
                else {
                    ++stats.frontier_dirty_words;
                    if (repair_.policy == FrontierRepairPolicy::SparseWords)
                        answer |= repair_word(w, lanes) & unknown;
                    else dirty_words_[dirty_count++] = static_cast<std::uint32_t>(w);
                }
                stitch(w, lanes, answer);
            }
            if (dirty_count) {
                // Compare integers: equality takes the dense path, with no
                // rounding or division ambiguity at partial final tiles.
                const bool dense = repair_.policy == FrontierRepairPolicy::DenseTiles ||
                    dirty_count * 100 >= words * repair_.dense_threshold_percent;
                if (dense) {
                    const auto repaired = dense_executor_->run_range(global + consumed, take,
                        std::span<std::uint64_t>(dense_output_).first(words));
                    stats.requested_payload_bytes += repaired.requested_payload_bytes;
                    stats.estimated_cacheline_bytes += repaired.estimated_cacheline_bytes;
                    stats.region_loads += repaired.region_loads;
                    stats.loaded_planes += repaired.loaded_planes;
                    stats.cache_reuses += repaired.cache_reuses;
                    stats.exact_evaluations += repaired.exact_evaluations;
                    stats.interval_evaluations += repaired.interval_evaluations;
                    stats.frontier_fallback_words += words;
                    stats.frontier_fallback_rows += take;
                    ++stats.frontier_dense_tiles;
                    if (repaired.kernel == "encoded-wordcode-avx2")
                        stats.kernel = "encoded-frontier-hybrid-avx2";
                }
                for (std::size_t i = 0; i < dirty_count; ++i) {
                    const auto w = dirty_words_[i];
                    const auto lanes = static_cast<unsigned>(std::min<std::size_t>(64, take - w * 64));
                    const auto unknown = word(views_[plan_.prefix.may_root], w) &
                        ~word(views_[plan_.prefix.must_root], w) & low_mask(lanes);
                    const auto exact = dense ? dense_output_[w] : repair_word(w, lanes);
                    // Disjoint from the must bits already stitched above.
                    stitch(w, lanes, exact & unknown);
                }
            }
            consumed += take;
        }
        produced += segment;
    }
    return stats;
}

} // namespace suffice
