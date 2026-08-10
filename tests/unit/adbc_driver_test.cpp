#include <gtest/gtest.h>

#include "ArrowIpcStream.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcStatement.h"
#include "IngestSqlBuilder.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <curl/curl.h>

#include <cstring>
#include <string>
#include <vector>

// Entry points defined in FireboltAdbcDriver.cpp
extern "C" AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error);
extern "C" AdbcStatusCode FireboltAdbcDriverInit(int version, void * raw_driver, AdbcError * error);

// Whether the libcurl this driver is linked against can speak TLS.  The
// shipped build sets -DWITH_SSL=OFF, so it cannot; the https:// rejection
// below is conditioned on this rather than on a build-time define, so the
// same test is correct for either configuration.
static bool CurlHasTls()
{
    const curl_version_info_data * v = curl_version_info(CURLVERSION_NOW);
    return v != nullptr && (v->features & CURL_VERSION_SSL) != 0;
}

// ============================================================
// Helper: call AdbcDriverInit and return a populated driver
// ============================================================

static AdbcDriver InitDriver()
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
    return driver;
}

// ============================================================
// Helper: build Arrow schemas for the type-mapping tests
// ============================================================

namespace
{

// Build an initialised top-level (struct) schema with `n_columns` unset columns,
// ready for the caller to type and name each one.
nanoarrow::UniqueSchema MakeTopLevelSchema(int64_t n_columns)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema.get(), n_columns), 0);
    return schema;
}

// Build a top-level struct schema with one column for the given type and name.
nanoarrow::UniqueSchema MakeStructSchemaWithColumn(ArrowType column_type, const std::string & column_name, bool nullable = true)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    EXPECT_EQ(ArrowSchemaSetType(schema->children[0], column_type), 0);
    EXPECT_EQ(ArrowSchemaSetName(schema->children[0], column_name.c_str()), 0);
    if (!nullable)
        schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    return schema;
}

} // namespace

// ============================================================
// Tests: driver init
// ============================================================

TEST(AdbcDriverInitTest, InitSucceeds)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    EXPECT_NE(driver.DatabaseNew, nullptr);
    EXPECT_NE(driver.StatementExecuteQuery, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(AdbcDriverInitTest, UnsupportedVersionFails)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverInit(999999, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_NOT_IMPLEMENTED);
    if (error.release)
        error.release(&error);
}

TEST(AdbcDriverInitTest, FireboltEntryPoint)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = FireboltAdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: database lifecycle
// ============================================================

TEST(DatabaseTest, NewInitRelease)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;

    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:9123", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.token", "tok", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.database", "mydb", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseRelease(&db, &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseTest, InitWithoutUrlFails)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;

    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.DatabaseInit(&db, &error);
    EXPECT_EQ(code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.message, nullptr);

    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: database option validation — an unusable option must
// not be accepted silently.  Every case below used to return
// ADBC_STATUS_OK (or throw across the C ABI), so a typo or a
// malformed value produced a connection that quietly did the
// wrong thing.
//
// A rejected option is reported by DatabaseInit, not by
// DatabaseSetOption, and deliberately so: see RejectOption in
// FireboltAdbcDriver.cpp — the driver manager replays pre-Init
// options from inside AdbcDatabaseInit and its failure path
// there overflows a heap buffer by one byte.
// ============================================================

namespace
{

// DatabaseNew + one SetOption + Init.  Returns the Init status, which is where a
// bad option surfaces; `error` is left populated for the caller.
AdbcStatusCode InitWithOption(AdbcDriver & driver, const char * key, const char * value, AdbcError * error)
{
    AdbcDatabase db{};
    EXPECT_EQ(driver.DatabaseNew(&db, error), ADBC_STATUS_OK);
    // A valid uri, so that Init fails on the option under test and nothing else.
    EXPECT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseSetOption(&db, key, value, error), ADBC_STATUS_OK)
        << "a pre-Init option must not be refused on the spot; Init reports it";
    AdbcStatusCode code = driver.DatabaseInit(&db, error);
    driver.DatabaseRelease(&db, nullptr);
    return code;
}

} // namespace

