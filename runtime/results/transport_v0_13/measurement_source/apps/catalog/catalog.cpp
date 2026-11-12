#include "suffice/catalog.hpp"

#include <algorithm>
#include <chrono>
#include <list>
#include <map>
#include <stdexcept>
#include <utility>

namespace suffice::catalog {
namespace {
using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("catalog: " + message);
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a) fail("byte count overflow");
    return a + b;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (a && b > UINT64_MAX / a) fail("byte count overflow");
    return a * b;
}
void increment(std::uint64_t& value) noexcept {
    if (value != UINT64_MAX) ++value;
}
double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
std::uint64_t string_storage(const std::string& text) {
    return add(text.capacity(), 1);
}
std::uint64_t snapshot_storage(const payload::Snapshot& snapshot) {
    const auto& s = snapshot.stats();
    return add(add(s.raw_feature_bytes, s.packed_feature_bytes),
               add(s.payload_store_bytes, s.block_range_bytes));
}
bool artifact_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find_first_of("\\:\0", 0, 3) != std::string_view::npos) return false;
    std::size_t first = 0;
    while (first < path.size()) {
        const auto slash = path.find('/', first);
        const auto component = path.substr(first, slash == std::string_view::npos
            ? path.size() - first : slash - first);
        if (component.empty() || component == "." || component == "..") return false;
        if (slash == std::string_view::npos) break;
        first = slash + 1;
    }
    return true;
}
bool digest_text(std::string_view digest) {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void output_limit(std::uint64_t rows, std::uint64_t bytes,
                  payload::OutputLimits limits) {
    if (rows > limits.max_rows || bytes > limits.max_bytes)
        fail("selected output budget exceeded");
}
void valid_backend(payload::Backend backend) {
    switch (backend) {
        case payload::Backend::Auto: case payload::Backend::Wordcode:
        case payload::Backend::Zoned: case payload::Backend::Dense:
        case payload::Backend::Scalar: return;
    }
    fail("invalid backend");
}
} // namespace

namespace detail {
struct Generation {
    std::shared_ptr<const void> catalog_identity;
    std::string project, generation;
    std::uint64_t revision{};
    std::shared_ptr<const payload::Snapshot> snapshot;
    std::vector<ArtifactRow> artifacts;
    std::uint64_t metadata_bytes{};
};
} // namespace detail

Lease::Lease(std::shared_ptr<const detail::Generation> owner) : owner_(std::move(owner)) {}
Lease::operator bool() const noexcept { return static_cast<bool>(owner_); }
std::string_view Lease::project() const noexcept {
    return owner_ ? std::string_view(owner_->project) : std::string_view{};
}
std::string_view Lease::generation() const noexcept {
    return owner_ ? std::string_view(owner_->generation) : std::string_view{};
}
std::uint64_t Lease::revision() const noexcept { return owner_ ? owner_->revision : 0; }
std::uint64_t Lease::rows() const noexcept { return owner_ ? owner_->snapshot->rows() : 0; }
std::uint32_t Lease::channels() const noexcept { return owner_ ? owner_->snapshot->channels() : 0; }
std::span<const ArtifactRow> Lease::artifacts() const noexcept {
    return owner_ ? std::span<const ArtifactRow>(owner_->artifacts) : std::span<const ArtifactRow>{};
}

struct Catalog::Impl {
    struct Guard {
        Impl& owner;
        explicit Guard(Impl& value) : owner(value) {
            if (owner.active) fail("reentrant catalog operation");
            owner.active = true;
        }
        ~Guard() { owner.active = false; }
    };
    struct Project {
        std::uint64_t revision{};
        std::shared_ptr<const detail::Generation> current;
    };
    struct SnapshotCharge {
        std::weak_ptr<const payload::Snapshot> snapshot;
        std::uint64_t bytes{};
    };
    struct Plan {
        std::shared_ptr<const detail::Generation> generation;
        std::shared_ptr<const std::string> source;
        std::shared_ptr<const payload::Query> query;
        std::unique_ptr<payload::Worker> worker;
    };
    struct Result {
        std::shared_ptr<const detail::Generation> generation;
        std::shared_ptr<const std::string> source;
        std::shared_ptr<const payload::Query> query;
        std::uint64_t first{}, count{};
        std::unique_ptr<payload::ResultCache> cache;
        payload::RunStats original;
    };

