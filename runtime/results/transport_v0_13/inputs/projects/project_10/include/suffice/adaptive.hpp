#pragma once

#include "suffice/runtime.hpp"
#include "suffice/frontier_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace suffice {

enum class AdaptiveBackend : std::uint8_t {
    Auto, Dense, RawVector, PackedVector, EncodedPredicate, DensePredicate, Progressive,
    EncodedWordcode, EncodedFrontier
};

enum class CalibrationMode : std::uint8_t { Sampled, Full };
[[nodiscard]] std::string_view calibration_mode_name(CalibrationMode) noexcept;
[[nodiscard]] CalibrationMode parse_calibration_mode(std::string_view);

enum class SamplingPolicy : std::uint8_t { Representative, LegacyEndpoints };
[[nodiscard]] std::string_view sampling_policy_name(SamplingPolicy) noexcept;
[[nodiscard]] SamplingPolicy parse_sampling_policy(std::string_view);

struct CalibrationWindow {
    std::uint64_t first_row{}, row_count{};
    std::size_t mask_word_offset{};
    // Performance strata only. Probe windows have population_rows == 0 and
    // timing_weight == 0. Legacy/full windows retain unit timing weights.
    std::uint64_t population_rows{};
    double timing_weight{1.0};
};

[[nodiscard]] std::string_view adaptive_backend_name(AdaptiveBackend) noexcept;
[[nodiscard]] AdaptiveBackend parse_adaptive_backend(std::string_view);

struct AdaptiveOptions {
    AdaptiveBackend backend{AdaptiveBackend::Auto};
    unsigned rounds{3};
    double tie_fraction{0.05};
    std::size_t tile_rows{256};
    unsigned frontier_prefix_bits{12};
    FrontierRepairOptions frontier_repair{};
    ExecutionOptions execution{};
    std::uint64_t max_candidate_scratch_bytes{128ull << 20};
    CalibrationMode calibration{CalibrationMode::Sampled};
    SamplingPolicy sample_policy{SamplingPolicy::Representative};
    std::uint64_t sample_rows{32768};
    unsigned sample_windows{8};
    std::uint64_t sample_seed{0x5355464649434533ull};
    // Hard bound on requested output-row evaluations in sampled preparation:
    // reference + candidate warmup + timed candidate calls, including untimed
    // boundary oracle/candidate probes. Input integrity
    // scans and whole packed blocks needed by a decoder are separate work.
    std::uint64_t max_calibration_row_evaluations{1ull << 20};
    std::uint64_t max_calibration_workspace_bytes{128ull << 20};
    // Soft deadline checked after a complete calibration round. A current range
    // execution cannot be interrupted. Zero disables this stop condition.
    double calibration_time_budget_ms{25.0};
    // Elimination starts only after three timed observations per active
    // candidate. The default three rounds therefore retain every candidate.
    bool successive_halving{true};

    // Explicit caller guarantee, not a partial validation mode. The caller must
    // already have verified the entire packed payload and, if raw is supplied,
    // exact dimensions and raw/packed value equality. Inputs must stay immutable.
    // Dimension and compiled-range checks are performed even when this is true.
    bool inputs_already_verified{false};
};

struct AdaptiveCandidateReport {
    AdaptiveBackend backend{AdaptiveBackend::Auto};
    bool eligible{};
    bool verified{};
    bool selected{};
    std::string reason;
    std::string description;
    std::string kernel;
    std::vector<double> samples_ms;
    std::vector<double> window_samples_ms;
    double median_ms{};
    double min_ms{};
    double max_ms{};
    std::uint64_t estimated_scratch_bytes{};
    std::uint64_t scratch_bytes{};
    std::uint64_t warmup_row_evaluations{}, timed_row_evaluations{}, mask_checks{};
    // Subsets of warmup_row_evaluations and mask_checks, respectively.
    std::uint64_t boundary_probe_row_evaluations{}, boundary_mask_checks{};
    bool eliminated{};
    unsigned eliminated_after_round{};
};

struct AdaptiveReport {
    AdaptiveBackend selected{AdaptiveBackend::Auto};
    bool forced{};
    unsigned rounds{};
    unsigned requested_rounds{};
    CalibrationMode calibration{CalibrationMode::Sampled};
    SamplingPolicy sample_policy{SamplingPolicy::Representative};
    std::uint64_t calibrated_rows{};
    std::uint64_t dataset_rows{};
    bool reference_is_full{};
    bool race_enabled{};
    unsigned minimum_rounds_before_elimination{3};
    bool time_budget_reached{};
    double calibration_time_budget_ms{};
    std::uint64_t sample_seed{};
    std::uint64_t sample_alignment_rows{}, sample_population_rows{}, sample_unrepresented_rows{};
    std::uint64_t boundary_probe_rows{};
    std::uint64_t calibration_row_budget{};
    std::uint64_t oracle_row_evaluations{}, warmup_row_evaluations{}, timed_row_evaluations{};
    // Subsets of oracle_row_evaluations and warmup_row_evaluations; do not add
    // these a second time when checking total_calibration_row_evaluations.
    std::uint64_t boundary_oracle_row_evaluations{}, boundary_candidate_row_evaluations{};
    std::uint64_t total_calibration_row_evaluations{};
    std::uint64_t max_calibration_workspace_bytes{};
    std::vector<CalibrationWindow> windows;
    std::vector<CalibrationWindow> boundary_probes;
    bool validation_performed{};
    double validation_ms{};
    double compile_prepare_ms{};
    double oracle_ms{};
    double warmup_ms{};
    double calibration_ms{};
    double total_prepare_ms{};
    double tie_fraction{};
    std::uint64_t calibration_workspace_bytes{};
    std::uint64_t selected_scratch_bytes{};
    std::uint64_t max_candidate_scratch_bytes{};
    std::uint64_t reference_checksum{};
    std::uint64_t reference_accepted{};
    std::vector<AdaptiveCandidateReport> candidates;
};

// Construction validates complete immutable inputs, then calibrates exact global
// ranges by default, with explicit row-work and modeled workspace limits. Full
// calibration is optional; sampled timing is not a statistical guarantee about
// unsampled rows. run() keeps the selected backend
// fixed and allocates no heap storage. It does not cache previous answer masks.
// Dataset mappings and the original compiled program must remain immutable and
// outlive the executor. One prepared executor is not concurrently thread-safe.
class AdaptiveExecutor {
public:
    AdaptiveExecutor(const PackedDataset&, const CompiledProgram&,
                     const RawDataset* raw = nullptr, std::string_view builtin = {},
                     AdaptiveOptions = {});
    ~AdaptiveExecutor();
    AdaptiveExecutor(AdaptiveExecutor&&) noexcept;
    AdaptiveExecutor& operator=(AdaptiveExecutor&&) noexcept;
    AdaptiveExecutor(const AdaptiveExecutor&) = delete;
    AdaptiveExecutor& operator=(const AdaptiveExecutor&) = delete;

    ExecutionStats run(std::span<std::uint64_t> mask);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> mask);
    [[nodiscard]] const AdaptiveReport& report() const noexcept;
    [[nodiscard]] std::string description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace suffice
