# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] — unreleased

First release. The driver already implemented queries, bulk ingest, metadata, and
transactions; this release makes it possible to *obtain and use* without building
it from source.

### Added

- Prebuilt `libfirebolt_adbc.so` for Linux x86_64 and aarch64, published to
  GitHub Releases with `sha256` checksums by `.github/workflows/release.yaml`.
- `firebolt.toml` ADBC driver manifest, so callers can use `driver="firebolt"`
  instead of an absolute path to the shared library.
- Documentation: a real `README.md` with a quickstart, Feature & Type Support tables and a
  troubleshooting table; `OPTIONS.md` covering every option at all three ADBC
  levels; `docs/authentication.md`; and `CONTRIBUTING.md`.
- Runnable examples under `examples/python/` — connect and query, results into
  pandas/polars with batch streaming, and bulk ingest including nested
  `ARRAY`/`STRUCT` columns. Covered by a CI smoke check.
- The driver version now comes from `project(firebolt_adbc VERSION ...)` via the
  generated `src/Version.h`, feeding both the shared library soname and
  `ADBC_INFO_DRIVER_VERSION`.

### Changed

- The shared library version is now `0.1.0` (soname `.so.0`), replacing the
  hardcoded `1.1.0` that mirrored the ADBC specification version.
  `ADBC_INFO_DRIVER_VERSION`, which independently reported `1.0.0`, agrees with
  it for the first time.

### Fixed

- A non-numeric or out-of-range `adbc.firebolt.timeout_sec` threw
  `std::invalid_argument` / `std::out_of_range` out through the C ABI, aborting
  the host process — a mistyped option would take down the calling Python
  interpreter. It now returns `ADBC_STATUS_INVALID_ARGUMENT`. A negative value is
  rejected too.
- An unrecognised `adbc.firebolt.*` database option was accepted and discarded,
  so a typo such as `adbc.firebolt.databse` silently connected to the wrong
  database. It now returns `ADBC_STATUS_NOT_FOUND`. Keys outside the
  `adbc.firebolt.*` namespace are still accepted, since the driver manager sets
  some of them itself.
- Both of the above are reported when the database is opened rather than from the
  individual option call. A driver manager replays options set before the driver
  was loaded from inside `AdbcDatabaseInit`, and the failure path of that replay
  in adbc-driver-manager (through at least 1.8.0) overflows a heap buffer by one
  byte and aborts the process — so returning a failure from `DatabaseSetOption`
  during the replay would have reintroduced the crash it was meant to remove.
  Options set after the database is open are still refused immediately.
- `uri` is validated at `DatabaseInit`: a missing or unsupported scheme, and an
  `https://` endpoint on a build without TLS, now produce an actionable error
  instead of surfacing as `CURLE_UNSUPPORTED_PROTOCOL` at the first query.
- Binding data without an ingest target — the shape produced by
  `cursor.execute(sql, parameters)` — took the multipart-insert path anyway,
  posting the caller's SQL with a stray `data.arrow` part and failing server-side
  with an opaque upload error. It now returns `ADBC_STATUS_NOT_IMPLEMENTED`
  explaining that parameter binding is not supported.

### Known limitations

See [README.md](README.md#supported-today). In short: plaintext `http://` only
(no TLS in this build), authentication-disabled engines plus an optional
pre-obtained bearer token, no discovery-based OAuth, Linux only, and no
parameterized queries.

[Unreleased]: https://github.com/firebolt-db/firebolt-adbc/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/firebolt-db/firebolt-adbc/releases/tag/v0.1.0