    Limits limits;
    bool active{};
    std::shared_ptr<const void> identity{std::make_shared<const int>(0)};
    std::map<std::string, Project, std::less<>> projects;
    std::vector<SnapshotCharge> snapshots;
    std::vector<std::weak_ptr<const detail::Generation>> generations;
    std::list<std::shared_ptr<Plan>> plans;
    std::list<Result> results;
    std::uint64_t revision{}, result_bytes{}, plan_text_bytes{};
    Stats counters;

    explicit Impl(Limits value) : limits(value) {}

    void label(std::string_view text, std::string_view kind) const {
        if (text.empty() || text.size() > limits.max_label_bytes ||
            text.find('\0') != std::string_view::npos)
            fail(std::string(kind) + " label is empty, contains NUL, or exceeds limit");
    }
    void collect() {
        std::erase_if(snapshots, [](const auto& charge) { return charge.snapshot.expired(); });
        std::erase_if(generations, [](const auto& generation) { return generation.expired(); });
    }
    Stats stats() {
        collect();
        auto report = counters;
        report.project_slots = projects.size();
        for (const auto& [name, project] : projects) {
            (void)name;
            if (project.current) ++report.active_projects;
        }
        for (const auto& charge : snapshots) {
            if (const auto snapshot = charge.snapshot.lock()) {
                ++report.live_snapshots;
                report.snapshot_bytes = add(report.snapshot_bytes, charge.bytes);
                bool current = false;
                for (const auto& [name, project] : projects) {
                    (void)name;
                    if (project.current && project.current->snapshot.get() == snapshot.get()) {
                        current = true; break;
                    }
                }
                if (!current) report.retired_snapshot_bytes = add(report.retired_snapshot_bytes, charge.bytes);
            }
        }
        for (const auto& weak : generations) {
            if (const auto generation = weak.lock()) {
                ++report.live_generations;
                report.metadata_bytes = add(report.metadata_bytes, generation->metadata_bytes);
                const auto found = projects.find(generation->project);
                if (found == projects.end() || found->second.current.get() != generation.get())
                    report.retired_metadata_bytes = add(report.retired_metadata_bytes, generation->metadata_bytes);
            }
        }
        report.result_entries = results.size();
        report.result_bytes = result_bytes;
        for (const auto& result : results)
            report.result_key_bytes = add(report.result_key_bytes, string_storage(*result.source));
        report.plan_entries = plans.size();
        report.plan_text_bytes = plan_text_bytes;
        for (const auto& plan : plans) {
            if (plan->worker) {
                ++report.worker_entries;
                report.plan_worker_bytes = add(report.plan_worker_bytes, plan->worker->scratch_bytes());
            }
        }
        return report;
    }
    bool retains(const Plan* plan) const noexcept {
        for (const auto& item : plans) if (item.get() == plan) return true;
        return false;
    }
    void trim_workers() noexcept {
        // A single oversized worker is transient and must not displace useful
        // existing workers. Their remaining LRU order is preserved.
        for (const auto& plan : plans) {
            if (plan->worker && plan->worker->scratch_bytes() > limits.max_worker_bytes) {
                plan->worker.reset();
                increment(counters.worker_evictions);
            }
        }
        // Keep the newest fitting prefix; discard all older workers once a
        // constraint is reached. Subtraction avoids overflowing an aggregate.
        std::uint64_t remaining = limits.max_worker_bytes, kept = 0;
        bool prefix = true;
        for (const auto& plan : plans) {
            if (!plan->worker) continue;
            const auto bytes = plan->worker->scratch_bytes();
            if (prefix && kept < limits.max_worker_entries && bytes <= remaining) {
                remaining -= bytes;
                ++kept;
            } else {
                prefix = false;
                plan->worker.reset();
                increment(counters.worker_evictions);
            }
        }
    }
    struct WorkerScope {
        Impl& state;
        const std::shared_ptr<Plan>& plan;
        RunReport& report;
        bool finished{};
        void finish() noexcept {
            if (finished) return;
            if (plan->worker) {
                report.worker_scratch_bytes = plan->worker->scratch_bytes();
                report.worker_oversized = report.worker_scratch_bytes > state.limits.max_worker_bytes;
                if (report.worker_oversized) increment(state.counters.worker_oversized_runs);
            }
            state.trim_workers();
            if (!state.retains(plan.get())) plan->worker.reset();
            report.worker_retained = static_cast<bool>(plan->worker);
            finished = true;
        }
        ~WorkerScope() { finish(); }
    };
    void invalidate(std::string_view project) noexcept {
        for (auto it = results.begin(); it != results.end();) {
            if (it->generation->project == project) {
                result_bytes -= it->cache->storage_bytes();
                it = results.erase(it);
                increment(counters.cache_invalidations);
            } else ++it;
        }
        for (auto it = plans.begin(); it != plans.end();) {
            if ((*it)->generation->project == project) {
                plan_text_bytes -= (*it)->source->capacity() + 1;
                if ((*it)->worker) increment(counters.worker_invalidations);
                it = plans.erase(it);
                increment(counters.plan_invalidations);
            } else ++it;
        }
    }
    std::shared_ptr<Plan> plan(const std::shared_ptr<const detail::Generation>& generation,
                               std::string_view program, bool& reused, bool retain) {
        for (auto it = plans.begin(); it != plans.end(); ++it) {
            if ((*it)->generation.get() == generation.get() && *(*it)->source == program) {
                auto result = *it;
                plans.splice(plans.begin(), plans, it);
                reused = true;
                return result;
            }
        }
        auto result = std::make_shared<Plan>();
        result->generation = generation;
        result->source = std::make_shared<const std::string>(program);
        result->query = payload::Query::prepare(generation->snapshot, parse_program(*result->source));
        increment(counters.plan_preparations);
        const auto bytes = string_storage(*result->source);
        if (retain && limits.max_plan_entries && bytes <= limits.max_plan_text_bytes) {
            // Allocate before evicting any existing plan. Splice is nonthrowing.
            std::list<std::shared_ptr<Plan>> staged;
            staged.push_back(result);
            while (!plans.empty() &&
                   (plans.size() >= limits.max_plan_entries ||
                    plan_text_bytes > limits.max_plan_text_bytes - bytes)) {
                plan_text_bytes -= string_storage(*plans.back()->source);
                if (plans.back()->worker) increment(counters.worker_evictions);
                plans.pop_back();
                increment(counters.plan_evictions);
            }
            plans.splice(plans.begin(), staged);
            plan_text_bytes += bytes;
        }
        return result;
    }
    void store(const std::shared_ptr<Plan>& plan, std::uint64_t first, std::uint64_t count,
               std::unique_ptr<payload::ResultCache> cache, const payload::RunStats& original) {
        const auto bytes = cache->storage_bytes();
        if (!limits.max_result_entries || bytes > limits.max_result_bytes)
            fail("internal cache admission exceeds limits");
        std::list<Result> staged;
        staged.push_back(Result{plan->generation, plan->source, plan->query, first, count,
                                std::move(cache), original});
        while (!results.empty() &&
               (results.size() >= limits.max_result_entries ||
                result_bytes > limits.max_result_bytes - bytes)) {
            result_bytes -= results.back().cache->storage_bytes();
            results.pop_back();
            increment(counters.cache_evictions);
        }
        results.splice(results.begin(), staged);
        result_bytes += bytes;
        increment(counters.cache_admissions);
    }
};

