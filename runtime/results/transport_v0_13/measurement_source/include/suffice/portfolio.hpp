#pragma once

// A bounded, allocation-free selector for already prepared exact kernels.
// Calibration observations are selection data, never held-out speed evidence.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace suffice {

inline constexpr unsigned portfolio_max_candidates = 8;
inline constexpr unsigned portfolio_max_cycles = 8;
inline constexpr unsigned portfolio_max_cycle_samples = 16;

struct PortfolioCandidate {
    std::string_view name;
    void* context{};
    std::uint64_t (*run_range)(void*, std::uint64_t, std::uint64_t,
                               std::span<std::uint64_t>){};
    bool uses_simd{};
};

struct PortfolioCalibrationOptions {
    unsigned cycles{3};
    unsigned minimum_cycles{2};
    unsigned warmups_per_candidate{1};
    unsigned minimum_improvement_basis_points{200};
    // Zero budgets or zero expected reuse disable calibration. These three
    // quantities must be chosen by the caller for its particular workload.
    std::uint64_t max_row_evaluations{};
    std::uint64_t max_elapsed_nanoseconds{};
    std::uint64_t expected_remaining_calls{};
    std::uint64_t order_seed{0x4f1bbcdc12345678ULL};
};

enum class PortfolioReason : std::uint8_t {
    Selected, NoExpectedReuse, EmptyRange, SingleCandidate,
    InsufficientRowBudget, InsufficientCycles, NoConsistentImprovement,
    NotAmortized
};
enum class PortfolioStopReason : std::uint8_t {
    Completed, Disabled, RowBudget, TimeBudget
};

[[nodiscard]] inline std::string_view portfolio_reason_name(PortfolioReason reason) noexcept {
    switch (reason) {
    case PortfolioReason::Selected: return "selected";
    case PortfolioReason::NoExpectedReuse: return "no_expected_reuse";
    case PortfolioReason::EmptyRange: return "empty_range";
    case PortfolioReason::SingleCandidate: return "single_candidate";
    case PortfolioReason::InsufficientRowBudget: return "insufficient_row_budget";
    case PortfolioReason::InsufficientCycles: return "insufficient_cycles";
    case PortfolioReason::NoConsistentImprovement: return "no_consistent_improvement";
    case PortfolioReason::NotAmortized: return "not_amortized";
    }
    return "invalid";
}
[[nodiscard]] inline std::string_view portfolio_stop_reason_name(PortfolioStopReason reason) noexcept {
    switch (reason) {
    case PortfolioStopReason::Completed: return "completed";
    case PortfolioStopReason::Disabled: return "disabled";
    case PortfolioStopReason::RowBudget: return "row_budget";
    case PortfolioStopReason::TimeBudget: return "time_budget";
    }
    return "invalid";
}

struct PortfolioCycleObservation {
    unsigned round_count{};
    std::array<std::array<std::uint8_t, portfolio_max_candidates>,
               portfolio_max_cycle_samples> orders{};
    // Medians represent uint64 nanosecond samples. The largest converted
    // uint64 may round upward to 2^64 in double; that endpoint is valid.
    std::array<double, portfolio_max_candidates> median_nanoseconds{};
};

struct PortfolioCandidateMeasurements {
    std::uint64_t calls{}, warmup_calls{}, timed_samples{}, row_evaluations{};
    std::uint64_t timed_nanoseconds{}, warmup_nanoseconds{};
    std::array<unsigned, portfolio_max_cycles> cycle_samples{};
    std::array<double, portfolio_max_cycles> cycle_median_nanoseconds{};
    std::array<std::array<std::uint64_t, portfolio_max_cycle_samples>,
               portfolio_max_cycles> sample_nanoseconds{};
};

struct PortfolioDecision {
    std::size_t incumbent_index{}, proposed_index{}, selected_index{};
    bool changed{};
    PortfolioReason reason{PortfolioReason::NoConsistentImprovement};
    std::uint64_t estimated_break_even_calls{};
    bool break_even_saturated{};
    double incumbent_median_nanoseconds{}, proposed_median_nanoseconds{};
    double estimated_saving_nanoseconds{};
};

