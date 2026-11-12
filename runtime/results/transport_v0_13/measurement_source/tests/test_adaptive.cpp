#include "suffice/adaptive.hpp"
#include "suffice/predicate.hpp"
#include "suffice/wordcode.hpp"
#include "suffice/benchmark_order.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using suffice::AdaptiveBackend;
using suffice::AdaptiveExecutor;
using suffice::AdaptiveOptions;
using suffice::Bounds;
using suffice::i64;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("adaptive test: ") + message);
}

template<class F> void must_throw(F&& action, const char* message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-adaptive-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create adaptive test directory");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

void write_values(const std::filesystem::path& path, const std::vector<std::int32_t>& values) {
    std::ofstream output(path, std::ios::binary);
    for (const auto value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned byte = 0; byte < 4; ++byte)
            output.put(static_cast<char>((bits >> (8 * byte)) & 255U));
    }
    output.close();
    if (!output) throw std::runtime_error("cannot write adaptive test data");
}

struct Fixture {
    Directory directory;
    std::vector<std::int32_t> values;
    std::unique_ptr<suffice::RawDataset> raw;
    std::unique_ptr<suffice::PackedDataset> packed;
    std::uint64_t rows;
    static constexpr std::uint32_t channels = 2;

    Fixture(std::uint64_t count, std::uint32_t block_rows) : values(count * channels), rows(count) {
        std::uint32_t random = 0x8ea4b915U;
        for (std::size_t index = 0; index < values.size(); ++index) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            switch (index % 9) {
            case 0: values[index] = std::numeric_limits<std::int32_t>::min(); break;
            case 1: values[index] = std::numeric_limits<std::int32_t>::max(); break;
            case 2: values[index] = -1; break;
            case 3: values[index] = 0; break;
            case 4: values[index] = 1; break;
            case 5: values[index] = 75; break;
            default: values[index] = static_cast<std::int32_t>(random % 1001) - 500; break;
            }
        }
        const auto raw_path = directory.path / "input.i32";
        write_values(raw_path, values);
        const auto packed_path = directory.path / "input.sfp";
        (void)suffice::pack_file(raw_path, packed_path, channels, block_rows);
        raw = std::make_unique<suffice::RawDataset>(raw_path, channels);
        packed = std::make_unique<suffice::PackedDataset>(packed_path);
    }

    std::vector<Bounds> ranges() const {
        std::vector<Bounds> result;
        for (const auto range : packed->info().ranges) result.push_back({range.min, range.max});
        return result;
    }

    i64 sample(std::uint64_t row, std::uint32_t channel, int offset = 0) const {
        const auto index = std::clamp<i64>(static_cast<i64>(row) + offset, 0, static_cast<i64>(rows) - 1);
        return values[static_cast<std::size_t>(index) * channels + channel];
    }
};

suffice::CompiledProgram make_program(const Fixture& fixture, unsigned kind) {
    if (kind == 0) return suffice::builtin_program("identity", -1, 1).compile(fixture.ranges());
    if (kind == 1) return suffice::builtin_program("fir3", 100000).compile(fixture.ranges());
    suffice::Program program;
    const auto x = program.load(0);
    const auto y = program.load(1);
    if (kind == 2) {
        const auto left = program.min(x, program.constant(100));
        const auto right = program.min(program.abs(y), program.constant(200));
        program.output(program.max(left, right), 75);
    } else if (kind == 3) {
        program.output(program.add(x, y), 0);
    } else {
        program.output(program.sub(x, x), -1);
    }
    return program.compile(fixture.ranges());
}

std::vector<std::uint64_t> expected_mask(const Fixture& fixture, unsigned kind) {
    std::vector<std::uint64_t> mask(suffice::mask_words(fixture.rows));
    for (std::uint64_t row = 0; row < fixture.rows; ++row) {
        const auto x = fixture.sample(row, 0), y = fixture.sample(row, 1);
        bool yes = false;
        if (kind == 0) yes = y >= 0;
        else if (kind == 1) {
            const auto value = fixture.sample(row, 0, -1) - 2 * x + fixture.sample(row, 0, 1);
            yes = (value < 0 ? -value : value) > 100000;
        } else if (kind == 2) {
            const auto magnitude = y < 0 ? -y : y;
            yes = std::max(std::min<i64>(x, 100), std::min<i64>(magnitude, 200)) > 75;
        } else if (kind == 3) yes = x + y > 0;
        else yes = true;
        if (yes) mask[static_cast<std::size_t>(row / 64)] |= std::uint64_t{1} << (row % 64);
    }
    return mask;
}

