#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>

#include <halcyon/halcyon.hpp>

// --- Reflected row types ---------------------------------------------------

struct Customer {
    int                        customer_id = 0;
    std::string                name;
    std::optional<std::string> email;        // maps the nullable EMAIL column
    halcyon::Date              created_at;
};
HALCYON_REFLECT(Customer, customer_id, name, email, created_at);

struct Order {
    std::string        order_no;
    int                customer_id = 0;
    std::string        status;
    halcyon::Decimal   total_amount;
    halcyon::Timestamp order_ts;
};
HALCYON_REFLECT(Order, order_no, customer_id, status, total_amount, order_ts);

// --- SQL -------------------------------------------------------------------
// IMPORTANT: column order in every SELECT must match the struct field order
// above, because queryAs<T> maps columns to fields positionally.

namespace sql {
// Demo rows this sample inserts; deleted up front so each run is idempotent.
inline constexpr const char* kResetDemoRows =
    "DELETE FROM orders WHERE order_no IN ('ORD-2001','ORD-2002','ORD-2003')";
inline constexpr const char* kSelectCustomers =
    "SELECT customer_id, name, email, created_at "
    "FROM customers ORDER BY customer_id";
inline constexpr const char* kSelectOrdersForCustomer =
    "SELECT order_no, customer_id, status, total_amount, order_ts "
    "FROM orders WHERE customer_id = :cid ORDER BY order_no";  // named param
inline constexpr const char* kInsertOrder =
    "INSERT INTO orders(order_no, customer_id, status, total_amount, order_ts) "
    "VALUES (?, ?, ?, ?, ?)";                                  // positional
inline constexpr const char* kUpdateOrderStatus =
    "UPDATE orders SET status = ? WHERE order_no = ?";
inline constexpr const char* kSelectOrderByNo =
    "SELECT order_no, customer_id, status, total_amount, order_ts "
    "FROM orders WHERE order_no = ?";
}  // namespace sql

// --- Shared helpers --------------------------------------------------------

inline std::string resolveDsn() {
    if (const char* env = std::getenv("HALCYON_TEST_DSN")) return env;
    return "DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;";
}

inline void printCustomer(const Customer& c) {
    std::cout << "  #" << c.customer_id << "  " << c.name << "  <"
              << (c.email ? *c.email : std::string("(no email)")) << ">"
              << "  joined " << c.created_at.value << "\n";
}

inline void printOrder(const Order& o) {
    std::cout << "  " << o.order_no << "  cust#" << o.customer_id << "  "
              << o.status << "  $" << o.total_amount.str() << "  "
              << o.order_ts.value << "\n";
}
