#include "ArrowIpcStream.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcDatabase.h"
#include "FireboltAdbcMetadata.h"
#include "FireboltAdbcStatement.h"
#include "HttpClient.h"
#include "IngestSqlBuilder.h"
#include "ScopeGuard.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <strings.h>

#include <curl/curl.h>

namespace firebolt::adbc
{

// ============================================================
// Utility helpers
// ============================================================

// SQL identifier quoting, table qualification, and Arrow→Firebolt type mapping
// helpers live in IngestSqlBuilder.{h,cpp}.

static AdbcStatusCode SetError(AdbcError * e, AdbcStatusCode code, const std::string & msg)
{
    if (e)
    {
        e->message = strdup(msg.c_str());
        e->release = [](AdbcError * err) {
            free(err->message);
            err->message = nullptr;
            err->release = nullptr;
        };
        e->vendor_code = 0;
    }
    return code;
}

static AdbcStatusCode HttpRespToStatus(const HttpResponse & resp, AdbcError * error)
{
    if (resp.curl_code != CURLE_OK)
        return SetError(error, ADBC_STATUS_IO, "curl error: " + std::string(curl_easy_strerror(resp.curl_code)));
    if (resp.http_code == 401 || resp.http_code == 403)
        return SetError(error, ADBC_STATUS_UNAUTHORIZED, "HTTP " + std::to_string(resp.http_code) + ": " + resp.error_message);
    if (resp.http_code >= 400 && resp.http_code < 500)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "HTTP " + std::to_string(resp.http_code) + ": " + resp.error_message);
    if (resp.http_code >= 500)
        return SetError(error, ADBC_STATUS_IO, "HTTP " + std::to_string(resp.http_code) + ": " + resp.error_message);
    return ADBC_STATUS_OK;
}

// Case-insensitive check that `url` begins with `scheme` (URI schemes are
// case-insensitive per RFC 3986 §3.1).
static bool hasSchemePrefix(const std::string & url, const char * scheme)
{
    const size_t n = strlen(scheme);
    return url.size() >= n && strncasecmp(url.c_str(), scheme, n) == 0;
}

// Whether the libcurl we are linked against can speak TLS.  Asked of libcurl
// rather than tracked as a build-time define so the answer always matches the
// library actually loaded.
static bool curlSupportsTls()
{
    const curl_version_info_data * v = curl_version_info(CURLVERSION_NOW);
    return v != nullptr && (v->features & CURL_VERSION_SSL) != 0;
}

// Apply server-advertised session updates only when the response succeeded.
// See applySessionUpdatesIfSuccess in HttpClient.h for the rationale — a 5xx
// or 4xx response can be from a MITM or a generic-error proxy, so its
// session-state hints are untrusted.
static void ApplySessionUpdates(FireboltConnection * conn, const HttpResponse & resp)
{
    applySessionUpdatesIfSuccess(conn->session_params, resp);
}

// ============================================================
// curl global init / cleanup (once per process)
// ============================================================

static std::once_flag g_curl_init_flag;

static void initCurl()
{
    std::call_once(g_curl_init_flag, []() { curl_global_init(CURL_GLOBAL_ALL); });
}

__attribute__((destructor)) static void cleanupCurl()
{
    curl_global_cleanup();
}

// Execute a simple SQL statement (no result rows expected).
// Used for BEGIN / COMMIT / ROLLBACK.
static AdbcStatusCode RunSimpleSql(FireboltConnection * conn, const char * sql, AdbcError * error)
{
    auto resp = conn->http->executeQuery(sql, conn->session_params);
    ApplySessionUpdates(conn, resp);
    if (!resp.isSuccess())
        return HttpRespToStatus(resp, error);
    return ADBC_STATUS_OK;
}

// ============================================================
// Arrow IPC serialisation helper for StatementBind / BindStream
// Serialises an ArrowArrayStream into Arrow IPC stream bytes using nanoarrow.
// ============================================================

