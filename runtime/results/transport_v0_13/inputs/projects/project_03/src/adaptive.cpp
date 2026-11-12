#include "suffice/adaptive.hpp"
#include "suffice/predicate.hpp"
#include "suffice/vector.hpp"
#include "suffice/wordcode.hpp"
#include "suffice/frontier_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace suffice {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

constexpr std::array<AdaptiveBackend, 8> backends{{
    AdaptiveBackend::Dense, AdaptiveBackend::RawVector, AdaptiveBackend::PackedVector,
    AdaptiveBackend::EncodedPredicate, AdaptiveBackend::DensePredicate,
    AdaptiveBackend::Progressive, AdaptiveBackend::EncodedWordcode, AdaptiveBackend::EncodedFrontier
}};

std::uint64_t random_word(std::uint64_t& state) noexcept {
    auto value = (state += 0x9e3779b97f4a7c15ull);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

std::uint64_t checked_sum(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("adaptive resource counter overflow");
    return a + b;
}

std::uint64_t random_below(std::uint64_t& state, std::uint64_t bound) noexcept {
    // Rejection avoids modulo bias over the finite 64-bit generator output.
    // This does not make repeated pseudorandom windows independent row samples.
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t value;
    do { value = random_word(state); } while (value < threshold);
    return value % bound;
}

void append_window(std::vector<CalibrationWindow>& result, std::size_t& words,
                   std::uint64_t first, std::uint64_t take,
                   std::uint64_t population, double weight = 1.0) {
    const auto added = mask_words(take);
    if (added > std::numeric_limits<std::size_t>::max() - words)
        throw std::overflow_error("adaptive calibration masks exceed address space");
    result.push_back({first, take, words, population, weight});
    words += added;
}

// Equal-width global strata, one contiguous window per stratum. The first and
// last windows include dataset endpoints when there are at least two windows;
// a sole partial window ends at the dataset end. Interior starts are reproducibly
// jittered and aligned to a word when the stratum allows it. No values, ranges,
// physical blocks, or halos are copied/redefined for sampling.
std::vector<CalibrationWindow> legacy_sample_windows(std::uint64_t rows, std::uint64_t wanted,
                                                      unsigned count, std::uint64_t seed) {
    if (wanted >= rows) return {{0, rows, 0, rows}};
    if (!wanted) throw std::invalid_argument("calibration budget cannot cover one row");
    count = static_cast<unsigned>(std::min<std::uint64_t>(count, wanted));
    std::vector<CalibrationWindow> result;
    result.reserve(count);
    std::size_t words = 0;
    const auto width = rows / count, extra = rows % count;
    for (unsigned i = 0; i < count; ++i) {
        const auto begin = width * i + std::min<std::uint64_t>(i, extra);
        const auto length = width + (i < extra ? 1 : 0);
        const auto take = wanted / count + (i < wanted % count ? 1 : 0);
        const auto last_start = begin + length - take;
        auto first = begin;
        if (i + 1 == count) first = last_start;
        else if (i) {
            first += random_word(seed) % (length - take + 1);
            auto aligned = first - first % 64;
            if (aligned < begin) aligned += 64;
            if (aligned <= last_start) first = aligned;
        }
        append_window(result, words, first, take, length);
    }
    return result;
}

// Sample original contiguous, aligned units. Prefer complete physical blocks
// (and at least a 256-row SIMD batch); degrade alignment only when even one unit
// cannot fit. The incomplete final unit is outside the timing population, and
// endpoints are never forced into these performance windows.
std::vector<CalibrationWindow> representative_windows(std::uint64_t rows,
        std::uint64_t wanted, unsigned count, std::uint64_t seed,
        std::uint32_t block_rows, std::uint64_t& alignment,
        std::uint64_t& population_rows) {
    if (wanted >= rows) { alignment = 0; population_rows = rows; return {{0, rows, 0, rows}}; }
    if (!wanted) throw std::invalid_argument("calibration budget cannot cover one row");
    alignment = std::max<std::uint64_t>(256, block_rows);
    if (alignment > wanted) alignment = 256;
    if (alignment > wanted) alignment = 64;
    if (alignment > wanted) alignment = 1;
    const auto units = rows / alignment;
    const auto sampled_units = wanted / alignment;
    population_rows = units * alignment;
    count = static_cast<unsigned>(std::min<std::uint64_t>(count, sampled_units));
    std::vector<CalibrationWindow> result;
    result.reserve(count);
    std::size_t words = 0;
    const auto width = units / count, extra = units % count;
    const auto sampled_rows = sampled_units * alignment;
    for (unsigned i = 0; i < count; ++i) {
        const auto begin = width * i + std::min<std::uint64_t>(i, extra);
        const auto length = width + (i < extra ? 1 : 0);
        const auto take = sampled_units / count + (i < sampled_units % count ? 1 : 0);
        const auto first = begin + random_below(seed, length - take + 1);
        // Normalize an expanded stratum cost back to sampled-row units. For
        // a one-unit window, uniform unit selection and this population weight
        // remove unequal-stratum allocation bias under an additive cost model.
        // Multi-unit contiguous windows still have unequal inclusion near edges.
        const auto weight = (static_cast<double>(length) / static_cast<double>(take)) *
                            (static_cast<double>(sampled_rows) / static_cast<double>(population_rows));
        append_window(result, words, first * alignment, take * alignment,
                      length * alignment, weight);
    }
    return result;
}

std::vector<CalibrationWindow> boundary_windows(std::uint64_t rows, std::uint64_t wanted) {
    if (!wanted) return {};
    std::vector<CalibrationWindow> result;
    std::size_t words = 0;
    // Two disjoint endpoint checks; avoid a word-aligned final start when the
    // budget permits, exercising shifted source planes and a short output tail.
    auto left = wanted / 2, right = wanted - left;
    if (right > 1 && (rows - right) % 64 == 0) { ++left; --right; }
    if (left) append_window(result, words, 0, left, 0, 0);
    if (right) append_window(result, words, rows - right, right, 0, 0);
    return result;
}

std::uint64_t clamp_row(std::uint64_t row, int offset, std::uint64_t rows) noexcept {
    if (offset < 0) {
        const auto amount = static_cast<std::uint64_t>(-offset);
        return row < amount ? 0 : row - amount;
    }
    const auto amount = static_cast<std::uint64_t>(offset);
    return amount >= rows || row >= rows - amount ? rows - 1 : row + amount;
}

std::size_t used_channels(const CompiledProgram& plan,
                          std::array<std::uint32_t, max_program_nodes>& channels) {
    std::size_t count = 0;
    for (const auto& load : plan.loads()) {
        if (std::find(channels.begin(), channels.begin() + static_cast<std::ptrdiff_t>(count),
                      load.channel) == channels.begin() + static_cast<std::ptrdiff_t>(count)) {
            channels[count++] = load.channel;
        }
    }
    return count;
}

void validate_inputs(const PackedDataset& packed, const CompiledProgram& plan,
                     const RawDataset* raw, bool already_verified) {
    if (!plan.node_count()) throw std::invalid_argument("adaptive executor requires a compiled program");
    const auto& info = packed.info();
    if (raw && (raw->info().rows != info.rows || raw->info().channels != info.channels))
        throw std::invalid_argument("adaptive raw/packed dimensions differ");
    for (const auto& load : plan.loads()) {
        if (load.channel >= info.channels)
            throw std::invalid_argument("adaptive input channel outside dataset");
        const auto range = info.ranges[load.channel];
        if (load.range.lo > range.min || load.range.hi < range.max)
            throw std::invalid_argument("adaptive compiled range does not cover dataset");
    }
    if (already_verified) return;
    packed.verify_payload();
    if (!raw) return;
    std::vector<std::int32_t> decoded(info.block_rows);
    for (std::uint64_t block = 0; block < packed.block_count(); ++block) {
        const auto count = packed.block_length(block);
        for (std::uint32_t channel = 0; channel < info.channels; ++channel) {
            packed.decode_block(block, channel, decoded);
            for (std::uint32_t row = 0; row < count; ++row) {
                if (decoded[row] != raw->value(block * info.block_rows + row, channel))
                    throw std::invalid_argument("adaptive raw/packed values differ");
            }
        }
    }
}

// The packed-only reference deliberately executes the original scalar IR.
// Its three-block cache compacts used channels, avoiding the legacy full-decode
// control's allocation for every dataset channel.
std::uint64_t packed_reference(const PackedDataset& data, const CompiledProgram& plan,
                               std::uint64_t first_row, std::uint64_t row_count,
                               std::span<std::uint64_t> output,
                               std::uint64_t& workspace_bytes) {
    std::fill(output.begin(), output.end(), std::uint64_t{0});
    const auto rows = data.info().rows;
    std::array<std::uint32_t, max_program_nodes> channels{};
    const auto channel_count = used_channels(plan, channels);
    const auto block_rows = data.info().block_rows;
    const auto value_count = std::size_t{3} * channel_count * block_rows;
    std::vector<std::int32_t> decoded(value_count);
    std::array<std::uint16_t, max_program_nodes> load_slots{};
    for (std::size_t i = 0; i < plan.loads().size(); ++i) {
        load_slots[i] = static_cast<std::uint16_t>(std::find(
            channels.begin(), channels.begin() + static_cast<std::ptrdiff_t>(channel_count),
            plan.loads()[i].channel) - channels.begin());
    }
    std::array<i64, max_program_nodes> values{};
    std::array<std::uint64_t, 3> resident{};
    resident.fill(std::numeric_limits<std::uint64_t>::max());
    workspace_bytes = decoded.capacity() * sizeof(std::int32_t) + sizeof(channels) +
                      sizeof(load_slots) + sizeof(values) + sizeof(resident) +
                      max_program_nodes * sizeof(i64) + 128;
    if (!row_count) return 0;
    std::uint64_t accepted = 0;
    const auto end_row = first_row + row_count;
    for (std::uint64_t block = first_row / block_rows; block <= (end_row - 1) / block_rows; ++block) {
        const auto first = std::max(first_row, block * block_rows);
        const auto last = std::min(end_row, block * block_rows + data.block_length(block));
        const auto length = last - first;
        if (channel_count != 0) {
            const auto begin = clamp_row(first, plan.min_offset(), rows) / block_rows;
            const auto end = clamp_row(first + length - 1, plan.max_offset(), rows) / block_rows;
            if (end - begin >= 3) throw std::logic_error("adaptive oracle halo exceeds cache");
            for (auto source = begin; source <= end; ++source) {
                if (resident[source % 3] == source) continue;
                for (std::size_t channel = 0; channel < channel_count; ++channel) {
                    const auto index = ((source % 3) * channel_count + channel) * block_rows;
                    data.decode_block(source, channels[channel],
                                      std::span<std::int32_t>(decoded).subspan(index, block_rows));
                }
                resident[source % 3] = source;
            }
        }
        for (std::uint64_t local = 0; local < length; ++local) {
            const auto row = first + local;
            for (std::size_t load = 0; load < plan.loads().size(); ++load) {
                const auto source = clamp_row(row, plan.loads()[load].offset, rows);
                const auto index = (((source / block_rows) % 3) * channel_count + load_slots[load]) *
                                       block_rows + source % block_rows;
                values[load] = decoded[index];
            }
            if (plan.evaluate_exact_unchecked(
                    std::span<const i64>(values.data(), plan.loads().size())) > plan.threshold()) {
                const auto relative = row - first_row;
                output[static_cast<std::size_t>(relative / 64)] |= std::uint64_t{1} << (relative % 64);
                ++accepted;
            }
        }
    }
    return accepted;
}

struct Runner {
    AdaptiveBackend backend;
    std::size_t report_index{};
    std::unique_ptr<DenseExecutor> dense;
    std::unique_ptr<VectorExecutor> vector;
    std::unique_ptr<EncodedPredicateExecutor> encoded_predicate;
    std::unique_ptr<DensePredicateExecutor> dense_predicate;
    std::unique_ptr<ProgressiveExecutor> progressive;
    std::unique_ptr<WordcodeExecutor> wordcode;
    std::unique_ptr<FrontierExecutor> frontier;

    explicit Runner(AdaptiveBackend value) : backend(value) {}

    ExecutionStats run(std::span<std::uint64_t> output) {
        switch (backend) {
        case AdaptiveBackend::Dense: return dense->run(output);
        case AdaptiveBackend::RawVector: case AdaptiveBackend::PackedVector: return vector->run(output);
        case AdaptiveBackend::EncodedPredicate: return encoded_predicate->run(output);
        case AdaptiveBackend::DensePredicate: return dense_predicate->run(output);
        case AdaptiveBackend::Progressive: return progressive->run(output);
        case AdaptiveBackend::EncodedWordcode: return wordcode->run(output);
        case AdaptiveBackend::EncodedFrontier: return frontier->run(output);
        default: throw std::logic_error("adaptive runner has no executable backend");
        }
    }

    ExecutionStats run_range(std::uint64_t first, std::uint64_t count, std::span<std::uint64_t> output) {
        switch (backend) {
        case AdaptiveBackend::Dense: return dense->run_range(first, count, output);
        case AdaptiveBackend::RawVector: case AdaptiveBackend::PackedVector: return vector->run_range(first, count, output);
        case AdaptiveBackend::EncodedPredicate: return encoded_predicate->run_range(first, count, output);
        case AdaptiveBackend::DensePredicate: return dense_predicate->run_range(first, count, output);
        case AdaptiveBackend::Progressive: return progressive->run_range(first, count, output);
        case AdaptiveBackend::EncodedWordcode: return wordcode->run_range(first, count, output);
        case AdaptiveBackend::EncodedFrontier: return frontier->run_range(first, count, output);
        default: throw std::logic_error("adaptive runner has no ranged backend");
        }
    }

    std::uint64_t scratch_bytes() const noexcept {
        switch (backend) {
        case AdaptiveBackend::Dense: return max_program_nodes * sizeof(i64) + sizeof(PredicatePreimage);
        case AdaptiveBackend::RawVector: case AdaptiveBackend::PackedVector: return vector->scratch_bytes();
        case AdaptiveBackend::EncodedPredicate: return encoded_predicate->scratch_bytes();
        case AdaptiveBackend::DensePredicate: return dense_predicate->scratch_bytes();
        case AdaptiveBackend::Progressive: return progressive->scratch_bytes();
        case AdaptiveBackend::EncodedWordcode: return wordcode->scratch_bytes();
        case AdaptiveBackend::EncodedFrontier: return frontier->scratch_bytes();
        default: return 0;
        }
    }

    std::string description() const {
        switch (backend) {
        case AdaptiveBackend::Dense: return "native dense builtin/preimage execution with exact IR fallback";
        case AdaptiveBackend::RawVector: case AdaptiveBackend::PackedVector: return vector->description();
        case AdaptiveBackend::EncodedPredicate: return encoded_predicate->description();
        case AdaptiveBackend::DensePredicate: return dense_predicate->description();
        case AdaptiveBackend::Progressive: return progressive->lowering_description();
        case AdaptiveBackend::EncodedWordcode: return wordcode->description();
        case AdaptiveBackend::EncodedFrontier: return frontier->description();
        default: return "unavailable";
        }
    }
};

void summarize(AdaptiveCandidateReport& candidate) {
    auto sorted = candidate.samples_ms;
    std::sort(sorted.begin(), sorted.end());
    candidate.min_ms = sorted.front();
    candidate.max_ms = sorted.back();
    const auto middle = sorted.size() / 2;
    candidate.median_ms = sorted.size() % 2 ? sorted[middle]
                                           : (sorted[middle - 1] + sorted[middle]) / 2;
}

} // namespace

