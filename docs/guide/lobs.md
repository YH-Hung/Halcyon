# Large Objects (LOBs)

v1.1 streams BLOB/CLOB values in both directions with O(chunk) memory — a
32 MiB document never occupies 32 MiB of your process.

## Writing: LOB parameter sources

Pass a `LobSource` positionally where the LOB value goes. Three factories:

```cpp
conn.execute("INSERT INTO docs(id, content) VALUES (?, ?)",
             id, halcyon::lobFile("report.pdf"));

std::ifstream in("report.pdf", std::ios::binary);
conn.execute("INSERT INTO docs(id, content) VALUES (?, ?)",
             id, halcyon::lobStream(in, /*sizeHint=*/fileSize));

conn.execute("INSERT INTO docs(id, content) VALUES (?, ?)",
             id, halcyon::lobCallback([&](std::byte* buf, std::size_t cap) {
                 return producer.fill(buf, cap);   // 0 = EOF
             }));
```

Rules:

- **Character LOB columns need `.asClob()`** — the driver cannot infer the
  target type: `halcyon::lobStream(in).asClob()`.
- **Positional binds only.** Named parameters (`params{...}`) cannot carry a
  stream.
- **Single-use, never retried.** The stream is consumed by the execute;
  `Database::execute` runs LOB statements exactly once, and `executeBatch`
  does not accept them (it will not compile).
- A source failure (callback returns `LobSource::npos`, unreadable file, bad
  stream) cancels the statement and returns a `Mapping` error.

## Reading: streaming queries

`queryStreaming` (on `Connection`, `Transaction`, and `Database`) returns a
row-at-a-time cursor. Scalars come through the normal typed mapping; LOB
cells are chunk-read through a `LobReader`:

```cpp
auto rs = conn.queryStreaming(
    "SELECT id, content FROM docs WHERE id = ?", id).value();
while (auto row = rs.next()) {
    auto id  = row->get<std::int64_t>(0).value();
    auto lob = row->lob(1).value();
    lob.toFile("out-" + std::to_string(id) + ".bin").value();
    // or: lob.read(buf, len) in a loop / toString() / toVector() / toStream(os)
}
if (!rs.ok()) { /* mid-stream driver error: rs.error() */ }
```

Constraints inherited from the CLI:

- **Columns must be read in ascending order** within a row (mixing `get` and
  `lob` is fine); out-of-order access returns `ErrorCode::InvalidState`.
- `next()` returns `nullopt` at the end **or** on error — check `rs.ok()`
  after the loop, exactly like the materializing `ResultSet`.
- A SQL NULL LOB reads as immediate EOF with `reader.isNull()` true (the
  flag is meaningful after the first read).
- `Database::queryStreaming` holds a pooled connection for the cursor's
  lifetime and is never auto-retried.

Abandoning a reader or result set mid-stream is safe: the cursor is closed
and pending data discarded when the object is destroyed.
