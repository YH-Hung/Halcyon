# Halcyon Developer Guide

This guide covers everything you need to use Halcyon effectively. Each page is
self-contained; follow them in order if you are new, or jump to any section.

| Page | What you will learn |
|---|---|
| [Getting Started](getting-started.md) | Opening a `Database`, your first query, project setup |
| [Error Handling](error-handling.md) | `Result<T>` vs exceptions, `ErrorCode`, structured errors |
| [Querying](querying.md) | `query`, `queryAs`, `Row::as<>`, streaming vs materialized |
| [Parameters & Types](parameters-and-types.md) | Anonymous `?`, named `:param`, built-in type mapping, `HALCYON_REFLECT` |
| [Connection Pool](connection-pool.md) | `PoolConfig` reference, sizing, timeouts, validation, reconnect |
| [Transactions](transactions.md) | RAII `ScopedTransaction`, functional `transaction()`, retry semantics |
| [Batch Operations](batch-operations.md) | `executeBatch`, `batchOf` for structs and raw tuples |
| [Async](async.md) | `executeAsync`, `queryAsync`, lifetime rules |
| [Observability](observability.md) | Prometheus metrics, OpenTelemetry tracing, full metric reference |
| [Advanced](advanced.md) | Statement cache, custom `TypeBinder<T>`, driver injection for tests |