std::string_view adaptive_backend_name(AdaptiveBackend backend) noexcept {
    switch (backend) {
    case AdaptiveBackend::Auto: return "auto";
    case AdaptiveBackend::Dense: return "dense";
    case AdaptiveBackend::RawVector: return "raw-vector";
    case AdaptiveBackend::PackedVector: return "packed-vector";
    case AdaptiveBackend::EncodedPredicate: return "encoded-predicate";
    case AdaptiveBackend::DensePredicate: return "dense-predicate";
    case AdaptiveBackend::Progressive: return "progressive";
    case AdaptiveBackend::EncodedWordcode: return "encoded-wordcode";
    case AdaptiveBackend::EncodedFrontier: return "encoded-frontier";
    }
    return "invalid";
}

AdaptiveBackend parse_adaptive_backend(std::string_view value) {
    if (value == "auto") return AdaptiveBackend::Auto;
    for (const auto backend : backends) if (value == adaptive_backend_name(backend)) return backend;
    throw std::invalid_argument("unknown adaptive backend: " + std::string(value));
}

std::string_view calibration_mode_name(CalibrationMode mode) noexcept {
    switch (mode) { case CalibrationMode::Sampled: return "sampled"; case CalibrationMode::Full: return "full"; }
    return "invalid";
}