Catalog::Catalog(Limits limits) : impl_(std::make_unique<Impl>(limits)) {}
Catalog::~Catalog() = default;
const Limits& Catalog::limits() const noexcept { return impl_->limits; }

Lease Catalog::publish(std::string project, std::uint64_t expected_revision,
                       std::string generation, std::shared_ptr<const payload::Snapshot> snapshot,
                       std::vector<ArtifactRow> artifacts) {
    Impl::Guard operation(*impl_);
    auto& state = *impl_;
    state.label(project, "project");
    state.label(generation, "generation");
    const auto found = state.projects.find(project);
    const auto current_revision = found == state.projects.end() ? 0 : found->second.revision;
    if (expected_revision != current_revision) fail("stale expected revision");
    if (!snapshot) fail("null snapshot");
    if (!snapshot->stats().all_payloads_verified) fail("publication requires Full payload validation");
    if (artifacts.size() != snapshot->rows()) fail("artifact row count differs from snapshot");
    if (found == state.projects.end() && state.projects.size() >= state.limits.max_project_slots)
        fail("project slot limit exceeded; drop tombstones retain slots");
    if (state.revision == UINT64_MAX) fail("revision space exhausted");

    auto candidate = std::make_shared<detail::Generation>();
    candidate->catalog_identity = state.identity;
    candidate->project = std::move(project);
    candidate->generation = std::move(generation);
    candidate->revision = state.revision + 1;
    candidate->snapshot = std::move(snapshot);
    candidate->artifacts = std::move(artifacts);
    auto metadata_bytes = add(sizeof(detail::Generation),
                              multiply(candidate->artifacts.capacity(), sizeof(ArtifactRow)));
    metadata_bytes = add(metadata_bytes,
        add(string_storage(candidate->project), string_storage(candidate->generation)));
    std::string_view previous;
    for (std::size_t i = 0; i < candidate->artifacts.size(); ++i) {
        const auto& row = candidate->artifacts[i];
        if (row.path.size() > state.limits.max_artifact_path_bytes || !artifact_path(row.path))
            fail("artifact path must be a bounded relative slash-separated path");
        if (i && !(previous < std::string_view(row.path)))
            fail("artifact paths must be unique and strictly sorted");
        previous = row.path;
        if (!digest_text(row.sha256)) fail("artifact digest must be 64 lowercase hexadecimal characters");
        if (row.size != candidate->snapshot->decoded_size(i)) fail("artifact size differs from snapshot row");
        metadata_bytes = add(metadata_bytes, add(string_storage(row.path), string_storage(row.sha256)));
    }
    candidate->metadata_bytes = metadata_bytes;
    const auto occupied = state.stats();
    bool snapshot_known = false;
    for (const auto& charge : state.snapshots) {
        if (const auto owner = charge.snapshot.lock(); owner && owner.get() == candidate->snapshot.get()) {
            snapshot_known = true; break;
        }
    }
    const auto snapshot_bytes = snapshot_storage(*candidate->snapshot);
    if ((!snapshot_known && occupied.live_snapshots >= state.limits.max_live_snapshots) ||
        occupied.live_generations >= state.limits.max_live_generations)
        fail("live snapshot/generation count limit exceeded");
    if (!snapshot_known && (snapshot_bytes > state.limits.max_snapshot_bytes ||
        occupied.snapshot_bytes > state.limits.max_snapshot_bytes - snapshot_bytes))
        fail("snapshot byte budget exceeded (retired owners remain charged)");
    if (metadata_bytes > state.limits.max_metadata_bytes ||
        occupied.metadata_bytes > state.limits.max_metadata_bytes - metadata_bytes)
        fail("metadata byte budget exceeded (retired leases remain charged)");

    // Finish every potentially allocating preparation before publication.
    state.generations.reserve(state.generations.size() + 1);
    if (!snapshot_known) state.snapshots.reserve(state.snapshots.size() + 1);
    std::map<std::string, Impl::Project, std::less<>> staged;
    if (found == state.projects.end())
        staged.emplace(candidate->project, Impl::Project{candidate->revision, candidate});

    state.generations.emplace_back(candidate);
    if (!snapshot_known) state.snapshots.push_back({candidate->snapshot, snapshot_bytes});
    if (found == state.projects.end()) state.projects.insert(staged.extract(staged.begin()));
    else found->second = {candidate->revision, candidate};
    state.revision = candidate->revision;
    state.invalidate(candidate->project);
    increment(state.counters.publications);
    return Lease(std::move(candidate));
}