TEST(DatabaseOptionTest, UnknownFireboltOptionRejected)
{
    // A typo in a driver-namespaced key is a configuration bug, not something
    // to swallow: `adbc.firebolt.databse` would otherwise leave the connection
    // pointed at the server's default database with no diagnostic anywhere.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.databse", "mydb", &error), ADBC_STATUS_NOT_FOUND);
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("adbc.firebolt.databse"), std::string::npos)
        << "the error should name the offending key, got: " << error.message;
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, UnknownFireboltOptionRejectedImmediatelyAfterInit)
{
    // Past Init there is no option replay, so there is no reason to defer.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.nonsense", "x", &error), ADBC_STATUS_NOT_FOUND);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, NonNamespacedOptionStillAccepted)
{
    // Keys outside the adbc.firebolt.* namespace are set by the driver manager
    // itself and by callers passing future connection parameters; they must
    // keep being accepted so that rejecting typos does not break them.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "username", "svc", &error), ADBC_STATUS_OK);
    EXPECT_EQ(InitWithOption(driver, "adbc.connection.catalog", "warehouse", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, NonNumericTimeoutRejected)
{
    // std::stol throws std::invalid_argument on this input.  The throw escaped
    // through the C ABI boundary into a driver manager with no handler, which
    // aborts the host process.  It has to become a status code.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.timeout_sec", "soon", &error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.message, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, TimeoutWithTrailingGarbageRejected)
{
    // std::stol would happily parse "30s" as 30 and drop the suffix.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.timeout_sec", "30s", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, OutOfRangeTimeoutRejected)
{
    // std::out_of_range, same C ABI problem as above.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.timeout_sec", "99999999999999999999999", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, NegativeTimeoutRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.timeout_sec", "-5", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, ValidTimeoutAccepted)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.timeout_sec", "30", &error), ADBC_STATUS_OK);
    EXPECT_EQ(InitWithOption(driver, "adbc.firebolt.timeout_sec", "0", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, FirstRejectedOptionIsTheOneReported)
{
    // Several bad options: the first is kept so the message is deterministic.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.first_typo", "a", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.second_typo", "b", &error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_NOT_FOUND);
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("first_typo"), std::string::npos) << "got: " << error.message;
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: DatabaseInit — uri validation.  Without these, a bad
// scheme surfaces as a bare libcurl string ("Unsupported
// protocol", "URL using bad/illegal format") at first query,
// far from the option that caused it.
// ============================================================

namespace
{

AdbcStatusCode InitWithUri(AdbcDriver & driver, const char * uri, AdbcError * error)
{
    AdbcDatabase db{};
    EXPECT_EQ(driver.DatabaseNew(&db, error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseSetOption(&db, "uri", uri, error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.DatabaseInit(&db, error);
    driver.DatabaseRelease(&db, nullptr);
    return code;
}

} // namespace

TEST(DatabaseInitTest, UriWithoutSchemeRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithUri(driver, "localhost:3473", &error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.message, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, UnsupportedSchemeRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithUri(driver, "firebolt://localhost:3473/db", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, PlainHttpUriAccepted)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithUri(driver, "http://localhost:3473", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, HttpsUriRejectedWhenCurlHasNoTls)
{
    // The shipped build links curl without TLS, so https:// can never work.
    // Say so at Init, naming the limitation, instead of letting the first
    // query fail with CURLE_UNSUPPORTED_PROTOCOL.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = InitWithUri(driver, "https://api.example.com", &error);
    if (CurlHasTls())
    {
        EXPECT_EQ(code, ADBC_STATUS_OK) << "this build has TLS; https:// must be accepted";
    }
    else
    {
        EXPECT_EQ(code, ADBC_STATUS_INVALID_ARGUMENT);
        ASSERT_NE(error.message, nullptr);
        EXPECT_NE(std::string(error.message).find("TLS"), std::string::npos)
            << "error should name the missing capability, got: " << error.message;
    }
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: connection lifecycle
// ============================================================

static void SetupDatabase(AdbcDriver & driver, AdbcDatabase & db)
{
    AdbcError error = ADBC_ERROR_INIT;
    driver.DatabaseNew(&db, &error);
    driver.DatabaseSetOption(&db, "uri", "http://localhost:9123", &error);
    driver.DatabaseSetOption(&db, "adbc.firebolt.token", "testtoken", &error);
    driver.DatabaseInit(&db, &error);
    if (error.release)
        error.release(&error);
}

TEST(ConnectionTest, NewInitRelease)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionRelease(&conn, &error), ADBC_STATUS_OK);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(ConnectionTest, SetOptionAutocommitIgnored)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);
    EXPECT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_OK);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// Two connections created from the same AdbcDatabase must be able to carry
// independent bearer tokens.
TEST(ConnectionTest, PerConnectionTokensAreIndependent)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn_a{};
    AdbcConnection conn_b{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.ConnectionNew(&conn_a, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn_a, &db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionNew(&conn_b, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn_b, &db, &error), ADBC_STATUS_OK);

    ASSERT_EQ(driver.ConnectionSetOption(&conn_a, "adbc.firebolt.token", "tokenA", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionSetOption(&conn_b, "adbc.firebolt.token", "tokenB", &error), ADBC_STATUS_OK);

    auto * fc_a = static_cast<firebolt::adbc::FireboltConnection *>(conn_a.private_data);
    auto * fc_b = static_cast<firebolt::adbc::FireboltConnection *>(conn_b.private_data);
    ASSERT_NE(fc_a, nullptr);
    ASSERT_NE(fc_b, nullptr);
    EXPECT_EQ(fc_a->token, "tokenA");
    EXPECT_EQ(fc_b->token, "tokenB")
        << "second connection's token did not land on its own FireboltConnection";
    EXPECT_NE(fc_a->token, fc_b->token)
        << "tokens collided — connections share a token store";

    driver.ConnectionRelease(&conn_a, nullptr);
    driver.ConnectionRelease(&conn_b, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// Setting a token on connection B must not retroactively change the token
// already set on connection A (the actual BOLA exploit primitive).
TEST(ConnectionTest, SecondConnectionDoesNotOverrideFirst)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn_a{};
    AdbcConnection conn_b{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn_a, &error);
    driver.ConnectionInit(&conn_a, &db, &error);
    driver.ConnectionNew(&conn_b, &error);
    driver.ConnectionInit(&conn_b, &db, &error);

    ASSERT_EQ(driver.ConnectionSetOption(&conn_a, "adbc.firebolt.token", "tokenA", &error), ADBC_STATUS_OK);
    auto * fc_a = static_cast<firebolt::adbc::FireboltConnection *>(conn_a.private_data);
    ASSERT_EQ(fc_a->token, "tokenA");

    ASSERT_EQ(driver.ConnectionSetOption(&conn_b, "adbc.firebolt.token", "tokenB", &error), ADBC_STATUS_OK);
    EXPECT_EQ(fc_a->token, "tokenA")
        << "connection A's token was clobbered when connection B set its token";

    driver.ConnectionRelease(&conn_a, nullptr);
    driver.ConnectionRelease(&conn_b, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// A token set via ConnectionSetOption *before* ConnectionInit must survive
// initialisation.  The ADBC C API permits options to be set on a Connection
// after New and before Init; an earlier version of ConnectionInit
// unconditionally clobbered fc->token with fdb->token, silently dropping
// the user-supplied per-connection identity and falling back to the
// database-default token.  fc->token is now the single source of truth —
// HttpClient reads it live via a const reference held on the connection.
TEST(ConnectionTest, TokenSetBeforeInitIsPreserved)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db); // db default token = "testtoken"

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionSetOption(&conn, "adbc.firebolt.token", "preInitToken", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->token, "preInitToken")
        << "pre-Init ConnectionSetOption('adbc.firebolt.token') was clobbered by fdb->token in Init";

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// The bearer token must never end up in session_params, because session_params
// are URL-encoded and appended to the query URL on every request — leaking the
// JWT into proxy/server access logs.  ConnectionSetOption used to fall through
// from the token branch into the catch-all session_params write.
TEST(ConnectionTest, TokenNotStoredInSessionParams)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);
    ASSERT_EQ(driver.ConnectionSetOption(&conn, "adbc.firebolt.token", "secret_jwt", &error), ADBC_STATUS_OK);

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->session_params.count("adbc.firebolt.token"), 0u)
        << "token leaked into session_params; will be appended to query URL";

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: statement lifecycle
// ============================================================

static void SetupConnection(AdbcDriver & driver, AdbcDatabase & db, AdbcConnection & conn)
{
    SetupDatabase(driver, db);
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);
    if (error.release)
        error.release(&error);
}

TEST(StatementTest, NewSetQueryRelease)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT 1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementPrepare(&stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementRelease(&stmt, &error), ADBC_STATUS_OK);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(StatementTest, NullQueryFails)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.StatementNew(&conn, &stmt, &error);
    EXPECT_EQ(driver.StatementSetSqlQuery(&stmt, nullptr, &error), ADBC_STATUS_INVALID_ARGUMENT);

    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: ArrowIpcStream — empty body produces empty stream
// ============================================================

TEST(ArrowIpcStreamTest, EmptyBodyProducesEmptyStream)
{
    ArrowArrayStream stream{};
    std::string err = firebolt::adbc::ExportIpcBytesAsArrowStream({}, &stream);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_NE(stream.get_schema, nullptr);

    ArrowSchema schema{};
    EXPECT_EQ(stream.get_schema(&stream, &schema), 0);
    if (schema.release)
        schema.release(&schema);

    ArrowArray array{};
    EXPECT_EQ(stream.get_next(&stream, &array), 0);
    EXPECT_EQ(array.release, nullptr); // end-of-stream

    if (stream.release)
        stream.release(&stream);
}

// ============================================================
// Tests: ArrowIpcStream — round-trip IPC bytes via nanoarrow
// ============================================================

TEST(ArrowIpcStreamTest, RoundTripIpcBytes)
{
    // Build a simple int32 record batch using nanoarrow
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);

    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_EQ(ArrowArrayAppendInt(batch->children[0], i + 1), 0);
        ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    }
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    // Serialise to IPC stream bytes using nanoarrow.
    // ArrowBasicArrayStreamInit takes ownership of schema; SetArray takes ownership of batch.
    nanoarrow::UniqueArrayStream basic_stream;
    ASSERT_EQ(ArrowBasicArrayStreamInit(basic_stream.get(), schema.get(), 1), 0);
    ArrowBasicArrayStreamSetArray(basic_stream.get(), 0, batch.get());

    ArrowBuffer buf{};
    ArrowBufferInit(&buf);
    ArrowIpcOutputStream ipc_out_stream{};
    ASSERT_EQ(ArrowIpcOutputStreamInitBuffer(&ipc_out_stream, &buf), 0);
    ArrowIpcWriter writer{};
    ASSERT_EQ(ArrowIpcWriterInit(&writer, &ipc_out_stream), 0);
    ArrowError na_error{};
    ASSERT_EQ(ArrowIpcWriterWriteArrayStream(&writer, basic_stream.get(), &na_error), 0) << na_error.message;
    ArrowIpcWriterReset(&writer);

    std::vector<uint8_t> ipc_bytes(buf.data, buf.data + buf.size_bytes);
    ArrowBufferReset(&buf);

    // Round-trip through ExportIpcBytesAsArrowStream
    ArrowArrayStream out_stream{};
    std::string out_err = firebolt::adbc::ExportIpcBytesAsArrowStream(ipc_bytes, &out_stream);
    ASSERT_TRUE(out_err.empty()) << out_err;

    ArrowSchema out_schema{};
    ASSERT_EQ(out_stream.get_schema(&out_stream, &out_schema), 0);
    EXPECT_EQ(out_schema.n_children, 1);
    EXPECT_STREQ(out_schema.children[0]->name, "x");
    if (out_schema.release)
        out_schema.release(&out_schema);

    ArrowArray out_batch{};
    ASSERT_EQ(out_stream.get_next(&out_stream, &out_batch), 0);
    ASSERT_NE(out_batch.release, nullptr);
    EXPECT_EQ(out_batch.length, 3);
    if (out_batch.release)
        out_batch.release(&out_batch);

    ArrowArray eos{};
    ASSERT_EQ(out_stream.get_next(&out_stream, &eos), 0);
    EXPECT_EQ(eos.release, nullptr);

    if (out_stream.release)
        out_stream.release(&out_stream);
}

// ============================================================
// Tests: IngestSqlBuilder — quoteIdentifier / qualifiedTable
// ============================================================

TEST(IngestSqlBuilderTest, QuoteIdentifierBasic)
{
    EXPECT_EQ(firebolt::adbc::quoteIdentifier("users"), "\"users\"");
}

TEST(IngestSqlBuilderTest, QuoteIdentifierEscapesEmbeddedQuotes)
{
    // Inner double-quotes must be doubled (standard SQL identifier quoting).
    EXPECT_EQ(firebolt::adbc::quoteIdentifier("we\"ird"), "\"we\"\"ird\"");
}

TEST(IngestSqlBuilderTest, QualifiedTableTableOnly)
{
    EXPECT_EQ(firebolt::adbc::qualifiedTable("", "", "events"), "\"events\"");
}

TEST(IngestSqlBuilderTest, QualifiedTableSchemaPrefixed)
{
    EXPECT_EQ(firebolt::adbc::qualifiedTable("", "public", "events"), "\"public\".\"events\"");
}

TEST(IngestSqlBuilderTest, QualifiedTableFullyQualified)
{
    EXPECT_EQ(
        firebolt::adbc::qualifiedTable("warehouse", "public", "events"),
        "\"warehouse\".\"public\".\"events\"");
}

// ============================================================
// Tests: GetTableSchema probe SQL — identifier quoting must double
// embedded `"` so caller-supplied names cannot inject SQL.
// ============================================================

TEST(GetTableSchemaSqlTest, NoSchemaProducesSinglePart)
{
    EXPECT_EQ(
        firebolt::adbc::buildTableSchemaSql("", "users"),
        "SELECT * FROM \"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, WithSchemaProducesTwoPart)
{
    EXPECT_EQ(
        firebolt::adbc::buildTableSchemaSql("public", "users"),
        "SELECT * FROM \"public\".\"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, QuotesEmbeddedDoubleQuoteInTable)
{
    // Adversarial table name that would otherwise close the identifier and
    // inject SQL.  Embedded `"` must be doubled.
    EXPECT_EQ(
        firebolt::adbc::buildTableSchemaSql("", "users\"x"),
        "SELECT * FROM \"users\"\"x\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, QuotesEmbeddedDoubleQuoteInSchema)
{
    EXPECT_EQ(
        firebolt::adbc::buildTableSchemaSql("pu\"blic", "users"),
        "SELECT * FROM \"pu\"\"blic\".\"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, RejectsSqlInjectionAttempt)
{
    // Classic SQLi payload: close the identifier, inject DDL, comment out the rest.
    // After fix the payload is safely contained inside one quoted identifier.
    const std::string payload = "x\"; DROP TABLE secrets; --";
    std::string sql = firebolt::adbc::buildTableSchemaSql("", payload);
    // Embedded `"` is doubled, so the only `"` characters surround the identifier
    // exactly twice (once at start, once at end of the doubled-up identifier).
    EXPECT_EQ(sql, "SELECT * FROM \"x\"\"; DROP TABLE secrets; --\" LIMIT 0");
    // Sanity: no semicolon outside the quoted identifier.
    auto last_quote = sql.find_last_of('"');
    auto trailing = sql.substr(last_quote);
    EXPECT_EQ(trailing.find(';'), std::string::npos)
        << "trailing=" << trailing;
}

// ============================================================
// Tests: IngestSqlBuilder — Arrow → Firebolt SQL type mapping
// ============================================================

TEST(ArrowToFireboltTypeTest, BoolMapsToBoolean)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_BOOL, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "BOOLEAN");
}

TEST(ArrowToFireboltTypeTest, Int32MapsToInt)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT32, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "INT");
}

TEST(ArrowToFireboltTypeTest, Int64MapsToBigint)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "BIGINT");
}

TEST(ArrowToFireboltTypeTest, FloatMapsToReal)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_FLOAT, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "REAL");
}

TEST(ArrowToFireboltTypeTest, DoubleMapsToDoublePrecision)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DOUBLE, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DOUBLE PRECISION");
}

TEST(ArrowToFireboltTypeTest, StringMapsToText)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_STRING, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TEXT");
}

TEST(ArrowToFireboltTypeTest, LargeStringMapsToText)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_LARGE_STRING, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TEXT");
}

TEST(ArrowToFireboltTypeTest, Date32MapsToDate)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DATE32, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DATE");
}

TEST(ArrowToFireboltTypeTest, TimestampNoTimezoneMapsToTimestampNtz)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, nullptr), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TIMESTAMPNTZ");
}

TEST(ArrowToFireboltTypeTest, TimestampWithTimezoneMapsToTimestampTz)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, "UTC"), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TIMESTAMPTZ");
}