CalibrationMode parse_calibration_mode(std::string_view name) {
    if (name == "sampled") return CalibrationMode::Sampled;
    if (name == "full") return CalibrationMode::Full;
    throw std::invalid_argument("calibration mode must be sampled or full");
}

std::string_view sampling_policy_name(SamplingPolicy policy) noexcept {
    switch (policy) {
    case SamplingPolicy::Representative: return "representative";
    case SamplingPolicy::LegacyEndpoints: return "legacy-endpoints";
    }
    return "invalid";
}

SamplingPolicy parse_sampling_policy(std::string_view name) {
    if (name == "representative") return SamplingPolicy::Representative;
    if (name == "legacy-endpoints") return SamplingPolicy::LegacyEndpoints;
    throw std::invalid_argument("sampling policy must be representative or legacy-endpoints");
}

struct AdaptiveExecutor::Impl {
    const PackedDataset& packed;
    const CompiledProgram& plan;
    const RawDataset* raw;
    std::string builtin;
    AdaptiveOptions options;
    PredicatePlan predicate;
    WordcodePlan wordcode;
    FrontierPlan frontier;
    AdaptiveReport report;
    std::unique_ptr<Runner> selected;

    Impl(const PackedDataset& p, const CompiledProgram& c, const RawDataset* r,
         std::string_view name, AdaptiveOptions o)
        : packed(p), plan(c), raw(r), builtin(name), options(std::move(o)) {}