void check_execution(AdaptiveExecutor& executor, const std::vector<std::uint64_t>& expected,
                     std::uint64_t rows) {
    const auto selected = executor.report().selected;
    std::vector<std::uint64_t> actual(expected.size(), ~std::uint64_t{0});
    const auto expected_count = suffice::mask_popcount(expected);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
        allocation_probe::start();
        suffice::ExecutionStats stats;
        try { stats = executor.run(actual); }
        catch (...) { (void)allocation_probe::stop(); throw; }
        const auto allocations = allocation_probe::stop();
        require(allocations == 0, "prepared selected run allocates");
        require(actual == expected, "selected run differs from independent scalar expected mask");
        require(stats.accepted == expected_count && stats.rows == rows, "selected result count differs");
        require(stats.output_bytes == expected.size() * sizeof(std::uint64_t), "selected output size differs");
        require(executor.report().selected == selected, "selected backend changed during run");
        require(std::accumulate(stats.resolved_by_bits.begin(), stats.resolved_by_bits.end(), std::uint64_t{0}) == rows,
                "selected resolution histogram does not cover rows");
    }
    const auto& report = executor.report();
    require(report.reference_checksum == suffice::mask_checksum(expected), "reference checksum differs");
    require(report.reference_accepted == expected_count, "reference count differs");
    require(report.calibrated_rows == rows, "calibrated row count differs");
    require(report.boundary_probes.empty() && report.boundary_probe_rows == 0 &&
            report.boundary_oracle_row_evaluations == 0 && report.boundary_candidate_row_evaluations == 0,
            "full coverage duplicated untimed endpoint checks");
    require(report.total_prepare_ms >= 0 && std::isfinite(report.total_prepare_ms), "invalid preparation timing");
    std::size_t selected_count = 0;
    for (const auto& candidate : report.candidates) {
        if (candidate.selected) {
            ++selected_count;
            require(candidate.backend == selected && candidate.verified, "selected report is inconsistent");
            require(candidate.scratch_bytes == report.selected_scratch_bytes, "selected scratch report differs");
        }
        if (candidate.eligible && !report.forced) {
            require(candidate.verified, "automatic candidate was not verified");
            require(candidate.samples_ms.size() == report.rounds, "automatic timing rounds missing");
            require(candidate.min_ms <= candidate.median_ms && candidate.median_ms <= candidate.max_ms,
                    "invalid candidate timing summary");
        }
    }
    require(selected_count == 1, "exactly one selected candidate was not reported");
    require(!executor.description().empty(), "selection description missing");
}

void forced_and_automatic() {
    const std::array<AdaptiveBackend, 8> backends{{
        AdaptiveBackend::Dense, AdaptiveBackend::RawVector, AdaptiveBackend::PackedVector,
        AdaptiveBackend::EncodedPredicate, AdaptiveBackend::DensePredicate, AdaptiveBackend::Progressive,
        AdaptiveBackend::EncodedWordcode, AdaptiveBackend::EncodedFrontier
    }};
    for (const auto rows : {0u, 1u, 33u, 65u, 129u}) {
        Fixture fixture(rows, rows == 129 ? 64 : 32);
        for (unsigned kind = 0; kind < 5; ++kind) {
            const auto plan = make_program(fixture, kind);
            const auto expected = expected_mask(fixture, kind);
            const auto predicate = suffice::compile_predicate(plan);
            for (const bool include_raw : {false, true}) {
                AdaptiveOptions automatic;
                automatic.rounds = 1;
                AdaptiveExecutor selected(*fixture.packed, plan, include_raw ? fixture.raw.get() : nullptr, {}, automatic);
                require(!selected.report().forced && selected.report().rounds == 1, "automatic mode not recorded");
                require(selected.report().validation_performed, "default full validation was skipped");
                check_execution(selected, expected, rows);
                // On larger nontrivial fixtures also check every forced backend.
                if (rows != 65) continue;
                for (const auto backend : backends) {
                    const bool needs_raw = backend == AdaptiveBackend::Dense || backend == AdaptiveBackend::RawVector ||
                                           backend == AdaptiveBackend::DensePredicate;
                    const bool needs_predicate = backend == AdaptiveBackend::EncodedPredicate ||
                                                 backend == AdaptiveBackend::DensePredicate || backend == AdaptiveBackend::EncodedWordcode || backend == AdaptiveBackend::EncodedFrontier;
                    AdaptiveOptions forced;
                    forced.backend = backend;
                    if ((needs_raw && !include_raw) || (needs_predicate && !predicate.complete)) {
                        must_throw([&] { AdaptiveExecutor unavailable(*fixture.packed, plan,
                            include_raw ? fixture.raw.get() : nullptr, {}, forced); },
                            "unavailable forced backend did not reject");
                    } else {
                        AdaptiveExecutor chosen(*fixture.packed, plan, include_raw ? fixture.raw.get() : nullptr, {}, forced);
                        require(chosen.report().forced && chosen.report().rounds == 0,
                                "forced mode unexpectedly performed timed tuning");
                        require(chosen.report().selected == backend, "forced backend was replaced");
                        check_execution(chosen, expected, rows);
                    }
                }
            }
        }
    }
}