TEST(ArrowToFireboltTypeTest, Decimal128PreservesPrecisionAndScale)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDecimal(schema->children[0], NANOARROW_TYPE_DECIMAL128, 18, 4), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DECIMAL(18, 4)");
}

TEST(ArrowToFireboltTypeTest, ListOfIntMapsToArrayInt)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "vals"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "ARRAY(INT)");
}

TEST(ArrowToFireboltTypeTest, StructMapsToFireboltStruct)
{
    // STRUCT<id:int32 not null, name:string>.  `id` is non-nullable on purpose:
    // Firebolt rejects a non-nullable STRUCT field ("STRUCT fields have to be
    // nullable"), so the field nullability has to be dropped here — otherwise
    // create-mode ingest emits DDL the server refuses.  Column-level nullability
    // is rendered by buildCreateTableColumns instead.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema->children[0], 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "person"), 0);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0]->children[0], "id"), 0);
    schema->children[0]->children[0]->flags &= ~ARROW_FLAG_NULLABLE;

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0]->children[1], "name"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "STRUCT(\"id\" INT, \"name\" TEXT)");
}

TEST(ArrowToFireboltTypeTest, NestedStructMapsToNestedFireboltStruct)
{
    // STRUCT<x:int32, child:STRUCT<y:int64, z:string>>
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);

    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "x"), 0);

    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer->children[1], 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1], "child"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[0], NANOARROW_TYPE_INT64), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1]->children[0], "y"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1]->children[1], "z"), 0);

    EXPECT_EQ(
        firebolt::adbc::arrowTypeToFireboltSqlType(outer),
        "STRUCT(\"x\" INT, \"child\" STRUCT(\"y\" BIGINT, \"z\" TEXT))");
}