    std::uint64_t estimate_scratch(AdaptiveBackend backend) const {
        std::array<std::uint32_t, max_program_nodes> channels{};
        const auto used = used_channels(plan, channels);
        const auto block = packed.info().block_rows;
        switch (backend) {
        case AdaptiveBackend::Dense:
            return sizeof(DenseExecutor) + sizeof(PredicatePreimage) + max_program_nodes * sizeof(i64);
        case AdaptiveBackend::RawVector: case AdaptiveBackend::PackedVector:
            return std::uint64_t(plan.node_count()) * options.tile_rows * sizeof(i64) +
                   (backend == AdaptiveBackend::PackedVector ? std::uint64_t{3} * used * block * sizeof(std::int32_t) + 128 : 0) +
                   max_program_nodes * sizeof(std::uint32_t) + 280;
        case AdaptiveBackend::EncodedPredicate:
            return EncodedPredicateExecutor::required_scratch_bytes(predicate);
        case AdaptiveBackend::DensePredicate:
            return DensePredicateExecutor::required_scratch_bytes(predicate);
        case AdaptiveBackend::EncodedWordcode:
            return WordcodeExecutor::required_scratch_bytes(wordcode);
        case AdaptiveBackend::EncodedFrontier:
            return FrontierExecutor::required_scratch_bytes(frontier, 128, options.frontier_repair);
        case AdaptiveBackend::Progressive: {
            // Conservative bound for the generic cache, including the small
            // fixed evaluator arrays. Specialized plans often need much less.
            const auto window = 3u + static_cast<unsigned>(
                (plan.max_offset() - plan.min_offset()) / static_cast<int>(block));
            const auto regions = std::max<std::uint64_t>(1, used * window);
            return regions * (std::uint64_t(block) * sizeof(std::uint32_t) + 128) +
                   used * sizeof(std::uint32_t) + block + regions +
                   2 * ((std::uint64_t(block) + 63) / 64) * sizeof(std::uint64_t) +
                   options.execution.stages.size() * sizeof(unsigned) +
                   3 * max_program_nodes * sizeof(Bounds) + max_program_nodes * sizeof(i64) +
                   sizeof(ProgressiveExecutor) + sizeof(PredicatePreimage);
        }
        default: return 0;
        }
    }

