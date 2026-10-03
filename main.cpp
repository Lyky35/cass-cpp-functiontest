#include <cassandra.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef CASS_DRIVER_VERSION
#define CASS_DRIVER_VERSION "unknown"
#endif

namespace {

// ---------------------------------------------------------------------------
// connection config: built-in defaults < config file < environment variables
// ---------------------------------------------------------------------------
struct ConnectionConfig {
  std::string host = "127.0.0.1";
  int port = 9042;
  int protocol = 4;
  std::string user = "appuser";
  std::string password = "appuser123";
  std::string keyspace = "demo";
  std::string source = "built-in defaults";
};

ConnectionConfig g_config;

// diagnostics of the test that is currently running
std::ostringstream g_diag;

bool g_color = false;

std::string trim(const std::string& text) {
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return std::string();
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

std::string lower(std::string text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] >= 'A' && text[i] <= 'Z') text[i] = static_cast<char>(text[i] - 'A' + 'a');
  }
  return text;
}

void apply_connection_string(ConnectionConfig* config, const std::string& value);

void apply_setting(ConnectionConfig* config, const std::string& raw_key,
                   const std::string& raw_value) {
  std::string key = lower(trim(raw_key));
  std::string value = trim(raw_value);
  if (key.empty()) return;
  if (key == "connection") {
    apply_connection_string(config, value);
  } else if (key == "host" || key == "contact points" || key == "contact_points") {
    config->host = value;
  } else if (key == "port") {
    int port = std::atoi(value.c_str());
    if (port > 0) config->port = port;
  } else if (key == "protocol") {
    std::string version = value;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
      version = version.substr(1);
    }
    int protocol = std::atoi(version.c_str());
    if (protocol >= 3 && protocol <= 5) config->protocol = protocol;
  } else if (key == "user" || key == "username") {
    config->user = value;
  } else if (key == "password") {
    config->password = value;
  } else if (key == "keyspace") {
    config->keyspace = value;
  }
}

// contact points=127.0.0.1; port=9042; username=appuser; password=secret; keyspace=demo
void apply_connection_string(ConnectionConfig* config, const std::string& value) {
  std::istringstream parts(value);
  std::string part;
  while (std::getline(parts, part, ';')) {
    size_t separator = part.find('=');
    if (separator == std::string::npos) continue;
    apply_setting(config, part.substr(0, separator), part.substr(separator + 1));
  }
}

bool load_config_file(const std::string& path, ConnectionConfig* config, std::string* error) {
  std::ifstream input(path.c_str());
  if (!input) {
    *error = "cannot open config file: " + path;
    return false;
  }
  std::string line;
  int line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    std::string text = trim(line);
    if (text.empty() || text[0] == '#' || text.rfind("//", 0) == 0) continue;
    size_t separator = text.find('=');
    if (separator == std::string::npos) {
      *error = path + ":" + std::to_string(line_number) + ": expected 'key = value'";
      return false;
    }
    apply_setting(config, text.substr(0, separator), text.substr(separator + 1));
  }
  config->source = path;
  return true;
}

void apply_environment(ConnectionConfig* config) {
  bool used = false;
  if (const char* value = std::getenv("CASSANDRA_HOST")) {
    config->host = value;
    used = true;
  }
  if (const char* value = std::getenv("CASSANDRA_PORT")) {
    int port = std::atoi(value);
    if (port > 0) config->port = port;
    used = true;
  }
  if (const char* value = std::getenv("CASSANDRA_PROTOCOL")) {
    int protocol = std::atoi(value);
    if (protocol >= 3 && protocol <= 5) config->protocol = protocol;
    used = true;
  }
  if (const char* value = std::getenv("CASSANDRA_USER")) {
    config->user = value;
    used = true;
  }
  if (const char* value = std::getenv("CASSANDRA_PASSWORD")) {
    config->password = value;
    used = true;
  }
  if (const char* value = std::getenv("CASSANDRA_KEYSPACE")) {
    config->keyspace = value;
    used = true;
  }
  if (used) config->source += " + environment";
}

std::string executable_dir() {
  char buffer[4096];
  ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return std::string();
  buffer[length] = '\0';
  std::string path(buffer);
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string paint(const char* code, const char* text) {
  if (!g_color) return text;
  return std::string("\033[") + code + "m" + text + "\033[0m";
}

// ---------------------------------------------------------------------------
// tiny test harness
// ---------------------------------------------------------------------------
struct TestCase {
  std::string name;
  bool (*fn)();
};

std::vector<TestCase>& all_tests() {
  static std::vector<TestCase> tests;
  return tests;
}

struct AutoRegister {
  AutoRegister(const char* name, bool (*fn)()) { all_tests().push_back({name, fn}); }
};

#define TEST(name)                       \
  bool name();                           \
  AutoRegister auto_register_##name(#name, &name); \
  bool name()

#define EXPECT_TRUE(cond)                                                       \
  do {                                                                          \
    if (!(cond)) {                                                              \
      g_diag << "      FAILED: " #cond << " (" << __FILE__ << ":" << __LINE__   \
             << ")" << std::endl;                                               \
      return false;                                                             \
    }                                                                           \
  } while (0)

template <typename A, typename B>
bool expect_eq(const A& actual, const B& expected, const char* expr, const char* file, int line) {
  if (actual == expected) return true;
  g_diag << "      FAILED: " << expr << " -> got [" << actual << "] expected [" << expected
         << "] (" << file << ":" << line << ")" << std::endl;
  return false;
}

#define EXPECT_EQ(actual, expected)                                                    \
  do {                                                                                 \
    if (!expect_eq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)) \
      return false;                                                                    \
  } while (0)

// ---------------------------------------------------------------------------
// driver helpers
// ---------------------------------------------------------------------------
void print_future_error(const std::string& context, CassFuture* future) {
  CassError code = cass_future_error_code(future);
  const char* message = NULL;
  size_t length = 0;
  cass_future_error_message(future, &message, &length);
  g_diag << "      " << context << " -> " << cass_error_desc(code) << " (" << code << ")";
  if (length > 0 && message) g_diag << ": " << std::string(message, length);
  g_diag << std::endl;
}

const CassResult* run(CassSession* session, const std::string& cql) {
  CassStatement* statement = cass_statement_new(cql.c_str(), 0);
  CassFuture* future = cass_session_execute(session, statement);
  cass_statement_free(statement);
  if (cass_future_error_code(future) != CASS_OK) {
    print_future_error("query [" + cql + "]", future);
    cass_future_free(future);
    return nullptr;
  }
  const CassResult* result = cass_future_get_result(future);
  cass_future_free(future);
  return result;
}

bool exec(CassSession* session, const std::string& cql) {
  const CassResult* result = run(session, cql);
  if (!result) return false;
  cass_result_free(result);
  return true;
}

const CassResult* execute_statement(CassSession* session, CassStatement* statement) {
  CassFuture* future = cass_session_execute(session, statement);
  if (cass_future_error_code(future) != CASS_OK) {
    print_future_error("statement execution", future);
    cass_future_free(future);
    return nullptr;
  }
  const CassResult* result = cass_future_get_result(future);
  cass_future_free(future);
  return result;
}

const CassPrepared* prepare(CassSession* session, const std::string& cql) {
  CassFuture* future = cass_session_prepare(session, cql.c_str());
  if (cass_future_error_code(future) != CASS_OK) {
    print_future_error("prepare [" + cql + "]", future);
    cass_future_free(future);
    return nullptr;
  }
  const CassPrepared* prepared = cass_future_get_prepared(future);
  cass_future_free(future);
  return prepared;
}