TEST(ArrowToFireboltTypeTest, ListOfStructMapsToArrayOfStruct)
{
    // LIST<STRUCT<k:int32, v:string>>
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * list = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetType(list, NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetName(list, "items"), 0);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(list->children[0], 2), 0);
    ASSERT_EQ(ArrowSchemaSetType(list->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(list->children[0]->children[0], "k"), 0);
    ASSERT_EQ(ArrowSchemaSetType(list->children[0]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(list->children[0]->children[1], "v"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(list), "ARRAY(STRUCT(\"k\" INT, \"v\" TEXT))");
}

TEST(ArrowToFireboltTypeTest, StructContainingListMapsToStructWithArray)
{
    // STRUCT<xs:LIST<int32>>
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 1), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "xs"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0]->children[0], NANOARROW_TYPE_INT32), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "STRUCT(\"xs\" ARRAY(INT))");
}

TEST(ArrowToFireboltTypeTest, NestedListOfStructMapsToNestedArrayOfStruct)
{
    // LIST<LIST<STRUCT<m:int32>>> — every list level has to be descended.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetType(outer, NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "aas"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_LARGE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer->children[0]->children[0], 1), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0]->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0]->children[0]->children[0], "m"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "ARRAY(ARRAY(STRUCT(\"m\" INT)))");
}