    std::unique_ptr<Runner> make_runner(AdaptiveBackend backend) const {
        auto runner = std::make_unique<Runner>(backend);
        switch (backend) {
        case AdaptiveBackend::Dense:
            runner->dense = std::make_unique<DenseExecutor>(*raw, plan, builtin, true, true, true); break;
        case AdaptiveBackend::RawVector:
            runner->vector = std::make_unique<VectorExecutor>(*raw, plan, options.tile_rows, true, true); break;
        case AdaptiveBackend::PackedVector:
            runner->vector = std::make_unique<VectorExecutor>(packed, plan, options.tile_rows); break;
        case AdaptiveBackend::EncodedPredicate:
            runner->encoded_predicate = std::make_unique<EncodedPredicateExecutor>(packed, predicate); break;
        case AdaptiveBackend::DensePredicate:
            runner->dense_predicate = std::make_unique<DensePredicateExecutor>(*raw, predicate, true, true); break;
        case AdaptiveBackend::Progressive:
            runner->progressive = std::make_unique<ProgressiveExecutor>(packed, plan, options.execution); break;
        case AdaptiveBackend::EncodedWordcode:
            runner->wordcode = std::make_unique<WordcodeExecutor>(packed, wordcode); break;
        case AdaptiveBackend::EncodedFrontier:
            runner->frontier = std::make_unique<FrontierExecutor>(packed, frontier, 128, true,
                                                                options.frontier_repair); break;
        default: throw std::invalid_argument("cannot construct automatic backend directly");
        }
        return runner;
    }

