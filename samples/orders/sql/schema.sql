-- Halcyon orders sample — schema.
-- Apply with: db2 connect to SAMPLE && db2 -tvf schema.sql
-- On a fresh database the two DROP statements report SQL0204N
-- ("name is an undefined name"); that is harmless — db2 -tvf continues.

DROP TABLE orders;
DROP TABLE customers;

CREATE TABLE customers (
    customer_id INTEGER       NOT NULL PRIMARY KEY,
    name        VARCHAR(100)  NOT NULL,
    email       VARCHAR(255),
    created_at  DATE          NOT NULL
);

CREATE TABLE orders (
    order_no     VARCHAR(20)   NOT NULL PRIMARY KEY,
    customer_id  INTEGER       NOT NULL REFERENCES customers(customer_id),
    status       VARCHAR(20)   NOT NULL,
    total_amount DECIMAL(11,2) NOT NULL,
    order_ts     TIMESTAMP     NOT NULL
);