TEST(ArrowToFireboltTypeTest, StructFieldNamesAreQuotedAndEscaped)
{
    // A reserved keyword and an embedded double quote both have to survive
    // quoting, or the generated DDL is either rejected or injectable.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "order"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1], "a\"b"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "STRUCT(\"order\" INT, \"a\"\"b\" TEXT)");
}

TEST(ArrowToFireboltTypeTest, EmptyStructReturnsEmpty)
{
    // Firebolt has no zero-field STRUCT; rendering "STRUCT()" would produce a
    // syntax error at the server. The mapping must fail up front instead, so
    // buildIngestSql reports ADBC_STATUS_NOT_IMPLEMENTED.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema->children[0], 0), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "s"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "");
}

TEST(ArrowToFireboltTypeTest, StructWithUnsupportedFieldReturnsEmpty)
{
    // A field type with no Firebolt equivalent (here MAP, which read_arrow() also
    // rejects) has to fail the whole struct rather than be skipped.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "ok"), 0);
    // MAP allocates an "entries" struct child; its key/value still need types.
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1], NANOARROW_TYPE_MAP), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[0]->children[0], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[0]->children[1], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1], "bad"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "");
}

TEST(ArrowToFireboltTypeTest, UnnamedStructFieldReturnsEmpty)
{
    // A nameless field would render as the empty identifier `""`; reject it the
    // same way buildCreateTableColumns rejects a nameless column.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 1), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], nullptr), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "");
}

