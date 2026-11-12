#include "sqlite_gate.hpp"

#include "suffice/storage.hpp"

#include <cctype>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define SUFFICE_SQLITE_CALL __cdecl
#else
#include <dlfcn.h>
#define SUFFICE_SQLITE_CALL
#endif

struct sqlite3;
struct sqlite3_stmt;

namespace suffice::payload {
namespace {

// A deliberately small adapter to documented, stable SQLite C ABI functions.
// There is no sqlite3.h dependency and no private SQLite structure is accessed.
// References: sqlite.org/c3ref/{open,prepare,bind_blob,step,reset,finalize,
// column_blob,db_status,close}.html. Optional db_status64 is detected at runtime.
using Sqlite = ::sqlite3;
using StatementHandle = ::sqlite3_stmt;
using SqliteInt64 = long long;
static_assert(std::numeric_limits<SqliteInt64>::digits == 63);
constexpr int sqlite_ok = 0;
constexpr int sqlite_integer = 1;
constexpr int sqlite_row = 100;
constexpr int sqlite_done = 101;
constexpr int sqlite_open_readwrite = 0x00000002;
constexpr int sqlite_open_create = 0x00000004;
constexpr int sqlite_open_privatecache = 0x00040000;

class Module {
public:
    Module() {
#if defined(_WIN32)
        handle_ = LoadLibraryW(L"sqlite3.dll");
#elif defined(__APPLE__)
        handle_ = dlopen("libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
#else
        handle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!handle_) throw std::runtime_error("native SQLite shared library is unavailable");
    }

    ~Module() {
        if (!handle_) return;
#if defined(_WIN32)
        (void)FreeLibrary(handle_);
#else
        (void)dlclose(handle_);
#endif
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    template<class Function>
    [[nodiscard]] Function symbol(const char* name, bool required = true) const {
#if defined(_WIN32)
        const auto address = GetProcAddress(handle_, name);
#else
        void* const address = dlsym(handle_, name);
#endif
        if (!address && required)
            throw std::runtime_error(std::string("native SQLite API is unavailable: ") + name);
        // POSIX and Windows dynamic-loader ABIs support function addresses here.
        // Copying avoids compiler-specific object/function pointer cast warnings.
        Function result{};
        static_assert(sizeof result == sizeof address);
        std::memcpy(&result, &address, sizeof result);
        return result;
    }

private:
#if defined(_WIN32)
    HMODULE handle_{};
#else
    void* handle_{};
#endif
};

struct Api {
    // The module is constructed first and destroyed last, including failure
    // while resolving an API. Every connection/statement dies before this Api.
    Module module;
    using Open = int (SUFFICE_SQLITE_CALL *)(const char*, Sqlite**, int, const char*);
    using Close = int (SUFFICE_SQLITE_CALL *)(Sqlite*);
    using Prepare = int (SUFFICE_SQLITE_CALL *)(Sqlite*, const char*, int, StatementHandle**, const char**);
    using StatementInt = int (SUFFICE_SQLITE_CALL *)(StatementHandle*);
    using BindInt64 = int (SUFFICE_SQLITE_CALL *)(StatementHandle*, int, SqliteInt64);
    using ColumnInt = int (SUFFICE_SQLITE_CALL *)(StatementHandle*, int);
    using ColumnInt64 = SqliteInt64 (SUFFICE_SQLITE_CALL *)(StatementHandle*, int);
    using Message = const char* (SUFFICE_SQLITE_CALL *)(Sqlite*);
    using Version = const char* (SUFFICE_SQLITE_CALL *)();
    using DbStatus64 = int (SUFFICE_SQLITE_CALL *)(Sqlite*, int, SqliteInt64*, SqliteInt64*, int);