void validation_and_options() {
    Fixture fixture(65, 32);
    const auto plan = make_program(fixture, 0);
    for (const auto backend : {AdaptiveBackend::Auto, AdaptiveBackend::Dense, AdaptiveBackend::RawVector,
            AdaptiveBackend::PackedVector, AdaptiveBackend::EncodedPredicate, AdaptiveBackend::DensePredicate,
            AdaptiveBackend::Progressive, AdaptiveBackend::EncodedWordcode, AdaptiveBackend::EncodedFrontier}) {
        require(suffice::parse_adaptive_backend(suffice::adaptive_backend_name(backend)) == backend,
                "backend name does not round trip");
    }
    must_throw([] { (void)suffice::parse_adaptive_backend("unavailable"); }, "unknown backend string accepted");
    for (unsigned kind = 0; kind < 12; ++kind) {
        AdaptiveOptions options;
        options.rounds = 1;
        switch (kind) {
        case 0: options.backend = static_cast<AdaptiveBackend>(255); break;
        case 1: options.rounds = 0; break;
        case 2: options.rounds = 10; break;
        case 3: options.tie_fraction = std::numeric_limits<double>::quiet_NaN(); break;
        case 4: options.tile_rows = 0; break;
        case 5: options.execution.stages = {4, 16}; break;
        case 6: options.max_candidate_scratch_bytes = 0; break;
        case 7: options.max_candidate_scratch_bytes = 1; break;
        case 8: options.backend = AdaptiveBackend::PackedVector; options.max_candidate_scratch_bytes = 1; break;
        case 9: options.frontier_repair.policy = static_cast<suffice::FrontierRepairPolicy>(255); break;
        case 10: options.frontier_repair.dense_threshold_percent = 0; break;
        default: options.frontier_repair.dense_threshold_percent = 101; break;
        }
        must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, plan, fixture.raw.get(), {}, options); },
                   "invalid options or impossible scratch cap accepted");
    }

    auto changed = fixture.values;
    changed[0] += 1;
    const auto mismatch_path = fixture.directory.path / "mismatch.i32";
    write_values(mismatch_path, changed);
    suffice::RawDataset mismatch(mismatch_path, Fixture::channels);
    AdaptiveOptions forced;
    forced.backend = AdaptiveBackend::PackedVector;
    must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, plan, &mismatch, {}, forced); },
               "different raw values accepted by default validation");
    suffice::RawDataset dimensions(mismatch_path, 1);
    must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, plan, &dimensions, {}, forced); },
               "different raw dimensions accepted");

    const std::array<Bounds, 2> narrow{{{0, 1}, {0, 1}}};
    const auto bad_range = suffice::builtin_program("identity", 0, 1).compile(narrow);
    must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, bad_range, nullptr, {}, forced); },
               "compiled range narrower than packed metadata accepted");
    suffice::CompiledProgram empty;
    must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, empty, nullptr, {}, forced); },
               "uncompiled program accepted");

    // The caller-guarantee mode is exercised only after establishing that exact
    // guarantee. Default construction above already checked this immutable pair.
    forced.inputs_already_verified = true;
    AdaptiveExecutor trusted(*fixture.packed, plan, fixture.raw.get(), {}, forced);
    require(!trusted.report().validation_performed, "explicit prevalidated mode not recorded");
    check_execution(trusted, expected_mask(fixture, 0), fixture.rows);
    must_throw([&] { trusted.run({}); }, "wrong output mask size accepted");

    // A cap can exclude an expensive candidate without invalidating selection.
    AdaptiveOptions capped;
    capped.rounds = 1;
    capped.max_candidate_scratch_bytes = 2048;
    AdaptiveExecutor limited(*fixture.packed, plan, fixture.raw.get(), {}, capped);
    bool excluded = false;
    for (const auto& candidate : limited.report().candidates)
        excluded |= !candidate.eligible && candidate.estimated_scratch_bytes > capped.max_candidate_scratch_bytes;
    require(excluded, "scratch-cap exclusions were not reported");
    check_execution(limited, expected_mask(fixture, 0), fixture.rows);
}