static std::string SerializeArrayStream(ArrowArrayStream * stream, std::vector<uint8_t> & out)
{
    // ArrowIpcOutputStreamInitBuffer stores a pointer to buf (does NOT take ownership).
    // ArrowIpcWriterInit takes ownership of the output stream via ArrowIpcOutputStreamMove.
    // After ArrowIpcWriterReset, buf is still valid and holds the written IPC bytes.
    ArrowBuffer buf{};
    ArrowBufferInit(&buf);

    ArrowIpcOutputStream out_stream{};
    if (ArrowIpcOutputStreamInitBuffer(&out_stream, &buf) != 0)
    {
        ArrowBufferReset(&buf);
        return "ArrowIpcOutputStreamInitBuffer failed";
    }

    ArrowIpcWriter writer{};
    if (ArrowIpcWriterInit(&writer, &out_stream) != 0)
    {
        if (out_stream.release)
            out_stream.release(&out_stream);
        ArrowBufferReset(&buf);
        return "ArrowIpcWriterInit failed";
    }
    // writer owns out_stream now; out_stream.release should not be called separately

    ArrowError error{};
    int rc = ArrowIpcWriterWriteArrayStream(&writer, stream, &error);
    ArrowIpcWriterReset(&writer); // releases writer + out_stream (not buf)

    if (rc != 0)
    {
        ArrowBufferReset(&buf);
        return std::string("ArrowIpcWriterWriteArrayStream: ") + error.message;
    }

    out.assign(buf.data, buf.data + buf.size_bytes);
    ArrowBufferReset(&buf);
    return {};
}

// ============================================================
// Database
// ============================================================

