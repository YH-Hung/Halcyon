#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "halcyon/halcyon.hpp"

using halcyon::Connection;
using halcyon::params;

namespace {
std::optional<std::string> dsn() {
    if (const char* v = std::getenv("HALCYON_TEST_DSN")) return std::string(v);
    return std::nullopt;
}
}  // namespace

struct Person {
    int id;
    std::string name;
    std::optional<int> age;
};
HALCYON_REFLECT(Person, id, name, age);

class Db2Integration : public ::testing::Test {
protected:
    void SetUp() override {
        auto d = dsn();
        if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
        driver_ = halcyon::detail::cli::make_db2_cli_driver();
        auto c = Connection::open(*driver_, {*d});
        ASSERT_TRUE(c.ok()) << c.error().message;
        conn_ = std::make_unique<Connection>(std::move(c.value()));
        conn_->execute("DROP TABLE halcyon_people");  // ignore error if absent
        ASSERT_TRUE(
            conn_->execute("CREATE TABLE halcyon_people("
                           "id INT NOT NULL, name VARCHAR(64), age INT)")
                .ok());
    }

    std::unique_ptr<halcyon::detail::cli::ICliDriver> driver_;
    std::unique_ptr<Connection> conn_;
};

TEST_F(Db2Integration, InsertSelectStructMapping) {
    ASSERT_EQ(conn_->execute("INSERT INTO halcyon_people VALUES (?, ?, ?)", 1,
                             std::string{"ada"}, 36)
                  .value(),
              1);
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_people(id, name) VALUES "
                              "(:id, :name)",
                              params{{"id", 2}, {"name", std::string{"bob"}}})
                    .ok());

    auto people =
        conn_->queryAs<Person>("SELECT id, name, age FROM halcyon_people ORDER BY id");
    ASSERT_TRUE(people.ok()) << people.error().message;
    ASSERT_EQ(people.value().size(), 2u);
    EXPECT_EQ(people.value()[0].name, "ada");
    EXPECT_EQ(*people.value()[0].age, 36);
    EXPECT_FALSE(people.value()[1].age.has_value());
}

TEST_F(Db2Integration, TupleIterationAndAnonymousParams) {
    ASSERT_TRUE(conn_->execute("INSERT INTO halcyon_people VALUES (?,?,?)", 10,
                               std::string{"x"}, 1).ok());
    auto rs = conn_->query("SELECT id, name FROM halcyon_people WHERE id >= ?", 10);
    ASSERT_TRUE(rs.ok()) << rs.error().message;
    int count = 0;
    for (auto& row : rs.value()) {
        auto [id, name] = row.as<int, std::string>();
        EXPECT_GE(id, 10);
        (void)name;
        ++count;
    }
    EXPECT_GE(count, 1);
}