void sampled_global_calibration() {
    Fixture fixture(10019, 64);
    const auto plan = make_program(fixture, 2);
    const auto expected = expected_mask(fixture, 2);
    AdaptiveOptions options;
    options.sample_policy = suffice::SamplingPolicy::LegacyEndpoints;
    options.rounds = 3;
    options.sample_rows = 257;
    options.sample_windows = 7;
    options.max_calibration_row_evaluations = 2048;
    options.calibration_time_budget_ms = 0;
    options.sample_seed = 12345;
    for (const bool raw_available : {false, true}) {
        AdaptiveExecutor executor(*fixture.packed, plan, raw_available ? fixture.raw.get() : nullptr, {}, options);
        const auto& report = executor.report();
        require(report.sample_policy == suffice::SamplingPolicy::LegacyEndpoints && report.boundary_probes.empty(),
                "legacy control changed sampling policy or introduced new probes");
        require(!report.reference_is_full && report.dataset_rows == fixture.rows, "sample scope misreported");
        require(!report.race_enabled && report.minimum_rounds_before_elimination == 3,
                "default policy can discard a candidate before a three-sample median");
        require(report.calibrated_rows <= options.sample_rows && report.calibrated_rows > 0, "sample size outside cap");
        require(report.total_calibration_row_evaluations <= options.max_calibration_row_evaluations,
                "hard requested-row budget exceeded");
        require(report.total_calibration_row_evaluations == report.oracle_row_evaluations +
                report.warmup_row_evaluations + report.timed_row_evaluations, "work counters do not add up");
        require(report.calibration_workspace_bytes <= options.max_calibration_workspace_bytes, "workspace cap exceeded");
        require(report.windows.size() == 7, "stratification lost requested windows");
        require(report.windows.front().first_row == 0 &&
                report.windows.back().first_row + report.windows.back().row_count == fixture.rows,
                "sampling omitted required endpoint strata");
        std::vector<std::uint64_t> sampled_reference;
        std::uint64_t sample_count = 0, sample_accepted = 0, previous_end = 0;
        for (const auto& window : report.windows) {
            require(window.first_row >= previous_end && window.row_count <= fixture.rows - window.first_row,
                    "global windows overlap or escape original input");
            require(window.mask_word_offset == sampled_reference.size(), "sample mask offset invalid");
            const auto base = sampled_reference.size();
            sampled_reference.resize(base + suffice::mask_words(window.row_count));
            for (std::uint64_t i = 0; i < window.row_count; ++i) {
                const auto source = window.first_row + i;
                const bool accepted = (expected[source / 64] >> (source % 64)) & 1U;
                if (accepted) { sampled_reference[base + i / 64] |= std::uint64_t{1} << (i % 64); ++sample_accepted; }
            }
            sample_count += window.row_count;
            previous_end = window.first_row + window.row_count;
        }
        require(sample_count == report.calibrated_rows && sample_count == report.oracle_row_evaluations,
                "sample coverage count inconsistent");
        require(report.reference_checksum == suffice::mask_checksum(sampled_reference) &&
                report.reference_accepted == sample_accepted, "sample oracle changed global meaning");
        std::uint64_t warm = 0, timed = 0;
        for (const auto& candidate : report.candidates) if (candidate.eligible) {
            require(candidate.verified, "sample candidate lacked exact range checks");
            require(!candidate.samples_ms.empty() && candidate.samples_ms.size() <= options.rounds,
                    "invalid successive-halving sample count");
            require(candidate.window_samples_ms.size() == candidate.samples_ms.size() * report.windows.size(),
                    "per-window timing evidence missing");
            require(candidate.mask_checks == (candidate.samples_ms.size() + 1) * report.windows.size(),
                    "candidate sample checks missing");
            require(candidate.warmup_row_evaluations == sample_count &&
                    candidate.timed_row_evaluations == sample_count * candidate.samples_ms.size(),
                    "candidate requested-row accounting differs");
            require(!(candidate.selected && candidate.eliminated), "eliminated candidate selected");
            require(!candidate.eliminated && candidate.samples_ms.size() == 3,
                    "default policy did not give every candidate three observations");
            warm += candidate.warmup_row_evaluations; timed += candidate.timed_row_evaluations;
        }
        require(warm == report.warmup_row_evaluations && timed == report.timed_row_evaluations,
                "candidate/global work accounting differs");
        std::vector<std::uint64_t> actual(expected.size());
        allocation_probe::start();
        const auto stats = executor.run(actual);
        const auto allocations = allocation_probe::stop();
        require(!allocations && actual == expected && stats.accepted == suffice::mask_popcount(expected),
                "sample-selected full execution is wrong or allocates");
        std::vector<std::uint64_t> slice(suffice::mask_words(137));
        allocation_probe::start();
        executor.run_range(63, 137, slice);
        const auto range_allocations = allocation_probe::stop();
        require(!range_allocations, "adaptive range run allocates");
        for (std::uint64_t i = 0; i < 137; ++i)
            require(((slice[i / 64] >> (i % 64)) & 1U) ==
                    ((expected[(i + 63) / 64] >> ((i + 63) % 64)) & 1U), "adaptive range rebased global values");
    }

    auto longer_race = options;
    longer_race.rounds = 5;
    AdaptiveExecutor racing(*fixture.packed, plan, fixture.raw.get(), {}, longer_race);
    require(racing.report().race_enabled, "longer requested race is unavailable");
    for (const auto& candidate : racing.report().candidates) if (candidate.eligible) {
        require(candidate.samples_ms.size() >= 3,
                "longer race discarded a candidate before three observations");
        require(!candidate.eliminated || candidate.eliminated_after_round >= 3,
                "reported elimination precedes required observations");
    }
    require(racing.report().total_calibration_row_evaluations <= longer_race.max_calibration_row_evaluations,
            "three-observation floor broke the hard work budget");
    std::vector<std::uint64_t> racing_mask(expected.size());
    const auto racing_stats = racing.run(racing_mask);
    require(racing_mask == expected && racing_stats.accepted == suffice::mask_popcount(expected),
            "longer sampled race selected an incorrect complete execution");

    // No-racing mode provides a deterministic worst-case work accounting control.
    options.successive_halving = false;
    AdaptiveExecutor complete(*fixture.packed, plan, fixture.raw.get(), {}, options);
    for (const auto& candidate : complete.report().candidates) if (candidate.eligible)
        require(candidate.samples_ms.size() == options.rounds && !candidate.eliminated,
                "no-racing mode omitted a balanced round");
    auto repeated_options = options;
    repeated_options.sample_seed = options.sample_seed;
    AdaptiveExecutor repeated(*fixture.packed, plan, fixture.raw.get(), {}, repeated_options);
    require(repeated.report().windows.size() == complete.report().windows.size(), "sample seed not reproducible");
    for (std::size_t i = 0; i < complete.report().windows.size(); ++i)
        require(repeated.report().windows[i].first_row == complete.report().windows[i].first_row &&
                repeated.report().windows[i].row_count == complete.report().windows[i].row_count,
                "same sample seed/shape produced different coverage");

    // A tiny soft deadline can stop only after a balanced timed round; it never
    // fabricates a hard wall-time guarantee or leaves candidates unmeasured.
    options.calibration_time_budget_ms = 1e-12;
    AdaptiveExecutor deadline(*fixture.packed, plan, fixture.raw.get(), {}, options);
    require(deadline.report().time_budget_reached && deadline.report().rounds == 1,
            "soft deadline did not stop after the first balanced round");
    for (const auto& candidate : deadline.report().candidates) if (candidate.eligible)
        require(candidate.samples_ms.size() == 1, "deadline left unbalanced first-round evidence");

    options.calibration = suffice::CalibrationMode::Full;
    options.rounds = 1;
    AdaptiveExecutor full(*fixture.packed, plan, fixture.raw.get(), {}, options);
    require(full.report().reference_is_full && full.report().calibrated_rows == fixture.rows &&
            full.report().calibration_row_budget == 0 && !full.report().time_budget_reached,
            "explicit full calibration incorrectly applies sampled budget");
    check_execution(full, expected, fixture.rows);

    for (unsigned failure = 0; failure < 6; ++failure) {
        AdaptiveOptions bad;
        if (failure == 0) bad.max_calibration_row_evaluations = 1;
        if (failure == 1) bad.sample_rows = 0;
        if (failure == 2) bad.sample_windows = 65;
        if (failure == 3) bad.max_calibration_workspace_bytes = 1;
        if (failure == 4) bad.calibration_time_budget_ms = std::numeric_limits<double>::infinity();
        if (failure == 5) bad.calibration = static_cast<suffice::CalibrationMode>(255);
        must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, plan, fixture.raw.get(), {}, bad); },
                   "invalid sampled policy accepted");
    }
    require(suffice::parse_calibration_mode("sampled") == suffice::CalibrationMode::Sampled &&
            suffice::parse_calibration_mode("full") == suffice::CalibrationMode::Full,
            "calibration mode parsing differs");
    must_throw([] { (void)suffice::parse_calibration_mode("unsupported"); }, "invalid calibration mode accepted");
}
void check_representative_report(const Fixture& fixture, const std::vector<std::uint64_t>& expected,
                                  AdaptiveExecutor& executor, const AdaptiveOptions& options) {
    const auto& report = executor.report();
    require(report.sample_policy == suffice::SamplingPolicy::Representative && !report.reference_is_full,
            "representative partial sample not reported");
    require(report.sample_alignment_rows != 0 && report.sample_population_rows <= fixture.rows &&
            report.sample_unrepresented_rows == fixture.rows - report.sample_population_rows &&
            report.sample_unrepresented_rows < report.sample_alignment_rows,
            "timing population or omitted tail is inconsistent");
    const auto units = report.sample_population_rows / report.sample_alignment_rows;
    const auto strata = report.windows.size();
    std::vector<std::uint64_t> sample_mask;
    std::uint64_t sampled = 0, population = 0, accepted = 0;
    for (std::size_t i = 0; i < strata; ++i) {
        const auto& window = report.windows[i];
        const auto start = (units / strata * i + std::min<std::uint64_t>(i, units % strata)) *
                           report.sample_alignment_rows;
        const auto extent = (units / strata + (i < units % strata ? 1 : 0)) * report.sample_alignment_rows;
        require(window.first_row % report.sample_alignment_rows == 0 &&
                window.row_count % report.sample_alignment_rows == 0 && window.row_count != 0,
                "performance sample breaks its declared alignment");
        require(window.first_row >= start && window.first_row + window.row_count <= start + extent &&
                window.population_rows == extent, "sample escaped its declared stratum");
        const auto weight = (static_cast<double>(extent) / window.row_count) *
                            (static_cast<double>(report.calibrated_rows) / report.sample_population_rows);
        require(std::abs(window.timing_weight - weight) < 1e-12,
                "population correction weight is wrong");
        require(window.mask_word_offset == sample_mask.size(), "representative mask offsets do not concatenate");
        const auto base = sample_mask.size();
        sample_mask.resize(base + suffice::mask_words(window.row_count));
        for (std::uint64_t row = 0; row < window.row_count; ++row) {
            const auto source = window.first_row + row;
            if ((expected[source / 64] >> (source % 64)) & 1U) {
                sample_mask[base + row / 64] |= std::uint64_t{1} << (row % 64);
                ++accepted;
            }
        }
        sampled += window.row_count;
        population += window.population_rows;
    }
    require(sampled == report.calibrated_rows && sampled <= options.sample_rows &&
            population == report.sample_population_rows, "representative population totals differ");
    require(suffice::mask_checksum(sample_mask) == report.reference_checksum && accepted == report.reference_accepted,
            "performance reference changed original global row/halo semantics");
    std::uint64_t probes = 0, previous_end = 0;
    std::size_t probe_words = 0;
    for (const auto& probe : report.boundary_probes) {
        require(probe.first_row >= previous_end && probe.row_count <= fixture.rows - probe.first_row &&
                probe.mask_word_offset == probe_words && probe.population_rows == 0 && probe.timing_weight == 0,
                "boundary probe geometry or metadata differs");
        probes += probe.row_count;
        probe_words += suffice::mask_words(probe.row_count);
        previous_end = probe.first_row + probe.row_count;
        std::vector<std::uint64_t> actual(suffice::mask_words(probe.row_count));
        allocation_probe::start();
        executor.run_range(probe.first_row, probe.row_count, actual);
        const auto allocations = allocation_probe::stop();
        require(allocations == 0, "selected boundary range allocates");
        for (std::uint64_t row = 0; row < probe.row_count; ++row) {
            const auto source = probe.first_row + row;
            require(((actual[row / 64] >> (row % 64)) & 1U) ==
                    ((expected[source / 64] >> (source % 64)) & 1U), "boundary result differs from numeric oracle");
        }
    }
    require(probes == report.boundary_probe_rows && probes <= 128 &&
            report.boundary_oracle_row_evaluations == probes && report.oracle_row_evaluations == sampled + probes,
            "boundary oracle work is missing or double counted");
    if (probes) require(report.boundary_probes.front().first_row == 0 && previous_end == fixture.rows,
                        "untimed checks omitted an original endpoint");
    std::uint64_t warm = 0, timed = 0, candidate_probes = 0;
    for (const auto& candidate : report.candidates) if (candidate.verified) {
        require(candidate.boundary_probe_row_evaluations == probes &&
                candidate.boundary_mask_checks == report.boundary_probes.size() &&
                candidate.warmup_row_evaluations == sampled + probes &&
                candidate.timed_row_evaluations == sampled * candidate.samples_ms.size(),
                "candidate probe work leaked into timing or was uncharged");
        require(candidate.mask_checks == report.boundary_probes.size() +
                (candidate.samples_ms.size() + 1) * strata &&
                candidate.window_samples_ms.size() == candidate.samples_ms.size() * strata,
                "candidate exact checks/timing metadata include wrong window set");
        for (std::size_t round = 0; round < candidate.samples_ms.size(); ++round) {
            double weighted = 0;
            for (std::size_t i = 0; i < strata; ++i)
                weighted += candidate.window_samples_ms[round * strata + i] * report.windows[i].timing_weight;
            require(std::abs(candidate.samples_ms[round] - weighted) < 1e-12,
                    "reported timing score does not equal weighted window evidence");
        }
        warm += candidate.warmup_row_evaluations;
        timed += candidate.timed_row_evaluations;
        candidate_probes += candidate.boundary_probe_row_evaluations;
    }
    require(warm == report.warmup_row_evaluations && timed == report.timed_row_evaluations &&
            candidate_probes == report.boundary_candidate_row_evaluations &&
            report.total_calibration_row_evaluations == report.oracle_row_evaluations + warm + timed &&
            report.total_calibration_row_evaluations <= options.max_calibration_row_evaluations &&
            report.calibration_workspace_bytes <= options.max_calibration_workspace_bytes,
            "representative hard resource accounting is inconsistent");
    std::vector<std::uint64_t> actual(expected.size());
    allocation_probe::start();
    const auto stats = executor.run(actual);
    const auto allocations = allocation_probe::stop();
    require(!allocations && actual == expected && stats.accepted == suffice::mask_popcount(expected),
            "representative selection changed complete output or allocated");
}

