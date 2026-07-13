#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

/// \brief Streaming parameter source for a BLOB/CLOB bind (spec §6 D.2).
///
/// Build with lobFile / lobStream / lobCallback and pass positionally to
/// execute(); the value is streamed to Db2 in chunks (data-at-exec) with
/// O(chunk) memory. Chain `.asClob()` when the target column is a character
/// LOB (the driver cannot infer the column type). A LobSource is SINGLE-USE:
/// its stream is consumed by one execute and must never be retried or reused
/// — build a fresh source per statement execution.
class LobSource {
public:
    using Pull = std::function<std::size_t(std::byte*, std::size_t)>;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    LobSource(Pull pull, std::optional<std::uint64_t> sizeHint)
        : pull_(std::move(pull)), sizeHint_(sizeHint) {}

    // Marks the source as character data: binds SQL_CLOB/SQL_C_CHAR instead
    // of the default SQL_BLOB/SQL_C_BINARY.
    LobSource& asClob() {
        isClob_ = true;
        return *this;
    }

    detail::cli::ParamStreamSource toSeamSource(std::size_t paramIndex) const {
        detail::cli::ParamStreamSource s;
        s.paramIndex = paramIndex;
        s.pull = pull_;
        s.sizeHint = sizeHint_;
        s.isClob = isClob_;
        return s;
    }

private:
    Pull pull_;
    std::optional<std::uint64_t> sizeHint_;
    bool isClob_ = false;
};

/// \brief LOB source from a pull callback: fill the buffer, return the byte
/// count, 0 at EOF, LobSource::npos on failure (aborts the execute).
inline LobSource lobCallback(
    LobSource::Pull pull,
    std::optional<std::uint64_t> sizeHint = std::nullopt) {
    return LobSource(std::move(pull), sizeHint);
}

/// \brief LOB source from an already-open std::istream. The stream must stay
/// alive until the execute call returns; a stream error reports npos.
inline LobSource lobStream(
    std::istream& in, std::optional<std::uint64_t> sizeHint = std::nullopt) {
    return LobSource(
        [&in](std::byte* buf, std::size_t cap) -> std::size_t {
            in.read(reinterpret_cast<char*>(buf),
                    static_cast<std::streamsize>(cap));
            if (in.bad()) return LobSource::npos;
            return static_cast<std::size_t>(in.gcount());
        },
        sizeHint);
}

/// \brief LOB source from a file, opened lazily on the first pull. A missing
/// or unreadable file reports npos on the first pull, failing the execute.
inline LobSource lobFile(std::string path) {
    auto file = std::make_shared<std::ifstream>();
    auto opened = std::make_shared<bool>(false);
    return LobSource(
        [file, opened, path = std::move(path)](std::byte* buf,
                                               std::size_t cap) -> std::size_t {
            if (!*opened) {
                file->open(path, std::ios::binary);
                *opened = true;
            }
            if (!file->is_open()) return LobSource::npos;
            file->read(reinterpret_cast<char*>(buf),
                       static_cast<std::streamsize>(cap));
            if (file->bad()) return LobSource::npos;
            return static_cast<std::size_t>(file->gcount());
        },
        std::nullopt);
}

template <class T>
struct is_lob_source : std::is_same<std::decay_t<T>, LobSource> {};

namespace detail {

template <class... Args>
struct has_lob_source : std::disjunction<is_lob_source<Args>...> {};

// True when every arg is an ordinary bindable OR a LobSource, and at least
// one LobSource is present — the SFINAE gate for the streaming execute
// overloads (disjoint from the all-bindable overloads).
template <class... Args>
struct stream_pack_ok
    : std::conjunction<
          std::disjunction<is_bindable<Args>, is_lob_source<Args>>...,
          has_lob_source<Args...>> {};

struct StreamPack {
    std::vector<cli::Value> values;  // Null placeholder at streamed positions
    std::vector<cli::ParamStreamSource> sources;
};

template <class... Args>
StreamPack pack_stream_params(const Args&... args) {
    StreamPack out;
    out.values.reserve(sizeof...(Args));
    std::size_t i = 0;
    auto add = [&](const auto& a) {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, LobSource>) {
            out.values.push_back(cli::Value{cli::Null{}});
            out.sources.push_back(a.toSeamSource(i));
        } else {
            out.values.push_back(detail::to_value(a));
        }
        ++i;
    };
    (add(args), ...);
    return out;
}

}  // namespace detail

}  // namespace halcyon
