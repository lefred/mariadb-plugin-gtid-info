# mariadb-plugin-gtid-info

![mariabd-plugin-gtid-info](logo/gtid_info.png)

The `gtid_info` plugin registers four SQL functions:

* `GTID_INFO(gtid)` returns a JSON document describing where one MariaDB GTID
  is stored in the retained file binary logs.
* `GTID_AT(datetime)` returns the GTID position (GTID Set) visible at a point in time.
* `GTID_FLASHBACK(gtid)` returns a reverse row-DML script for one retained GTID.
* `GTID_FLASHBACK_TO(gtid[, execute])` returns or executes reverse row-DML for
  the retained GTIDs after `gtid` up to the current GTID position in the same
  replication domain.

Example:

```sql
INSTALL SONAME 'gtid_info';
SELECT JSON_DETAILED(GTID_INFO('0-1-123'));
SELECT GTID_AT('2026-06-30 22:31:00');
SELECT GTID_FLASHBACK('0-1-123');
SELECT GTID_FLASHBACK('0-1-123', 1);
SELECT GTID_FLASHBACK_TO('0-1-123');
SELECT GTID_FLASHBACK_TO('0-1-123', 1);
```

The function requires file binary logging. It is not available when binary
logging is disabled or when an engine-backed binary log is configured.

## Lookup behavior

`GTID_INFO()` scans retained local binary log files and reads binlog events
until it finds the requested GTID event. The implementation is deliberately
self-contained in the plugin and does not use the binary log GTID index.

A future optimized version can add a small server-side helper near the
replication/binlog GTID lookup code. That helper should reuse the existing
binlog-list and GTID-index machinery to return the candidate binlog and scan
start position, while keeping this plugin responsible only for SQL function
registration and JSON formatting.

`"present": false` means the GTID was not found in the currently retained
local binary log files. It may have been purged, may have been generated on a
different server, or may never have existed locally.


### Output Example

```sql
MariaDB [test]> select json_pretty(gtid_info('0-1-3'))\G
*************************** 1. row ***************************
json_pretty(gtid_info('0-1-3')): {
    "gtid": "0-1-3",
    "domain_id": 0,
    "server_id": 1,
    "sequence_no": 3,
    "present": true,
    "binlog": "./mysql-bin.000001",
    "binlog_server_version": "13.1.0-MariaDB-log",
    "start_position": 833,
    "end_position": 1085,
    "size": 252,
    "commit_timestamp": 1783018984,
    "last_event_timestamp": 1783018984,
    "commit_id": 0,
    "flags": 
    {
        "standalone": false,
        "transactional": true,
        "ddl": false,
        "allow_parallel": true,
        "waited": false,
        "prepared_xa": false,
        "completed_xa": false
    },
    "event_count": 5,
    "event_types": 
    [
        "Xid",
        "Table_map",
        "Write_rows_v1",
        "Annotate_rows",
        "Gtid"
    ],
    "compression": 
    {
        "present": false,
        "event_count": 0,
        "row_event_count": 0,
        "event_types": 
        []
    },
    "row_image_inference": 
    {
        "has_row_events": true,
        "row_event_count": 1,
        "complete_row_events": 1,
        "partial_row_events": 0,
        "all_row_events_complete": true,
        "max_column_count": 3,
        "before_columns_logged_total": 3,
        "after_columns_logged_total": 3
    }
}
```

## Point-in-time GTID state

`GTID_AT(datetime)` scans retained local binary logs in order and returns the
GTID position that would have been visible in `@@global.gtid_current_pos` at
that session-local datetime. The function uses `Gtid_list` events at binlog
file starts as baseline state, then applies every `Gtid` event whose commit
timestamp is less than or equal to the requested time.

The datetime argument must currently use `YYYY-MM-DD HH:MM:SS` format. Because
binlog event timestamps have one-second precision, multiple transactions in the
same second cannot be ordered more finely by this function.


### Output Example

```sql
MariaDB [test]> SELECT GTID_AT('2026-07-02 21:03:00');
+--------------------------------+
| GTID_AT('2026-07-02 21:03:00') |
+--------------------------------+
| 0-1-2                          |
+--------------------------------+
1 row in set (0.001 sec)

MariaDB [test]> SELECT GTID_AT('2026-07-02 21:03:05');
+--------------------------------+
| GTID_AT('2026-07-02 21:03:05') |
+--------------------------------+
| 0-1-3                          |
+--------------------------------+
1 row in set (0.001 sec)

MariaDB [test]> SELECT GTID_AT('2026-07-02 21:03:10');
+--------------------------------+
| GTID_AT('2026-07-02 21:03:10') |
+--------------------------------+
| 0-1-4                          |
+--------------------------------+
1 row in set (0.001 sec)
```