TEST(ArrowToFireboltTypeTest, UnsupportedTypeReturnsEmpty)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INTERVAL_DAY_TIME, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "");
}

// ============================================================
// Tests: IngestSqlBuilder — buildCreateTableColumns
// ============================================================

TEST(BuildCreateTableColumnsTest, MultipleColumns)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(3);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "id"), 0);
    schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE; // NOT NULL

    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "label"), 0);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_DOUBLE), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[2], "value"), 0);

    EXPECT_EQ(
        firebolt::adbc::buildCreateTableColumns(schema.get()),
        "\"id\" INT NOT NULL, \"label\" TEXT, \"value\" DOUBLE PRECISION");
}

TEST(BuildCreateTableColumnsTest, NotNullStructColumnKeepsColumnLevelNotNull)
{
    // NOT NULL is legal on the column but not on the struct's fields; the two
    // levels must not be conflated.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * col = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(col, 1), 0);
    ASSERT_EQ(ArrowSchemaSetName(col, "s"), 0);
    col->flags &= ~ARROW_FLAG_NULLABLE; // column: NOT NULL
    ASSERT_EQ(ArrowSchemaSetType(col->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(col->children[0], "a"), 0);
    col->children[0]->flags &= ~ARROW_FLAG_NULLABLE; // field: must be dropped

    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "\"s\" STRUCT(\"a\" INT) NOT NULL");
}

TEST(BuildCreateTableColumnsTest, EmptySchemaReturnsEmpty)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(0);
    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "");
}

TEST(BuildCreateTableColumnsTest, UnsupportedColumnTypeReturnsEmpty)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INTERVAL_DAY_TIME), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "");
}

// ============================================================
// Tests: StatementSetOption — ingest options
// ============================================================

namespace
{

struct IngestOptionFixture
{
    AdbcDriver driver{};
    AdbcDatabase db{};
    AdbcConnection conn{};
    AdbcStatement stmt{};

    IngestOptionFixture()
    {
        driver = InitDriver();
        SetupConnection(driver, db, conn);
        AdbcError error = ADBC_ERROR_INIT;
        EXPECT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
        if (error.release)
            error.release(&error);
    }

    ~IngestOptionFixture()
    {
        driver.StatementRelease(&stmt, nullptr);
        driver.ConnectionRelease(&conn, nullptr);
        driver.DatabaseRelease(&db, nullptr);
    }
};

} // namespace

// ============================================================
// Tests: not-implemented stubs
// ============================================================