    void prepare(Clock::time_point total_start) {
        if (adaptive_backend_name(options.backend) == "invalid" || calibration_mode_name(options.calibration) == "invalid" ||
            sampling_policy_name(options.sample_policy) == "invalid")
            throw std::invalid_argument("invalid adaptive backend, calibration, or sampling policy enum");
        if (!options.rounds || options.rounds > 9)
            throw std::invalid_argument("adaptive rounds must be in 1..9");
        if (!std::isfinite(options.tie_fraction) || options.tie_fraction < 0 || options.tie_fraction > 0.25)
            throw std::invalid_argument("adaptive tie fraction must be finite and in [0, 0.25]");
        if (!options.tile_rows || options.tile_rows > 65536)
            throw std::invalid_argument("adaptive tile rows must be in 1..65536");
        if (options.frontier_prefix_bits > 32)
            throw std::invalid_argument("adaptive frontier prefix must be in 0..32");
        if (frontier_repair_policy_name(options.frontier_repair.policy) == "invalid" ||
            !options.frontier_repair.dense_threshold_percent || options.frontier_repair.dense_threshold_percent > 100)
            throw std::invalid_argument("adaptive frontier repair policy/threshold is invalid");
        if (!options.max_candidate_scratch_bytes || !options.max_calibration_workspace_bytes)
            throw std::invalid_argument("adaptive workspace caps must be positive");
        if (!options.sample_rows || !options.sample_windows || options.sample_windows > 64 ||
            !options.max_calibration_row_evaluations)
            throw std::invalid_argument("sample rows/work budget must be positive and windows must be in 1..64");
        if (!std::isfinite(options.calibration_time_budget_ms) || options.calibration_time_budget_ms < 0)
            throw std::invalid_argument("calibration time budget must be finite and nonnegative");
        unsigned previous = 0;
        for (const auto bits : options.execution.stages) {
            if (bits <= previous || bits > 32)
                throw std::invalid_argument("adaptive refinement stages must increase in 1..32");
            previous = bits;
        }
        if (previous != 32) throw std::invalid_argument("adaptive refinement stages must end at 32");

        report.forced = options.backend != AdaptiveBackend::Auto;
        report.requested_rounds = report.forced ? 0 : options.rounds;
        report.calibration = report.forced ? CalibrationMode::Full : options.calibration;
        report.sample_policy = options.sample_policy;
        report.dataset_rows = packed.info().rows;
        report.validation_performed = !options.inputs_already_verified;
        report.tie_fraction = options.tie_fraction;
        report.max_candidate_scratch_bytes = options.max_candidate_scratch_bytes;
        report.max_calibration_workspace_bytes = options.max_calibration_workspace_bytes;
        report.sample_seed = options.sample_seed;
        report.calibration_time_budget_ms = options.calibration_time_budget_ms;
        const bool sampled = !report.forced && options.calibration == CalibrationMode::Sampled;
        report.calibration_row_budget = sampled ? options.max_calibration_row_evaluations : 0;
        const auto validation_start = Clock::now();
        validate_inputs(packed, plan, raw, options.inputs_already_verified);
        report.validation_ms = elapsed_ms(validation_start);

        const auto compile_start = Clock::now();
        predicate = compile_predicate(plan);
        wordcode = compile_wordcode(predicate);
        frontier = compile_frontier(predicate, options.frontier_prefix_bits);
        std::vector<std::unique_ptr<Runner>> runners;
        runners.reserve(backends.size());
        report.candidates.reserve(backends.size());
        std::uint64_t available = 0;
        for (const auto backend : backends) {
            AdaptiveCandidateReport candidate;
            candidate.backend = backend;
            const bool needs_raw = backend == AdaptiveBackend::Dense || backend == AdaptiveBackend::RawVector ||
                                   backend == AdaptiveBackend::DensePredicate;
            const bool needs_predicate = backend == AdaptiveBackend::EncodedPredicate ||
                                         backend == AdaptiveBackend::DensePredicate;
            if (needs_raw && !raw) candidate.reason = "raw representation was not supplied";
            else if (needs_predicate && !predicate.complete) candidate.reason = "predicate lowering unavailable: " + predicate.reason;
            else if (backend == AdaptiveBackend::EncodedWordcode && !wordcode.complete)
                candidate.reason = "wordcode lowering unavailable: " + wordcode.reason;
            else if (backend == AdaptiveBackend::EncodedFrontier && !frontier.complete)
                candidate.reason = "frontier lowering unavailable: " + frontier.reason;
            else {
                candidate.estimated_scratch_bytes = estimate_scratch(backend);
                if (candidate.estimated_scratch_bytes > options.max_candidate_scratch_bytes)
                    candidate.reason = "conservative workspace estimate exceeds the candidate scratch cap";
                else { candidate.eligible = true; ++available; }
            }
            report.candidates.push_back(std::move(candidate));
        }
        if (!available) throw std::invalid_argument("no adaptive candidate fits representations and candidate workspace cap");
        auto requested = packed.info().rows;
        std::uint64_t probe_rows = 0;
        if (sampled) {
            // This worst-case reservation also covers later-eliminated candidates.
            // The reference contributes one S, warmup contributes K*S, and at
            // most R rounds contribute R*K*S. Separate boundary probes add
            // (1+K)*P. Actual racing and alignment rounding can use fewer rows.
            const auto multiplicity = 1 + available * (std::uint64_t(options.rounds) + 1);
            requested = std::min({requested, options.sample_rows,
                                  options.max_calibration_row_evaluations / multiplicity});
            if (packed.info().rows && !requested)
                throw std::invalid_argument("calibration row budget is too small for all eligible candidates");
            if (options.sample_policy == SamplingPolicy::Representative && requested < packed.info().rows) {
                // At most 128 boundary rows; scale their footprint down for tiny
                // timing samples. Reserve at least one timing row before probes.
                // If fewer than two probe rows fit, omit both and expose zero.
                const auto desired = std::min<std::uint64_t>(128, std::max<std::uint64_t>(2, requested / 8));
                const auto affordable = (options.max_calibration_row_evaluations - multiplicity) / (1 + available);
                probe_rows = std::min({packed.info().rows, desired, affordable});
                if (probe_rows < 2) probe_rows = 0;
                requested = std::min(requested,
                    (options.max_calibration_row_evaluations - probe_rows * (1 + available)) / multiplicity);
            }
        }
        if (sampled && options.sample_policy == SamplingPolicy::Representative) {
            report.windows = representative_windows(packed.info().rows, requested, options.sample_windows,
                options.sample_seed, packed.info().block_rows, report.sample_alignment_rows,
                report.sample_population_rows);
            report.boundary_probes = boundary_windows(packed.info().rows, probe_rows);
        } else {
            report.windows = legacy_sample_windows(packed.info().rows, requested,
                sampled ? options.sample_windows : 1, options.sample_seed);
            report.sample_population_rows = packed.info().rows;
        }
        for (const auto& window : report.windows) report.calibrated_rows += window.row_count;
        report.sample_unrepresented_rows = packed.info().rows - report.sample_population_rows;
        report.boundary_probe_rows = probe_rows;
        report.reference_is_full = report.calibrated_rows == packed.info().rows;
        report.race_enabled = sampled && options.successive_halving && !report.reference_is_full &&
                              options.rounds > report.minimum_rounds_before_elimination;
        std::size_t words = 0, probe_words = 0;
        for (const auto& window : report.windows) words += mask_words(window.row_count);
        for (const auto& window : report.boundary_probes) probe_words += mask_words(window.row_count);
        const auto combined_words = checked_sum(words, probe_words);
        if (combined_words > std::numeric_limits<std::uint64_t>::max() / 16)
            throw std::overflow_error("calibration mask workspace overflow");
        std::array<std::uint32_t, max_program_nodes> channels{};
        const auto used = used_channels(plan, channels);
        const auto oracle_estimate = raw ? std::uint64_t{4096} :
            std::uint64_t{3} * used * packed.info().block_rows * sizeof(std::int32_t) + 4096;
        auto reserved_workspace = checked_sum(combined_words * 16, oracle_estimate);
        if (reserved_workspace > options.max_calibration_workspace_bytes)
            throw std::invalid_argument("reference and masks exceed the calibration workspace cap");
        for (std::size_t index = 0; index < report.candidates.size(); ++index) {
            auto& candidate = report.candidates[index];
            if (!candidate.eligible) continue;
            if (report.forced && candidate.backend != options.backend) {
                candidate.reason = "available, but not prepared in forced mode";
                continue;
            }
            if (candidate.estimated_scratch_bytes > options.max_calibration_workspace_bytes - reserved_workspace) {
                candidate.eligible = false;
                candidate.reason = "cumulative conservative workspaces exceed calibration workspace cap";
                continue;
            }
            reserved_workspace += candidate.estimated_scratch_bytes;
            auto runner = make_runner(candidate.backend);
            runner->report_index = index;
            candidate.description = runner->description();
            candidate.scratch_bytes = runner->scratch_bytes();
            candidate.samples_ms.reserve(report.requested_rounds);
            candidate.window_samples_ms.reserve(report.requested_rounds * report.windows.size());
            candidate.reason = report.forced ? "forced backend; full output check without timed tuning" :
                sampled ? "eligible for bounded stratified global-range calibration" : "eligible for full-dataset calibration";
            runners.push_back(std::move(runner));
        }
        if (runners.empty()) {
            if (report.forced) {
                const auto found = std::find_if(report.candidates.begin(), report.candidates.end(),
                    [&](const auto& candidate) { return candidate.backend == options.backend; });
                throw std::invalid_argument("forced adaptive backend unavailable: " +
                    (found == report.candidates.end() ? std::string("invalid backend") : found->reason));
            }
            throw std::invalid_argument("no candidate fits cumulative calibration workspace cap");
        }
        std::vector<std::uint64_t> reference(words), output(words);
        std::vector<std::uint64_t> probe_reference(probe_words), probe_output(probe_words);
        std::vector<std::uint64_t> reference_counts(report.windows.size());
        std::vector<std::uint64_t> probe_reference_counts(report.boundary_probes.size());
        report.compile_prepare_ms = elapsed_ms(compile_start);
        const auto oracle_start = Clock::now();
        std::uint64_t oracle_workspace = 0;
        std::unique_ptr<DenseExecutor> oracle;
        if (raw) oracle = std::make_unique<DenseExecutor>(*raw, plan, std::string_view{}, false, false, true);
        auto make_reference = [&](const auto& windows, auto& mask, auto& counts, bool boundary) {
            for (std::size_t index = 0; index < windows.size(); ++index) {
                const auto& window = windows[index];
                auto destination = std::span<std::uint64_t>(mask).subspan(window.mask_word_offset, mask_words(window.row_count));
                if (oracle) {
                    const auto stats = oracle->run_range(window.first_row, window.row_count, destination);
                    counts[index] = stats.accepted;
                    oracle_workspace = std::max(oracle_workspace, stats.peak_scratch_bytes);
                } else {
                    std::uint64_t scratch = 0;
                    counts[index] = packed_reference(packed, plan, window.first_row,
                                                     window.row_count, destination, scratch);
                    oracle_workspace = std::max(oracle_workspace, scratch);
                }
                if (boundary) report.boundary_oracle_row_evaluations += window.row_count;
                else report.reference_accepted += counts[index];
                report.oracle_row_evaluations += window.row_count;
            }
        };
        make_reference(report.windows, reference, reference_counts, false);
        make_reference(report.boundary_probes, probe_reference, probe_reference_counts, true);
        report.reference_checksum = mask_checksum(reference);
        report.oracle_ms = elapsed_ms(oracle_start);
        oracle.reset();

        auto execute_windows = [&](Runner& runner, bool timed, bool boundary) {
            auto& candidate = report.candidates[runner.report_index];
            const auto& windows = boundary ? report.boundary_probes : report.windows;
            auto& output_mask = boundary ? probe_output : output;
            const auto& reference_mask = boundary ? probe_reference : reference;
            const auto& counts = boundary ? probe_reference_counts : reference_counts;
            double duration = 0;
            for (std::size_t index = 0; index < windows.size(); ++index) {
                const auto& window = windows[index];
                auto actual = std::span<std::uint64_t>(output_mask).subspan(window.mask_word_offset, mask_words(window.row_count));
                const auto expected = std::span<const std::uint64_t>(reference_mask).subspan(window.mask_word_offset, mask_words(window.row_count));
                ExecutionStats stats;
                if (timed) {
                    const auto start = Clock::now();
                    stats = runner.run_range(window.first_row, window.row_count, actual);
                    const auto elapsed = elapsed_ms(start);
                    duration += elapsed * window.timing_weight;
                    candidate.window_samples_ms.push_back(elapsed);
                } else stats = runner.run_range(window.first_row, window.row_count, actual);
                if (!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()) ||
                    stats.accepted != counts[index] || stats.rows != window.row_count)
                    throw std::runtime_error("adaptive exact range-output verification failed for " +
                                             std::string(adaptive_backend_name(runner.backend)));
                candidate.kernel = stats.kernel;
                candidate.scratch_bytes = std::max(candidate.scratch_bytes, stats.peak_scratch_bytes);
                ++candidate.mask_checks;
                if (timed) candidate.timed_row_evaluations += window.row_count;
                else candidate.warmup_row_evaluations += window.row_count;
                if (boundary) {
                    candidate.boundary_probe_row_evaluations += window.row_count;
                    ++candidate.boundary_mask_checks;
                }
            }
            candidate.verified = true;
            if (timed) candidate.samples_ms.push_back(duration);
        };
        const auto warmup_start = Clock::now();
        for (auto& runner : runners) {
            execute_windows(*runner, false, true);
            execute_windows(*runner, false, false);
        }
        report.warmup_ms = elapsed_ms(warmup_start);