ProjectState Catalog::state(std::string_view project) {
    Impl::Guard operation(*impl_);
    impl_->label(project, "project");
    const auto found = impl_->projects.find(project);
    if (found == impl_->projects.end()) return {};
    const auto& entry = found->second;
    return {static_cast<bool>(entry.current), entry.revision,
            entry.current ? entry.current->generation : std::string{},
            entry.current ? entry.current->snapshot->rows() : 0};
}
Lease Catalog::acquire(std::string_view project) {
    Impl::Guard operation(*impl_);
    impl_->label(project, "project");
    const auto found = impl_->projects.find(project);
    if (found == impl_->projects.end() || !found->second.current) fail("project is not loaded");
    return Lease(found->second.current);
}
void Catalog::drop(std::string_view project, std::uint64_t expected_revision) {
    Impl::Guard operation(*impl_);
    impl_->label(project, "project");
    const auto found = impl_->projects.find(project);
    const auto revision = found == impl_->projects.end() ? 0 : found->second.revision;
    if (expected_revision != revision) fail("stale expected revision");
    if (found == impl_->projects.end() || !found->second.current) fail("project is not loaded");
    if (impl_->revision == UINT64_MAX) fail("revision space exhausted");
    found->second.current.reset();
    found->second.revision = ++impl_->revision;
    impl_->invalidate(project);
    increment(impl_->counters.drops);
}