bool get_string(const CassRow* row, const char* column, std::string* out) {
  const CassValue* value = cass_row_get_column_by_name(row, column);
  if (!value || cass_value_is_null(value)) return false;
  const char* text = NULL;
  size_t length = 0;
  if (cass_value_get_string(value, &text, &length) != CASS_OK) return false;
  out->assign(text, length);
  return true;
}

bool get_int32(const CassRow* row, const char* column, cass_int32_t* out) {
  const CassValue* value = cass_row_get_column_by_name(row, column);
  if (!value || cass_value_is_null(value)) return false;
  return cass_value_get_int32(value, out) == CASS_OK;
}

bool get_int64(const CassRow* row, const char* column, cass_int64_t* out) {
  const CassValue* value = cass_row_get_column_by_name(row, column);
  if (!value || cass_value_is_null(value)) return false;
  return cass_value_get_int64(value, out) == CASS_OK;
}

bool get_bool(const CassRow* row, const char* column, cass_bool_t* out) {
  const CassValue* value = cass_row_get_column_by_name(row, column);
  if (!value || cass_value_is_null(value)) return false;
  return cass_value_get_bool(value, out) == CASS_OK;
}

bool get_uuid(const CassRow* row, const char* column, CassUuid* out) {
  const CassValue* value = cass_row_get_column_by_name(row, column);
  if (!value || cass_value_is_null(value)) return false;
  return cass_value_get_uuid(value, out) == CASS_OK;
}

std::string uuid_str(const CassUuid& uuid) {
  char buffer[CASS_UUID_STRING_LENGTH];
  cass_uuid_string(uuid, buffer);
  return std::string(buffer);
}

// ---------------------------------------------------------------------------
// session fixture
// ---------------------------------------------------------------------------
CassCluster* make_cluster() {
  CassCluster* cluster = cass_cluster_new();
  cass_cluster_set_contact_points(cluster, g_config.host.c_str());
  cass_cluster_set_port(cluster, g_config.port);
  cass_cluster_set_credentials(cluster, g_config.user.c_str(), g_config.password.c_str());
  cass_cluster_set_protocol_version(cluster, g_config.protocol);
  if (g_config.protocol >= 5) cass_cluster_set_use_beta_protocol_version(cluster, cass_true);
  cass_cluster_set_connect_timeout(cluster, 10000);
  cass_cluster_set_request_timeout(cluster, 15000);
  cass_cluster_set_token_aware_routing(cluster, cass_true);
  return cluster;
}

CassCluster* g_cluster = NULL;
CassSession* g_session = NULL;

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------
TEST(t01_connect_with_auth) {
  CassCluster* cluster = make_cluster();
  CassSession* session = cass_session_new();
  CassFuture* future = cass_session_connect(session, cluster);
  EXPECT_TRUE(cass_future_wait_timed(future, 30 * 1000 * 1000ULL));
  CassError code = cass_future_error_code(future);
  cass_future_free(future);
  EXPECT_EQ(code, CASS_OK);
  const CassResult* result = run(session, "SELECT release_version FROM system.local");
  EXPECT_TRUE(result != NULL);
  cass_result_free(result);
  cass_session_free(session);
  cass_cluster_free(cluster);

  // wrong credentials must be rejected by PasswordAuthenticator
  CassCluster* bad_cluster = cass_cluster_new();
  cass_cluster_set_contact_points(bad_cluster, g_config.host.c_str());
  cass_cluster_set_port(bad_cluster, g_config.port);
  cass_cluster_set_credentials(bad_cluster, g_config.user.c_str(), "definitely-not-the-password");
  CassSession* bad_session = cass_session_new();
  CassFuture* bad_future = cass_session_connect(bad_session, bad_cluster);
  cass_future_wait_timed(bad_future, 30 * 1000 * 1000ULL);
  CassError bad_code = cass_future_error_code(bad_future);
  cass_future_free(bad_future);
  EXPECT_EQ(bad_code, CASS_ERROR_SERVER_BAD_CREDENTIALS);
  cass_session_free(bad_session);
  cass_cluster_free(bad_cluster);
  return true;
}

TEST(t02_cluster_metadata) {
  const CassResult* result =
      run(g_session, "SELECT cluster_name, release_version, partitioner FROM system.local");
  EXPECT_TRUE(result != NULL);
  const CassRow* row = cass_result_first_row(result);
  EXPECT_TRUE(row != NULL);
  std::string cluster_name, release_version, partitioner;
  EXPECT_TRUE(get_string(row, "cluster_name", &cluster_name));
  EXPECT_TRUE(get_string(row, "release_version", &release_version));
  EXPECT_TRUE(get_string(row, "partitioner", &partitioner));
  EXPECT_EQ(cluster_name, std::string("simple-cluster"));
  EXPECT_TRUE(release_version.rfind("5.", 0) == 0);
  EXPECT_TRUE(partitioner.find("Murmur3") != std::string::npos);
  EXPECT_EQ(cass_result_row_count(result), (size_t)1);
  cass_result_free(result);

  const CassResult* peers = run(g_session, "SELECT peer FROM system.peers");
  EXPECT_TRUE(peers != NULL);
  EXPECT_EQ(cass_result_row_count(peers), (size_t)0);  // single node cluster
  cass_result_free(peers);
  return true;
}

TEST(t03_simple_query) {
  const CassResult* result = run(g_session, "SELECT release_version FROM system.local");
  EXPECT_TRUE(result != NULL);
  EXPECT_EQ(cass_result_row_count(result), (size_t)1);
  const CassRow* row = cass_result_first_row(result);
  EXPECT_TRUE(row != NULL);
  std::string version;
  EXPECT_TRUE(get_string(row, "release_version", &version));
  EXPECT_EQ(version, std::string("5.0.9"));
  cass_result_free(result);

  // non-row result (schema change) must also succeed
  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_tmp (k int PRIMARY KEY)"));
  EXPECT_TRUE(exec(g_session, "DROP TABLE IF EXISTS demo.t_tmp"));
  return true;
}

TEST(t04_column_metadata) {
  const CassResult* result = run(g_session, "SELECT release_version FROM system.local");
  EXPECT_TRUE(result != NULL);
  EXPECT_EQ(cass_result_column_count(result), (size_t)1);
  const char* name = NULL;
  size_t name_length = 0;
  EXPECT_EQ(cass_result_column_name(result, 0, &name, &name_length), CASS_OK);
  EXPECT_EQ(std::string(name, name_length), std::string("release_version"));
  CassValueType type = cass_result_column_type(result, 0);
  EXPECT_TRUE(type == CASS_VALUE_TYPE_TEXT || type == CASS_VALUE_TYPE_VARCHAR);
  cass_result_free(result);
  return true;
}

