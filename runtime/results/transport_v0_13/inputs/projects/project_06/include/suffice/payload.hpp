#pragma once
#include "suffice/wordcode.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace suffice::payload {
enum class Validation { Full, OnDemand };
enum class Backend { Auto, Wordcode, Zoned, Dense, Scalar };
struct Limits {
    // Logical owned input bytes plus block-bound arrays; allocator overhead,
    // transient admission copies, query plans and worker scratch are separate.
    std::uint64_t max_snapshot_bytes{512ULL << 20};
    std::uint64_t max_rows{1ULL << 20};
    std::uint32_t max_row_bytes{1U << 20};
    std::uint64_t max_total_decoded_bytes{4ULL << 30};
};
struct SnapshotStats {
    std::uint64_t raw_feature_bytes{}, packed_feature_bytes{}, payload_store_bytes{};
    std::uint64_t decoded_payload_bytes{};
    std::uint32_t max_decoded_row_bytes{};
    bool all_payloads_verified{};
    std::uint64_t block_range_bytes{};
};
class Decoder;
class Snapshot {
public:
    // Copies every input byte before publication. Caller buffers must remain
    // valid and unmodified during this call; later mutation cannot affect it.
    static std::shared_ptr<const Snapshot> copy_bytes(
        std::span<const std::uint8_t> raw, std::uint32_t channels,
        std::span<const std::uint8_t> packed, std::span<const std::uint8_t> payload,
        Limits limits = {}, Validation validation = Validation::Full);
    static std::shared_ptr<const Snapshot> load(
        const std::filesystem::path& raw, std::uint32_t channels,
        const std::filesystem::path& packed, const std::filesystem::path& payload,
        Limits limits = {}, Validation validation = Validation::Full);
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    [[nodiscard]] const RawDataset& raw_features() const noexcept { return raw_; }
    [[nodiscard]] const PackedDataset& packed_features() const noexcept { return packed_; }
    [[nodiscard]] const SnapshotStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t rows() const noexcept { return raw_.info().rows; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return raw_.info().channels; }
    [[nodiscard]] std::span<const std::uint8_t> frame(std::uint64_t row) const;
    [[nodiscard]] std::uint32_t decoded_size(std::uint64_t row) const;
    // Conservative bounds over whole verified physical blocks covering a range.
    [[nodiscard]] Bounds feature_bounds(std::uint32_t channel, std::uint64_t first,
                                        std::uint64_t count) const;
private:
    friend class Decoder;
    Snapshot(std::span<const std::uint8_t>, std::uint32_t,
             std::span<const std::uint8_t>, std::span<const std::uint8_t>, Limits);
    [[nodiscard]] const std::uint8_t* entry(std::uint64_t) const;
    RawDataset raw_;
    PackedDataset packed_;
    std::vector<std::uint8_t> payload_;
    std::vector<Bounds> block_ranges_;
    SnapshotStats stats_;
};
struct RowSink {
    virtual ~RowSink() = default;
    // Bytes are borrowed only until this call returns. Copy them to retain them.
    // If the sink throws, preceding rows may already have been delivered.
    virtual void row(std::uint64_t global_row, std::span<const std::uint8_t> bytes) = 0;
};
class Decoder {
public:
    explicit Decoder(std::shared_ptr<const Snapshot>);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    // Returned storage is invalidated by the next decode or destruction.
    [[nodiscard]] std::span<const std::uint8_t> decode(std::uint64_t global_row);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
private:
    std::shared_ptr<const Snapshot> snapshot_;
    std::vector<std::uint8_t> scratch_;
    void* context_{};
};
class Query {
public:
    static std::shared_ptr<const Query> prepare(std::shared_ptr<const Snapshot>,
                                                const Program&);
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    [[nodiscard]] const std::shared_ptr<const Snapshot>& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const CompiledProgram& program() const noexcept { return program_; }
    [[nodiscard]] const PredicatePlan& predicate() const noexcept { return predicate_; }
    [[nodiscard]] const WordcodePlan& wordcode() const noexcept { return wordcode_; }
    [[nodiscard]] Decision constant_decision() const noexcept;
    [[nodiscard]] std::span<const Decision> block_decisions() const noexcept { return block_decisions_; }
private:
    Query(std::shared_ptr<const Snapshot>, const Program&);
    std::shared_ptr<const Snapshot> snapshot_;
    CompiledProgram program_;
    PredicatePlan predicate_;
    WordcodePlan wordcode_;
    std::vector<Decision> block_decisions_;
};
struct RunStats {
    Backend backend{Backend::Scalar};
    std::uint64_t first_row{}, row_count{}, selected_rows{};
    std::uint64_t feature_requested_bytes{};
    std::uint64_t compressed_payload_bytes{}, decoded_payload_bytes{}, output_bytes{};
    std::uint64_t zone_true_rows{}, zone_false_rows{}, zone_refined_rows{};
};
struct OutputLimits {
    std::uint64_t max_rows{UINT64_MAX}, max_bytes{UINT64_MAX};
};
class Worker {
public:
    explicit Worker(std::shared_ptr<const Query>);
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    // Owns the stable query and snapshot. One worker per thread: no reentrancy.
    // Masks are internal and consumed against the same owner and global range.
    // Finite output limits are checked against the exact selection before any
    // sink callback; they bound delivered data, not a sink's private allocation.
    RunStats run(Backend, std::uint64_t first, std::uint64_t count, RowSink&,
                 OutputLimits limits = {});
    [[nodiscard]] std::span<const std::uint64_t> last_mask() const noexcept;
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
private:
    std::shared_ptr<const Query> query_;
    std::unique_ptr<WordcodeExecutor> word_;
    std::unique_ptr<DensePredicateExecutor> dense_;
    Decoder decoder_;
    std::vector<std::uint64_t> mask_;
    std::vector<std::uint64_t> block_mask_;
    std::size_t active_words_{};
};
// A successful cache owns complete output bytes. Failed bounded construction
// publishes nothing. Replay requires the identical query owner and range, not
// merely equal dimensions or a noncryptographic fingerprint.
class ResultCache {
public:
    ResultCache(const ResultCache&) = delete;
    ResultCache& operator=(const ResultCache&) = delete;
    ResultCache(ResultCache&&) = delete;
    ResultCache& operator=(ResultCache&&) = delete;
    static std::unique_ptr<ResultCache> build(std::shared_ptr<const Query>,
        Backend, std::uint64_t first, std::uint64_t count, std::uint64_t byte_budget);
    void replay(const std::shared_ptr<const Query>&, std::uint64_t first,
                std::uint64_t count, RowSink&) const;
    [[nodiscard]] std::uint64_t storage_bytes() const noexcept;
    [[nodiscard]] std::uint64_t selected_rows() const noexcept { return rows_.size(); }
private:
    ResultCache() = default;
    struct Entry { std::uint64_t row, offset, length; };
    std::shared_ptr<const Query> query_;
    std::uint64_t first_{}, count_{};
    std::vector<Entry> rows_;
    std::vector<std::uint8_t> bytes_;
};
[[nodiscard]] const char* backend_name(Backend) noexcept;
[[nodiscard]] std::uint64_t fnv64(std::span<const std::uint8_t>) noexcept;
[[nodiscard]] std::string zstd_version();
} // namespace suffice::payload
