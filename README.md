# Firebolt ADBC driver

A standalone C++ shared library implementing the
[ADBC 1.1.0](https://arrow.apache.org/adbc/) C API against Firebolt's HTTP query
interface. Results arrive as Arrow record batches with no row-by-row conversion,
so a query lands straight in pyarrow, pandas, or polars.

It is a client-side library only: a driver manager
([Python](https://pypi.org/project/adbc-driver-manager/),
[R](https://cran.r-project.org/package=adbcdrivermanager), Go, …) loads
`libfirebolt_adbc.so` at runtime. Nothing needs to be installed on the server.

## Supported today

Read this before you build anything on it.

| | |
|---|---|
| **Transport** | Plaintext `http://` **only**. This build has no TLS: an `https://` endpoint is rejected when you open the database. |
| **Authentication** | Engines with authentication **disabled**, plus an optional bearer token you obtained elsewhere. Firebolt's discovery-based OAuth flow is **not** implemented. See [docs/authentication.md](docs/authentication.md). |
| **Platforms** | Linux x86_64 and aarch64, glibc 2.34 or newer. No macOS or Windows build. |
| **ADBC** | The full ADBC 1.0.0 function set. Reports itself as 1.1.0, but the 1.1.0-only entry points are not implemented — see [Feature & Type Support](#feature--type-support). |

In practice that means this driver is ready for local development, CI, and
trusted-network deployments where the engine does not require authentication. It
is not ready to reach an engine that requires authentication over TLS.

## Quickstart

Takes about two minutes and needs Docker plus Python 3.9+.

**1. Install the driver manager**

```bash
pip install adbc-driver-manager pyarrow
```

**2. Download the driver**

Grab `libfirebolt_adbc-x86_64.so` (or `-aarch64`) from the
[latest release](https://github.com/firebolt-db/firebolt-adbc/releases/latest):

```bash
curl -sSLO https://github.com/firebolt-db/firebolt-adbc/releases/latest/download/libfirebolt_adbc-x86_64.so
curl -sSLO https://github.com/firebolt-db/firebolt-adbc/releases/latest/download/libfirebolt_adbc-x86_64.so.sha256
sha256sum -c libfirebolt_adbc-x86_64.so.sha256
```

**3. Start a Firebolt engine**

```bash
docker run -d --name firebolt -p 3473:3473 ghcr.io/firebolt-db/engine:latest
```

With no configuration the engine starts as a single node with authentication
disabled and a database called `firebolt` — which is why the quickstart needs no
credentials. Give it a few seconds, then check it is up:

```bash
curl -fsS http://localhost:3473/ping && echo ok
```

**4. Query it**

```python
import adbc_driver_manager.dbapi as dbapi

with dbapi.connect(
    driver="./libfirebolt_adbc-x86_64.so",
    db_kwargs={"uri": "http://localhost:3473"},
) as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT 1 AS n, 'hello' AS greeting")
        print(cur.fetch_arrow_table())
```

```
pyarrow.Table
n: int32 not null
greeting: string not null
----
n: [[1]]
greeting: [["hello"]]
```

That is the whole setup. Runnable versions of everything below are in
[`examples/python/`](examples/python).

### Naming the driver instead of its path

If you would rather write `driver="firebolt"` than an absolute path, install the
[`firebolt.toml`](firebolt.toml) manifest into a directory the driver manager
searches (`~/.config/adbc/drivers`, `/etc/adbc/drivers`, or anything on
`ADBC_DRIVER_PATH`) and point its `Driver.shared` entries at your `.so`. Needs
`adbc-driver-manager >= 1.5`.

## Common tasks

### Into pandas or polars

```python
cur.execute("SELECT * FROM my_table")

table = cur.fetch_arrow_table()   # pyarrow.Table
df = cur.fetch_df()               # pandas.DataFrame
```

For a result larger than memory, stream it in batches instead:

```python
cur.execute("SELECT * FROM big_table")
for batch in cur.fetch_record_batch():
    process(batch)
```

### Bulk-loading Arrow data

`adbc_ingest` uploads Arrow data directly — no `INSERT` statement to build, no
per-row round trips. Nested `ARRAY` and `STRUCT` columns are supported, including
the `CREATE TABLE` the driver generates for you:

```python
import pyarrow as pa

table = pa.table({
    "id":   pa.array([1, 2], pa.int32()),
    "tags": pa.array([["a", "b"], ["c"]], pa.list_(pa.string())),
    "meta": pa.array([{"k": 1}, {"k": 2}], pa.struct([("k", pa.int32())])),
})

with dbapi.connect(driver=DRIVER, db_kwargs={"uri": URI}, autocommit=True) as conn:
    with conn.cursor() as cur:
        cur.adbc_ingest("events", table, mode="replace")
```

`mode` is `append`, `create`, `create_append`, or `replace`. Which Arrow types you
can ingest, and what each becomes, is in
[Arrow to Database](#arrow-to-database) below.

### Transactions

`dbapi.connect()` defaults to `autocommit=False` for PEP 249 compliance, so you
are in a transaction whether or not you asked for one and the driver issues
`BEGIN` before your first statement:

```python
with dbapi.connect(driver=DRIVER, db_kwargs={"uri": URI}, autocommit=False) as conn:
    cur = conn.cursor()
    cur.execute("INSERT INTO events VALUES (1, 'a')")
    conn.commit()   # or conn.rollback()
```

Pass `autocommit=True` for each statement to stand on its own.

### Metadata

```python
conn.adbc_get_info()                              # driver and vendor identity
conn.adbc_get_table_types()                       # e.g. BASE TABLE, VIEW
conn.adbc_get_table_schema("events")              # pyarrow.Schema
conn.adbc_get_objects(depth="columns")            # catalogs → schemas → tables → columns
```

## Feature & Type Support

### Features

| Feature | Firebolt |
|---------|----------|
| Bulk Ingestion: Create | ✅ |
| Bulk Ingestion: Append | ✅ |
| Bulk Ingestion: Create/Append | ✅ |
| Bulk Ingestion: Replace | ✅ |
| Bulk Ingestion: Temporary Table | ❌ <sup>[1](#fn1)</sup> |
| Bulk Ingestion: Target Catalog | ✅ <sup>[2](#fn2)</sup> |
| Bulk Ingestion: Target Schema | ✅ |
| Non-nullable fields are marked NOT NULL | ✅ <sup>[3](#fn3)</sup> |
| Catalog (GetObjects): depth=catalogs | ✅ |
| Catalog (GetObjects): depth=db_schemas | ✅ |
| Catalog (GetObjects): depth=tables | ✅ |
| Catalog (GetObjects): depth=columns (all) | ✅ |
| Get Parameter Schema | ❌ <sup>[4](#fn4)</sup> |
| Get Table Schema | ✅ |
| Prepared Statements | ⚠️ <sup>[5](#fn5)</sup> |
| Transactions | ✅ |

### Beyond the standard table

Driver-level capabilities the shared table does not cover.

| | Firebolt |
|---|----------|
| Result streaming as Arrow record batches | ✅ |
| Session parameter passthrough, server-driven session updates | ✅ |
| Per-connection bearer token | ✅ |
| Query parameter binding (`execute(sql, params)`) | ❌ <sup>[4](#fn4)</sup> |
| `rowcount` on DML | ❌ always `-1` <sup>[6](#fn6)</sup> |
| TLS / `https://` endpoints | ❌ not in this build |
| Discovery-based authentication | ❌ see [docs/authentication.md](docs/authentication.md) |
| Partitioned execution, Substrait plans | ❌ |
| ADBC 1.1.0-only entry points <sup>[7](#fn7)</sup> | ❌ |

### Types

#### Database to Arrow

What each Firebolt column type becomes when you read it. Firebolt has no
`SMALLINT` and no `TIME` type; `INT`/`INTEGER`, `VARCHAR`/`TEXT`, and
`FLOAT`/`DOUBLE`/`DOUBLE PRECISION` are aliases that collapse as shown.

| Database Type | Firebolt |
|---|---|
| `BOOLEAN` | `bool` |
| `INTEGER` (`INT`) | `int32` |
| `BIGINT` | `int64` |
| `REAL` | `float` |
| `DOUBLE PRECISION` (`FLOAT`, `DOUBLE`) | `double` |
| `TEXT` (`VARCHAR`) | `string` |
| `BYTEA` | `binary` |
| `DATE` | `date32[day]` |
| `TIMESTAMP` (`TIMESTAMPNTZ`) | `timestamp[us]` |
| `TIMESTAMPTZ` | `timestamp[us, tz=UTC]` |
| `NUMERIC(p, s)` (`DECIMAL(p, s)`) | `decimal128(p, s)` |
| `GEOGRAPHY` | `binary` <sup>[8](#fn8)</sup> |
| `ARRAY(T)` | `list<item: T>` |
| `STRUCT(…)` | `struct<…>` |

Nesting is preserved to any depth: `ARRAY(ARRAY(INT))` reads as
`list<item: list<item: int32>>`, and `ARRAY(STRUCT("k" INT))` as
`list<item: struct<k: int32>>`.

#### Arrow to Database

Applies to **bulk ingest**, where the driver generates `CREATE TABLE` DDL from the
bound Arrow schema. There is no Bind column because Firebolt's HTTP interface has
no query parameter binding <sup>[4](#fn4)</sup>.

| Arrow Type | Firebolt Type | Ingest |
|---|---|---|
| `bool` | `BOOLEAN` | ✅ |
| `int8`, `int16`, `int32`, `uint8`, `uint16` | `INTEGER` | ✅ |
| `int64`, `uint32` | `BIGINT` | ✅ |
| `uint64` | `BIGINT` | ⚠️ <sup>[9](#fn9)</sup> |
| `float` | `REAL` | ✅ |
| `double` | `DOUBLE PRECISION` | ✅ |
| `string`, `large_string` | `TEXT` | ✅ |
| `string_view` | `TEXT` | ❌ <sup>[10](#fn10)</sup> |
| `binary`, `large_binary`, `fixed_size_binary` | `BYTEA` | ✅ |
| `binary_view` | `BYTEA` | ❌ <sup>[10](#fn10)</sup> |
| `date32[day]`, `date64[ms]` | `DATE` | ✅ |
| `timestamp[s\|ms\|us\|ns]` | `TIMESTAMP` | ✅ |
| `timestamp` (with time zone) | `TIMESTAMPTZ` | ✅ <sup>[11](#fn11)</sup> |
| `decimal128(p, s)` | `NUMERIC(p, s)` | ✅ <sup>[12](#fn12)</sup> |
| `decimal32`, `decimal64`, `decimal256` | `NUMERIC(p, s)` | ❌ <sup>[13](#fn13)</sup> |
| `list<T>`, `large_list<T>` | `ARRAY(T)` | ✅ |
| `fixed_size_list<T>` | `ARRAY(T)` | ❌ <sup>[14](#fn14)</sup> |
| `struct<…>` | `STRUCT(…)` | ✅ <sup>[3](#fn3)</sup> |
| `dictionary` | (no mapping) | ❌ <sup>[15](#fn15)</sup> |
| `map` | (no mapping) | ❌ <sup>[16](#fn16)</sup> |
| `null`, `duration`, `interval`, `time32`, `time64` | (no mapping) | ❌ <sup>[16](#fn16)</sup> |

`ARRAY` and `STRUCT` compose to any depth, so `list<struct<…>>` and
`struct<…, list<…>>` both generate correct DDL.

### Footnotes

1. <a id="fn1"></a>Firebolt has no session-temporary tables.
   `adbc.ingest.temporary=true` returns `ADBC_STATUS_NOT_IMPLEMENTED`; `false` is
   accepted as a no-op, which is what the Python dbapi layer sends by default.
2. <a id="fn2"></a>Honoured as the leading part of a qualified name. Set a schema
   alongside it: a catalog on its own renders a two-part name that Firebolt reads
   as `schema.table`.
3. <a id="fn3"></a>A non-nullable Arrow *column* becomes `NOT NULL`. A
   non-nullable *struct field* is widened to nullable, because Firebolt rejects
   `STRUCT(… NOT NULL)` with "STRUCT fields have to be nullable". A zero-field
   struct has no mapping — there is no `STRUCT()` in Firebolt — so ingest fails
   rather than emitting DDL the server would reject.
4. <a id="fn4"></a>Firebolt's HTTP interface has no parameter binding, so
   `GetParameterSchema` returns `ADBC_STATUS_NOT_IMPLEMENTED` and
   `cursor.execute(sql, params)` raises `NotSupportedError`. Inline literals into
   the SQL.
5. <a id="fn5"></a>`Prepare` succeeds but is a no-op: there is no server-side
   prepare. Calling it is harmless and buys nothing.
6. <a id="fn6"></a>The server does not report affected rows over this interface.
   Use `SELECT count(*)` when you need a number.
7. <a id="fn7"></a>`GetOption*`, `SetOptionInt`/`Double`/`Bytes`, `Cancel`,
   `ExecuteSchema`, `GetStatistics`, `ErrorGetDetail`. The driver implements the
   ADBC 1.0.0 function set, although `GetInfo` reports ADBC 1.1.0.
8. <a id="fn8"></a>Raw WKB bytes, with no `geoarrow` extension metadata — unlike
   some other drivers, which surface `extension<geoarrow.wkt>`.
9. <a id="fn9"></a>`BIGINT` is signed, so a value above `int64` max
   (9223372036854775807) is rejected by the server with "Convert overflow".
   Values at or below it round-trip exactly.
10. <a id="fn10"></a>The driver maps the view types to `TEXT`/`BYTEA`, but
    nanoarrow's IPC writer cannot encode Arrow's view layouts, so the upload fails
    with an `ADBC_STATUS_INTERNAL` error before reaching the server. Cast to
    `string`/`binary` first.
11. <a id="fn11"></a>The instant is preserved and normalised to UTC; the original
    offset is not retained. `12:00+02:00` reads back as `10:00Z`.
12. <a id="fn12"></a>Precision must be 38 or less, Firebolt's maximum. Above that
    the server rejects it with "Decimal precision overflow".
13. <a id="fn13"></a>The server cannot read these Arrow decimal layouts, and
    `decimal256` fails even within precision 38. Cast to `decimal128`.
14. <a id="fn14"></a>The generated DDL is correct, but the server rejects the
    fixed-size-list schema in the uploaded file. Cast to `list`.
15. <a id="fn15"></a>nanoarrow's IPC writer refuses dictionary encoding. Decode to
    the value type before ingesting.
16. <a id="fn16"></a>No Firebolt equivalent, so ingest fails with
    `ADBC_STATUS_NOT_IMPLEMENTED` rather than uploading DDL the server would
    reject.

## Options

The four database options, in full:

| Key | Required | Meaning |
|-----|----------|---------|
| `uri` | **yes** | Engine HTTP endpoint, e.g. `http://localhost:3473`. |
| `adbc.firebolt.database` | no | Database name; sent as `database=` on every request. |
| `adbc.firebolt.token` | no | Bearer token. Omit for an engine with authentication disabled. |
| `adbc.firebolt.timeout_sec` | no | Request timeout in whole seconds; `0` (the default) disables it. |

Connection and statement options, ingest modes, the session-parameter protocol,
and the type mapping are all in **[OPTIONS.md](OPTIONS.md)**.

Note that these names are provisional: Firebolt is standardising one parameter
set across all its SDKs, and this driver has not migrated yet. The mapping from
today's names to the canonical ones is in
[OPTIONS.md](OPTIONS.md#names-that-are-going-to-change).

## Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| `Database 'uri' option is required` | No `uri` in `db_kwargs`. |
| `Database 'uri' must start with http:// or https://` | You passed a bare host (`localhost:3473`) or a `firebolt://` URI. This driver takes the engine's HTTP endpoint. |
| `... is https:// but this driver was built without TLS support` | Expected — see [Supported today](#supported-today). Use an `http://` endpoint, or build with `-DWITH_SSL=ON`. |
| `IO: curl error: Couldn't connect to server` | Nothing is listening. Check the container is up and the port matches: `curl -fsS http://localhost:3473/ping`. |
| `Cluster not yet healthy` | The engine answers `/ping` before it can serve queries. Retry `SELECT 1` for a few seconds. |
| `UNAUTHORIZED: HTTP 401` / `403` | The engine wants authentication. Supply `adbc.firebolt.token`; see [docs/authentication.md](docs/authentication.md). |
| `NOT_IMPLEMENTED: Query parameter binding is not supported` | `cursor.execute(sql, params)` — inline literals into the SQL instead. |
| `NOT_FOUND: Unknown Firebolt database option '…'` | A misspelled `adbc.firebolt.*` key. Compare against [OPTIONS.md](OPTIONS.md). |
| `NOT_IMPLEMENTED: Temporary ingest tables are not supported` | `adbc_ingest(..., temporary=True)`. Firebolt has no session-temporary tables. |
| `NOT_IMPLEMENTED: ingest column type cannot be mapped …` | The Arrow schema has a type with no Firebolt equivalent. Cast it before ingesting. |
| `current transaction is aborted, commands will be ignored …` | A statement failed inside an open transaction. Call `conn.rollback()`. `dbapi.connect()` disables autocommit by default, so you may be in a transaction you did not open. |
| `INVALID_ARGUMENT: Option 'adbc.connection.autocommit' must be exactly …` | Use the string `"true"` or `"false"`; `"0"` and `"FALSE"` are refused rather than guessed at. |
| `dlopen() failed: … cannot open shared object file` | The driver path is wrong, or the manifest name does not match the `driver=` value. |
| `cursor.rowcount` is `-1` | Expected; see [Feature & Type Support](#feature--type-support). Use `SELECT count(*)` if you need a count. |

## Build from source

Only needed to develop the driver — consumers should use a
[release](https://github.com/firebolt-db/firebolt-adbc/releases). Requires Docker
and git.

```bash
./scripts/build.sh              # → build/libfirebolt_adbc.so
./scripts/test-unit.sh          # C++ unit tests, no server needed
./scripts/test-integration.sh   # pytest against a throwaway 1-node engine
```

`scripts/build.sh` builds inside a pinned Ubuntu 22.04 + clang-18 image, so the
resulting `.so` runs on older glibc than the host. Details and the dependency
policy are in [CONTRIBUTING.md](CONTRIBUTING.md) and [CLAUDE.md](CLAUDE.md).

## Documentation

| | |
|---|---|
| [OPTIONS.md](OPTIONS.md) | Every option at all three ADBC levels |
| [docs/authentication.md](docs/authentication.md) | What works now, Firebolt's actual auth model, and the gaps |
| [examples/python/](examples/python) | Runnable scripts for each task above |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building, testing, and submitting changes |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

## License

Apache 2.0 — see [LICENSE](LICENSE).