RunReport Catalog::run(const Lease& lease, std::string_view program,
                       std::uint64_t first, std::uint64_t count,
                       payload::RowSink& sink, RunOptions options) {
    Impl::Guard operation(*impl_);
    auto& state = *impl_;
    // Hold an independent owner even if a callback overwrites the caller's lease.
    const auto generation = lease.owner_;
    if (!generation || generation->catalog_identity.get() != state.identity.get())
        fail("lease is empty or belongs to another catalog");
    if (program.empty() || program.size() > state.limits.max_program_bytes ||
        program.find('\0') != std::string_view::npos)
        fail("program source is empty, contains NUL, or exceeds limit");
    if (first > generation->snapshot->rows() || count > generation->snapshot->rows() - first)
        fail("row range outside snapshot");
    valid_backend(options.backend);
    if (options.cache != CacheMode::Bypass && options.cache != CacheMode::Use &&
        options.cache != CacheMode::Populate) fail("invalid cache mode");
    if (options.population != PopulateStrategy::Capture &&
        options.population != PopulateStrategy::Recompute) fail("invalid population strategy");

    RunReport report;
    const auto current_project = state.projects.find(generation->project);
    report.retired_generation = current_project == state.projects.end() ||
        current_project->second.current.get() != generation.get();
    if (options.cache != CacheMode::Bypass) {
        for (auto it = state.results.begin(); it != state.results.end(); ++it) {
            if (it->generation.get() == generation.get() && *it->source == program &&
                it->first == first && it->count == count) {
                increment(state.counters.cache_hits);
                output_limit(it->original.selected_rows, it->original.output_bytes, options.output);
                state.results.splice(state.results.begin(), state.results, it);
                auto& result = state.results.front();
                report.cache_hit = true;
                report.cache_storage_bytes = result.cache->storage_bytes();
                report.pipeline = result.original;
                // This operation emits cached plaintext; it does no feature or
                // compressed-payload work and applies no block certificates.
                report.pipeline.feature_requested_bytes = 0;
                report.pipeline.compressed_payload_bytes = 0;
                report.pipeline.decoded_payload_bytes = 0;
                report.pipeline.zone_true_rows = 0;
                report.pipeline.zone_false_rows = 0;
                report.pipeline.zone_refined_rows = 0;
                const auto start = Clock::now();
                result.cache->replay(result.query, first, count, sink);
                report.execution_ms = elapsed(start);
                increment(state.counters.runs);
                return report;
            }
        }
        increment(state.counters.cache_misses);
    }

    const auto plan_start = Clock::now();
    const auto plan = state.plan(generation, program, report.plan_reused, !report.retired_generation);
    Impl::WorkerScope worker_scope{state, plan, report};
    if (plan->worker) {
        report.worker_reused = true;
        increment(state.counters.worker_reuses);
    } else {
        plan->worker = std::make_unique<payload::Worker>(plan->query);
        increment(state.counters.worker_preparations);
    }
    report.plan_ms = elapsed(plan_start);
    const auto run_start = Clock::now();
    const bool populate = options.cache == CacheMode::Populate &&
        !report.retired_generation && state.limits.max_result_entries;
    if (populate && options.population == PopulateStrategy::Capture) {
        auto capture = plan->worker->run_captured(options.backend, first, count, sink,
                                                 state.limits.max_result_bytes, options.output);
        const auto total_run_ms = elapsed(run_start);
        report.pipeline = capture.pipeline;
        report.execution_ms = std::max(0.0, total_run_ms - capture.capture_ms);
        report.cache_build_ms = capture.capture_ms;
        report.cache_oversized = capture.oversized;
        report.cache_build_failed = capture.failed;
        if (capture.cache) {
            const auto store_start = Clock::now();
            try {
                const auto bytes = capture.cache->storage_bytes();
                state.store(plan, first, count, std::move(capture.cache), report.pipeline);
                report.cache_stored = true;
                report.cache_storage_bytes = bytes;
            } catch (const std::exception&) {
                report.cache_build_failed = true;
            }
            report.cache_build_ms += elapsed(store_start);
        }
    } else {
        report.pipeline = plan->worker->run(options.backend, first, count, sink, options.output);
        report.execution_ms = elapsed(run_start);
    }

    if (populate && options.population == PopulateStrategy::Recompute) {
        // An Entry consists of three uint64 values in the inherited API. This
        // early size check avoids pointless construction; the authoritative
        // capacity check remains ResultCache::build and storage_bytes().
        const auto entry_bytes = multiply(report.pipeline.selected_rows, 3 * sizeof(std::uint64_t));
        const auto expected_bytes = add(entry_bytes, report.pipeline.output_bytes);
        if (expected_bytes > state.limits.max_result_bytes) {
            report.cache_oversized = true;
        } else {
            const auto cache_start = Clock::now();
            try {
                auto cache = payload::ResultCache::build(plan->query, options.backend, first, count,
                                                        state.limits.max_result_bytes);
                const auto bytes = cache->storage_bytes();
                state.store(plan, first, count, std::move(cache), report.pipeline);
                report.cache_stored = true;
                report.cache_storage_bytes = bytes;
            } catch (const std::exception&) {
                // Optional retention cannot invalidate output already delivered
                // by a successful Full-snapshot run. No partial cache is stored.
                report.cache_build_failed = true;
            }
            report.cache_build_ms = elapsed(cache_start);
        }
    }
    worker_scope.finish();
    increment(state.counters.runs);
    return report;
}

