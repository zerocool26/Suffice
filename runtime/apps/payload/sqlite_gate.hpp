#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace suffice {
class RawDataset;
}

namespace suffice::payload {

// Optional external-engine control. Copies the int32 features into an owned
// in-memory SQLite table; it neither retains the RawDataset nor stores payloads.
// where_sql is a trusted application-generated predicate over c0, c1, ...,
// containing no additional SQL parameters. This is not a public SQL sandbox.
// Instances are not thread-safe or reentrant. The synchronous visitor receives
// selected global row IDs in ascending order and must not destroy this object.
class SqliteGate {
public:
    SqliteGate(const RawDataset& source, const std::string& where_sql);
    ~SqliteGate();
    SqliteGate(const SqliteGate&) = delete;
    SqliteGate& operator=(const SqliteGate&) = delete;
    SqliteGate(SqliteGate&&) = delete;
    SqliteGate& operator=(SqliteGate&&) = delete;

    void select(std::uint64_t first, std::uint64_t count, void* context,
                void (*visitor)(void*, std::uint64_t row));

    // Reports the loaded native library, which may differ from Python sqlite3.
    [[nodiscard]] std::string version() const;

    // Approximate CURRENT SQLite cache + schema + prepared-statement bytes,
    // only when the runtime supplies the nontruncating db_status64 interface.
    // Zero means unknown/unsupported, not no memory. Excludes other connection
    // allocations, module code, payloads and process RSS; it is not a peak bound.
    [[nodiscard]] std::uint64_t database_bytes() const;

    // Only checks dynamic-library/API availability, not a particular SQL query.
    [[nodiscard]] static bool available() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace suffice::payload