TEST(t05_prepared_statement) {
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_prep (k text PRIMARY KEY, v int)"));

  const CassPrepared* insert = prepare(
      g_session, "INSERT INTO demo.t_prep (k, v) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);
  const CassDataType* param_type = cass_prepared_parameter_data_type(insert, 1);
  EXPECT_TRUE(param_type != NULL);
  EXPECT_EQ(cass_data_type_type(param_type), CASS_VALUE_TYPE_INT);

  CassStatement* bind = cass_prepared_bind(insert);
  EXPECT_EQ(cass_statement_bind_string(bind, 0, "prepared-key"), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int32(bind, 1, 4242), CASS_OK);
  const CassResult* written = execute_statement(g_session, bind);
  cass_statement_free(bind);
  cass_prepared_free(insert);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  // positional parameter
  const CassPrepared* select = prepare(
      g_session, "SELECT v FROM demo.t_prep WHERE k = ?");
  EXPECT_TRUE(select != NULL);
  CassStatement* read = cass_prepared_bind(select);
  cass_statement_bind_string(read, 0, "prepared-key");
  const CassResult* rows = execute_statement(g_session, read);
  cass_statement_free(read);
  cass_prepared_free(select);
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  cass_int32_t value = 0;
  EXPECT_TRUE(get_int32(row, "v", &value));
  EXPECT_EQ(value, (cass_int32_t)4242);
  cass_result_free(rows);

  // named parameter
  const CassPrepared* named = prepare(
      g_session, "SELECT v FROM demo.t_prep WHERE k = :key");
  EXPECT_TRUE(named != NULL);
  CassStatement* named_stmt = cass_prepared_bind(named);
  EXPECT_EQ(cass_statement_bind_string_by_name(named_stmt, "key", "prepared-key"), CASS_OK);
  const CassResult* named_rows = execute_statement(g_session, named_stmt);
  cass_statement_free(named_stmt);
  cass_prepared_free(named);
  EXPECT_TRUE(named_rows != NULL);
  EXPECT_EQ(cass_result_row_count(named_rows), (size_t)1);
  cass_result_free(named_rows);
  return true;
}

TEST(t06_bound_types) {
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_types ("
                   "k text PRIMARY KEY, c_tiny tinyint, c_small smallint, c_int int, "
                   "c_bigint bigint, c_float float, c_double double, c_bool boolean, "
                   "c_text text, c_blob blob, c_uuid uuid, c_ts timestamp, "
                   "c_dec decimal, c_date date, c_time time)"));
  EXPECT_TRUE(exec(g_session, "ALTER TABLE demo.t_types ADD IF NOT EXISTS c_varint varint"));

  CassUuidGen* uuid_gen = cass_uuid_gen_new();
  CassUuid uuid;
  cass_uuid_gen_random(uuid_gen, &uuid);

  const char* insert_cql =
      "INSERT INTO demo.t_types (k, c_tiny, c_small, c_int, c_bigint, c_float, c_double, "
      "c_bool, c_text, c_blob, c_uuid, c_ts, c_dec, c_date, c_time) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  const CassPrepared* insert = prepare(g_session, insert_cql);
  EXPECT_TRUE(insert != NULL);

  const cass_byte_t blob[] = {0xDE, (cass_byte_t)0xAD, (cass_byte_t)0xBE, (cass_byte_t)0xEF};
  const cass_byte_t decimal_unscaled[] = {0x01, (cass_byte_t)0xE2, 0x40};  // 123456 scale 2
  const cass_byte_t varint_bytes[] = {0x11, 0x22, 0x10, (cass_byte_t)0xF4, 0x7D,
                                      (cass_byte_t)0xE9, (cass_byte_t)0x81, 0x15};
  const cass_int64_t timestamp_ms = 1700000000000LL;
  const cass_uint32_t date_days = 2147483648u + 19737u;  // 2024-01-15; Cassandra stores days + 2^31
  const cass_int64_t time_nanos = 49530123000000LL;      // 13:45:30.123

  CassStatement* stmt = cass_prepared_bind(insert);
  EXPECT_EQ(cass_statement_bind_string(stmt, 0, "types-key"), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int8(stmt, 1, 42), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int16(stmt, 2, -1234), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int32(stmt, 3, 123456), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int64(stmt, 4, 9007199254740993LL), CASS_OK);
  EXPECT_EQ(cass_statement_bind_float(stmt, 5, 3.14f), CASS_OK);
  EXPECT_EQ(cass_statement_bind_double(stmt, 6, 2.718281828), CASS_OK);
  EXPECT_EQ(cass_statement_bind_bool(stmt, 7, cass_true), CASS_OK);
  EXPECT_EQ(cass_statement_bind_string(stmt, 8, "hello cassandra"), CASS_OK);
  EXPECT_EQ(cass_statement_bind_bytes(stmt, 9, blob, sizeof(blob)), CASS_OK);
  EXPECT_EQ(cass_statement_bind_uuid(stmt, 10, uuid), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int64(stmt, 11, timestamp_ms), CASS_OK);
  EXPECT_EQ(cass_statement_bind_decimal(stmt, 12, decimal_unscaled, sizeof(decimal_unscaled), 2),
            CASS_OK);
  EXPECT_EQ(cass_statement_bind_uint32(stmt, 13, date_days), CASS_OK);
  EXPECT_EQ(cass_statement_bind_int64(stmt, 14, time_nanos), CASS_OK);
  const CassResult* written = execute_statement(g_session, stmt);
  cass_statement_free(stmt);
  cass_prepared_free(insert);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  // varint has no dedicated bind call: it is bound as raw bytes
  EXPECT_TRUE(exec(g_session,
                   "INSERT INTO demo.t_types (k, c_varint) VALUES ('types-key', 0)"));
  const CassPrepared* varint_insert =
      prepare(g_session, "UPDATE demo.t_types SET c_varint = ? WHERE k = 'types-key'");
  EXPECT_TRUE(varint_insert != NULL);
  CassStatement* varint_stmt = cass_prepared_bind(varint_insert);
  EXPECT_EQ(cass_statement_bind_bytes(varint_stmt, 0, varint_bytes, sizeof(varint_bytes)), CASS_OK);
  const CassResult* varint_written = execute_statement(g_session, varint_stmt);
  cass_statement_free(varint_stmt);
  cass_prepared_free(varint_insert);
  EXPECT_TRUE(varint_written != NULL);
  cass_result_free(varint_written);

  const CassPrepared* select =
      prepare(g_session, "SELECT * FROM demo.t_types WHERE k = ?");
  EXPECT_TRUE(select != NULL);
  CassStatement* read = cass_prepared_bind(select);
  cass_statement_bind_string(read, 0, "types-key");
  const CassResult* rows = execute_statement(g_session, read);
  cass_statement_free(read);
  cass_prepared_free(select);
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);

  cass_int8_t tiny = 0;
  cass_int16_t small = 0;
  cass_int32_t integer = 0;
  cass_int64_t bigint = 0;
  cass_float_t flt = 0.0f;
  cass_double_t dbl = 0.0;
  cass_bool_t boolean = cass_false;
  CassUuid read_uuid;
  std::string text_value;
  EXPECT_TRUE(get_string(row, "k", &text_value));
  EXPECT_EQ(text_value, std::string("types-key"));
  EXPECT_EQ(cass_value_get_int8(cass_row_get_column_by_name(row, "c_tiny"), &tiny), CASS_OK);
  EXPECT_EQ(tiny, (cass_int8_t)42);
  EXPECT_EQ(cass_value_get_int16(cass_row_get_column_by_name(row, "c_small"), &small), CASS_OK);
  EXPECT_EQ(small, (cass_int16_t)-1234);
  EXPECT_TRUE(get_int32(row, "c_int", &integer));
  EXPECT_EQ(integer, (cass_int32_t)123456);
  EXPECT_TRUE(get_int64(row, "c_bigint", &bigint));
  EXPECT_EQ(bigint, (cass_int64_t)9007199254740993LL);
  EXPECT_EQ(cass_value_get_float(cass_row_get_column_by_name(row, "c_float"), &flt), CASS_OK);
  EXPECT_TRUE(flt > 3.13f && flt < 3.15f);
  EXPECT_EQ(cass_value_get_double(cass_row_get_column_by_name(row, "c_double"), &dbl), CASS_OK);
  EXPECT_TRUE(dbl > 2.718 && dbl < 2.719);
  EXPECT_TRUE(get_bool(row, "c_bool", &boolean));
  EXPECT_EQ(boolean, cass_true);
  EXPECT_TRUE(get_string(row, "c_text", &text_value));
  EXPECT_EQ(text_value, std::string("hello cassandra"));

  const CassValue* blob_value = cass_row_get_column_by_name(row, "c_blob");
  const cass_byte_t* blob_data = NULL;
  size_t blob_size = 0;
  EXPECT_EQ(cass_value_get_bytes(blob_value, &blob_data, &blob_size), CASS_OK);
  EXPECT_EQ(blob_size, sizeof(blob));
  EXPECT_TRUE(std::memcmp(blob_data, blob, sizeof(blob)) == 0);

  EXPECT_TRUE(get_uuid(row, "c_uuid", &read_uuid));
  EXPECT_EQ(uuid_str(read_uuid), uuid_str(uuid));

  cass_int64_t timestamp_value = 0;
  EXPECT_TRUE(get_int64(row, "c_ts", &timestamp_value));
  EXPECT_EQ(timestamp_value, timestamp_ms);

  const CassValue* decimal_value = cass_row_get_column_by_name(row, "c_dec");
  const cass_byte_t* decimal_data = NULL;
  size_t decimal_size = 0;
  cass_int32_t scale = 0;
  EXPECT_EQ(cass_value_get_decimal(decimal_value, &decimal_data, &decimal_size, &scale), CASS_OK);
  EXPECT_EQ(scale, (cass_int32_t)2);
  EXPECT_EQ(decimal_size, sizeof(decimal_unscaled));
  EXPECT_TRUE(std::memcmp(decimal_data, decimal_unscaled, decimal_size) == 0);

  cass_uint32_t date_value = 0;
  EXPECT_EQ(cass_value_get_uint32(cass_row_get_column_by_name(row, "c_date"), &date_value),
            CASS_OK);
  EXPECT_EQ(date_value, date_days);

  cass_int64_t time_value = 0;
  EXPECT_TRUE(get_int64(row, "c_time", &time_value));
  EXPECT_EQ(time_value, time_nanos);

  const CassValue* varint_value = cass_row_get_column_by_name(row, "c_varint");
  const cass_byte_t* varint_data = NULL;
  size_t varint_size = 0;
  EXPECT_EQ(cass_value_get_bytes(varint_value, &varint_data, &varint_size), CASS_OK);
  EXPECT_EQ(varint_size, sizeof(varint_bytes));
  EXPECT_TRUE(std::memcmp(varint_data, varint_bytes, varint_size) == 0);
  cass_result_free(rows);

  // verify server-side textual representation of the temporal / numeric types
  const CassResult* casts = run(g_session,
                                "SELECT cast(c_ts AS text) AS ts, cast(c_date AS text) AS d, "
                                "cast(c_time AS text) AS t, cast(c_varint AS text) AS v "
                                "FROM demo.t_types WHERE k = 'types-key'");
  EXPECT_TRUE(casts != NULL);
  const CassRow* cast_row = cass_result_first_row(casts);
  EXPECT_TRUE(cast_row != NULL);
  std::string ts_text, date_text, time_text, varint_text;
  EXPECT_TRUE(get_string(cast_row, "ts", &ts_text));
  EXPECT_TRUE(ts_text == "2023-11-14T22:13:20.000Z" ||
              ts_text == "2023-11-14T22:13:20.000+0000");
  EXPECT_TRUE(get_string(cast_row, "d", &date_text));
  EXPECT_EQ(date_text, std::string("2024-01-15"));
  EXPECT_TRUE(get_string(cast_row, "t", &time_text));
  EXPECT_TRUE(time_text.rfind("13:45:30.123", 0) == 0);
  EXPECT_TRUE(get_string(cast_row, "v", &varint_text));
  EXPECT_EQ(varint_text, std::string("1234567890123456789"));
  cass_result_free(casts);

  // driver helper: date + time -> epoch seconds (2024-01-15T13:45:30Z)
  EXPECT_EQ(cass_date_time_to_epoch(date_days, time_nanos), (cass_int64_t)1705326330LL);

  cass_uuid_gen_free(uuid_gen);
  return true;
}