struct PortfolioCalibrationReport : PortfolioDecision {
    std::size_t candidate_count{};
    PortfolioStopReason stop_reason{PortfolioStopReason::Completed};
    unsigned completed_cycles{};
    std::uint64_t total_calls{}, row_evaluations{}, elapsed_nanoseconds{};
    std::array<PortfolioCandidateMeasurements, portfolio_max_candidates> candidates{};
    std::array<PortfolioCycleObservation, portfolio_max_cycles> cycles{};
};

namespace portfolio_detail {

inline void validate_options(const PortfolioCalibrationOptions& options) {
    if (options.cycles < 2 || options.cycles > portfolio_max_cycles ||
        options.minimum_cycles < 2 || options.minimum_cycles > options.cycles ||
        options.warmups_per_candidate > 4 ||
        options.minimum_improvement_basis_points >= 10000)
        throw std::invalid_argument("invalid bounded portfolio calibration limits");
}

inline std::size_t validate_range(std::uint64_t rows, std::uint64_t first,
                                  std::uint64_t count, std::size_t words) {
    if (first > rows || count > rows - first)
        throw std::invalid_argument("portfolio row range is outside dataset");
    const auto required = count / 64 + (count % 64 != 0);
    if (required > std::numeric_limits<std::size_t>::max() || words != required)
        throw std::invalid_argument("portfolio output mask shape is invalid");
    if (words > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t))
        throw std::invalid_argument("portfolio output byte size overflows");
    return static_cast<std::size_t>(required);
}

inline bool overlap(std::span<const std::uint64_t> a,
                     std::span<const std::uint64_t> b) noexcept {
    if (a.empty() || b.empty()) return false;
    const auto x = reinterpret_cast<std::uintptr_t>(a.data());
    const auto y = reinterpret_cast<std::uintptr_t>(b.data());
    // Subtraction avoids overflowing either span's end address.
    return x <= y ? y - x < a.size_bytes() : x - y < b.size_bytes();
}

template<class T, std::size_t N>
inline double median(std::array<T, N> values, unsigned count) {
    if (count == 0 || count > N)
        throw std::invalid_argument("invalid bounded portfolio median sample count");
    // At most sixteen samples: insertion sort keeps all accesses visibly
    // bounded, and avoids generic std::sort's larger internal insertion window.
    for (unsigned i = 1; i < count; ++i) {
        const auto value = values[i];
        auto slot = i;
        while (slot && value < values[slot - 1]) {
            values[slot] = values[slot - 1];
            --slot;
        }
        values[slot] = value;
    }
    const auto middle = count / 2;
    return count % 2 ? static_cast<double>(values[middle]) :
        static_cast<double>(values[middle - 1]) * 0.5 +
        static_cast<double>(values[middle]) * 0.5;
}

