#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "halcyon/lob.hpp"

using halcyon::LobSource;

namespace {

std::string drain(const halcyon::detail::cli::ParamStreamSource& src,
                  std::size_t cap) {
    std::string out;
    std::vector<std::byte> buf(cap);
    for (;;) {
        const std::size_t n = src.pull(buf.data(), buf.size());
        EXPECT_NE(n, LobSource::npos);
        if (n == 0 || n == LobSource::npos) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n);
    }
    return out;
}

}  // namespace

TEST(LobSource, CallbackSourceStreamsAndCarriesHint) {
    const std::string payload = "streaming payload";
    std::size_t off = 0;
    auto src = halcyon::lobCallback(
        [&](std::byte* buf, std::size_t cap) -> std::size_t {
            const std::size_t n = std::min(cap, payload.size() - off);
            std::memcpy(buf, payload.data() + off, n);
            off += n;
            return n;
        },
        payload.size());
    auto seam = src.toSeamSource(3);
    EXPECT_EQ(seam.paramIndex, 3u);
    ASSERT_TRUE(seam.sizeHint.has_value());
    EXPECT_EQ(*seam.sizeHint, payload.size());
    EXPECT_FALSE(seam.isClob);
    EXPECT_EQ(drain(seam, 4), payload);
}

TEST(LobSource, StreamSourceReadsIstream) {
    std::istringstream in("hello stream");
    auto seam = halcyon::lobStream(in).toSeamSource(0);
    EXPECT_EQ(drain(seam, 5), "hello stream");
}

TEST(LobSource, FileSourceStreamsFileLazily) {
    const std::string path = ::testing::TempDir() + "halcyon_lob_src.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << "file payload";
    }
    auto seam = halcyon::lobFile(path).toSeamSource(0);
    EXPECT_EQ(drain(seam, 3), "file payload");
}

TEST(LobSource, MissingFileReportsNposOnFirstPull) {
    auto seam = halcyon::lobFile("/nonexistent/halcyon.bin").toSeamSource(0);
    std::byte buf[8];
    EXPECT_EQ(seam.pull(buf, sizeof(buf)), LobSource::npos);
}

TEST(LobSource, AsClobSetsSeamFlag) {
    std::istringstream in("text");
    EXPECT_TRUE(halcyon::lobStream(in).asClob().toSeamSource(0).isClob);
}

TEST(LobPack, PackStreamParamsSplitsValuesAndSources) {
    std::istringstream in("blobbytes");
    auto pack = halcyon::detail::pack_stream_params(
        std::int64_t{42}, halcyon::lobStream(in), std::string{"name"});
    ASSERT_EQ(pack.values.size(), 3u);
    EXPECT_EQ(std::get<std::int64_t>(pack.values[0]), 42);
    EXPECT_TRUE(std::holds_alternative<halcyon::detail::cli::Null>(pack.values[1]));
    EXPECT_EQ(std::get<std::string>(pack.values[2]), "name");
    ASSERT_EQ(pack.sources.size(), 1u);
    EXPECT_EQ(pack.sources[0].paramIndex, 1u);
}

TEST(LobPack, Traits) {
    static_assert(halcyon::is_lob_source<halcyon::LobSource>::value);
    static_assert(!halcyon::is_lob_source<int>::value);
    static_assert(
        halcyon::detail::stream_pack_ok<int, halcyon::LobSource>::value);
    static_assert(!halcyon::detail::stream_pack_ok<int, double>::value);  // no LOB
    SUCCEED();
}
