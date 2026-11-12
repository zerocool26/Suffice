#pragma once

#include "suffice/payload.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace suffice::catalog {

struct ArtifactRow {
    std::string path;
    std::string sha256;
    std::uint64_t size{};
};

struct Limits {
    // Logical admitted Snapshot inputs, including externally retained aliases.
    // Construction buffers, workers, allocators and consumers are separate.
    std::uint64_t max_snapshot_bytes{256ULL << 20};
    // Retained Generation vector/string capacities; old leases remain charged.
    std::uint64_t max_metadata_bytes{32ULL << 20};
    // Retained ResultCache vector capacities, globally across all projects.
    std::uint64_t max_result_bytes{64ULL << 20};
    std::uint64_t max_plan_text_bytes{512ULL << 10};
    std::uint32_t max_project_slots{32};
    std::uint32_t max_live_snapshots{64};
    std::uint32_t max_live_generations{64};
    std::uint32_t max_plan_entries{32};
    std::uint32_t max_result_entries{64};
    std::uint32_t max_program_bytes{64U << 10};
    std::uint32_t max_label_bytes{256};
    std::uint32_t max_artifact_path_bytes{4096};
    // Retained mutable worker components at API quiescence, not peak RSS.
    // Oversized workers may execute transiently and are released on return.
    std::uint64_t max_worker_bytes{16ULL << 20};
    std::uint32_t max_worker_entries{32};
};

namespace detail { struct Generation; }

// A lease is an immutable owner. It does not export a Snapshot or Query owner.
// Views returned by its accessors remain valid while the lease is retained.
class Lease {
public:
    Lease() = default;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::string_view project() const noexcept;
    [[nodiscard]] std::string_view generation() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t rows() const noexcept;
    [[nodiscard]] std::uint32_t channels() const noexcept;
    [[nodiscard]] std::span<const ArtifactRow> artifacts() const noexcept;
private:
    friend class Catalog;
    explicit Lease(std::shared_ptr<const detail::Generation> owner);
    std::shared_ptr<const detail::Generation> owner_;
};

struct ProjectState {
    bool loaded{};
    std::uint64_t revision{}; // 0 only for a never-seen project name.
    std::string generation;
    std::uint64_t rows{};
};

enum class CacheMode { Bypass, Use, Populate };
enum class PopulateStrategy { Capture, Recompute };

struct RunOptions {
    // Applies to computed misses/bypass. A complete cache hit is independent of
    // the requested execution backend and uses the original cached Query owner.
    payload::Backend backend{payload::Backend::Auto};
    CacheMode cache{CacheMode::Use};
    payload::OutputLimits output{};
    PopulateStrategy population{PopulateStrategy::Capture};
};

struct RunReport {
    payload::RunStats pipeline;
    bool retired_generation{};
    bool plan_reused{};
    bool cache_hit{};
    bool cache_stored{};
    bool cache_oversized{};
    bool cache_build_failed{};
    std::uint64_t cache_storage_bytes{};
    std::uint64_t worker_scratch_bytes{};
    // plan_ms includes Query/Worker acquisition. Capture's measured optional
    // allocation/copy work is partitioned out of execution_ms into cache_build_ms.
    double plan_ms{}, execution_ms{}, cache_build_ms{};
    bool worker_reused{};
    bool worker_retained{};
    bool worker_oversized{};
};

struct Stats {
    std::uint64_t project_slots{}, active_projects{};
    std::uint64_t live_snapshots{}, live_generations{};
    std::uint64_t snapshot_bytes{}, metadata_bytes{};
    std::uint64_t retired_snapshot_bytes{}, retired_metadata_bytes{};
    std::uint64_t result_entries{}, result_bytes{}, result_key_bytes{};
    std::uint64_t plan_entries{}, plan_text_bytes{}, plan_worker_bytes{};
    std::uint64_t publications{}, drops{}, runs{}, cache_hits{}, cache_misses{};
    std::uint64_t cache_admissions{}, cache_evictions{}, cache_invalidations{};
    std::uint64_t plan_preparations{}, plan_evictions{}, plan_invalidations{};
    std::uint64_t worker_entries{};
    std::uint64_t worker_preparations{}, worker_reuses{}, worker_evictions{};
    std::uint64_t worker_invalidations{}, worker_oversized_runs{};
};

// Registry operations are serialized by the caller. Reentrant calls on the
// same Catalog throw, including calls made by a RowSink (the immutable limits()
// accessor is exempt). Immutable leases may
// cross threads; using this Catalog concurrently requires external exclusion.
class Catalog {
public:
    explicit Catalog(Limits limits = {});
    ~Catalog();
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;
    Catalog(Catalog&&) = delete;
    Catalog& operator=(Catalog&&) = delete;

    // Full validation is required. expected_revision is compared against the
    // current state, including retained drop tombstones. Successful publication
    // returns a new monotonic revision. Failed publication leaves the current
    // generation unchanged. Admission conservatively requires old+new headroom.
    // Artifacts must be sorted, unique relative paths with matching row sizes
    // and digest syntax. The caller/loader establishes UTF-8, SHA-256-to-body
    // agreement and external feature extraction semantics.
    Lease publish(std::string project, std::uint64_t expected_revision,
                  std::string generation,
                  std::shared_ptr<const payload::Snapshot> snapshot,
                  std::vector<ArtifactRow> artifacts);
    [[nodiscard]] ProjectState state(std::string_view project);
    [[nodiscard]] Lease acquire(std::string_view project);
    // Leaves a bounded project tombstone, preventing a stale expected revision
    // from succeeding after an absent -> present -> absent transition.
    void drop(std::string_view project, std::uint64_t expected_revision);

    // Cache keys are exact source bytes + immutable generation owner + range.
    // Populate/Capture constructs a private cache while delivering one normal
    // decoded output stream; publication follows the final successful callback.
    // Recompute preserves the old normal run plus two cache-construction passes.
    // Oversized/failed optional cache construction preserves successful output.
    // Retired leases execute exactly but retain no new plans or result caches.
    RunReport run(const Lease&, std::string_view program,
                  std::uint64_t first, std::uint64_t count,
                  payload::RowSink&, RunOptions options = {});
    void clear_results();
    // Releases mutable execution storage, preserving prepared queries/results.
    void clear_workers();
    void clear_plans();
    void collect();
    [[nodiscard]] Stats stats();
    [[nodiscard]] const Limits& limits() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char* cache_mode_name(CacheMode) noexcept;

} // namespace suffice::catalog