inline std::uint64_t random(std::uint64_t& seed) noexcept {
    auto value = (seed += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

template<std::size_t N>
inline void shuffle(std::array<unsigned, N>& values, unsigned count,
                     std::uint64_t& seed) noexcept {
    for (unsigned left = count; left > 1; --left)
        std::swap(values[left - 1], values[random(seed) % left]);
}

inline unsigned cycle_rows(unsigned count) noexcept {
    return count == 1 || count % 2 == 0 ? count : 2 * count;
}

inline PortfolioCycleObservation timing_cycle(unsigned count, std::uint64_t& seed) noexcept {
    PortfolioCycleObservation result;
    result.round_count = cycle_rows(count);
    std::array<unsigned, portfolio_max_candidates> base{}, labels{};
    std::array<unsigned, portfolio_max_cycle_samples> row_order{};
    for (unsigned i = 0; i < count; ++i) {
        base[i] = i == 0 ? 0 : i % 2 ? (i + 1) / 2 : count - i / 2;
        labels[i] = i;
    }
    for (unsigned i = 0; i < result.round_count; ++i) row_order[i] = i;
    shuffle(labels, count, seed);
    shuffle(row_order, result.round_count, seed);
    for (unsigned round = 0; round < result.round_count; ++round) {
        const auto row = row_order[round];
        const auto shift = row % count;
        const bool reversed = row >= count;
        for (unsigned column = 0; column < count; ++column) {
            const auto index = reversed ? count - 1 - column : column;
            result.orders[round][column] = static_cast<std::uint8_t>(labels[(base[index] + shift) % count]);
        }
    }
    return result;
}

using Clock = std::chrono::steady_clock;
inline std::uint64_t elapsed(Clock::time_point start, Clock::time_point end) noexcept {
    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return duration > 0 ? static_cast<std::uint64_t>(duration) : 0;
}

inline std::uint64_t add_saturated(std::uint64_t a, std::uint64_t b) noexcept {
    return b > std::numeric_limits<std::uint64_t>::max() - a ?
        std::numeric_limits<std::uint64_t>::max() : a + b;
}

} // namespace portfolio_detail

// Pure decision rule: useful for deterministic tests and audit. This function
// does not select a runtime kernel. Only BoundedPortfolio::calibrate can commit
// a selection, using fresh, complete observations collected during that call.
[[nodiscard]] inline PortfolioDecision decide_portfolio(
    std::span<const PortfolioCycleObservation> observations,
    std::size_t candidate_count, std::size_t incumbent_index,
    std::uint64_t calibration_elapsed_nanoseconds,
    const PortfolioCalibrationOptions& options) {
    portfolio_detail::validate_options(options);
    if (candidate_count == 0 || candidate_count > portfolio_max_candidates ||
        incumbent_index >= candidate_count || observations.size() > options.cycles)
        throw std::invalid_argument("invalid portfolio decision dimensions");
    // Keep diagnostic inputs in the runtime timer's representable domain.
    // Besides rejecting physically unrepresentable observations, this ensures
    // that scaling by 10000 below cannot overflow when long double has the
    // same range as double (for example with MSVC). Do not cast this rounded
    // endpoint back to uint64: double(UINT64_MAX) may equal 2^64.
    const auto maximum_observation = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    for (const auto& cycle : observations)
        for (std::size_t i = 0; i < candidate_count; ++i)
            if (!std::isfinite(cycle.median_nanoseconds[i]) || cycle.median_nanoseconds[i] < 0 ||
                cycle.median_nanoseconds[i] > maximum_observation)
                throw std::invalid_argument("invalid portfolio timing observation");
    PortfolioDecision result;
    result.incumbent_index = result.proposed_index = result.selected_index = incumbent_index;
    if (!options.expected_remaining_calls) {
        result.reason = PortfolioReason::NoExpectedReuse;
        return result;
    }
    if (candidate_count == 1) {
        result.reason = PortfolioReason::SingleCandidate;
        return result;
    }
    if (observations.size() < options.minimum_cycles) {
        result.reason = PortfolioReason::InsufficientCycles;
        return result;
    }
    auto aggregate = [&](std::size_t index) {
        std::array<double, portfolio_max_cycles> medians{};
        for (std::size_t i = 0; i < observations.size(); ++i)
            medians[i] = observations[i].median_nanoseconds[index];
        return portfolio_detail::median(medians, static_cast<unsigned>(observations.size()));
    };
    result.incumbent_median_nanoseconds = aggregate(incumbent_index);
    result.proposed_median_nanoseconds = result.incumbent_median_nanoseconds;
    const auto permitted_basis_points = static_cast<long double>(10000 - options.minimum_improvement_basis_points);
    for (std::size_t candidate = 0; candidate < candidate_count; ++candidate) {
        if (candidate == incumbent_index) continue;
        bool consistent = true;
        for (const auto& cycle : observations) {
            const auto baseline = static_cast<long double>(cycle.median_nanoseconds[incumbent_index]);
            const auto alternative = static_cast<long double>(cycle.median_nanoseconds[candidate]);
            // Zero timer observations cannot establish a useful cost estimate.
            if (!(alternative > 0 && alternative < baseline && alternative * 10000.0L <= baseline * permitted_basis_points)) {
                consistent = false;
                break;
            }
        }
        if (!consistent) continue;
        const auto cost = aggregate(candidate);
        if (cost < result.proposed_median_nanoseconds ||
            (cost == result.proposed_median_nanoseconds && candidate < result.proposed_index)) {
            result.proposed_index = candidate;
            result.proposed_median_nanoseconds = cost;
        }
    }
    if (result.proposed_index == incumbent_index) return result;
    result.estimated_saving_nanoseconds = result.incumbent_median_nanoseconds - result.proposed_median_nanoseconds;
    const auto saving = static_cast<long double>(result.estimated_saving_nanoseconds);
    const auto break_even = std::ceil(static_cast<long double>(calibration_elapsed_nanoseconds) / saving);
    // Conversion at UINT64_MAX is unsafe on platforms whose long double is
    // double: UINT64_MAX rounds to 2^64. Saturate conservatively at that edge.
    if (!(break_even < static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))) {
        result.estimated_break_even_calls = std::numeric_limits<std::uint64_t>::max();
        result.break_even_saturated = true;
    } else result.estimated_break_even_calls = static_cast<std::uint64_t>(break_even);
    if (result.break_even_saturated || options.expected_remaining_calls < result.estimated_break_even_calls) {
        result.reason = PortfolioReason::NotAmortized;
        return result;
    }
    result.selected_index = result.proposed_index;
    result.changed = true;
    result.reason = PortfolioReason::Selected;
    return result;
}

