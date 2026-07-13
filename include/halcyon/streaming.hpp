#pragma once

#include <cstddef>
#include <fstream>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/connection.hpp"
#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/detail/statement_cache.hpp"
#include "halcyon/error.hpp"
#include "halcyon/parameters.hpp"
#include "halcyon/result.hpp"
#include "halcyon/transaction.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

class StreamingRow;
class LobReader;

/// \brief Forward-only streaming cursor (spec §6 D.1): one row at a time,
/// scalar cells via typed getValue, LOB cells via chunked LobReader, O(chunk)
/// memory. Move-only; owns the statement lease. next() returns nullopt at the
/// end OR on a driver error — check ok()/error() after the loop, exactly like
/// ResultSet. Rows/readers borrow this object: they are invalidated by the
/// next next() call and by moving the StreamingResultSet.
class StreamingResultSet {
public:
    StreamingResultSet(StreamingResultSet&&) = default;
    StreamingResultSet& operator=(StreamingResultSet&&) = default;
    StreamingResultSet(const StreamingResultSet&) = delete;
    StreamingResultSet& operator=(const StreamingResultSet&) = delete;

    std::size_t column_count() const noexcept { return columns_; }
    std::optional<StreamingRow> next();
    bool ok() const noexcept { return !error_.has_value(); }
    const std::optional<Error>& error() const noexcept { return error_; }

private:
    friend class Connection;
    friend class StreamingRow;
    friend class LobReader;

    StreamingResultSet(detail::cli::ICliDriver* driver,
                       detail::StatementLease lease, std::size_t columns)
        : driver_(driver), lease_(std::move(lease)), columns_(columns) {}

    void fail(const Error& e) {
        error_ = e;
        lease_.poison();
        exhausted_ = true;
    }

    // Ascending-column-order gate shared by get<T>() and lob() (SQLGetData
    // constraint: a column at or before the last accessed one is gone).
    Result<void> claim_column(std::size_t col) {
        if (col >= columns_) return detail::mapping_error("column index out of range");
        if (static_cast<long>(col) <= lastCol_) {
            Error e;
            e.code = ErrorCode::InvalidState;
            e.message = "streaming rows require ascending column access "
                        "(CLI SQLGetData constraint)";
            return e;
        }
        lastCol_ = static_cast<long>(col);
        return Result<void>();
    }

    detail::cli::ICliDriver* driver_;
    detail::StatementLease lease_;
    std::size_t columns_;
    long lastCol_ = -1;  // reset per row
    bool exhausted_ = false;
    std::optional<Error> error_;
};

/// \brief Chunked reader over one LOB cell of the current streaming row.
/// read() returns 0 at EOF; isNull() is meaningful after the first read()
/// (a SQL NULL reads as immediate EOF with isNull() true).
class LobReader {
public:
    Result<std::size_t> read(std::byte* buf, std::size_t len);
    bool isNull() const noexcept { return isNull_; }

    // Convenience sinks (Task 14).
    Result<std::string> toString();
    Result<std::vector<std::byte>> toVector();
    Result<void> toStream(std::ostream& out);
    Result<void> toFile(const std::string& path);

private:
    friend class StreamingRow;
    LobReader(StreamingResultSet* rs, std::size_t col) : rs_(rs), col_(col) {}

    StreamingResultSet* rs_;
    std::size_t col_;
    bool isNull_ = false;
    bool done_ = false;
};

/// \brief Borrowed view of the current row of a StreamingResultSet.
class StreamingRow {
public:
    std::size_t column_count() const noexcept { return rs_->column_count(); }

    // Materializes one scalar cell through the normal TypeBinder mapping.
    template <class T>
    Result<T> get(std::size_t col) {
        static_assert(is_readable<T>::value,
                      "T must have a readable TypeBinder");
        auto guard = rs_->claim_column(col);
        if (!guard.ok()) return guard.error();
        auto v = rs_->driver_->getValue(rs_->lease_.handle(), col);
        if (!v.ok()) {
            rs_->fail(v.error());
            return v.error();
        }
        return TypeBinder<T>::from_value(v.value());
    }

    // Opens chunked access to a LOB cell.
    Result<LobReader> lob(std::size_t col) {
        auto guard = rs_->claim_column(col);
        if (!guard.ok()) return guard.error();
        return LobReader(rs_, col);
    }

private:
    friend class StreamingResultSet;
    explicit StreamingRow(StreamingResultSet* rs) : rs_(rs) {}
    StreamingResultSet* rs_;
};