void representative_calibration() {
    Fixture fixture(18 * 4096 + 17, 4096);
    const auto plan = make_program(fixture, 2);
    const auto expected = expected_mask(fixture, 2);
    AdaptiveOptions options;
    options.rounds = 1;
    options.sample_rows = 4 * 4096 + 17;
    options.sample_seed = 12345;
    options.calibration_time_budget_ms = 0;
    options.successive_halving = false;
    AdaptiveExecutor executor(*fixture.packed, plan, fixture.raw.get(), {}, options);
    check_representative_report(fixture, expected, executor, options);
    require(executor.report().sample_alignment_rows == 4096 && executor.report().windows.size() == 4 &&
            executor.report().calibrated_rows == 4 * 4096 && executor.report().sample_unrepresented_rows == 17 &&
            executor.report().boundary_probe_rows == 128, "block-shaped bounded default sample differs");
    require(executor.report().boundary_probes.back().first_row % 64 != 0,
            "boundary checks did not exercise shifted planes");
    AdaptiveExecutor repeat(*fixture.packed, plan, fixture.raw.get(), {}, options);
    require(repeat.report().windows.size() == executor.report().windows.size(), "sample shape depends on timings");
    for (std::size_t i = 0; i < executor.report().windows.size(); ++i) {
        const auto& a = executor.report().windows[i];
        const auto& b = repeat.report().windows[i];
        require(a.first_row == b.first_row && a.row_count == b.row_count && a.timing_weight == b.timing_weight,
                "representative seed did not reproduce windows");
    }
    bool changed = false, first_omitted = false, last_omitted = false;
    for (std::uint64_t seed = 12346; seed < 12350; ++seed) {
        options.sample_seed = seed;
        AdaptiveExecutor shifted(*fixture.packed, plan, fixture.raw.get(), {}, options);
        const auto& windows = shifted.report().windows;
        for (std::size_t i = 0; i < windows.size(); ++i)
            changed |= windows[i].first_row != executor.report().windows[i].first_row;
        first_omitted |= windows.front().first_row != 0;
        last_omitted |= windows.back().first_row + windows.back().row_count < shifted.report().sample_population_rows;
    }
    require(changed && first_omitted && last_omitted, "randomized strata still force performance endpoints");

    // All fallback grains retain original halo reads. This numeric fir3 oracle
    // distinguishes sample-local clamping from dataset-global clamping.
    Fixture halo_fixture(10019, 4096);
    const auto halo_plan = make_program(halo_fixture, 1);
    const auto halo_expected = expected_mask(halo_fixture, 1);
    for (const auto wanted : {1024u, 128u, 31u}) {
        options.sample_rows = wanted;
        for (const bool raw_available : {false, true}) {
            AdaptiveExecutor fallback(*halo_fixture.packed, halo_plan,
                raw_available ? halo_fixture.raw.get() : nullptr, {}, options);
            check_representative_report(halo_fixture, halo_expected, fallback, options);
            require(fallback.report().sample_alignment_rows == (wanted == 1024 ? 256u : wanted == 128 ? 64u : 1u),
                    "budget-limited alignment did not use declared fallback");
        }
    }

    // Eight eligible candidates, one reference, one warmup and one timing
    // round: 17 evaluations buy one performance row. Probes must not consume
    // that last row's reservation; two endpoint rows cost another 18.
    options.sample_rows = 4096;
    for (const auto budget : {17u, 26u, 35u}) {
        options.max_calibration_row_evaluations = budget;
        AdaptiveExecutor tiny_budget(*fixture.packed, plan, fixture.raw.get(), {}, options);
        check_representative_report(fixture, expected, tiny_budget, options);
        require(tiny_budget.report().calibrated_rows == 1 &&
                tiny_budget.report().boundary_probe_rows == (budget == 35 ? 2u : 0u),
                "smallest budget boundary accounting differs");
        require(tiny_budget.report().total_calibration_row_evaluations == (budget == 35 ? 35u : 17u),
                "smallest budget exceeded its exact reservation");
    }
    options.max_calibration_row_evaluations = 16;
    must_throw([&] { AdaptiveExecutor invalid(*fixture.packed, plan, fixture.raw.get(), {}, options); },
               "unaffordable single-row calibration accepted");

    // Tiny datasets can still use partial samples and overlapping performance /
    // probe coverage. Every invocation is charged, even when rows repeat.
    Fixture tiny(3, 32);
    const auto tiny_plan = make_program(tiny, 0);
    options.max_calibration_row_evaluations = 4096;
    options.sample_rows = 1;
    AdaptiveExecutor tiny_sample(*tiny.packed, tiny_plan, tiny.raw.get(), {}, options);
    check_representative_report(tiny, expected_mask(tiny, 0), tiny_sample, options);
    require(tiny_sample.report().calibrated_rows == 1 && tiny_sample.report().boundary_probe_rows == 2,
            "tiny partial sample omitted affordable endpoint checks");
    for (const auto policy : {suffice::SamplingPolicy::Representative, suffice::SamplingPolicy::LegacyEndpoints})
        require(suffice::parse_sampling_policy(suffice::sampling_policy_name(policy)) == policy,
                "sampling policy name does not round trip");
    must_throw([] { (void)suffice::parse_sampling_policy("unsupported"); }, "unknown sample policy accepted");
    options.sample_policy = static_cast<suffice::SamplingPolicy>(255);
    must_throw([&] { AdaptiveExecutor invalid(*tiny.packed, tiny_plan, tiny.raw.get(), {}, options); },
               "invalid sampling enum accepted");
}