TEST(t07_batch_statement) {
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_batch (k text PRIMARY KEY, v int)"));

  const CassPrepared* insert = prepare(g_session, "INSERT INTO demo.t_batch (k, v) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);

  CassBatch* batch = cass_batch_new(CASS_BATCH_TYPE_LOGGED);
  for (int i = 0; i < 3; ++i) {
    CassStatement* stmt = cass_prepared_bind(insert);
    std::string key = "batch-" + std::to_string(i);
    cass_statement_bind_string(stmt, 0, key.c_str());
    cass_statement_bind_int32(stmt, 1, i);
    CassError code = cass_batch_add_statement(batch, stmt);
    cass_statement_free(stmt);
    EXPECT_EQ(code, CASS_OK);
  }
  CassFuture* future = cass_session_execute_batch(g_session, batch);
  CassError batch_code = cass_future_error_code(future);
  cass_future_free(future);
  cass_batch_free(batch);
  cass_prepared_free(insert);
  EXPECT_EQ(batch_code, CASS_OK);

  const CassResult* rows = run(g_session, "SELECT count(*) AS n FROM demo.t_batch");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  cass_int64_t count = 0;
  EXPECT_TRUE(get_int64(row, "n", &count));
  EXPECT_EQ(count, (cass_int64_t)3);
  cass_result_free(rows);

  // counter batch type (truncate so the counter starts from zero on every run)
  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_ctr (k text PRIMARY KEY, c counter)"));
  EXPECT_TRUE(exec(g_session, "TRUNCATE demo.t_ctr"));
  CassBatch* counter_batch = cass_batch_new(CASS_BATCH_TYPE_COUNTER);
  for (int i = 0; i < 5; ++i) {
    CassStatement* stmt = cass_statement_new("UPDATE demo.t_ctr SET c = c + 1 WHERE k = 'ctr'", 0);
    CassError code = cass_batch_add_statement(counter_batch, stmt);
    cass_statement_free(stmt);
    EXPECT_EQ(code, CASS_OK);
  }
  CassFuture* counter_future = cass_session_execute_batch(g_session, counter_batch);
  CassError counter_code = cass_future_error_code(counter_future);
  cass_future_free(counter_future);
  cass_batch_free(counter_batch);
  EXPECT_EQ(counter_code, CASS_OK);

  const CassResult* counter_rows = run(g_session, "SELECT c FROM demo.t_ctr WHERE k = 'ctr'");
  EXPECT_TRUE(counter_rows != NULL);
  const CassRow* counter_row = cass_result_first_row(counter_rows);
  EXPECT_TRUE(counter_row != NULL);
  cass_int64_t counter = 0;
  EXPECT_TRUE(get_int64(counter_row, "c", &counter));
  EXPECT_EQ(counter, (cass_int64_t)5);
  cass_result_free(counter_rows);
  return true;
}

TEST(t08_paging) {
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_page (k int PRIMARY KEY, v text)"));
  const CassPrepared* insert = prepare(g_session, "INSERT INTO demo.t_page (k, v) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);
  for (int i = 0; i < 40; ++i) {
    CassStatement* stmt = cass_prepared_bind(insert);
    cass_statement_bind_int32(stmt, 0, i);
    std::string value = "row-" + std::to_string(i);
    cass_statement_bind_string(stmt, 1, value.c_str());
    const CassResult* written = execute_statement(g_session, stmt);
    cass_statement_free(stmt);
    EXPECT_TRUE(written != NULL);
    cass_result_free(written);
  }
  cass_prepared_free(insert);

  CassStatement* statement =
      cass_statement_new("SELECT k, v FROM demo.t_page", 0);
  cass_statement_set_paging_size(statement, 7);

  size_t total_rows = 0;
  int pages = 0;
  bool saw_paging_token = false;
  while (true) {
    const CassResult* page = execute_statement(g_session, statement);
    EXPECT_TRUE(page != NULL);
    ++pages;
    total_rows += cass_result_row_count(page);

    const char* token = NULL;
    size_t token_length = 0;
    CassError token_code = cass_result_paging_state_token(page, &token, &token_length);
    if (token_code != CASS_OK || token == NULL || token_length == 0) {
      cass_result_free(page);
      break;
    }
    saw_paging_token = true;
    EXPECT_EQ(cass_statement_set_paging_state_token(statement, token, token_length), CASS_OK);
    cass_result_free(page);
  }
  cass_statement_free(statement);

  EXPECT_TRUE(saw_paging_token);
  EXPECT_TRUE(pages > 1);
  EXPECT_EQ(total_rows, (size_t)40);
  return true;
}

TEST(t09_collections) {
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_coll ("
                   "k text PRIMARY KEY, l list<text>, s set<text>, m map<text, int>)"));

  CassCollection* list = cass_collection_new(CASS_COLLECTION_TYPE_LIST, 3);
  EXPECT_EQ(cass_collection_append_string(list, "alpha"), CASS_OK);
  EXPECT_EQ(cass_collection_append_string(list, "beta"), CASS_OK);
  EXPECT_EQ(cass_collection_append_string(list, "gamma"), CASS_OK);

  CassCollection* set = cass_collection_new(CASS_COLLECTION_TYPE_SET, 2);
  EXPECT_EQ(cass_collection_append_string(set, "red"), CASS_OK);
  EXPECT_EQ(cass_collection_append_string(set, "blue"), CASS_OK);

  CassCollection* map = cass_collection_new(CASS_COLLECTION_TYPE_MAP, 2);
  EXPECT_EQ(cass_collection_append_string(map, "one"), CASS_OK);
  EXPECT_EQ(cass_collection_append_int32(map, 1), CASS_OK);
  EXPECT_EQ(cass_collection_append_string(map, "two"), CASS_OK);
  EXPECT_EQ(cass_collection_append_int32(map, 2), CASS_OK);

  CassStatement* statement = cass_statement_new(
      "INSERT INTO demo.t_coll (k, l, s, m) VALUES (?, ?, ?, ?)", 4);
  EXPECT_EQ(cass_statement_bind_string(statement, 0, "coll-key"), CASS_OK);
  EXPECT_EQ(cass_statement_bind_collection(statement, 1, list), CASS_OK);
  EXPECT_EQ(cass_statement_bind_collection(statement, 2, set), CASS_OK);
  EXPECT_EQ(cass_statement_bind_collection(statement, 3, map), CASS_OK);
  const CassResult* written = execute_statement(g_session, statement);
  cass_statement_free(statement);
  cass_collection_free(list);
  cass_collection_free(set);
  cass_collection_free(map);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  const CassResult* rows =
      run(g_session, "SELECT l, s, m FROM demo.t_coll WHERE k = 'coll-key'");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);

  const CassValue* list_value = cass_row_get_column_by_name(row, "l");
  EXPECT_EQ(cass_value_type(list_value), CASS_VALUE_TYPE_LIST);
  CassIterator* it = cass_iterator_from_collection(list_value);
  std::vector<std::string> list_items;
  while (cass_iterator_next(it)) {
    const CassValue* item = cass_iterator_get_value(it);
    const char* text = NULL;
    size_t length = 0;
    EXPECT_EQ(cass_value_get_string(item, &text, &length), CASS_OK);
    list_items.push_back(std::string(text, length));
  }
  cass_iterator_free(it);
  EXPECT_EQ(list_items.size(), (size_t)3);
  EXPECT_EQ(list_items[0], std::string("alpha"));
  EXPECT_EQ(list_items[2], std::string("gamma"));

  const CassValue* set_value = cass_row_get_column_by_name(row, "s");
  EXPECT_EQ(cass_value_type(set_value), CASS_VALUE_TYPE_SET);
  it = cass_iterator_from_collection(set_value);
  size_t set_items = 0;
  while (cass_iterator_next(it)) ++set_items;
  cass_iterator_free(it);
  EXPECT_EQ(set_items, (size_t)2);

  const CassValue* map_value = cass_row_get_column_by_name(row, "m");
  EXPECT_EQ(cass_value_type(map_value), CASS_VALUE_TYPE_MAP);
  it = cass_iterator_from_map(map_value);
  std::vector<std::pair<std::string, cass_int32_t> > map_items;
  while (cass_iterator_next(it)) {
    const CassValue* key = cass_iterator_get_map_key(it);
    const CassValue* value = cass_iterator_get_map_value(it);
    const char* text = NULL;
    size_t length = 0;
    EXPECT_EQ(cass_value_get_string(key, &text, &length), CASS_OK);
    cass_int32_t number = 0;
    EXPECT_EQ(cass_value_get_int32(value, &number), CASS_OK);
    map_items.push_back(std::make_pair(std::string(text, length), number));
  }
  cass_iterator_free(it);
  EXPECT_EQ(map_items.size(), (size_t)2);
  EXPECT_EQ(map_items[0].first, std::string("one"));
  EXPECT_EQ(map_items[0].second, (cass_int32_t)1);
  cass_result_free(rows);
  return true;
}

