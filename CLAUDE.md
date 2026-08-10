# CLAUDE.md — Firebolt ADBC Driver

## Project Overview

A standalone C++ shared library (`libfirebolt_adbc.so`) implementing the
[ADBC 1.1.0](https://arrow.apache.org/adbc/) C API against Firebolt's HTTP query
interface. Client-side only — loaded at runtime by ADBC driver managers (Python
`adbc_driver_manager`, R `adbcdrivermanager`, etc.).

This repository (`firebolt-db/firebolt-adbc`) is the driver and nothing else: a
**completely independent CMake project** with no coupling to the packdb build
system. It was extracted from `adbc/` in the packdb repo — paths in this document
are relative to *this* repository's root, not to packdb.

Private for now; it will be made public once it is ready.

## Directory layout

```
.
├── README.md                              # user-facing overview, quickstart, feature/type support
├── OPTIONS.md                             # full options reference (all three levels)
├── CHANGELOG.md                           # release history (Keep a Changelog)
├── CONTRIBUTING.md                        # build/test/release workflow for contributors
├── CLAUDE.md                              # this file
├── firebolt.toml                          # ADBC driver manifest (shipped with releases)
│
├── docs/
│   └── authentication.md                  # what auth works today + the SDK-spec target model
│
├── examples/
│   └── python/                            # runnable, env-var configured, no framework
│       ├── quickstart.py                  # connect + query, dbapi and low-level paths
│       ├── query_to_pandas.py             # pyarrow / pandas / polars + batch streaming
│       └── bulk_ingest.py                 # 4 ingest modes, nested ARRAY/STRUCT, bind_stream
│
├── CMakeLists.txt                         # standalone CMake project; project(VERSION) is authoritative
├── CMakePresets.json                      # presets: standalone-clang / standalone-gcc
├── firebolt_adbc.version                  # linker version script (exports only AdbcDriverInit)
├── adbc.h                                 # vendored ADBC 1.1.0 C header + Arrow C ABI structs
├── submodule/                             # all non-system deps (git submodules + local static builds)
│
├── src/                                   # driver implementation
│   ├── FireboltAdbcDriver.cpp             # ADBC function table, entry point, curl init/cleanup
│   ├── FireboltAdbcDatabase.h             # FireboltDatabase struct (AdbcDatabase::private_data)
│   ├── FireboltAdbcConnection.h           # FireboltConnection struct; owns HttpClient + session state
│   ├── FireboltAdbcStatement.h            # FireboltStatement struct; holds SQL + bound IPC bytes
│   ├── FireboltAdbcMetadata.h/.cpp        # GetInfo, GetTableTypes, GetTableSchema, GetObjects
│   ├── HttpClient.h/.cpp                  # libcurl wrapper: query POST, multipart insert, session headers
│   ├── HttpHeaderParse.h/.cpp             # session-state response header parsing
│   ├── IngestSqlBuilder.h/.cpp            # identifier quoting, Arrow→Firebolt types, ingest DDL/DML
│   ├── ScopeGuard.h                       # RAII exit guard
│   ├── Version.h.in                       # → build/generated/Version.h; FIREBOLT_ADBC_VERSION
│   └── ArrowIpcStream.h/.cpp              # Arrow IPC bytes → ArrowArrayStream via nanoarrow 0.8.0
│
├── scripts/                               # locally runnable, idempotent — also called from CI
│   ├── build.sh                           # builds inside the pinned firebolt-adbc-builder Docker
│   │                                      #   image (Ubuntu 22.04 + clang-18) for glibc portability;
│   │                                      #   inits submodules, cmake + ninja, tests ON, SSL OFF
│   ├── test-unit.sh                       # ctest --output-on-failure in build/
│   └── test-integration.sh                # forwards args to tests/integration/runner.py
│
├── docker/
│   └── builder/Dockerfile                 # the pinned build image scripts/build.sh uses
│
├── .github/
│   └── workflows/
│       ├── enable-merge-to-main.yaml      # build + unit + integration + examples on every PR
│       └── release.yaml                   # tag v* → multi-arch .so + sha256 + manifest on a release
│
└── tests/
    ├── unit/
    │   └── adbc_driver_test.cpp           # Google Test unit tests (no server needed)
    └── integration/                       # pytest harness — runs inside a runner container
        ├── runner.py                      # spins up runner image + 1-node Firebolt engine,
        │                                  #   exec's pytest. Args: --engine-image, --adbc-binary
        ├── conftest.py                    # shared fixtures: started_engine, server_url, conn,
        │                                  #   dbapi_conn, cursor, run_query, ingest, table_name,
        │                                  #   temp_table, scratch_table, mock_server
        ├── pytest.ini                     # python_files = test.py
        ├── docker/
        │   ├── Dockerfile                 # firebolt-adbc-integration-test-runner image
        │   └── requirements.txt           # adbc-driver-manager, pyarrow, pytest, requests
        ├── helpers/
        │   ├── __init__.py
        │   └── firebolt_engine.py         # minimal docker-compose 1-node Firebolt engine fixture
        └── tests/                         # one directory per test area
            ├── adbc_sanity/test.py        # connectivity, literal selects, arithmetic, strings
            ├── advanced_queries/test.py   # joins, CTEs, CASE, HAVING, subqueries, window funcs
            ├── array_type/test.py         # ARRAY literals, roundtrip, functions, nested arrays
            ├── arrow/test.py              # schema, result shape, error handling, C-stream
            ├── datatypes/test.py          # int/float/string/bool/null/date/timestamp scalars
            ├── decimal_type/test.py       # DECIMAL literals, arithmetic, precision/scale
            ├── dml/test.py                # DDL, INSERT/SELECT, aggregates, type roundtrip
            ├── ingest/test.py             # bulk ingest via dbapi Cursor.adbc_ingest()
            ├── ingest_low_level/test.py   # bulk ingest via set_options + bind_stream
            ├── security_*/test.py         # regression tests for fixed security issues
            └── struct_type/test.py        # STRUCT / ARRAY(STRUCT) retrieval and ingest
```

## Build

### Recommended: `./scripts/build.sh`

Builds inside the pinned `firebolt-adbc-builder:latest` image (Ubuntu 22.04 +
clang-18), which it builds from `docker/builder/Dockerfile` on first use. The
older glibc is the point: the resulting `.so` needs only glibc 2.34, so it loads
on distributions older than the host. This is what CI and the release workflow
use, and it passes `-DFIREBOLT_ADBC_BUILD_TESTS=ON -DWITH_SSL=OFF`.

```bash
./scripts/build.sh
# → build/libfirebolt_adbc.so -> libfirebolt_adbc.so.0 -> libfirebolt_adbc.so.0.1.0
```

### Direct host build (faster to iterate, not shippable)

Carries the host's glibc requirement, so never release the output.

```bash
git submodule update --init --recursive
cmake --preset standalone-clang -DFIREBOLT_ADBC_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```

`standalone-gcc` is the same dependency policy with GCC. Note that a `build/`
directory configured on the host can break `scripts/build.sh` afterwards — a
`ccache` compiler launcher baked into `CMakeCache.txt` does not exist inside the
builder image. Delete `build/` and re-run.

## Testing

The reusable scripts under `scripts/` are the canonical entry points — both
local development and CI invoke the same commands.

```bash
# Build with -DFIREBOLT_ADBC_BUILD_TESTS=ON (idempotent):
./scripts/build.sh

# C++ unit tests (no server needed):
./scripts/test-unit.sh

# Integration tests against a 1-node Firebolt engine (Docker required):
./scripts/test-integration.sh                                    # all tests
./scripts/test-integration.sh -k test_connect                    # filter by name
./scripts/test-integration.sh tests/dml                          # one suite
./scripts/test-integration.sh --engine-image=...:latest -x       # override engine image
```

`runner.py` builds `firebolt-adbc-integration-test-runner:latest` locally on
first invocation (from `tests/integration/docker/Dockerfile`) — no registry
needed for the runner. The engine image (`--engine-image`, default in
`runner.py::DEFAULT_ENGINE_IMAGE`) is pulled from the public GHCR repo on every
run — it is a floating `:latest` tag, so a cached copy is only used as a
fallback when the pull fails.

The image ships the unified `firebolt` binary (server + client): its entrypoint
execs `firebolt <args>` and its default command is
`server --data-dir /var/lib/firebolt`. With no config file supplied the server
starts from its built-in structured (YAML) defaults — one node, all interfaces,
default ports — so the 1-node fixture writes no config at all. The legacy
`--node N` + `/firebolt-core/config.json` startup contract is gone; a multi-node
setup would bind-mount a `config.yaml` at `/var/lib/firebolt/config.yaml`.

## Key Design Decisions

- **No packdb internal headers** — the `.so` must be loadable outside the server process.
- **No dependency discovery in CMake** — this project does not use `find_package`,
  `find_library`, `find_path`, `find_program`, or `FetchContent` for required deps.
  All non-system deps must exist under `submodule`.
- **nanoarrow** instead of packdb's `arrow_static` — `arrow_static` is compiled without
  `-fPIC` (it targets an executable) so it can't be linked into a shared library.
  nanoarrow is Apache Arrow's official embedded C implementation: zero external deps,
  fully PIC, ~30 KB compiled, supports the full Arrow IPC stream format.
- **Static third-party deps** — `curl`, `BoringSSL`, `c-ares`, `nanoarrow`, and test
  dependencies are linked statically from `submodule` build outputs. System runtime
  libs (e.g. `libc`, `libm`, `libdl`, `libpthread`) remain dynamic.
- **Version script** (`firebolt_adbc.version`) — exports only `AdbcDriverInit` and
  `FireboltAdbcDriverInit`; all other symbols (including libc++ internals) are hidden.
- **Post-build dependency report** — every build prints concise `DT_NEEDED` `.so` names
  for `libfirebolt_adbc.so` so dynamic dependencies are visible in Ninja logs.
- **SQL injection safety** — `quoteIdentifier()` in `IngestSqlBuilder.cpp` wraps table
  names, column names and struct field names in double quotes (embedding `"` doubled)
  before they are interpolated into auto-generated DDL/INSERT SQL.
- **Nested-type DDL follows Firebolt's nullability rules** — `arrowTypeToFireboltSqlType()`
  renders Arrow STRUCT as `STRUCT("field" TYPE, …)` and LIST/LARGE_LIST/FIXED_SIZE_LIST as
  `ARRAY(TYPE)`, recursing to any depth. A non-nullable Arrow *field* is widened to nullable
  because Firebolt rejects `STRUCT(… NOT NULL)` ("STRUCT fields have to be nullable"); `NOT
  NULL` is only emitted for a non-nullable top-level *column*. A zero-field struct maps to
  nothing (there is no `STRUCT()` in Firebolt), so ingest fails with
  `ADBC_STATUS_NOT_IMPLEMENTED` instead of uploading DDL the server would reject. MAP has no
  mapping either and fails the same way.
- **Single source of truth for the engine image** — the registry string lives only in
  `tests/integration/runner.py::DEFAULT_ENGINE_IMAGE`, flows through the
  `FIREBOLT_ENGINE_IMAGE` env var into
  `helpers/firebolt_engine.py::FireboltInstance.__init__`, and is inlined into the
  generated docker-compose yaml at run time.
- **Readiness is `/ping` *and* `SELECT 1`** — `/ping` turns green before the engine can
  serve queries ("Cluster not yet healthy"), so `FireboltInstance.start()` probes both
  before handing the URL to tests.
- **Generated test artifacts are gitignored** — every test run regenerates
  `tests/integration/_test_runtime_root/` (compose yaml, container logs);
  the directory is in `.gitignore` so it is never committed.
- **One version number** — `project(firebolt_adbc VERSION …)` in `CMakeLists.txt` is
  authoritative. `configure_file` renders `src/Version.h.in` into
  `build/generated/Version.h`, whose `FIREBOLT_ADBC_VERSION` supplies
  `ADBC_INFO_DRIVER_VERSION`; the same value sets the target `VERSION`/`SOVERSION`.
  `release.yaml` refuses to publish when the git tag disagrees with it.
- **A bad option is an error, not a shrug** — an unrecognised `adbc.firebolt.*` database
  key returns `ADBC_STATUS_NOT_FOUND` (keys outside that namespace stay accepted, since
  the driver manager sets some itself), a malformed `timeout_sec` returns
  `ADBC_STATUS_INVALID_ARGUMENT` rather than throwing `std::invalid_argument` through the
  C ABI and aborting the host process, and `uri` is scheme-checked at `DatabaseInit`.
- **A rejected database option is reported by `DatabaseInit`, not by `DatabaseSetOption`** —
  see `RejectOption()`. This is a workaround for a heap overflow in the driver manager, not
  a style choice. A driver manager buffers options set before the driver is loaded and
  replays them from inside `AdbcDatabaseInit`; the failure path of that replay in
  adbc-driver-manager (through at least 1.8.0, `adbc_driver_manager.cc`
  `SetError(AdbcError*, AdbcError*)`) does
  `error->message = new char[strlen(src)]` and then writes the terminator at
  `[strlen(src)]` — one byte past the allocation, which aborts the process. Conventional
  drivers accept and discard unknown options, so nothing had exercised it. Returning the
  error from `Init` instead keeps the diagnostic; the manager forwards `Init`'s error
  struct through untouched. Options set *after* `Init` are refused immediately, since no
  replay is involved. Revisit if the upstream bug is fixed and the pinned version moves.
- **No exception may cross the C ABI** — the caller is a C driver manager with no handler,
  so anything that escapes aborts the host process. Every entry point that can throw
  wraps its body; `std::stol` and friends need explicit guards.
- **TLS capability is asked of libcurl, not tracked in a define** — `curl_version_info`
  reports whether the linked curl has SSL, so the `https://` rejection is always correct
  for the library actually loaded rather than for what the build flags claimed.
- **Parameter binding is refused explicitly** — Firebolt's HTTP interface has none, and
  `Bind`/`BindStream` here exist only to carry an ingest payload. Bound data with no
  ingest target returns `ADBC_STATUS_NOT_IMPLEMENTED` instead of silently taking the
  multipart path and failing server-side with an opaque upload error.

## Dependencies

All required non-system deps are expected under `submodule`:

| Dep | Path |
|-----|---------------------|
| curl headers | `submodule/curl/include` |
| libcurl | `build/submodule/curl/lib/libcurl.a` |
| BoringSSL | `build/submodule/boringssl/libssl.a`, `libcrypto.a` |
| c-ares | `build/submodule/c-ares/src/lib/libcares.a` |
| nanoarrow source | `submodule/nanoarrow` |
| googletest source (tests) | `submodule/googletest` |

## ADBC Options Reference

**[OPTIONS.md](OPTIONS.md) is the authoritative reference** — every option, every
error status, and the full Arrow→Firebolt type mapping. Summary only here.

| Key | Set on | Description |
|-----|--------|-------------|
| `"uri"` | Database | HTTP query endpoint, e.g. `http://localhost:3473`. Scheme-validated at `Init`; `https://` requires a `-DWITH_SSL=ON` build, which is not what we ship. |
| `"adbc.firebolt.token"` | Database, Connection | Bearer token — omit for an auth-disabled engine. Per-connection when set on the connection. Contradicts the SDK auth spec (a raw JWT belongs in `FIREBOLT_TOKEN`) and will be removed; see `docs/authentication.md`. |
| `"adbc.firebolt.database"` | Database | Database name (appended as `?database=…` query param) |
| `"adbc.firebolt.timeout_sec"` | Database | Total request timeout in whole seconds; `0` (the default) disables it |
| `ADBC_CONNECTION_OPTION_AUTOCOMMIT` | Connection | `false` enables explicit transactions: lazy `BEGIN`, then `Commit`/`Rollback` |
| `ADBC_INGEST_OPTION_TARGET_TABLE` | Statement | Target table for the bind-data ingest path; auto-generates `INSERT INTO {target} ({cols}) SELECT * FROM read_arrow('upload://data.arrow')` on `ExecuteUpdate` |
| Unknown keys on `ConnectionSetOption` after init | Connection | Stored as session params appended to query URL |

`ADBC_INGEST_OPTION_MODE` (append / create / replace / create_append) and the
catalog/schema target options are honoured: the create-style modes synthesise
`CREATE TABLE` DDL from the bound Arrow schema, including nested
`STRUCT`/`ARRAY` columns. `ADBC_INGEST_OPTION_TEMPORARY` is rejected with
`ADBC_STATUS_NOT_IMPLEMENTED` — Firebolt has no session-temporary tables. Both
the high-level `adbc_driver_manager.dbapi.Cursor.adbc_ingest()` and the
low-level `bind_stream` + `execute_update` path land data in the target table
(`tests/integration/tests/ingest/`, `ingest_low_level/`, `struct_type/`;
`dml/test.py` covers INSERT-via-SQL).

## HTTP Protocol

- **SELECT / DDL**: `POST {url}?output_format=ArrowStream&database=...` with URL-encoded
  SQL body. Response is an Arrow IPC stream. Sends `Firebolt-Protocol-Version: 2.4`.
- **INSERT with bind data**: `POST {url}` as `multipart/form-data` — one `sql` part
  referencing `upload://data.arrow`, one `data.arrow` part with Arrow IPC bytes.
- **Session state**: updated via `Firebolt-Update-Parameters` / `Firebolt-Remove-Parameters`
  / `Firebolt-Reset-Session` response headers (see `src/HttpClient.cpp`). Applied only on a
  successful response — a 4xx/5xx body may come from a proxy or an attacker.

## Authentication: where this driver stands

**Do not infer Firebolt's auth model from this driver's options, and do not carry over
the Firebolt SaaS 2.0 model.** There are no service accounts, no `account_name`, no
`api_endpoint`, and no control-plane engine resolution; those are explicitly legacy.

The authoritative specs live in the packdb repo, and `specs/sdk-authentication.md` names
ADBC directly. In short: a client discovers everything from
`GET <host>/.well-known/firebolt` (`instance.auth` is `null` for auth-disabled, else it
carries `oauth.protectedResource` plus `authorizationServers[]` and an optional
`preferredAuthorizationServer`), then runs OAuth 2.0 `client_credentials` against the
selected server's `token_endpoint` with `username`→`client_id`, `password`→`client_secret`
and the RFC 8707 `resource` bound to the instance. Token precedence is `FIREBOLT_TOKEN`
(one-shot, never cached) > connection credentials > optionally `firebolt token <host>`.
Transport is a separate `ssl_mode` parameter defaulting to `verify-full`.

This driver implements **none** of that yet: no discovery, no `client_credentials`, no
`FIREBOLT_TOKEN`, no `ssl_mode`, no TLS in the shipped build, and canonical parameter
names (`host`, `database`, `query_timeout`, … per
`specs/schemas/connection-parameters.v1.json`) not yet adopted. `docs/authentication.md`
documents the gap for users; the rename, when it happens, replaces the current
`adbc.firebolt.*` keys outright rather than aliasing them — nothing external consumes
them yet.