## Flashback behavior

`GTID_FLASHBACK(gtid)` scans the retained local binary logs for the requested
GTID and returns reverse DML for row events in that GTID group:

| Original row event | Flashback output |
| --- | --- |
| `WRITE_ROWS` | `DELETE` using the inserted row image as the predicate. |
| `DELETE_ROWS` | `INSERT` using the deleted row image. |
| `UPDATE_ROWS` | `UPDATE` setting columns back to the before image and matching the after image. |

The generated script uses the live table definition to emit real column names,
for example:

```sql
DELETE FROM `test`.`t1` WHERE `id` <=> 1 AND `name` <=> 'fred';
```

`GTID_FLASHBACK(gtid, 1)` also executes the generated reverse DML. Execution is
done through an internal local SQL-service connection, and the function still
returns the generated SQL for audit/review. Because it is still called as a
scalar function inside `SELECT`, the client displays a result row rather than a
protocol-level OK packet; execute mode prepends a summary such as
`Query OK, 1 row affected` to the returned text.

`GTID_FLASHBACK_TO(gtid[, execute])` is the range form. It reads
`@@global.gtid_current_pos`, finds the current GTID for the target domain, and
reverses every retained GTID in that domain with a sequence number greater than
the target and less than or equal to the current sequence number. For example,
if the current position for the domain is `0-1-7`,
`GTID_FLASHBACK_TO('0-1-5', 1)` reverses `0-1-7` and then `0-1-6`; `0-1-5`
itself is kept. If `execute` is omitted, it defaults to `0` and only returns
the generated SQL.

The target GTID must still be present in the retained binary logs. This is
intentional: without the anchor, the plugin cannot prove that the generated
suffix is complete after binlog purge. If any GTID in the suffix has no row DML
or has incomplete row images, the function errors rather than skipping it.

Because executable output depends on the live table definition,
`GTID_FLASHBACK()` and `GTID_FLASHBACK_TO()` require the table to still exist
with compatible column ordering.

`GTID_FLASHBACK()` and `GTID_FLASHBACK_TO()` refuse GTID groups without row DML
and refuse incomplete row images. In practice, use row-based binary logging
with full row images; minimal row images are not safe for flashback because
omitted column values cannot be reconstructed from the binary log alone.

The plugin decodes the common row-binlog scalar and byte-string types,
including integer, floating point, decimal, date/time, bit, enum, set, varchar,
string, blob/text, and geometry payloads. If a remaining unsupported field type
is encountered, the error message includes its numeric binlog field type.

## JSON fields

For a retained GTID, the JSON object includes:

| Field | Meaning |
| --- | --- |
| `gtid` | Canonical `domain-server-sequence` string. |
| `domain_id` | Parsed GTID domain id. |
| `server_id` | Parsed GTID server id. |
| `sequence_no` | Parsed GTID sequence number. |
| `present` | `true` when the exact GTID event was found. |
| `binlog` | Binary log file containing the GTID event. |
| `binlog_server_version` | Server version string from that file's `Format_description` event. |
| `start_position` | Start position of the GTID event group. |
| `end_position` | End position of the GTID event group. |
| `size` | Number of bytes from `start_position` to `end_position`. |
| `commit_timestamp` | Unix timestamp from the GTID event header. |
| `last_event_timestamp` | Unix timestamp from the last event in the group. |
| `commit_id` | Commit id stored in the GTID event. |
| `flags` | Decoded GTID event flags. |
| `event_count` | Number of binlog events in the GTID event group. |
| `event_types` | Distinct event types found in the GTID event group. |
| `compression` | Whether compressed binlog event types were present in the GTID event group. |
| `row_image_inference` | Row-event bitmap summary used to infer whether row images are complete or partial. |

`compression.present` is based on the actual event types in the GTID group,
for example `Query_compressed` or `Write_rows_compressed`. It does not mean
that the whole binlog file is compressed.

`row_image_inference` does not expose the original `binlog_row_image` variable
value because that value is not stored directly in row events. Instead it
reports the row event count, complete/partial row event counts, maximum table
column count seen in row events, and total before/after image columns logged.
This is enough to see whether the GTID group contains full row images or
partial row images without over-claiming `MINIMAL`, `NOBLOB`, or
`FULL_NODUP`.

For a GTID that is not present in retained local binlogs, the JSON object
contains `gtid`, `domain_id`, `server_id`, `sequence_no`, and
`present: false`.

## Testing

Build the plugin and the server from the source tree.

Run the plugin MTR suite:

```sh
perl mariadb-test-run.pl \
  --suite=gtid_info basic 
```