TEST(t10_user_defined_type) {
  EXPECT_TRUE(exec(g_session, "CREATE TYPE IF NOT EXISTS demo.addr (street text, zip int)"));
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_udt (k text PRIMARY KEY, a addr)"));

  const CassPrepared* insert = prepare(
      g_session, "INSERT INTO demo.t_udt (k, a) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);
  const CassDataType* udt_type = cass_prepared_parameter_data_type(insert, 1);
  EXPECT_TRUE(udt_type != NULL);
  EXPECT_EQ(cass_data_type_type(udt_type), CASS_VALUE_TYPE_UDT);

  CassUserType* user_type = cass_user_type_new_from_data_type(udt_type);
  EXPECT_TRUE(user_type != NULL);
  EXPECT_EQ(cass_user_type_set_string_by_name(user_type, "street", "Main St"), CASS_OK);
  EXPECT_EQ(cass_user_type_set_int32_by_name(user_type, "zip", 12345), CASS_OK);

  CassStatement* statement = cass_prepared_bind(insert);
  EXPECT_EQ(cass_statement_bind_string(statement, 0, "udt-key"), CASS_OK);
  EXPECT_EQ(cass_statement_bind_user_type(statement, 1, user_type), CASS_OK);
  const CassResult* written = execute_statement(g_session, statement);
  cass_statement_free(statement);
  cass_user_type_free(user_type);
  cass_prepared_free(insert);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  const CassResult* rows = run(g_session, "SELECT a FROM demo.t_udt WHERE k = 'udt-key'");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  const CassValue* value = cass_row_get_column_by_name(row, "a");
  EXPECT_EQ(cass_value_type(value), CASS_VALUE_TYPE_UDT);

  CassIterator* it = cass_iterator_fields_from_user_type(value);
  std::string street;
  cass_int32_t zip = 0;
  while (cass_iterator_next(it)) {
    const char* field_name = NULL;
    size_t field_length = 0;
    EXPECT_EQ(cass_iterator_get_user_type_field_name(it, &field_name, &field_length), CASS_OK);
    const CassValue* field_value = cass_iterator_get_user_type_field_value(it);
    std::string name(field_name, field_length);
    if (name == "street") {
      const char* text = NULL;
      size_t length = 0;
      EXPECT_EQ(cass_value_get_string(field_value, &text, &length), CASS_OK);
      street.assign(text, length);
    } else if (name == "zip") {
      EXPECT_EQ(cass_value_get_int32(field_value, &zip), CASS_OK);
    }
  }
  cass_iterator_free(it);
  EXPECT_EQ(street, std::string("Main St"));
  EXPECT_EQ(zip, (cass_int32_t)12345);
  cass_result_free(rows);
  return true;
}

TEST(t11_tuple) {
  EXPECT_TRUE(exec(g_session,
                   "CREATE TABLE IF NOT EXISTS demo.t_tuple "
                   "(k text PRIMARY KEY, tp tuple<int, text>)"));

  const CassPrepared* insert = prepare(
      g_session, "INSERT INTO demo.t_tuple (k, tp) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);

  CassTuple* tuple = cass_tuple_new(2);
  EXPECT_EQ(cass_tuple_set_int32(tuple, 0, 7), CASS_OK);
  EXPECT_EQ(cass_tuple_set_string(tuple, 1, "seven"), CASS_OK);

  CassStatement* statement = cass_prepared_bind(insert);
  EXPECT_EQ(cass_statement_bind_string(statement, 0, "tuple-key"), CASS_OK);
  EXPECT_EQ(cass_statement_bind_tuple(statement, 1, tuple), CASS_OK);
  const CassResult* written = execute_statement(g_session, statement);
  cass_statement_free(statement);
  cass_tuple_free(tuple);
  cass_prepared_free(insert);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  const CassResult* rows = run(g_session, "SELECT tp FROM demo.t_tuple WHERE k = 'tuple-key'");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  const CassValue* value = cass_row_get_column_by_name(row, "tp");
  EXPECT_EQ(cass_value_type(value), CASS_VALUE_TYPE_TUPLE);

  CassIterator* it = cass_iterator_from_tuple(value);
  int index = 0;
  while (cass_iterator_next(it)) {
    const CassValue* item = cass_iterator_get_value(it);
    if (index == 0) {
      cass_int32_t number = 0;
      EXPECT_EQ(cass_value_get_int32(item, &number), CASS_OK);
      EXPECT_EQ(number, (cass_int32_t)7);
    } else if (index == 1) {
      const char* text = NULL;
      size_t length = 0;
      EXPECT_EQ(cass_value_get_string(item, &text, &length), CASS_OK);
      EXPECT_EQ(std::string(text, length), std::string("seven"));
    }
    ++index;
  }
  cass_iterator_free(it);
  EXPECT_EQ(index, 2);
  cass_result_free(rows);
  return true;
}

TEST(t12_counter) {
  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_inc (k text PRIMARY KEY, c counter)"));
  EXPECT_TRUE(exec(g_session, "TRUNCATE demo.t_inc"));
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(exec(g_session, "UPDATE demo.t_inc SET c = c + 2 WHERE k = 'inc-key'"));
  }
  const CassResult* rows = run(g_session, "SELECT c FROM demo.t_inc WHERE k = 'inc-key'");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  cass_int64_t counter = 0;
  EXPECT_TRUE(get_int64(row, "c", &counter));
  EXPECT_EQ(counter, (cass_int64_t)6);
  cass_result_free(rows);
  return true;
}

TEST(t13_json) {
  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_json "
                               "(k text PRIMARY KEY, n int, s text)"));
  EXPECT_TRUE(exec(g_session,
                   "INSERT INTO demo.t_json JSON "
                   "'{\"k\": \"json-key\", \"n\": 7, \"s\": \"seven\"}'"));

  const CassResult* rows =
      run(g_session, "SELECT k, n, s FROM demo.t_json WHERE k = 'json-key'");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  cass_int32_t number = 0;
  std::string text;
  EXPECT_TRUE(get_int32(row, "n", &number));
  EXPECT_EQ(number, (cass_int32_t)7);
  EXPECT_TRUE(get_string(row, "s", &text));
  EXPECT_EQ(text, std::string("seven"));
  cass_result_free(rows);

  const CassResult* json_rows =
      run(g_session, "SELECT JSON k, n, s FROM demo.t_json WHERE k = 'json-key'");
  EXPECT_TRUE(json_rows != NULL);
  const CassRow* json_row = cass_result_first_row(json_rows);
  EXPECT_TRUE(json_row != NULL);
  const CassValue* json_value = cass_row_get_column_by_name(json_row, "[json]");
  EXPECT_TRUE(json_value != NULL);
  const char* json_text = NULL;
  size_t json_length = 0;
  EXPECT_EQ(cass_value_get_string(json_value, &json_text, &json_length), CASS_OK);
  std::string json(json_text, json_length);
  EXPECT_TRUE(json.find("json-key") != std::string::npos);
  EXPECT_TRUE(json.find("seven") != std::string::npos);
  cass_result_free(json_rows);
  return true;
}

