#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <variant>

#include "capturing_logger.hpp"
#include "halcyon/observability/logging.hpp"

using halcyon::obs::LogField;
using halcyon::obs::LogLevel;
using halcyon::testing::CapturingLogger;

TEST(LogField, AcceptsCommonTypes) {
    LogField a{"i", 42};
    LogField b{"u", std::size_t{7}};
    LogField c{"d", 1.5};
    LogField d{"f", true};
    LogField e{"s", "text"};
    std::string owned = "owned";
    LogField g{"sv", std::string_view(owned)};
    EXPECT_EQ(std::get<std::int64_t>(a.value), 42);
    EXPECT_EQ(std::get<std::int64_t>(b.value), 7);
    EXPECT_DOUBLE_EQ(std::get<double>(c.value), 1.5);
    EXPECT_TRUE(std::get<bool>(d.value));
    EXPECT_EQ(std::get<std::string_view>(e.value), "text");
    EXPECT_EQ(std::get<std::string_view>(g.value), "owned");
}

TEST(LogLevel, ToString) {
    EXPECT_STREQ(halcyon::obs::to_string(LogLevel::Debug), "debug");
    EXPECT_STREQ(halcyon::obs::to_string(LogLevel::Error), "error");
}

TEST(CapturingLoggerTest, RecordsEventsAndFields) {
    CapturingLogger lg;
    lg.log(LogLevel::Warn, "retry.attempt", {{"attempt", 2}, {"op", "select"}});
    ASSERT_EQ(lg.events.size(), 1u);
    EXPECT_EQ(lg.events[0].event, "retry.attempt");
    ASSERT_EQ(lg.events[0].fields.size(), 2u);
    EXPECT_EQ(lg.events[0].fields[0].first, "attempt");
    EXPECT_EQ(lg.events[0].fields[0].second, "2");
    EXPECT_EQ(lg.count("retry.attempt"), 1u);
}

TEST(StderrLogger, QuotesAndEscapesUnsafeValues) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        halcyon::obs::StderrLogger lg(f);
        // A value with spaces and a newline must not break field parsing or
        // inject a second log line.
        lg.log(LogLevel::Debug, "stmt_cache.evict",
               {{"sql", "SELECT a FROM t\nWHERE b=1"}});
    }
    std::rewind(f);
    char buf[512] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::string line(buf);
    // Exactly one real newline (the line terminator): the embedded one was
    // escaped, so it did not split the record.
    EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
    EXPECT_NE(line.find("sql=\"SELECT a FROM t"), std::string::npos);  // quoted
    EXPECT_NE(line.find("\\n"), std::string::npos);   // newline escaped
    EXPECT_NE(line.find("b=1\""), std::string::npos);  // '=' inside stays quoted
    std::fclose(f);
}

TEST(StderrLogger, WritesLogfmtLine) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        halcyon::obs::StderrLogger lg(f);
        lg.log(LogLevel::Warn, "retry.attempt", {{"attempt", 2}, {"op", "select"}});
    }
    std::rewind(f);
    char buf[512] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::string line(buf);
    EXPECT_NE(line.find("level=warn"), std::string::npos);
    EXPECT_NE(line.find("event=retry.attempt"), std::string::npos);
    EXPECT_NE(line.find("attempt=2"), std::string::npos);
    EXPECT_NE(line.find("op=select"), std::string::npos);
    EXPECT_NE(line.find("ts="), std::string::npos);
    std::fclose(f);
}

TEST(StderrLogger, EscapesUnsafeEventAndKeys) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        halcyon::obs::StderrLogger lg(f);
        // A public caller could pass an event/key with delimiters; both must be
        // quoted so the record stays parseable and injection-free.
        lg.log(LogLevel::Info, "weird event", {{"a b", std::int64_t{1}}});
    }
    std::rewind(f);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::string line(buf);
    EXPECT_NE(line.find("event=\"weird event\""), std::string::npos);
    EXPECT_NE(line.find("\"a b\"=1"), std::string::npos);
    std::fclose(f);
}
