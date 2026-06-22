-- Halcyon orders sample — seed data. Apply after schema.sql:
-- db2 connect to SAMPLE && db2 -tvf seed.sql

INSERT INTO customers(customer_id, name, email, created_at) VALUES
    (1, 'Ada Lovelace',   'ada@example.com',   '2026-01-15'),
    (2, 'Linus Torvalds',  NULL,               '2026-02-03'),
    (3, 'Grace Hopper',   'grace@example.com', '2026-03-21');

INSERT INTO orders(order_no, customer_id, status, total_amount, order_ts) VALUES
    ('ORD-1001', 1, 'PAID',     '199.99', '2026-04-01 10:00:00'),
    ('ORD-1002', 1, 'NEW',       '49.50', '2026-04-02 11:30:00'),
    ('ORD-1003', 2, 'SHIPPED',  '320.00', '2026-04-03 09:15:00'),
    ('ORD-1004', 3, 'CANCELLED', '15.00', '2026-04-04 14:45:00');
