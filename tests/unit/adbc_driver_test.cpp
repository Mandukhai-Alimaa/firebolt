#include <gtest/gtest.h>

#include "ArrowIpcStream.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcStatement.h"
#include "IngestSqlBuilder.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <cstring>
#include <string>
#include <vector>

// Entry points defined in FireboltAdbcDriver.cpp
extern "C" AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error);
extern "C" AdbcStatusCode FireboltAdbcDriverInit(int version, void * raw_driver, AdbcError * error);

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
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
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

namespace
{

// Build a top-level struct schema with one column for the given type and name.
// `setup` runs after ArrowSchemaInit + ArrowSchemaSetType[FromType] so callers
// can adjust nullability or set parameters that need a typed schema.
nanoarrow::UniqueSchema MakeStructSchemaWithColumn(
    ArrowType column_type, const std::string & column_name, bool nullable = true)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
    EXPECT_EQ(ArrowSchemaSetType(schema->children[0], column_type), 0);
    EXPECT_EQ(ArrowSchemaSetName(schema->children[0], column_name.c_str()), 0);
    if (!nullable)
        schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    return schema;
}

} // namespace

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
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, nullptr), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TIMESTAMPNTZ");
}

TEST(ArrowToFireboltTypeTest, TimestampWithTimezoneMapsToTimestampTz)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, "UTC"), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TIMESTAMPTZ");
}

TEST(ArrowToFireboltTypeTest, Decimal128PreservesPrecisionAndScale)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
    ASSERT_EQ(ArrowSchemaSetTypeDecimal(schema->children[0], NANOARROW_TYPE_DECIMAL128, 18, 4), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DECIMAL(18, 4)");
}

TEST(ArrowToFireboltTypeTest, ListOfIntMapsToArrayInt)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "vals"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "ARRAY(INT)");
}

TEST(ArrowToFireboltTypeTest, StructMapsToFireboltStruct)
{
    // STRUCT<id:int32 not null, name:string>
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema->children[0], 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "person"), 0);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0]->children[0], "id"), 0);
    schema->children[0]->children[0]->flags &= ~ARROW_FLAG_NULLABLE;

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0]->children[1], "name"), 0);

    EXPECT_EQ(
        firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]),
        "STRUCT(\"id\" INT NOT NULL, \"name\" TEXT)");
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
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), 0);

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

TEST(BuildCreateTableColumnsTest, EmptySchemaReturnsEmpty)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 0), 0);
    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "");
}

TEST(BuildCreateTableColumnsTest, UnsupportedColumnTypeReturnsEmpty)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
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