TEST(StatementSetOptionTest, TargetTableAccepted)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "events", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, TargetCatalogAndSchemaAccepted)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_CATALOG, "warehouse", &error), ADBC_STATUS_OK);
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "public", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, KnownIngestModesAccepted)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    for (const char * mode : {
             ADBC_INGEST_OPTION_MODE_APPEND,
             ADBC_INGEST_OPTION_MODE_CREATE,
             ADBC_INGEST_OPTION_MODE_REPLACE,
             ADBC_INGEST_OPTION_MODE_CREATE_APPEND,
         })
    {
        EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_MODE, mode, &error), ADBC_STATUS_OK)
            << "mode=" << mode;
    }
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, UnknownIngestModeRejected)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(
        f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_MODE, "adbc.ingest.mode.bogus", &error),
        ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, TemporaryFalseSilentlyAccepted)
{
    // Firebolt has no notion of session-temporary tables.  temporary=false is a
    // no-op that dbapi sends by default; it must not cause adbc_ingest() to fail.
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(
        f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_DISABLED, &error),
        ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, TemporaryTrueRejectedWithNotImplemented)
{
    // dbapi catches NotSupportedError to fall through; surface it explicitly.
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(
        f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_ENABLED, &error),
        ADBC_STATUS_NOT_IMPLEMENTED);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, NullKeyRejected)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, nullptr, "x", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

// Regression for ingest atomicity: when an ingest target is configured in a
// mode that would otherwise generate DDL (Replace / Create / CreateAppend)
// but no Arrow IPC payload has been bound, ExecuteQuery must fail BEFORE
// running any pre-SQL — otherwise mode=Replace would silently DROP the
// existing table even though no INSERT could ever land.
//
// We exercise this by manually populating ingest->schema (simulating a
// BindStream call where schema capture succeeded but serialization failed)
// while leaving ipc_bytes empty.  The driver is pointed at 127.0.0.1:1, an
// unreachable port — without the fix, ExecuteQuery would attempt the DROP
// pre-SQL and return ADBC_STATUS_IO; with the fix it short-circuits on
// ADBC_STATUS_INVALID_STATE before any HTTP attempt.
TEST(StatementExecuteTest, IngestReplaceWithoutBoundDataRejectedBeforeAnyHttp)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "foo", &error), ADBC_STATUS_OK);
    ASSERT_EQ(
        driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_REPLACE, &error),
        ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->ingest.has_value());

    // Synthesize the post-bind state: schema captured, ipc_bytes empty.
    nanoarrow::UniqueSchema bound_schema;
    ArrowSchemaInit(bound_schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(bound_schema.get(), 1), 0);
    ASSERT_EQ(ArrowSchemaSetType(bound_schema->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(bound_schema->children[0], "x"), 0);
    fs->ingest->schema.reset();
    ASSERT_EQ(ArrowSchemaDeepCopy(bound_schema.get(), fs->ingest->schema.get()), 0);
    ASSERT_TRUE(fs->ingest->ipc_bytes.empty());

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_INVALID_STATE)
        << "ingest with no bound data must short-circuit before any HTTP / pre-SQL";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// Regression for ingest-state lifecycle: changing the statement's SQL must
// invalidate any previously bound Arrow payload, so that the bytes do not
// silently re-attach to a query for which they were not intended.  Before
// the fix the multipart-insert branch keyed on `target_table` + `ipc_bytes`,
// so a bind from a prior ingest could ride along on a subsequent SELECT.
TEST(StatementReuseTest, IngestStateClearedOnSetSqlQuery)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "foo", &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->ingest.has_value());
    fs->ingest->ipc_bytes = {0xDE, 0xAD, 0xBE, 0xEF};

    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT 1", &error), ADBC_STATUS_OK);

    EXPECT_FALSE(fs->ingest.has_value())
        << "stale bind data persisted across SetSqlQuery; would attach to next query";

    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// Regression for the same lifecycle bug on the failure path: a failed
// ExecuteQuery must not leave the bound payload in place.
TEST(StatementReuseTest, IngestStateClearedOnFailedExecute)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    // Unreachable port — any HTTP attempt will fail with curl error.
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "foo", &error), ADBC_STATUS_OK);
    // Append mode: buildIngestSql does not require a schema, so the new
    // commit-6 ipc_bytes-empty gate is the only thing standing between us
    // and the HTTP attempt.  Populate ipc_bytes with arbitrary bytes (the
    // remote will reject them, but only after the request leaves the box).
    ASSERT_EQ(
        driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &error),
        ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->ingest.has_value());
    fs->ingest->ipc_bytes = {0xDE, 0xAD, 0xBE, 0xEF};

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_IO) << "expected HTTP layer to reject the unreachable target";

    EXPECT_FALSE(fs->ingest.has_value())
        << "ingest state survived a failed ExecuteQuery; bound bytes would attach to next query";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// Firebolt's HTTP interface has no parameter binding, and this driver's