void Catalog::clear_results() {
    Impl::Guard operation(*impl_);
    for (std::size_t i = 0; i < impl_->results.size(); ++i)
        increment(impl_->counters.cache_evictions);
    impl_->results.clear();
    impl_->result_bytes = 0;
}
void Catalog::clear_plans() {
    Impl::Guard operation(*impl_);
    for (const auto& plan : impl_->plans) {
        increment(impl_->counters.plan_evictions);
        if (plan->worker) increment(impl_->counters.worker_evictions);
    }
    impl_->plans.clear();
    impl_->plan_text_bytes = 0;
}
void Catalog::clear_workers() {
    Impl::Guard operation(*impl_);
    for (const auto& plan : impl_->plans) {
        if (plan->worker) {
            plan->worker.reset();
            increment(impl_->counters.worker_evictions);
        }
    }
}
void Catalog::collect() {
    Impl::Guard operation(*impl_);
    impl_->collect();
}
Stats Catalog::stats() {
    Impl::Guard operation(*impl_);
    return impl_->stats();
}
const char* cache_mode_name(CacheMode mode) noexcept {
    switch (mode) {
        case CacheMode::Bypass: return "bypass";
        case CacheMode::Use: return "use";
        case CacheMode::Populate: return "populate";
    }
    return "invalid";
}

} // namespace suffice::catalog