TEST(t14_ttl_and_timestamp) {
  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_ttl (k text PRIMARY KEY, v int)"));

  const CassPrepared* ttl_insert = prepare(
      g_session, "INSERT INTO demo.t_ttl (k, v) VALUES (?, ?) USING TTL 60");
  EXPECT_TRUE(ttl_insert != NULL);
  CassStatement* stmt = cass_prepared_bind(ttl_insert);
  cass_statement_bind_string(stmt, 0, "ttl-key");
  cass_statement_bind_int32(stmt, 1, 1);
  const CassResult* written = execute_statement(g_session, stmt);
  cass_statement_free(stmt);
  cass_prepared_free(ttl_insert);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  const CassResult* ttl_rows = run(g_session, "SELECT ttl(v) AS t FROM demo.t_ttl WHERE k = 'ttl-key'");
  EXPECT_TRUE(ttl_rows != NULL);
  const CassRow* ttl_row = cass_result_first_row(ttl_rows);
  EXPECT_TRUE(ttl_row != NULL);
  cass_int32_t ttl = 0;
  EXPECT_TRUE(get_int32(ttl_row, "t", &ttl));
  EXPECT_TRUE(ttl > 0 && ttl <= 60);
  cass_result_free(ttl_rows);

  // explicit client-side write timestamp (unique key so the write is the LWW winner)
  std::ostringstream key;
  key << "ts-key-" << std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  std::string insert_cql =
      "INSERT INTO demo.t_ttl (k, v) VALUES ('" + key.str() + "', 3)";
  CassStatement* stamped = cass_statement_new(insert_cql.c_str(), 0);
  EXPECT_EQ(cass_statement_set_timestamp(stamped, 1700000000000000LL), CASS_OK);
  const CassResult* stamped_result = execute_statement(g_session, stamped);
  cass_statement_free(stamped);
  EXPECT_TRUE(stamped_result != NULL);
  cass_result_free(stamped_result);

  const CassResult* writetime_rows =
      run(g_session, "SELECT writetime(v) AS w FROM demo.t_ttl WHERE k = '" + key.str() + "'");
  EXPECT_TRUE(writetime_rows != NULL);
  const CassRow* writetime_row = cass_result_first_row(writetime_rows);
  EXPECT_TRUE(writetime_row != NULL);
  cass_int64_t writetime = 0;
  EXPECT_TRUE(get_int64(writetime_row, "w", &writetime));
  EXPECT_EQ(writetime, (cass_int64_t)1700000000000000LL);
  cass_result_free(writetime_rows);
  return true;
}

