# cass-cpp-functiontest

Function test suite for the DataStax/Apache **C/C++ driver for Cassandra** (cpp-driver), run
against a single-node Cassandra cluster with authentication enabled.

Each test exercises one area of the driver API (connection/auth, prepared statements, types,
paging, collections, UDTs, tuples, counters, JSON, TTL/timestamps, async futures, errors,
UUIDs, tracing, iterators, schema metadata, protocol v5 types) and reports `PASSED` or `FAILED`
per test.

- Driver: **2.18.0**, native protocol **v5** — built from the
  [`Lyky35/cassandra-cpp-driver`](https://github.com/Lyky35/cassandra-cpp-driver) fork,
  branch `proto-v5`
- Tested against: **Cassandra 5.0.9**

## Running the tests

Prebuilt binary (from the [release](https://github.com/Lyky35/cass-cpp-functiontest/releases)):

```bash
tar -xzf cass-cpp-functiontest-linux-x86_64.tar.gz
./cassandra_function_tests
```

From source (requires the proto-v5 driver installed with its `libcassandra.pc` visible to
pkg-config):

```bash
# driver: https://github.com/Lyky35/cassandra-cpp-driver (branch proto-v5)
#   git clone -b proto-v5 git@github.com:Lyky35/cassandra-cpp-driver.git
#   cmake -S cassandra-cpp-driver -B cassandra-cpp-driver/build \
#     -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_INSTALL_PREFIX="$PWD/cassandra-cpp-driver/_install"
#   cmake --build cassandra-cpp-driver/build -j"$(nproc)"
#   cmake --install cassandra-cpp-driver/build

cmake -S . -B build -DCASSANDRA_DRIVER_ROOT=/path/to/cassandra-cpp-driver/_install
cmake --build build -j"$(nproc)"
./build/cassandra_function_tests       # or: (cd build && ctest --output-on-failure)

# self-contained package (binary + libcassandra.so.2.18.0 + cassandra.conf)
cmake --install build --prefix "$PWD/dist"
```

`CASSANDRA_DRIVER_ROOT` points CMake at an install prefix of the fork (it only prepends that
prefix's `pkgconfig` directory). Without it, whatever `pkg-config` finds is used —
`PKG_CONFIG_PATH=/path/to/_install/lib/pkgconfig cmake -S . -B build` works as well.

Exit code is `0` when every test passes, `1` when any test fails, `2` on startup/config errors.

### Command line

```
usage: cassandra_function_tests [options] [test-name-filter]

  --config <file>  connection config file (default: ./cassandra.conf,
                   then <executable dir>/cassandra.conf)
  --no-color       print PASSED/FAILED without colors
  --verbose        show the driver's own log output
  --help           show this help
```

A positional argument filters tests by substring, e.g. `cassandra_function_tests t08` runs only
`t08_paging` (skipped tests are reported in the summary).

## The tests

| Test | What it verifies |
|------|------------------|
| `t01_connect_with_auth` | Connects with username/password, and a wrong password is rejected with `Bad credentials` |
| `t02_cluster_metadata` | `system.local` cluster name/version/partitioner; `system.peers` empty (single node) |
| `t03_simple_query` | Simple `SELECT` row result plus a non-row (schema change) result |
| `t04_column_metadata` | Result column count, name and value type |
| `t05_prepared_statement` | Prepared insert/select, positional and named (`:key`) parameters, parameter type metadata |
| `t06_bound_types` | Round-trips `tinyint, smallint, int, bigint, float, double, boolean, text, blob, uuid, timestamp, decimal, date, time, varint` incl. `cast(... as text)` checks and `cass_date_time_to_epoch()` |
| `t07_batch_statement` | Logged batch (3 inserts, count = 3) and counter batch (5 increments) |
| `t08_paging` | 40 rows read with page size 7, following the paging state token across pages |
| `t09_collections` | `list<text>`, `set<text>`, `map<text,int>` bound via `CassCollection`, read back with iterators |
| `t10_user_defined_type` | `CREATE TYPE`, UDT built from prepared parameter metadata, fields iterated by name |
| `t11_tuple` | `tuple<int, text>` bound via `CassTuple`, elements read back |
| `t12_counter` | Counter increments (`+2` three times = 6); table truncated first for determinism |
| `t13_json` | `INSERT ... JSON` and `SELECT ... JSON` |
| `t14_ttl_and_timestamp` | `USING TTL` visible through `ttl()`, client-side `cass_statement_set_timestamp()` visible through `writetime()` |
| `t15_async_future_and_callback` | Future callback fires exactly once, `cass_future_wait_timed()` succeeds |
| `t16_error_handling` | Syntax error returns `CASS_ERROR_SERVER_SYNTAX_ERROR` with a server message; missing table fails cleanly |
| `t17_uuid_generation` | v1/v4 UUID generation, `cass_uuid_gen_from_time()`, string round-trip, `timeuuid` column storage |
| `t18_tracing` | Per-statement tracing enabled, `cass_future_tracing_id()` returns a non-zero trace id |
| `t19_row_iteration` | Result/row iterators, column access by name and by index |
| `t20_schema_metadata` | Schema metadata: keyspace, table and UDT lookups |
| `t21_v5_duration_type` | Protocol v5 only: `duration` (wire type `0x0015`) bound with `cass_statement_bind_duration()` and a CQL duration literal, both read back with `cass_value_get_duration()` |

## Configuration

Connection settings come from a config file, overridable by environment variables
(file < environment):

| Key | Env override | Default |
|-----|--------------|---------|
| `host` | `CASSANDRA_HOST` | `127.0.0.1` |
| `port` | `CASSANDRA_PORT` | `9042` |
| `protocol` (3, 4 or 5) | `CASSANDRA_PROTOCOL` | `v5` |
| `user` / `username` | `CASSANDRA_USER` | `appuser` |
| `password` | `CASSANDRA_PASSWORD` | `appuser123` |
| `keyspace` | `CASSANDRA_KEYSPACE` | `demo` |

File lookup order: `--config <file>` → `$CASSANDRA_CONFIG` → `./cassandra.conf` →
`<directory of the executable>/cassandra.conf`. An explicitly requested file that cannot be
read is a fatal error (exit 2); if no file is found the defaults above are used.

`cassandra.conf` — either one key per line:

```ini
# Connection settings for cassandra_function_tests
host     = 127.0.0.1
port     = 9042
protocol = v5
user     = appuser
password = appuser123
keyspace = demo
```

…or a single connection string:

```ini
connection = contact points=127.0.0.1; port=9042; protocol=v5; username=appuser; password=appuser123; keyspace=demo
```

Lines starting with `#` or `//` are comments. The cluster must run with
`PasswordAuthenticator`/`CassandraAuthorizer` for the credential tests to be meaningful.

## Typical output

```text
Cassandra C/C++ driver function tests
  driver            : 2.18.0
  protocol          : v5
  cassandra cluster : 5.0.9 ("simple-cluster")
  endpoint          : 127.0.0.1:9042
  user              : appuser
  keyspace          : demo
  config            : /home/user/cass-cpp-functiontest/cassandra.conf

  t01_connect_with_auth ... PASSED
  t02_cluster_metadata ... PASSED
  t03_simple_query ... PASSED
  t04_column_metadata ... PASSED
  t05_prepared_statement ... PASSED
  t06_bound_types ... PASSED
  t07_batch_statement ... PASSED
  t08_paging ... PASSED
  t09_collections ... PASSED
  t10_user_defined_type ... PASSED
  t11_tuple ... PASSED
  t12_counter ... PASSED
  t13_json ... PASSED
  t14_ttl_and_timestamp ... PASSED
  t15_async_future_and_callback ... PASSED
  t16_error_handling ... PASSED
  t17_uuid_generation ... PASSED
  t18_tracing ... PASSED
  t19_row_iteration ... PASSED
  t20_schema_metadata ... PASSED
  t21_v5_duration_type ... PASSED
Summary: 21 passed, 0 failed out of 21 total
```

`PASSED` is printed in green and `FAILED` in red when stdout is a terminal (disabled when
piped, with `--no-color`, or when `NO_COLOR` is set). On failure the assertion detail is
printed on the lines after the test name:

```text
  t20_schema_metadata ... FAILED
      FAILED: keyspace != NULL (main.cpp:1225)
  failed: t20_schema_metadata
Summary: 0 passed, 1 failed out of 21 total (20 skipped)
```

The last line always reports `Summary: <passed> passed, <failed> failed out of <total> total`,
where *total* is all tests in the suite (skipped tests are listed in parentheses when a filter
is used).

## Requirements

Linux x86-64 with glibc, libstdc++, libuv, openssl and zlib. The release tarball bundles
`libcassandra.so.2.18.0` (built from the `proto-v5` branch of Lyky35's fork), so no driver
install is needed.