static AdbcStatusCode DatabaseNew(AdbcDatabase * db, AdbcError * error)
{
    if (!db)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcDatabase is null");
    try
    {
        db->private_data = new FireboltDatabase();
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

// Refuse an option, but not necessarily right now.
//
// A driver manager collects options set before the driver is even loaded and
// replays them from inside AdbcDatabaseInit.  The failure path of that replay in
// adbc-driver-manager (through at least 1.8.0) copies the driver's error with
//
//     error->message = new char[strlen(src)];  error->message[strlen(src)] = '\0';
//
// — a one-byte heap overflow that aborts the host process.  No driver had
// exercised it, because drivers conventionally accept and discard unknown
// options.  Reporting from DatabaseInit instead keeps the diagnostic and avoids
// that path: the manager forwards Init's error struct through untouched.
//
// Once the database is initialised there is no replay involved, so an option set
// after that point is refused immediately.
static AdbcStatusCode RejectOption(FireboltDatabase * fdb, AdbcError * error, AdbcStatusCode code, std::string message)
{
    if (fdb->initialized)
        return SetError(error, code, message);
    if (fdb->option_error.empty())
    {
        fdb->option_error = std::move(message);
        fdb->option_error_code = code;
    }
    return ADBC_STATUS_OK;
}

static AdbcStatusCode DatabaseSetOption(AdbcDatabase * db, const char * key, const char * value, AdbcError * error)
{
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);
    if (!key)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Option key is null");
    const std::string k(key);
    const std::string v(value ? value : "");
    if (k == "uri")
        fdb->url = v;
    else if (k == "adbc.firebolt.token")
        fdb->token = v;
    else if (k == "adbc.firebolt.database")
        fdb->database = v;
    else if (k == "adbc.firebolt.timeout_sec")
    {
        // std::stol throws on non-numeric and out-of-range input.  Letting that
        // propagate would unwind through the C ABI into a driver manager with no
        // handler, aborting the host process (a Python interpreter, an R session)
        // over a mistyped option.
        long parsed = 0;
        try
        {
            size_t consumed = 0;
            parsed = std::stol(v, &consumed);
            if (consumed != v.size())
                throw std::invalid_argument("trailing characters");
        }
        catch (const std::exception &)
        {
            return RejectOption(
                fdb,
                error,
                ADBC_STATUS_INVALID_ARGUMENT,
                "Option 'adbc.firebolt.timeout_sec' must be a whole number of seconds (0 disables the timeout); got '" + v + "'");
        }
        if (parsed < 0)
            return RejectOption(
                fdb,
                error,
                ADBC_STATUS_INVALID_ARGUMENT,
                "Option 'adbc.firebolt.timeout_sec' must not be negative (0 disables the timeout); got '" + v + "'");
        fdb->timeout_sec = parsed;
    }
    else if (k.rfind("adbc.firebolt.", 0) == 0)
    {
        // A misspelled driver option is a configuration bug.  Accepting it
        // silently produced connections that used the server's defaults with no
        // diagnostic anywhere.
        return RejectOption(fdb, error, ADBC_STATUS_NOT_FOUND, "Unknown Firebolt database option '" + k + "'");
    }
    // Keys outside the adbc.firebolt.* namespace are accepted and ignored: the
    // driver manager sets some of them itself, and callers pass connection
    // parameters this driver does not consume yet.
    return ADBC_STATUS_OK;
}

static AdbcStatusCode DatabaseInit(AdbcDatabase * db, AdbcError * error)
{
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);

    // An option rejected before Init is reported here — see RejectOption.
    if (!fdb->option_error.empty())
        return SetError(error, fdb->option_error_code, fdb->option_error);

    if (fdb->url.empty())
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Database 'uri' option is required");

    // Validate the endpoint here rather than letting libcurl reject it on the
    // first query, where the failure reads as a network error ("Unsupported
    // protocol") with nothing pointing back at the option that caused it.
    const bool is_http = hasSchemePrefix(fdb->url, "http://");
    const bool is_https = hasSchemePrefix(fdb->url, "https://");
    if (!is_http && !is_https)
        return SetError(
            error,
            ADBC_STATUS_INVALID_ARGUMENT,
            "Database 'uri' must start with http:// or https://; got '" + fdb->url
                + "'. Pass the engine's HTTP endpoint, for example http://localhost:3473");
    if (is_https && !curlSupportsTls())
        return SetError(
            error,
            ADBC_STATUS_INVALID_ARGUMENT,
            "Database 'uri' is https:// but this driver was built without TLS support, so it can only reach plaintext http:// "
            "endpoints. Use an http:// endpoint, or rebuild the driver with -DWITH_SSL=ON.");

    initCurl();
    fdb->initialized = true;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode DatabaseRelease(AdbcDatabase * db, AdbcError * /*error*/)
{
    if (!db || !db->private_data)
        return ADBC_STATUS_OK;
    delete static_cast<FireboltDatabase *>(db->private_data);
    db->private_data = nullptr;
    return ADBC_STATUS_OK;
}

// ============================================================
// Connection
// ============================================================

static AdbcStatusCode ConnectionNew(AdbcConnection * conn, AdbcError * error)
{
    if (!conn)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcConnection is null");
    try
    {
        conn->private_data = new FireboltConnection();
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode ConnectionSetOption(AdbcConnection * conn, const char * key, const char * value, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    if (!key)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Option key is null");
    const std::string k(key);
    const std::string v(value ? value : "");

    if (k == ADBC_CONNECTION_OPTION_AUTOCOMMIT)
    {
        bool want_autocommit = (v != ADBC_OPTION_VALUE_DISABLED);
        if (want_autocommit && !fc->autocommit && fc->in_transaction && fc->http)
        {
            // Switching back to autocommit while inside a transaction: commit it.
            AdbcStatusCode rc = RunSimpleSql(fc, "COMMIT", error);
            if (rc != ADBC_STATUS_OK)
                return rc;
            fc->in_transaction = false;
        }
        fc->autocommit = want_autocommit;
        return ADBC_STATUS_OK;
    }
    if (k == "adbc.firebolt.token")
    {
        // Update the bearer token used in the Authorization header.  Stored
        // per-connection (NOT on the shared FireboltDatabase) so two
        // connections sharing the same AdbcDatabase keep independent
        // identities.  HttpClient holds a const reference to this
        // FireboltConnection and reads conn_.token live on each request, so
        // no sync call is needed.  Never written into session_params: those
        // are URL-encoded into the query string on every request and would
        // leak the JWT into proxy and server access logs.
        fc->token = v;
        return ADBC_STATUS_OK;
    }

    // Everything else stored as a session parameter appended to query URL
    fc->session_params[k] = v;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode ConnectionInit(AdbcConnection * conn, AdbcDatabase * db, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);
    if (!fdb->initialized)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    try
    {
        fc->db = fdb;
        // Initialise the per-connection token from the database default,
        // unless the caller already set a per-connection token via
        // ConnectionSetOption("adbc.firebolt.token") between New and Init —
        // ADBC permits options to be applied before Init, and silently
        // dropping that token would fall back to the database identity.
        // Subsequent ConnectionSetOption("adbc.firebolt.token") on this
        // connection only mutates fc->token, never fdb->token.
        if (fc->token.empty())
            fc->token = fdb->token;
        // HttpClient borrows a reference to this FireboltConnection and
        // reads token / url / database / timeout live on every request.
        // No need to seed or replay anything — the connection is the
        // single source of truth.
        fc->http = std::make_unique<HttpClient>(*fc);
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode ConnectionRelease(AdbcConnection * conn, AdbcError * /*error*/)
{
    if (!conn || !conn->private_data)
        return ADBC_STATUS_OK;
    delete static_cast<FireboltConnection *>(conn->private_data);
    conn->private_data = nullptr;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode
ConnectionGetInfo(AdbcConnection * conn, const uint32_t * info_codes, size_t info_codes_len, ArrowArrayStream * out, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetInfo(static_cast<FireboltConnection *>(conn->private_data), info_codes, info_codes_len, out, error);
}

static AdbcStatusCode ConnectionGetObjects(
    AdbcConnection * conn,
    int depth,
    const char * catalog,
    const char * db_schema,
    const char * table_name,
    const char ** table_type,
    const char * column_name,
    ArrowArrayStream * out,
    AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetObjects(
        static_cast<FireboltConnection *>(conn->private_data), depth, catalog, db_schema, table_name, table_type, column_name, out, error);
}

static AdbcStatusCode ConnectionGetTableSchema(
    AdbcConnection * conn, const char * catalog, const char * db_schema, const char * table_name, ArrowSchema * out, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetTableSchema(
        static_cast<FireboltConnection *>(conn->private_data), catalog, db_schema, table_name, out, error);
}

static AdbcStatusCode ConnectionGetTableTypes(AdbcConnection * conn, ArrowArrayStream * out, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetTableTypes(static_cast<FireboltConnection *>(conn->private_data), out, error);
}

static AdbcStatusCode ConnectionReadPartition(
    AdbcConnection * /*conn*/,
    const uint8_t * /*serialized_partition*/,
    size_t /*serialized_length*/,
    ArrowArrayStream * /*out*/,
    AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "ConnectionReadPartition not implemented");
}

static AdbcStatusCode ConnectionCommit(AdbcConnection * conn, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    if (fc->autocommit)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Cannot commit: connection is in autocommit mode");
    if (!fc->in_transaction)
        return ADBC_STATUS_OK; // nothing to commit
    AdbcStatusCode rc = RunSimpleSql(fc, "COMMIT", error);
    if (rc == ADBC_STATUS_OK)
        fc->in_transaction = false;
    return rc;
}

static AdbcStatusCode ConnectionRollback(AdbcConnection * conn, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    if (fc->autocommit)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Cannot rollback: connection is in autocommit mode");
    if (!fc->in_transaction)
        return ADBC_STATUS_OK; // nothing to roll back
    AdbcStatusCode rc = RunSimpleSql(fc, "ROLLBACK", error);
    if (rc == ADBC_STATUS_OK)
        fc->in_transaction = false;
    return rc;
}

// ============================================================
// Statement
// ============================================================

static AdbcStatusCode StatementNew(AdbcConnection * conn, AdbcStatement * stmt, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    if (!stmt)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcStatement is null");
    try
    {
        auto * fs = new FireboltStatement();
        fs->conn = static_cast<FireboltConnection *>(conn->private_data);
        stmt->private_data = fs;
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode StatementRelease(AdbcStatement * stmt, AdbcError * /*error*/)
{
    if (!stmt || !stmt->private_data)
        return ADBC_STATUS_OK;
    delete static_cast<FireboltStatement *>(stmt->private_data);
    stmt->private_data = nullptr;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementSetSqlQuery(AdbcStatement * stmt, const char * query, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    if (!query)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Query is null");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    fs->sql = query;
    // Bound ingest payload is tied to the previous SQL and must be dropped
    // when the statement text changes.
    fs->ingest.reset();
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementSetSubstraitPlan(AdbcStatement * /*stmt*/, const uint8_t * /*plan*/, size_t /*length*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "Substrait plans not supported");
}

static AdbcStatusCode StatementPrepare(AdbcStatement * stmt, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    // No-op: Firebolt HTTP has no server-side prepare
    return ADBC_STATUS_OK;
}

// Snapshot the bound stream's schema into fs->ingest->schema (deep copy).
// Caller must have already populated fs->ingest.  The schema is needed after
// Bind/BindStream returns so that ExecuteQuery can build CREATE TABLE /
// INSERT statements with column names and types.
static AdbcStatusCode CaptureBoundSchema(FireboltStatement * fs, const ArrowSchema * schema, AdbcError * error)
{
    fs->ingest->schema.reset();
    if (!schema || !schema->release)
        return ADBC_STATUS_OK;
    if (ArrowSchemaDeepCopy(schema, fs->ingest->schema.get()) != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "ArrowSchemaDeepCopy failed");
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementBind(AdbcStatement * stmt, ArrowArray * values, ArrowSchema * schema, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);

    fs->initAndGetIngestState();

    // Deep-copy the schema before the stream takes ownership.
    AdbcStatusCode capture_rc = CaptureBoundSchema(fs, schema, error);
    if (capture_rc != ADBC_STATUS_OK)
        return capture_rc;

    // Wrap the single record batch in a one-shot ArrowArrayStream, then serialise.
    // ArrowBasicArrayStreamInit(stream, schema, n_arrays) takes ownership of schema.
    // ArrowBasicArrayStreamSetArray(stream, i, array) takes ownership of array (returns void).
    nanoarrow::UniqueArrayStream stream;
    int rc = ArrowBasicArrayStreamInit(stream.get(), schema, 1);
    if (rc != 0)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "ArrowBasicArrayStreamInit failed");

    ArrowBasicArrayStreamSetArray(stream.get(), 0, values);

    std::string err = SerializeArrayStream(stream.get(), fs->ingest->ipc_bytes);
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, err);

    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementBindStream(AdbcStatement * stmt, ArrowArrayStream * stream, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);

    fs->initAndGetIngestState();

    // Capture the stream schema (deep-copied) before consuming the stream.
    fs->ingest->schema.reset();
    if (stream && stream->get_schema)
    {
        ArrowSchema schema{};
        if (stream->get_schema(stream, &schema) == 0)
        {
            AdbcStatusCode capture_rc = CaptureBoundSchema(fs, &schema, error);
            if (schema.release)
                schema.release(&schema);
            if (capture_rc != ADBC_STATUS_OK)
                return capture_rc;
        }
    }

    std::string err = SerializeArrayStream(stream, fs->ingest->ipc_bytes);
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, err);

    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementGetParameterSchema(AdbcStatement * /*stmt*/, ArrowSchema * /*schema*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "StatementGetParameterSchema not implemented");
}

static AdbcStatusCode StatementSetOption(AdbcStatement * stmt, const char * key, const char * value, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    if (!key)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Option key is null");

    const std::string_view k{key};
    const std::string v(value ? value : "");

    if (k == ADBC_INGEST_OPTION_TARGET_TABLE)
    {
        fs->initAndGetIngestState().target_table = v;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TARGET_CATALOG)
    {
        fs->initAndGetIngestState().target_catalog = v;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TARGET_DB_SCHEMA)
    {
        fs->initAndGetIngestState().target_db_schema = v;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_MODE)
    {
        auto & ingest = fs->initAndGetIngestState();
        if (v == ADBC_INGEST_OPTION_MODE_APPEND)
            ingest.mode = IngestMode::Append;
        else if (v == ADBC_INGEST_OPTION_MODE_CREATE)
            ingest.mode = IngestMode::Create;
        else if (v == ADBC_INGEST_OPTION_MODE_REPLACE)
            ingest.mode = IngestMode::Replace;
        else if (v == ADBC_INGEST_OPTION_MODE_CREATE_APPEND)
            ingest.mode = IngestMode::CreateAppend;
        else
            return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Unknown ingest mode: " + v);
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TEMPORARY)
    {
        // Firebolt has no notion of session-temporary tables.  Accept the no-op
        // value silently so dbapi callers that pass temporary=False work.  Any
        // other value (including the standard "true") is reported as not
        // implemented so the dbapi cursor's NotSupportedError fallback can
        // engage.
        if (v == ADBC_OPTION_VALUE_DISABLED || v.empty())
            return ADBC_STATUS_OK;
        return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "Temporary ingest tables are not supported");
    }
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementExecuteQuery(AdbcStatement * stmt, ArrowArrayStream * out, int64_t * rows_affected, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    auto * conn = fs->conn;
    if (!conn || !conn->http)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");

    if (rows_affected)
        *rows_affected = -1;

    // Clear ingest state on every exit path so bound IPC payload is consumed
    // exactly once (including failures/exceptions).
    FIREBOLT_SCOPE_GUARD(if (fs->ingest.has_value()) fs->ingest.reset());

    try
    {
        // Lazy BEGIN: start a transaction on the first statement when autocommit is off.
        if (!conn->autocommit && !conn->in_transaction)
        {
            AdbcStatusCode rc = RunSimpleSql(conn, "BEGIN", error);
            if (rc != ADBC_STATUS_OK)
                return rc;
            conn->in_transaction = true;
        }

        // Bulk ingest: synthesise CREATE / DROP / INSERT statements based on the
        // configured mode and the deep-copied bound schema.  Regenerated on every
        // call to support statement reuse.  Each pre-SQL entry is sent as a
        // separate HTTP request — Firebolt rejects multiple statements in one body.
        const bool is_ingest = fs->ingest && !fs->ingest->target_table.empty();

        // Atomicity gate: pre-SQL for Replace/Create/CreateAppend modes runs
        // DDL (DROP / CREATE) before the multipart INSERT.  If no Arrow IPC
        // payload was bound, the INSERT cannot land — Replace would have
        // dropped the existing table and left it empty.  Reject the request
        // BEFORE generating any pre-SQL so DDL never runs without DML.
        if (is_ingest && fs->ingest->ipc_bytes.empty())
            return SetError(
                error,
                ADBC_STATUS_INVALID_STATE,
                "Ingest target set but no data bound; call Bind/BindStream before ExecuteQuery");

        // Data bound with no ingest target.  Firebolt's HTTP interface has no
        // query-parameter binding, and the multipart branch below keys on
        // ipc_bytes alone — so this would otherwise POST the caller's SQL with a
        // stray data.arrow part and fail server-side with an opaque upload
        // error.  This is the shape `cursor.execute(sql, parameters)` produces.
        if (!is_ingest && fs->ingest && !fs->ingest->ipc_bytes.empty())
            return SetError(
                error,
                ADBC_STATUS_NOT_IMPLEMENTED,
                "Query parameter binding is not supported. Bind/BindStream is only valid for bulk ingest, with the "
                "ingest target-table option set; inline literals into the SQL instead.");

        std::vector<std::string> pre_sql;
        if (is_ingest)
        {
            AdbcStatusCode rc = buildIngestSql(fs, pre_sql, fs->sql, error);
            if (rc != ADBC_STATUS_OK)
                return rc;
        }

        for (const auto & stmt_sql : pre_sql)
        {
            AdbcStatusCode pre_rc = RunSimpleSql(conn, stmt_sql.c_str(), error);
            if (pre_rc != ADBC_STATUS_OK)
                return pre_rc;
        }

        HttpResponse resp;
        // Use the multipart-insert path only when Bind/BindStream actually
        // produced IPC bytes.
        if (fs->ingest && !fs->ingest->ipc_bytes.empty())
            resp = conn->http->executeInsert(fs->sql, fs->ingest->ipc_bytes, conn->session_params);
        else
            resp = conn->http->executeQuery(fs->sql, conn->session_params);

        ApplySessionUpdates(conn, resp);

        if (!resp.isSuccess())
            return HttpRespToStatus(resp, error);

        // Ingest state is cleared by FIREBOLT_SCOPE_GUARD at function exit.

        if (out)
        {
            std::string err = ExportIpcBytesAsArrowStream(std::move(resp.body), out);
            if (!err.empty())
                return SetError(error, ADBC_STATUS_INTERNAL, err);
        }

        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode StatementExecutePartitions(
    AdbcStatement * /*stmt*/, ArrowSchema * /*schema*/, AdbcPartitions * /*partitions*/, int64_t * /*rows_affected*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "StatementExecutePartitions not implemented");
}

// ============================================================
// Driver init
// ============================================================

static AdbcStatusCode DriverRelease(AdbcDriver * /*driver*/, AdbcError * /*error*/)
{
    return ADBC_STATUS_OK;
}

static AdbcStatusCode PopulateDriver(AdbcDriver * driver, AdbcError * error)
{
    if (!driver)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcDriver is null");

    driver->release = DriverRelease;

    driver->DatabaseNew = DatabaseNew;
    driver->DatabaseSetOption = DatabaseSetOption;
    driver->DatabaseInit = DatabaseInit;
    driver->DatabaseRelease = DatabaseRelease;

    driver->ConnectionNew = ConnectionNew;
    driver->ConnectionSetOption = ConnectionSetOption;
    driver->ConnectionInit = ConnectionInit;
    driver->ConnectionRelease = ConnectionRelease;
    driver->ConnectionGetInfo = ConnectionGetInfo;
    driver->ConnectionGetObjects = ConnectionGetObjects;
    driver->ConnectionGetTableSchema = ConnectionGetTableSchema;
    driver->ConnectionGetTableTypes = ConnectionGetTableTypes;
    driver->ConnectionReadPartition = ConnectionReadPartition;
    driver->ConnectionCommit = ConnectionCommit;
    driver->ConnectionRollback = ConnectionRollback;

    driver->StatementNew = StatementNew;
    driver->StatementRelease = StatementRelease;
    driver->StatementExecuteQuery = StatementExecuteQuery;
    driver->StatementPrepare = StatementPrepare;
    driver->StatementSetSqlQuery = StatementSetSqlQuery;
    driver->StatementSetSubstraitPlan = StatementSetSubstraitPlan;
    driver->StatementBind = StatementBind;
    driver->StatementBindStream = StatementBindStream;
    driver->StatementGetParameterSchema = StatementGetParameterSchema;
    driver->StatementSetOption = StatementSetOption;
    driver->StatementExecutePartitions = StatementExecutePartitions;

    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc

// ============================================================
// Public entry points (C linkage, exported by version script)
// ============================================================

extern "C" __attribute__((visibility("default"))) AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error)
{
    if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0)
    {
        if (error)
        {
            error->vendor_code = 0;
            error->message = strdup("Unsupported ADBC version");
            error->release = [](AdbcError * e) {
                free(e->message);
                e->message = nullptr;
                e->release = nullptr;
            };
        }
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }
    return firebolt::adbc::PopulateDriver(static_cast<AdbcDriver *>(raw_driver), error);
}

extern "C" __attribute__((visibility("default"))) AdbcStatusCode FireboltAdbcDriverInit(int version, void * raw_driver, AdbcError * error)
{
    return AdbcDriverInit(version, raw_driver, error);
}