void on_future_complete(CassFuture* future, void* data) {
  (void)future;
  static_cast<std::atomic<int>*>(data)->fetch_add(1);
}

TEST(t15_async_future_and_callback) {
  static std::atomic<int> callback_count(0);
  callback_count.store(0);

  CassStatement* statement = cass_statement_new("SELECT release_version FROM system.local", 0);
  CassFuture* future = cass_session_execute(g_session, statement);
  cass_statement_free(statement);
  EXPECT_EQ(cass_future_set_callback(future, on_future_complete, &callback_count), CASS_OK);
  EXPECT_TRUE(cass_future_wait_timed(future, 15 * 1000 * 1000ULL));
  EXPECT_EQ(cass_future_error_code(future), CASS_OK);
  const CassResult* result = cass_future_get_result(future);
  cass_future_free(future);
  EXPECT_TRUE(result != NULL);
  EXPECT_EQ(cass_result_row_count(result), (size_t)1);
  cass_result_free(result);
  EXPECT_EQ(callback_count.load(), 1);
  return true;
}

TEST(t16_error_handling) {
  CassStatement* bad = cass_statement_new("SELEC 1", 0);
  CassFuture* future = cass_session_execute(g_session, bad);
  cass_statement_free(bad);
  CassError code = cass_future_error_code(future);
  const char* message = NULL;
  size_t length = 0;
  cass_future_error_message(future, &message, &length);
  EXPECT_EQ(code, CASS_ERROR_SERVER_SYNTAX_ERROR);
  EXPECT_TRUE(length > 0);
  EXPECT_TRUE(std::string(cass_error_desc(code)).size() > 0);
  std::string text(message ? message : "", length);
  EXPECT_TRUE(text.find("line 1") != std::string::npos ||
              text.find("Syntax") != std::string::npos);
  cass_future_free(future);

  const CassResult* missing = run(g_session, "SELECT * FROM demo.no_such_table");
  EXPECT_TRUE(missing == NULL);
  return true;
}

TEST(t17_uuid_generation) {
  CassUuidGen* generator = cass_uuid_gen_new();
  EXPECT_TRUE(generator != NULL);

  CassUuid time_uuid;
  cass_uuid_gen_time(generator, &time_uuid);
  EXPECT_EQ(cass_uuid_version(time_uuid), (cass_uint8_t)1);
  EXPECT_TRUE(cass_uuid_timestamp(time_uuid) > 0);

  CassUuid random_uuid;
  cass_uuid_gen_random(generator, &random_uuid);
  EXPECT_EQ(cass_uuid_version(random_uuid), (cass_uint8_t)4);

  cass_uint64_t now_ms = (cass_uint64_t)std::time(NULL) * 1000;
  CassUuid from_time;
  cass_uuid_gen_from_time(generator, now_ms, &from_time);
  EXPECT_EQ(cass_uuid_version(from_time), (cass_uint8_t)1);
  EXPECT_TRUE(cass_uuid_timestamp(from_time) >= now_ms);

  std::string rendered = uuid_str(time_uuid);
  EXPECT_EQ(rendered.size(), (size_t)(CASS_UUID_STRING_LENGTH - 1));
  CassUuid parsed;
  EXPECT_EQ(cass_uuid_from_string(rendered.c_str(), &parsed), CASS_OK);
  EXPECT_EQ(uuid_str(parsed), rendered);

  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_uuid "
                               "(k int PRIMARY KEY, id timeuuid)"));
  const CassPrepared* insert = prepare(g_session, "INSERT INTO demo.t_uuid (k, id) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);
  CassStatement* statement = cass_prepared_bind(insert);
  cass_statement_bind_int32(statement, 0, 1);
  cass_statement_bind_uuid(statement, 1, time_uuid);
  const CassResult* written = execute_statement(g_session, statement);
  cass_statement_free(statement);
  cass_prepared_free(insert);
  EXPECT_TRUE(written != NULL);
  cass_result_free(written);

  const CassResult* rows = run(g_session, "SELECT id FROM demo.t_uuid WHERE k = 1");
  EXPECT_TRUE(rows != NULL);
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  CassUuid stored;
  EXPECT_TRUE(get_uuid(row, "id", &stored));
  EXPECT_EQ(uuid_str(stored), rendered);
  cass_result_free(rows);

  cass_uuid_gen_free(generator);
  return true;
}

TEST(t18_tracing) {
  CassStatement* statement = cass_statement_new("SELECT release_version FROM system.local", 0);
  EXPECT_EQ(cass_statement_set_tracing(statement, cass_true), CASS_OK);
  CassFuture* future = cass_session_execute(g_session, statement);
  cass_statement_free(statement);
  EXPECT_TRUE(cass_future_wait_timed(future, 15 * 1000 * 1000ULL));
  EXPECT_EQ(cass_future_error_code(future), CASS_OK);
  CassUuid trace_id;
  EXPECT_EQ(cass_future_tracing_id(future, &trace_id), CASS_OK);
  CassUuid empty_id;
  std::memset(&empty_id, 0, sizeof(empty_id));
  EXPECT_TRUE(std::memcmp(&trace_id, &empty_id, sizeof(CassUuid)) != 0);
  EXPECT_TRUE(cass_uuid_timestamp(trace_id) > 0);
  const CassResult* result = cass_future_get_result(future);
  cass_future_free(future);
  EXPECT_TRUE(result != NULL);
  cass_result_free(result);
  return true;
}

TEST(t19_row_iteration) {
  EXPECT_TRUE(exec(g_session, "CREATE TABLE IF NOT EXISTS demo.t_iter (k int PRIMARY KEY, v text)"));
  const CassPrepared* insert = prepare(g_session, "INSERT INTO demo.t_iter (k, v) VALUES (?, ?)");
  EXPECT_TRUE(insert != NULL);
  for (int i = 0; i < 5; ++i) {
    CassStatement* stmt = cass_prepared_bind(insert);
    cass_statement_bind_int32(stmt, 0, i);
    std::string value = "value-" + std::to_string(i);
    cass_statement_bind_string(stmt, 1, value.c_str());
    const CassResult* written = execute_statement(g_session, stmt);
    cass_statement_free(stmt);
    EXPECT_TRUE(written != NULL);
    cass_result_free(written);
  }
  cass_prepared_free(insert);

  const CassResult* rows = run(g_session, "SELECT k, v FROM demo.t_iter");
  EXPECT_TRUE(rows != NULL);
  EXPECT_EQ(cass_result_row_count(rows), (size_t)5);

  CassIterator* it = cass_iterator_from_result(rows);
  EXPECT_TRUE(it != NULL);
  int seen = 0;
  while (cass_iterator_next(it)) {
    const CassRow* row = cass_iterator_get_row(it);
    EXPECT_TRUE(row != NULL);
    cass_int32_t key = 0;
    std::string value;
    EXPECT_TRUE(get_int32(row, "k", &key));
    EXPECT_TRUE(get_string(row, "v", &value));
    EXPECT_EQ(value, "value-" + std::to_string(key));
    ++seen;
  }
  cass_iterator_free(it);
  EXPECT_EQ(seen, 5);

  // column by name and by index on a single row
  const CassRow* row = cass_result_first_row(rows);
  EXPECT_TRUE(row != NULL);
  const CassValue* by_name = cass_row_get_column_by_name(row, "v");
  const CassValue* by_index = cass_row_get_column(row, 1);
  EXPECT_TRUE(by_name == by_index);
  cass_result_free(rows);
  return true;
}

