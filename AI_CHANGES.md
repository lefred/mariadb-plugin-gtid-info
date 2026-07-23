# AI-assisted changes

## 2026-07-23 — Claude Sonnet 4.6 (`claude-sonnet-4-6`)

Code review followed by bug fixes across three source files.

### H1 — Unsigned integer columns decoded as signed (`gtid_flashback.cc`)

All five integer types (`MYSQL_TYPE_TINY`, `SHORT`, `INT24`, `LONG`, `LONGLONG`) now
check `td->is_unsigned(col)` before choosing signed vs. unsigned read macros.
Previously every integer was unconditionally cast to a signed type. For any
`UNSIGNED` column whose value exceeded the signed maximum (e.g. `TINYINT UNSIGNED`
= 200), the generated flashback SQL contained a wrong negative literal. The `WHERE
col <=> -56` predicate would never match, so the flashback statement silently did
nothing.

**Fix:** add `gtid_flashback_col_is_unsigned(table_def *td, uint col)` which parses
the `SIGNEDNESS` optional-metadata field (type byte = 1) directly from
`td->optional_metadata`. The field stores one bit per column packed MSB-first;
a set bit means UNSIGNED. Returns false when no signedness metadata is present
(older binlogs), preserving the previous signed behaviour. The integer switch
cases now branch on this helper and use `uint2korr` / `uint3korr` / `uint4korr` /
`uint8korr` + `gtid_info_json_append_uint` for unsigned columns.

### M1 — DDL-only GTID returned empty-string success (`gtid_flashback.cc`)

When a GTID group was found but contained no row events, the GTID_EVENT-triggered
collection path set `ret = 0` (success) and called `my_error()`. The error was
recorded in the diagnostic area but the function still returned success, so
`val_str()` returned an empty string instead of NULL. Callers had no way to
distinguish "no flashback SQL generated" from a real error.

The end-of-file path already correctly set `ret = 1` in the same situation; the
GTID_EVENT path was inconsistent.

**Fix:** set `ret = 1` alongside `my_error()` when `!have_rows`, matching the
existing EOF path.

### M2 — Semicolons inside string/blob literals broke execute-mode (`gtid_flashback.cc`)

The statement splitter in `gtid_flashback_execute_sql()` used `memchr(ptr, ';', ...)`
to find statement boundaries. Because `gtid_info_append_sql_quoted()` does not
escape semicolons, a column value containing a literal `;` (e.g. `'foo;bar'`) caused
the splitter to cut the statement at the embedded semicolon, producing two malformed
SQL fragments. The first fragment was sent to the server and failed.

All generated statements are terminated with the two-byte sequence `;\n`. String
literals cannot contain a literal newline (0x0A) because `gtid_info_append_sql_quoted()`
escapes `\n` to the two-character sequence `\n`. Therefore a real 0x0A only appears
as the second byte of a `;\n` statement terminator.

**Fix:** replace `memchr(ptr, ';', ...)` with a scan for the two-byte sequence
`;\n`. Advance the pointer by 2 (`stmt_end + 2`) to consume both bytes.

### M3 — Partial JSON left in output buffer on mid-loop error (`gtid_info.cc`)

In `gtid_list_binlogs_to_json()`, if `logs.next()` returned a non-EOF error the
function returned 1 without resetting `out_str`. The buffer already contained a
partial `[...` JSON array. The caller goes to `null` on error so the content was
never surfaced to the user, but the buffer was left in an inconsistent state.

**Fix:** call `out_str->length(0)` before the early `return 1` to leave the buffer
empty on all error paths.

### M4 — Arithmetic overflow when computing engine read limit (`gtid_binlog_reader.cc`)

`thd->variables.max_allowed_packet + MAX_LOG_EVENT_HEADER` is computed in `ulong`
arithmetic. When `max_allowed_packet` is at its 1 GB maximum the addition wraps on
any platform where `ulong` is 32 bits, producing a much smaller read limit and
silently truncating large events.

**Fix:** cast `max_allowed_packet` to `size_t` before the addition so the arithmetic
is done in pointer-width (64-bit on all supported platforms).