// Bind/BindStream exist only to carry a bulk-ingest payload.  When a caller
// binds data with no ingest target — exactly what
// `cursor.execute(sql, params)` does in the dbapi layer — ExecuteQuery used
// to take the multipart-insert branch anyway (it keys on `ipc_bytes` alone),
// POSTing the user's SELECT plus a data.arrow part.  The server then fails
// with an opaque error about the upload.  Refuse it up front instead.
TEST(StatementExecuteTest, BindWithoutIngestTargetRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    // Unreachable port: if the request is attempted, the status is IO, not
    // NOT_IMPLEMENTED, so this also proves nothing left the box.
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT ?", &error), ADBC_STATUS_OK);

    // Simulate a parameter bind: payload present, no ingest target configured.
    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    fs->initAndGetIngestState().ipc_bytes = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(fs->ingest->target_table.empty());

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_NOT_IMPLEMENTED) << "bound data with no ingest target must be refused, not sent as a multipart insert";
    if (error.message)
        EXPECT_NE(std::string(error.message).find("parameter"), std::string::npos)
            << "error should point at parameter binding, got: " << error.message;

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// ADBC canonicalises exactly two values for a boolean option: "true" and
// "false" (ADBC_OPTION_VALUE_ENABLED / _DISABLED).  The autocommit branch used
// to read `v != "false"`, so every other spelling — "0", "FALSE", a typo, the
// empty string — silently meant autocommit ON.  That is the one option where
// misreading the value changes durability: a caller who believes they opened a
// transaction gets each statement committed as it executes, and their
// subsequent commit()/rollback() fails with INVALID_STATE against data that is
// already permanent.  Anything non-canonical has to be refused.
namespace
{

// Set the autocommit option on a fresh connection and return its status.
AdbcStatusCode SetAutocommit(AdbcDriver & driver, AdbcDatabase & db, const char * value, AdbcError * error)
{
    AdbcConnection conn{};
    EXPECT_EQ(driver.ConnectionNew(&conn, error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.ConnectionInit(&conn, &db, error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, value, error);
    driver.ConnectionRelease(&conn, nullptr);
    return code;
}

} // namespace

TEST(AutocommitTest, CanonicalValuesAccepted)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);
    AdbcError error = ADBC_ERROR_INIT;

    EXPECT_EQ(SetAutocommit(driver, db, ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_OK);
    EXPECT_EQ(SetAutocommit(driver, db, ADBC_OPTION_VALUE_DISABLED, &error), ADBC_STATUS_OK);

    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(AutocommitTest, NonCanonicalValuesRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    // "0" and "FALSE" are the dangerous ones: a caller means autocommit off and
    // silently gets it on.  The rest guard against the same class of typo.
    for (const char * value : {"0", "1", "FALSE", "True", "flase", "", "yes", "off"})
    {
        AdbcError error = ADBC_ERROR_INIT;
        EXPECT_EQ(SetAutocommit(driver, db, value, &error), ADBC_STATUS_INVALID_ARGUMENT)
            << "value=" << value << " must not be silently reinterpreted";
        if (error.message)
            EXPECT_NE(std::string(error.message).find("autocommit"), std::string::npos)
                << "error should name the option, got: " << error.message;
        if (error.release)
            error.release(&error);
    }

    driver.DatabaseRelease(&db, nullptr);
}

TEST(AutocommitTest, RejectedValueLeavesModeUnchanged)
{
    // A refused value must not half-apply: the connection has to keep whatever
    // mode it had, so the caller's next statement behaves predictably.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);
    AdbcError error = ADBC_ERROR_INIT;

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    ASSERT_TRUE(fc->autocommit) << "autocommit is the ADBC default";

    // Turn it off legitimately, then try to turn it back on with a bad spelling.
    ASSERT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, ADBC_OPTION_VALUE_DISABLED, &error), ADBC_STATUS_OK);
    ASSERT_FALSE(fc->autocommit);

    EXPECT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, "1", &error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_FALSE(fc->autocommit) << "a rejected value must not change the mode";

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// The autocommit key must not fall through to the session-parameter catch-all
// either: a rejected value that still landed in session_params would be
// URL-encoded onto every request.
TEST(AutocommitTest, RejectedValueNotStoredAsSessionParam)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);
    AdbcError error = ADBC_ERROR_INIT;

    EXPECT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, "0", &error), ADBC_STATUS_INVALID_ARGUMENT);

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->session_params.count(ADBC_CONNECTION_OPTION_AUTOCOMMIT), 0u);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(AutocommitTest, CommitRollbackInAutocommitFails)
{
    // Commit and rollback in autocommit mode (the default) must fail with
    // ADBC_STATUS_INVALID_STATE so callers know they need to disable autocommit first.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(driver.ConnectionCommit(&conn, &error), ADBC_STATUS_INVALID_STATE);
    if (error.release)
        error.release(&error);
    error = ADBC_ERROR_INIT;
    EXPECT_EQ(driver.ConnectionRollback(&conn, &error), ADBC_STATUS_INVALID_STATE);
    if (error.release)
        error.release(&error);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}