TEST(t20_schema_metadata) {
  const CassSchemaMeta* schema = cass_session_get_schema_meta(g_session);
  EXPECT_TRUE(schema != NULL);
  const CassKeyspaceMeta* keyspace = cass_schema_meta_keyspace_by_name(schema, g_config.keyspace.c_str());
  EXPECT_TRUE(keyspace != NULL);
  const char* name = NULL;
  size_t name_length = 0;
  cass_keyspace_meta_name(keyspace, &name, &name_length);
  EXPECT_TRUE(name != NULL);
  EXPECT_EQ(std::string(name, name_length), g_config.keyspace);

  const CassTableMeta* table = cass_keyspace_meta_table_by_name(keyspace, "t_prep");
  EXPECT_TRUE(table != NULL);

  const CassDataType* udt = cass_keyspace_meta_user_type_by_name(keyspace, "addr");
  EXPECT_TRUE(udt != NULL);
  EXPECT_EQ(cass_data_type_type(udt), CASS_VALUE_TYPE_UDT);
  EXPECT_TRUE(cass_data_type_sub_type_count(udt) == 2);
  cass_schema_meta_free(schema);
  return true;
}

}  // namespace

namespace {

struct Options {
  std::string config_path;
  std::string filter;
  bool verbose = false;
  bool no_color = false;
  bool help = false;
};

void print_usage(const char* program) {
  std::cout << "usage: " << program << " [options] [test-name-filter]\n"
            << "\n"
            << "  --config <file>  connection config file (default: ./cassandra.conf,\n"
            << "                   then <executable dir>/cassandra.conf)\n"
            << "  --no-color       print PASSED/FAILED without colors\n"
            << "  --verbose        show the driver's own log output\n"
            << "  --help           show this help\n"
            << "\n"
            << "Connection settings come from the config file, overridable via\n"
            << "CASSANDRA_HOST, CASSANDRA_PORT, CASSANDRA_PROTOCOL, CASSANDRA_USER,\n"
            << "CASSANDRA_PASSWORD, CASSANDRA_KEYSPACE and CASSANDRA_CONFIG.\n";
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      options->help = true;
    } else if (arg == "--no-color") {
      options->no_color = true;
    } else if (arg == "--verbose") {
      options->verbose = true;
    } else if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "error: --config requires a file path" << std::endl;
        return false;
      }
      options->config_path = argv[++i];
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "error: unknown option: " << arg << std::endl;
      return false;
    } else if (options->filter.empty()) {
      options->filter = arg;
    }
  }
  return true;
}

// returns false when an explicitly requested config file cannot be loaded
bool resolve_config(const Options& options, ConnectionConfig* config) {
  std::string error;

  if (!options.config_path.empty()) {
    if (!load_config_file(options.config_path, config, &error)) {
      std::cerr << error << std::endl;
      return false;
    }
    return true;
  }

  const char* env_path = std::getenv("CASSANDRA_CONFIG");
  if (env_path && *env_path) {
    if (!load_config_file(env_path, config, &error)) {
      std::cerr << error << std::endl;
      return false;
    }
    return true;
  }

  std::vector<std::string> candidates;
  candidates.push_back("cassandra.conf");
  std::string dir = executable_dir();
  if (!dir.empty()) candidates.push_back(dir + "/cassandra.conf");

  for (size_t i = 0; i < candidates.size(); ++i) {
    std::ifstream probe(candidates[i].c_str());
    if (!probe) continue;
    probe.close();
    if (!load_config_file(candidates[i], config, &error)) {
      std::cerr << error << std::endl;
      return false;
    }
    return true;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    print_usage(argv[0]);
    return 2;
  }
  if (options.help) {
    print_usage(argv[0]);
    return 0;
  }

  if (!resolve_config(options, &g_config)) return 2;
  apply_environment(&g_config);

  g_color = !options.no_color && isatty(fileno(stdout)) && std::getenv("NO_COLOR") == NULL;
  if (!options.verbose) cass_log_set_level(CASS_LOG_DISABLED);

  g_cluster = make_cluster();
  g_session = cass_session_new();
  CassFuture* connect_future = cass_session_connect(g_session, g_cluster);
  if (!cass_future_wait_timed(connect_future, 30 * 1000 * 1000ULL) ||
      cass_future_error_code(connect_future) != CASS_OK) {
    print_future_error("unable to connect", connect_future);
    cass_future_free(connect_future);
    std::cerr << g_config.host << ":" << g_config.port << "\n" << g_diag.str();
    cass_session_free(g_session);
    cass_cluster_free(g_cluster);
    return 2;
  }
  cass_future_free(connect_future);

  // server info for the banner
  std::string cluster_version = "unknown";
  std::string cluster_name = "unknown";
  g_diag.str("");
  g_diag.clear();
  const CassResult* info = run(g_session, "SELECT release_version, cluster_name FROM system.local");
  if (info) {
    const CassRow* row = cass_result_first_row(info);
    if (row) {
      get_string(row, "release_version", &cluster_version);
      get_string(row, "cluster_name", &cluster_name);
    }
    cass_result_free(info);
  }
  g_diag.str("");
  g_diag.clear();

  std::cout << "Cassandra C/C++ driver function tests\n"
            << "  driver            : " << CASS_DRIVER_VERSION << "\n"
            << "  protocol          : v" << g_config.protocol << "\n"
            << "  cassandra cluster : " << cluster_version << " (\"" << cluster_name << "\")\n"
            << "  endpoint          : " << g_config.host << ":" << g_config.port << "\n"
            << "  user              : " << g_config.user << "\n"
            << "  keyspace          : " << g_config.keyspace << "\n"
            << "  config            : " << g_config.source << "\n"
            << std::endl;

  const std::string& filter = options.filter;
  int passed = 0;
  int failed = 0;
  int skipped = 0;
  std::vector<std::string> failures;

  for (size_t i = 0; i < all_tests().size(); ++i) {
    const TestCase& test = all_tests()[i];
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    std::cout << "  " << test.name << " ... " << std::flush;
    g_diag.str("");
    g_diag.clear();
    bool ok = test.fn();
    std::cout << (ok ? paint("32", "PASSED") : paint("31", "FAILED")) << std::endl;
    if (!ok) {
      std::string diagnostics = g_diag.str();
      if (!diagnostics.empty()) std::cout << diagnostics;
      failures.push_back(test.name);
      ++failed;
    } else {
      ++passed;
    }
  }

  if (!failures.empty()) {
    for (size_t i = 0; i < failures.size(); ++i) {
      std::cout << "  " << paint("31", "failed") << ": " << failures[i] << "\n";
    }
  }

  std::cout << "Summary: " << passed << " passed, " << failed << " failed out of "
            << (passed + failed + skipped) << " total";
  if (skipped > 0) std::cout << " (" << skipped << " skipped)";
  std::cout << std::endl;

  CassFuture* close_future = cass_session_close(g_session);
  cass_future_wait(close_future);
  cass_future_free(close_future);
  cass_session_free(g_session);
  cass_cluster_free(g_cluster);

  return failed == 0 ? 0 : 1;
}