void balanced_measurement_orders() {
    for (std::size_t count = 1; count <= 18; ++count) {
        const auto cycle = suffice::balanced_timing_cycle_rows(count);
        const auto rounds = static_cast<unsigned>(cycle * 2);
        const auto orders = suffice::balanced_timing_orders(count, rounds, 0x4f5244455253ull);
        require(orders == suffice::balanced_timing_orders(count, rounds, 0x4f5244455253ull),
                "timing order seed is not reproducible");
        std::vector<unsigned> positions(count * count), predecessors(count * count);
        for (const auto& order : orders) {
            auto sorted = order;
            std::sort(sorted.begin(), sorted.end());
            for (std::size_t i = 0; i < count; ++i) {
                require(sorted[i] == i, "timing round is not a complete candidate permutation");
                ++positions[order[i] * count + i];
                if (i) ++predecessors[order[i - 1] * count + order[i]];
            }
        }
        const auto expected = 2 * static_cast<unsigned>(cycle / count);
        for (std::size_t a = 0; a < count; ++a) for (std::size_t b = 0; b < count; ++b) {
            require(positions[a * count + b] == expected, "Williams positions are not balanced");
            require(predecessors[a * count + b] == (a == b ? 0 : expected),
                    "Williams within-round predecessors are not balanced");
        }
        const auto partial = suffice::balanced_timing_orders(count, rounds + 1, 0x4f5244455253ull);
        require(partial.size() == rounds + 1 && std::equal(orders.begin(), orders.end(), partial.begin()),
                "partial timing cycle changes an already completed prefix");
    }
    require(suffice::balanced_timing_orders(1, 0, 0).empty(), "zero timing rounds are not empty");
    must_throw([] { (void)suffice::balanced_timing_orders(0, 1, 0); }, "empty timing candidate set accepted");
    must_throw([] { (void)suffice::balanced_timing_orders(257, 1, 0); }, "unbounded timing candidate set accepted");
}
} // namespace

void test_adaptive() {
    forced_and_automatic();
    validation_and_options();
    sampled_global_calibration();
    representative_calibration();
    balanced_measurement_orders();
}