class BoundedPortfolio {
public:
    BoundedPortfolio(std::uint64_t rows, std::span<const PortfolioCandidate> candidates,
                     std::size_t initial_index = 0)
        : rows_(rows), count_(candidates.size()), selected_(initial_index) {
        if (count_ == 0 || count_ > portfolio_max_candidates || initial_index >= count_)
            throw std::invalid_argument("invalid bounded portfolio candidate count or index");
        for (std::size_t i = 0; i < count_; ++i) {
            if (candidates[i].name.empty() || !candidates[i].context || !candidates[i].run_range)
                throw std::invalid_argument("invalid bounded portfolio candidate");
            for (std::size_t previous = 0; previous < i; ++previous)
                if (candidates[previous].name == candidates[i].name)
                    throw std::invalid_argument("duplicate bounded portfolio candidate name");
            candidates_[i] = candidates[i];
        }
    }

    [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t candidate_count() const noexcept { return count_; }
    [[nodiscard]] std::size_t selected_index() const noexcept { return selected_; }
    [[nodiscard]] std::string_view selected_name() const noexcept { return candidates_[selected_].name; }
    [[nodiscard]] bool uses_simd() const noexcept { return candidates_[selected_].uses_simd; }
    [[nodiscard]] const PortfolioCandidate& candidate(std::size_t index) const {
        if (index >= count_) throw std::invalid_argument("portfolio candidate index is out of bounds");
        return candidates_[index];
    }

    std::uint64_t run(std::span<std::uint64_t> mask) const { return run_range(0, rows_, mask); }
    std::uint64_t run_range(std::uint64_t first, std::uint64_t count,
                            std::span<std::uint64_t> mask) const {
        portfolio_detail::validate_range(rows_, first, count, mask.size());
        if (!count) return 0;
        const auto& kernel = candidates_[selected_];
        return kernel.run_range(kernel.context, first, count, mask);
    }

    [[nodiscard]] PortfolioCalibrationReport calibrate(
        std::uint64_t first, std::uint64_t count,
        std::span<const std::uint64_t> expected, std::span<std::uint64_t> scratch,
        const PortfolioCalibrationOptions& options) {
        const auto started = portfolio_detail::Clock::now();
        portfolio_detail::validate_options(options);
        portfolio_detail::validate_range(rows_, first, count, scratch.size());
        portfolio_detail::validate_range(rows_, first, count, expected.size());
        if (portfolio_detail::overlap(expected, scratch))
            throw std::invalid_argument("portfolio expected and scratch masks overlap");
        if (count % 64 && (expected.back() >> (count % 64)))
            throw std::invalid_argument("portfolio expected mask has nonzero tail bits");
        std::uint64_t expected_accepted = 0;
        for (const auto word : expected) expected_accepted += std::popcount(word);
        PortfolioCalibrationReport report;
        report.candidate_count = count_;
        report.incumbent_index = report.proposed_index = report.selected_index = selected_;
        auto elapsed = [&] { return portfolio_detail::elapsed(started, portfolio_detail::Clock::now()); };
        auto disabled = [&](PortfolioReason reason, PortfolioStopReason stop) {
            report.reason = reason;
            report.stop_reason = stop;
            report.elapsed_nanoseconds = elapsed();
            return report;
        };
        if (!count) return disabled(PortfolioReason::EmptyRange, PortfolioStopReason::Disabled);
        if (count_ == 1) return disabled(PortfolioReason::SingleCandidate, PortfolioStopReason::Disabled);
        if (!options.expected_remaining_calls)
            return disabled(PortfolioReason::NoExpectedReuse, PortfolioStopReason::Disabled);
        if (!options.max_elapsed_nanoseconds)
            return disabled(PortfolioReason::InsufficientCycles, PortfolioStopReason::TimeBudget);
        const auto rounds = portfolio_detail::cycle_rows(static_cast<unsigned>(count_));
        const auto cycle_calls = static_cast<std::uint64_t>(rounds) * count_;
        const auto warmup_calls = static_cast<std::uint64_t>(options.warmups_per_candidate) * count_;
        // Divide first: neither huge ranges nor a caller's UINT64_MAX budget
        // can overflow the planning arithmetic or the exact row-work counters.
        const auto allowed_calls = options.max_row_evaluations / count;
        if (allowed_calls < warmup_calls + cycle_calls * options.minimum_cycles)
            return disabled(PortfolioReason::InsufficientRowBudget, PortfolioStopReason::RowBudget);

        auto invoke = [&](std::size_t index, bool timed, unsigned cycle) {
            // A partial write is detected even if this kernel repeats the last
            // candidate's result. Poisoning and every equality check are outside
            // the candidate timer, but inside calibration's elapsed budget.
            for (std::size_t word = 0; word < scratch.size(); ++word) scratch[word] = ~expected[word];
            const auto begin = portfolio_detail::Clock::now();
            const auto accepted = candidates_[index].run_range(candidates_[index].context, first, count, scratch);
            const auto duration = portfolio_detail::elapsed(begin, portfolio_detail::Clock::now());
            auto& stats = report.candidates[index];
            ++stats.calls;
            ++report.total_calls;
            stats.row_evaluations += count;
            report.row_evaluations += count;
            if (timed) {
                stats.sample_nanoseconds[cycle][stats.cycle_samples[cycle]++] = duration;
                ++stats.timed_samples;
                stats.timed_nanoseconds = portfolio_detail::add_saturated(stats.timed_nanoseconds, duration);
            } else {
                ++stats.warmup_calls;
                stats.warmup_nanoseconds = portfolio_detail::add_saturated(stats.warmup_nanoseconds, duration);
            }
            if (accepted != expected_accepted || !std::equal(scratch.begin(), scratch.end(), expected.begin()))
                throw std::runtime_error("portfolio candidate differs from supplied exact mask");
        };

        for (unsigned warmup = 0; warmup < options.warmups_per_candidate; ++warmup)
            for (std::size_t index = 0; index < count_; ++index) invoke(index, false, 0);
        auto seed = options.order_seed;
        for (unsigned cycle = 0; cycle < options.cycles; ++cycle) {
            if (allowed_calls - report.total_calls < cycle_calls) {
                report.stop_reason = PortfolioStopReason::RowBudget;
                break;
            }
            if (elapsed() >= options.max_elapsed_nanoseconds) {
                report.stop_reason = PortfolioStopReason::TimeBudget;
                break;
            }
            auto& observation = report.cycles[cycle];
            observation = portfolio_detail::timing_cycle(static_cast<unsigned>(count_), seed);
            // Once started, finish a whole Williams cycle. The time budget is
            // deliberately soft; a single cycle may overshoot it. The row budget
            // is hard, and was checked before any of the cycle's calls.
            for (unsigned round = 0; round < observation.round_count; ++round)
                for (std::size_t column = 0; column < count_; ++column)
                    invoke(observation.orders[round][column], true, cycle);
            for (std::size_t index = 0; index < count_; ++index) {
                auto& stats = report.candidates[index];
                const auto median = portfolio_detail::median(stats.sample_nanoseconds[cycle], stats.cycle_samples[cycle]);
                stats.cycle_median_nanoseconds[cycle] = median;
                observation.median_nanoseconds[index] = median;
            }
            ++report.completed_cycles;
        }
        report.elapsed_nanoseconds = elapsed();
        if (report.completed_cycles == options.cycles &&
            report.elapsed_nanoseconds >= options.max_elapsed_nanoseconds)
            report.stop_reason = PortfolioStopReason::TimeBudget;
        // No external/stale decision can be applied. A failed callback or any
        // validation failure throws before this single transactional commit.
        static_cast<PortfolioDecision&>(report) = decide_portfolio(
            std::span<const PortfolioCycleObservation>(report.cycles.data(), report.completed_cycles),
            count_, selected_, report.elapsed_nanoseconds, options);
        selected_ = report.selected_index;
        return report;
    }

private:
    std::uint64_t rows_{};
    std::size_t count_{}, selected_{};
    std::array<PortfolioCandidate, portfolio_max_candidates> candidates_{};
};

} // namespace suffice