        std::uint64_t all_workspace = 0;
        for (const auto& runner : runners)
            all_workspace = checked_sum(all_workspace, report.candidates[runner->report_index].scratch_bytes);
        const auto masks_workspace = std::uint64_t(reference.capacity() + output.capacity() +
                                                  probe_reference.capacity() + probe_output.capacity()) * sizeof(std::uint64_t);
        report.calibration_workspace_bytes = checked_sum(checked_sum(masks_workspace, all_workspace), oracle_workspace);
        if (report.calibration_workspace_bytes > options.max_calibration_workspace_bytes)
            throw std::logic_error("actual modeled workspaces exceed reserved calibration cap");

        std::vector<std::size_t> active;
        for (std::size_t i = 0; i < runners.size(); ++i) active.push_back(i);
        std::size_t selected_index = 0;
        if (!report.forced) {
            const auto calibration_start = Clock::now();
            std::uint64_t random_state = options.sample_seed ^ 0x524143455f563033ull;
            for (unsigned round = 0; round < options.rounds; ++round) {
                auto order = active;
                // Deterministic Fisher-Yates changes predecessors as well as
                // positions; report records samples in chronological order.
                for (std::size_t left = order.size(); left > 1; --left)
                    std::swap(order[left - 1], order[random_word(random_state) % left]);
                for (const auto index : order) execute_windows(*runners[index], true, false);
                ++report.rounds;
                for (const auto index : active) summarize(report.candidates[runners[index]->report_index]);
                if (sampled && options.calibration_time_budget_ms > 0 &&
                    elapsed_ms(calibration_start) >= options.calibration_time_budget_ms) {
                    report.time_budget_reached = true;
                    break;
                }
                // Two-sample medians average their observations, so a single
                // elapsed-time spike can discard a genuinely fast candidate.
                // Obtain three observations before any irreversible pruning.
                if (report.race_enabled && round + 1 >= report.minimum_rounds_before_elimination &&
                    round + 1 < options.rounds && active.size() > 1) {
                    std::stable_sort(active.begin(), active.end(), [&](auto a, auto b) {
                        return report.candidates[runners[a]->report_index].median_ms <
                               report.candidates[runners[b]->report_index].median_ms;
                    });
                    const auto best = report.candidates[runners[active.front()]->report_index].median_ms;
                    auto keep = (active.size() + 1) / 2;
                    while (keep < active.size() && report.candidates[runners[active[keep]]->report_index].median_ms <=
                           best * (1 + options.tie_fraction)) ++keep;
                    for (std::size_t i = keep; i < active.size(); ++i) {
                        auto& eliminated = report.candidates[runners[active[i]]->report_index];
                        eliminated.eliminated = true;
                        eliminated.eliminated_after_round = round + 1;
                    }
                    active.resize(keep);
                }
            }
            double best = std::numeric_limits<double>::infinity();
            for (const auto index : active)
                best = std::min(best, report.candidates[runners[index]->report_index].median_ms);
            const auto limit = best * (1 + options.tie_fraction);
            bool found = false;
            for (const auto index : active) {
                const auto& candidate = report.candidates[runners[index]->report_index];
                if (candidate.median_ms > limit) continue;
                if (!found) { selected_index = index; found = true; continue; }
                const auto& chosen = report.candidates[runners[selected_index]->report_index];
                if (candidate.backend == AdaptiveBackend::Dense ||
                    (chosen.backend != AdaptiveBackend::Dense && candidate.scratch_bytes < chosen.scratch_bytes))
                    selected_index = index;
            }
            report.calibration_ms = elapsed_ms(calibration_start);
        }
        for (const auto& candidate : report.candidates) {
            report.warmup_row_evaluations = checked_sum(report.warmup_row_evaluations, candidate.warmup_row_evaluations);
            report.timed_row_evaluations = checked_sum(report.timed_row_evaluations, candidate.timed_row_evaluations);
            report.boundary_candidate_row_evaluations = checked_sum(report.boundary_candidate_row_evaluations,
                                                                     candidate.boundary_probe_row_evaluations);
        }
        report.total_calibration_row_evaluations = checked_sum(report.oracle_row_evaluations,
            checked_sum(report.warmup_row_evaluations, report.timed_row_evaluations));
        if (sampled && report.total_calibration_row_evaluations > report.calibration_row_budget)
            throw std::logic_error("sampled calibration exceeded its requested-row budget");
        selected = std::move(runners[selected_index]);
        auto& chosen = report.candidates[selected->report_index];
        chosen.selected = true;
        report.selected = selected->backend;
        report.selected_scratch_bytes = chosen.scratch_bytes;
        runners.clear();
        std::vector<std::uint64_t>().swap(reference);
        std::vector<std::uint64_t>().swap(output);
        std::vector<std::uint64_t>().swap(probe_reference);
        std::vector<std::uint64_t>().swap(probe_output);
        report.total_prepare_ms = elapsed_ms(total_start);
    }

};