inline std::optional<StreamingRow> StreamingResultSet::next() {
    if (exhausted_) return std::nullopt;
    auto more = driver_->fetchNext(lease_.handle());
    if (!more.ok()) {
        fail(more.error());
        return std::nullopt;
    }
    if (!more.value()) {
        exhausted_ = true;
        return std::nullopt;
    }
    lastCol_ = -1;
    return StreamingRow(this);
}

inline Result<std::size_t> LobReader::read(std::byte* buf, std::size_t len) {
    if (done_ || len == 0) return std::size_t{0};
    auto c = rs_->driver_->getDataChunk(rs_->lease_.handle(), col_, buf, len);
    if (!c.ok()) {
        done_ = true;
        rs_->fail(c.error());
        return c.error();
    }
    if (c.value().isNull) {
        isNull_ = true;
        done_ = true;
        return std::size_t{0};
    }
    if (c.value().done) done_ = true;
    return c.value().bytes;
}

namespace detail {
inline constexpr std::size_t kLobSinkBufBytes = 64 * 1024;
}  // namespace detail

inline Result<std::vector<std::byte>> LobReader::toVector() {
    std::vector<std::byte> out;
    std::vector<std::byte> buf(detail::kLobSinkBufBytes);
    for (;;) {
        auto n = read(buf.data(), buf.size());
        if (!n.ok()) return n.error();
        if (n.value() == 0) break;
        out.insert(out.end(), buf.begin(),
                   buf.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    return out;
}

inline Result<std::string> LobReader::toString() {
    std::string out;
    std::vector<std::byte> buf(detail::kLobSinkBufBytes);
    for (;;) {
        auto n = read(buf.data(), buf.size());
        if (!n.ok()) return n.error();
        if (n.value() == 0) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n.value());
    }
    return out;
}

inline Result<void> LobReader::toStream(std::ostream& out) {
    std::vector<std::byte> buf(detail::kLobSinkBufBytes);
    for (;;) {
        auto n = read(buf.data(), buf.size());
        if (!n.ok()) return n.error();
        if (n.value() == 0) break;
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(n.value()));
        if (!out.good()) return detail::mapping_error("output stream write failed");
    }
    return Result<void>();
}

inline Result<void> LobReader::toFile(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return detail::mapping_error("cannot open '" + path + "' for writing");
    auto r = toStream(f);
    if (!r.ok()) return r;
    f.flush();
    if (!f.good()) return detail::mapping_error("flush to '" + path + "' failed");
    return Result<void>();
}

// --- Connection::queryStreaming (declared in connection.hpp) ---

inline Result<StreamingResultSet> Connection::queryStreamingImpl(
    const std::string& sql, const std::vector<detail::cli::Value>& ps) {
    auto lease = cache_->acquire(sql);
    if (!lease.ok()) return lease.error();
    auto b = driver_->bindParams(lease.value().handle(), ps);
    if (!b.ok()) {
        lease.value().poison();
        return b.error();
    }
    auto e = driver_->execute(lease.value().handle());
    if (!e.ok()) {
        lease.value().poison();
        return e.error();
    }
    auto cc = driver_->columnCount(lease.value().handle());
    if (!cc.ok()) {
        lease.value().poison();
        return cc.error();
    }
    return StreamingResultSet(driver_, std::move(lease.value()), cc.value());
}

template <class... Args,
          std::enable_if_t<(is_bindable<Args>::value && ...), int>>
Result<StreamingResultSet> Connection::queryStreaming(const std::string& sql,
                                                      const Args&... args) {
    return queryStreamingImpl(sql, detail::pack_params(args...));
}

inline Result<StreamingResultSet> Connection::queryStreaming(
    const std::string& sql, const params& named) {
    auto pre = detail::bind_named(sql, named);
    if (!pre.ok()) return pre.error();
    return queryStreamingImpl(pre.value().sql, pre.value().params);
}

// --- Transaction::queryStreaming (declared in transaction.hpp) ---

template <class... Args>
Result<StreamingResultSet> Transaction::queryStreaming(const std::string& sql,
                                                       const Args&... args) {
    return conn_->queryStreaming(sql, args...);
}

inline Result<StreamingResultSet> Transaction::queryStreaming(
    const std::string& sql, const params& named) {
    return conn_->queryStreaming(sql, named);
}

}  // namespace halcyon