    Open open = module.symbol<Open>("sqlite3_open_v2");
    Close close = module.symbol<Close>("sqlite3_close_v2");
    Prepare prepare = module.symbol<Prepare>("sqlite3_prepare_v2");
    StatementInt step = module.symbol<StatementInt>("sqlite3_step");
    StatementInt reset = module.symbol<StatementInt>("sqlite3_reset");
    StatementInt clear_bindings = module.symbol<StatementInt>("sqlite3_clear_bindings");
    StatementInt finalize = module.symbol<StatementInt>("sqlite3_finalize");
    StatementInt bind_parameter_count = module.symbol<StatementInt>("sqlite3_bind_parameter_count");
    StatementInt column_count = module.symbol<StatementInt>("sqlite3_column_count");
    StatementInt readonly = module.symbol<StatementInt>("sqlite3_stmt_readonly");
    BindInt64 bind_int64 = module.symbol<BindInt64>("sqlite3_bind_int64");
    ColumnInt column_type = module.symbol<ColumnInt>("sqlite3_column_type");
    ColumnInt64 column_int64 = module.symbol<ColumnInt64>("sqlite3_column_int64");
    Message errmsg = module.symbol<Message>("sqlite3_errmsg");
    Version libversion = module.symbol<Version>("sqlite3_libversion");
    DbStatus64 db_status64 = module.symbol<DbStatus64>("sqlite3_db_status64", false);
};

[[noreturn]] void fail(const Api& api, Sqlite* db, const char* action, int code) {
    const char* message = db ? api.errmsg(db) : nullptr;
    throw std::runtime_error(std::string("SQLite ") + action + " failed (" +
                             std::to_string(code) + "): " +
                             (message ? message : "no database error message"));
}

void check(const Api& api, Sqlite* db, const char* action, int code) {
    if (code != sqlite_ok) fail(api, db, action, code);
}

struct Connection {
    explicit Connection(const Api& api_value) noexcept : api(api_value) {}
    ~Connection() {
        // All owned statements have already been finalized. close_v2 also rolls
        // back an unfinished ingestion transaction during constructor unwinding.
        // Destruction cannot throw; operational errors are checked at their call.
        if (handle) (void)api.close(handle);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    const Api& api;
    Sqlite* handle{};
};

class Statement {
public:
    Statement(const Api& api, Sqlite* db) noexcept : api_(api), db_(db) {}
    ~Statement() {
        // finalize also reports a preceding step error, already propagated by
        // the checked execution path. Cleanup must preserve an active exception.
        if (handle_) (void)api_.finalize(handle_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void prepare(const std::string& sql) {
        if (handle_) throw std::logic_error("SQLite statement already prepared");
        if (sql.find('\0') != std::string::npos ||
            sql.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("SQLite SQL text contains NUL or exceeds ABI length");
        const char* tail = nullptr;
        const auto rc = api_.prepare(db_, sql.c_str(), static_cast<int>(sql.size()+1), &handle_, &tail);
        check(api_, db_, "prepare", rc);
        if (!handle_) throw std::invalid_argument("SQLite SQL produced no statement");
        while (tail && *tail && std::isspace(static_cast<unsigned char>(*tail))) ++tail;
        if (tail && *tail) throw std::invalid_argument("SQLite SQL contains a trailing statement");
    }

    void finish() {
        const auto rc = api_.finalize(std::exchange(handle_, nullptr));
        check(api_, db_, "finalize", rc);
    }

    [[nodiscard]] StatementHandle* get() const noexcept { return handle_; }

private:
    const Api& api_;
    Sqlite* db_;
    StatementHandle* handle_{};
};

void execute(const Api& api, Sqlite* db, const std::string& sql) {
    Statement statement(api, db);
    statement.prepare(sql);
    const auto rc = api.step(statement.get());
    if (rc != sqlite_done) fail(api, db, "execute", rc);
    statement.finish();
}

} // namespace

struct SqliteGate::Impl {
    explicit Impl(const RawDataset& source, const std::string& where_sql)
        : connection(api), rows(source.info().rows) {
        if (rows > static_cast<std::uint64_t>(std::numeric_limits<SqliteInt64>::max()))
            throw std::invalid_argument("SQLite row domain exceeds signed 64-bit range");
        const auto channels = source.info().channels;
        if (!channels || channels >= static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("SQLite feature count exceeds parameter index range");
        if (where_sql.empty()) throw std::invalid_argument("SQLite predicate is empty");
        const auto open_rc = api.open(":memory:", &connection.handle,
              sqlite_open_readwrite | sqlite_open_create | sqlite_open_privatecache, nullptr);
        check(api, connection.handle, "open", open_rc);
        // Copy the version while its module is pinned; do not use Python metadata.
        const auto* native_version = api.libversion();
        if (!native_version) throw std::runtime_error("SQLite returned a null version");
        version_text = native_version;
        std::string create = "CREATE TABLE features(id INTEGER PRIMARY KEY";
        std::string insert = "INSERT INTO features VALUES(?1";
        for (std::uint32_t c = 0; c < channels; ++c) {
            create += ",c" + std::to_string(c) + " INTEGER NOT NULL";
            insert += ",?" + std::to_string(c+2);
        }
        create += ");"; insert += ");";
        execute(api, connection.handle, create);
        execute(api, connection.handle, "BEGIN TRANSACTION;");
        Statement insertion(api, connection.handle);
        insertion.prepare(insert);
        if (api.bind_parameter_count(insertion.get()) != static_cast<int>(channels+1))
            throw std::runtime_error("SQLite insertion parameter count differs");
        for (std::uint64_t row = 0; row < rows; ++row) {
            check(api, connection.handle, "bind row", api.bind_int64(insertion.get(), 1, static_cast<SqliteInt64>(row)));
            for (std::uint32_t c = 0; c < channels; ++c)
                check(api, connection.handle, "bind feature", api.bind_int64(insertion.get(), static_cast<int>(c+2), source.value(row,c)));
            const auto rc = api.step(insertion.get());
            if (rc != sqlite_done) fail(api, connection.handle, "insert", rc);
            check(api, connection.handle, "reset insert", api.reset(insertion.get()));
        }
        insertion.finish();
        execute(api, connection.handle, "COMMIT;");
        selection = std::make_unique<Statement>(api, connection.handle);
        selection->prepare("SELECT id FROM features WHERE id>=?1 AND id<?2 AND (" +
                           where_sql + ") ORDER BY id;");
        if (api.bind_parameter_count(selection->get()) != 2 ||
            api.column_count(selection->get()) != 1 || !api.readonly(selection->get()))
            throw std::invalid_argument("SQLite predicate violates the selection interface");
    }

    Api api;
    Connection connection;
    std::unique_ptr<Statement> selection;
    std::uint64_t rows{};
    std::string version_text;
    bool selecting{};
    bool cleanup_failed{};
};

SqliteGate::SqliteGate(const RawDataset& source, const std::string& where_sql)
    : impl_(std::make_unique<Impl>(source, where_sql)) {}

SqliteGate::~SqliteGate() = default;

void SqliteGate::select(std::uint64_t first, std::uint64_t count, void* context,
                        void (*visitor)(void*, std::uint64_t)) {
    auto& state = *impl_;
    if (!visitor) throw std::invalid_argument("SQLite row visitor is null");
    if (first > state.rows || count > state.rows-first)
        throw std::out_of_range("SQLite selection range exceeds the owned snapshot");
    if (state.selecting) throw std::logic_error("SQLite gate selection is not reentrant");
    if (state.cleanup_failed) throw std::runtime_error("SQLite gate has failed statement cleanup");
    if (!count) return;
    const auto end = first+count; // bounded above by the constructor's int64 limit
    auto* statement = state.selection->get();
    state.selecting = true;
    try {
        check(state.api, state.connection.handle, "bind range start",
              state.api.bind_int64(statement, 1, static_cast<SqliteInt64>(first)));
        check(state.api, state.connection.handle, "bind range end",
              state.api.bind_int64(statement, 2, static_cast<SqliteInt64>(end)));
        bool have_previous = false;
        std::uint64_t previous = 0;
        for (;;) {
            const auto rc = state.api.step(statement);
            if (rc == sqlite_done) break;
            if (rc != sqlite_row) fail(state.api, state.connection.handle, "select", rc);
            if (state.api.column_type(statement, 0) != sqlite_integer)
                throw std::runtime_error("SQLite selected a noninteger row ID");
            const auto signed_row = state.api.column_int64(statement, 0);
            if (signed_row < 0) throw std::runtime_error("SQLite selected a negative row ID");
            const auto row = static_cast<std::uint64_t>(signed_row);
            if (row < first || row >= end || (have_previous && row <= previous))
                throw std::runtime_error("SQLite selected an unordered or out-of-range row ID");
            previous = row; have_previous = true;
            visitor(context, row);
        }
        check(state.api, state.connection.handle, "reset selection", state.api.reset(statement));
        check(state.api, state.connection.handle, "clear selection bindings", state.api.clear_bindings(statement));
        state.selecting = false;
    } catch (...) {
        // A visitor exception must survive cleanup. reset returns any preceding
        // step error as well; conservatively disable reuse if cleanup reports one.
        const auto reset_rc = state.api.reset(statement);
        const auto clear_rc = state.api.clear_bindings(statement);
        state.cleanup_failed = reset_rc != sqlite_ok || clear_rc != sqlite_ok;
        state.selecting = false;
        throw;
    }
}

std::string SqliteGate::version() const { return impl_->version_text; }

std::uint64_t SqliteGate::database_bytes() const {
    const auto& state = *impl_;
    if (!state.api.db_status64) return 0;
    std::uint64_t total = 0;
    // Documented CACHE_USED, SCHEMA_USED, STMT_USED. All are approximate current
    // values, not peaks. The older 32-bit API may truncate and is not substituted.
    for (int option = 1; option <= 3; ++option) {
        SqliteInt64 current = 0, highwater = 0;
        const auto rc = state.api.db_status64(state.connection.handle, option, &current, &highwater, 0);
        if (rc != sqlite_ok || current < 0) return 0;
        const auto bytes = static_cast<std::uint64_t>(current);
        if (bytes > std::numeric_limits<std::uint64_t>::max()-total) return 0;
        total += bytes;
    }
    return total;
}

bool SqliteGate::available() noexcept {
    try { const Api api; return true; }
    catch (...) { return false; }
}

} // namespace suffice::payload

#undef SUFFICE_SQLITE_CALL