AdaptiveExecutor::AdaptiveExecutor(const PackedDataset& packed, const CompiledProgram& plan,
                                   const RawDataset* raw, std::string_view builtin,
                                   AdaptiveOptions options) {
    const auto start = Clock::now();
    impl_ = std::make_unique<Impl>(packed, plan, raw, builtin, std::move(options));
    impl_->prepare(start);
}

AdaptiveExecutor::~AdaptiveExecutor() = default;
AdaptiveExecutor::AdaptiveExecutor(AdaptiveExecutor&&) noexcept = default;
AdaptiveExecutor& AdaptiveExecutor::operator=(AdaptiveExecutor&&) noexcept = default;

ExecutionStats AdaptiveExecutor::run(std::span<std::uint64_t> mask) {
    if (!impl_) throw std::logic_error("cannot run a moved-from adaptive executor");
    return impl_->selected->run(mask);
}

ExecutionStats AdaptiveExecutor::run_range(std::uint64_t first_row, std::uint64_t row_count,
                                           std::span<std::uint64_t> mask) {
    if (!impl_) throw std::logic_error("cannot run a moved-from adaptive executor");
    return impl_->selected->run_range(first_row, row_count, mask);
}

const AdaptiveReport& AdaptiveExecutor::report() const noexcept { return impl_->report; }

std::string AdaptiveExecutor::description() const {
    const auto& report = impl_->report;
    const auto found = std::find_if(report.candidates.begin(), report.candidates.end(),
                                   [](const auto& candidate) { return candidate.selected; });
    return std::string(report.forced ? "forced " : report.reference_is_full ? "full-calibrated " : "sample-calibrated ") +
           std::string(adaptive_backend_name(report.selected)) + "; " + found->description;
}

} // namespace suffice
