#pragma once

#include <cctype>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/result.hpp"
#include "halcyon/types.hpp"

namespace halcyon {

// A prepared set of positional parameter rows for executeBatch. Defined here
// (below the seam's Value, above both transaction.hpp and database.hpp) so the
// Batch overloads of executeBatch are available on Transaction and
// ScopedTransaction alike. Built via the batchOf helpers in database.hpp.
struct Batch {
    std::vector<std::vector<detail::cli::Value>> rows;
};

// One named binding; constructible from any bindable value (erased to Value).
struct NamedParam {
    std::string name;
    detail::cli::Value value;

    template <class T>
    NamedParam(std::string n, const T& v)  // NOLINT(google-explicit-constructor)
        : name(std::move(n)), value(detail::to_value(v)) {}
};

// Collection of named bindings: params{{"age", 21}, {"city", "NYC"}}.
class params {
public:
    params() = default;
    params(std::initializer_list<NamedParam> xs)  // NOLINT
        : items_(xs) {}

    const std::vector<NamedParam>& items() const noexcept { return items_; }

private:
    std::vector<NamedParam> items_;
};

namespace detail {

// Packs variadic anonymous arguments into a positional Value vector.
template <class... Args>
std::vector<detail::cli::Value> pack_params(const Args&... args) {
    return std::vector<detail::cli::Value>{detail::to_value(args)...};
}

struct PreparedSql {
    std::string sql;  // ':name' rewritten to positional '?'
    std::vector<detail::cli::Value> params;
};

// Rewrites ':name' placeholders to '?' in appearance order, resolving each
// against p. A repeated name binds its value again. '::' is treated literally.
//
// The scanner is literal-aware: text inside '...' string literals, "..."
// delimited identifiers (both honouring the SQL doubled-quote escape), -- line
// comments, and /* */ block comments is copied through verbatim, so a ':name'
// that appears there is never mistaken for a placeholder.
inline Result<PreparedSql> bind_named(const std::string& sql, const params& p) {
    auto find = [&](const std::string& name) -> const detail::cli::Value* {
        for (const auto& it : p.items())
            if (it.name == name) return &it.value;
        return nullptr;
    };

    PreparedSql out;
    out.sql.reserve(sql.size());
    const std::size_t n = sql.size();
    for (std::size_t i = 0; i < n;) {
        const char c = sql[i];

        // Quoted string literal / delimited identifier: copy through to the
        // matching close quote, treating a doubled quote as an escaped quote
        // that stays inside the literal.
        if (c == '\'' || c == '"') {
            const char quote = c;
            out.sql += c;
            ++i;
            while (i < n) {
                out.sql += sql[i];
                if (sql[i] == quote) {
                    if (i + 1 < n && sql[i + 1] == quote) {
                        out.sql += sql[i + 1];  // doubled quote = escaped
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        // -- line comment: copy through to (but not including) the newline.
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') {
                out.sql += sql[i];
                ++i;
            }
            continue;
        }

        // /* block comment */: copy through to the closing */ (or end of input).
        if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
            out.sql += "/*";
            i += 2;
            while (i < n) {
                if (sql[i] == '*' && i + 1 < n && sql[i + 1] == '/') {
                    out.sql += "*/";
                    i += 2;
                    break;
                }
                out.sql += sql[i];
                ++i;
            }
            continue;
        }

        if (c == ':' && i + 1 < n && sql[i + 1] == ':') {
            out.sql += "::";
            i += 2;
            continue;
        }
        const bool starts_name =
            c == ':' && i + 1 < n &&
            (std::isalpha(static_cast<unsigned char>(sql[i + 1])) ||
             sql[i + 1] == '_');
        if (!starts_name) {
            out.sql += c;
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < n && (std::isalnum(static_cast<unsigned char>(sql[j])) ||
                         sql[j] == '_')) {
            ++j;
        }
        std::string name = sql.substr(i + 1, j - (i + 1));
        const detail::cli::Value* v = find(name);
        if (v == nullptr)
            return mapping_error("unknown named parameter ':" + name + "'");
        out.sql += '?';
        out.params.push_back(*v);
        i = j;
    }
    return out;
}

}  // namespace detail

}  // namespace halcyon
