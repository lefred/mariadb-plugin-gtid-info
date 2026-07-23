/* Copyright (c) 2026 MariaDB Corporation.
   Copyright (c) 2026 lefred.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql_version.h>
#include <mysql/plugin.h>
#include "sql_plugin.h"
#include "sql_class.h"
#include "item.h"
#include "item_strfunc.h"
#include "log.h"
#include "tztime.h"
#include "my_time.h"
#include "log_event.h"
#include "mysqld.h"
#include "rpl_constants.h"
#include "rpl_gtid.h"
#include "gtid_flashback.h"
#include "gtid_binlog_reader.h"


#ifdef HAVE_REPLICATION
struct Gtid_info_group_stats
{
  bool compressed_event_types[ENUM_END_EVENT];
  uint compressed_event_count;
  uint compressed_row_event_count;
  uint row_event_count;
  uint complete_row_event_count;
  uint partial_row_event_count;
  uint max_column_count;
  ulonglong before_columns_logged_total;
  ulonglong after_columns_logged_total;
};

struct Gtid_at_domain_state
{
  rpl_gtid gtid;
};

typedef std::vector<Gtid_at_domain_state> Gtid_at_domain_vec;

typedef std::vector<rpl_gtid> Gtid_info_gtid_vec;



static bool
gtid_info_json_append_quoted(String *str, const char *ptr, size_t len)
{
  if (str->append('"'))
    return true;
  for (size_t i= 0; i < len; ++i)
  {
    char ch= ptr[i];
    switch (ch) {
    case '"':
      if (str->append(STRING_WITH_LEN("\\\"")))
        return true;
      break;
    case '\\':
      if (str->append(STRING_WITH_LEN("\\\\")))
        return true;
      break;
    case '\n':
      if (str->append(STRING_WITH_LEN("\\n")))
        return true;
      break;
    case '\r':
      if (str->append(STRING_WITH_LEN("\\r")))
        return true;
      break;
    case '\t':
      if (str->append(STRING_WITH_LEN("\\t")))
        return true;
      break;
    default:
      if (static_cast<uchar>(ch) < 0x20)
      {
        char buf[7];
        my_snprintf(buf, sizeof(buf), "\\u%04x", static_cast<uint>(ch));
        if (str->append(buf, 6))
          return true;
      }
      else if (str->append(ch))
        return true;
    }
  }
  return str->append('"');
}


static bool
gtid_info_json_append_uint(String *str, ulonglong value)
{
  char buf[MY_INT64_NUM_DECIMAL_DIGITS + 1];
  size_t len= longlong10_to_str(value, buf, 10) - buf;
  return str->append(buf, len);
}


static bool
gtid_info_json_append_bool(String *str, bool value)
{
  return value ? str->append(STRING_WITH_LEN("true")) :
                 str->append(STRING_WITH_LEN("false"));
}


static bool
gtid_info_json_append_event_type_array(String *str, const bool *seen)
{
  bool first= true;
  if (str->append('['))
    return true;
  for (uint i= 0; i < ENUM_END_EVENT; ++i)
  {
    if (!seen[i])
      continue;
    if (!first && str->append(','))
      return true;
    first= false;
    const char *type_name=
      Log_event::get_type_str(static_cast<Log_event_type>(i));
    if (gtid_info_json_append_quoted(str, type_name, strlen(type_name)))
      return true;
  }
  return str->append(']');
}


static bool
gtid_info_json_append_key(String *str, const char *key, bool prepend_comma)
{
  return (prepend_comma && str->append(',')) ||
         gtid_info_json_append_quoted(str, key, strlen(key)) ||
         str->append(':');
}


static bool
gtid_info_json_append_object_key(String *str, const char *key,
                                 bool prepend_comma)
{
  return gtid_info_json_append_key(str, key, prepend_comma) ||
         str->append('{');
}


static bool
gtid_info_json_append_bool_member(String *str, const char *key,
                                  bool value, bool prepend_comma)
{
  return gtid_info_json_append_key(str, key, prepend_comma) ||
         gtid_info_json_append_bool(str, value);
}


static bool
gtid_info_json_append_string_member(String *str, const char *key,
                                    const char *value, size_t value_len,
                                    bool prepend_comma)
{
  return gtid_info_json_append_key(str, key, prepend_comma) ||
         gtid_info_json_append_quoted(str, value, value_len);
}


static bool
gtid_info_json_append_uint_member(String *str, const char *key,
                                  ulonglong value, bool prepend_comma)
{
  return gtid_info_json_append_key(str, key, prepend_comma) ||
         gtid_info_json_append_uint(str, value);
}


static bool
gtid_info_json_append_event_type_member(String *str, const char *key,
                                        const bool *seen, bool prepend_comma)
{
  return gtid_info_json_append_key(str, key, prepend_comma) ||
         gtid_info_json_append_event_type_array(str, seen);
}


static bool
gtid_info_json_append_gtid_value(String *str, const rpl_gtid *gtid)
{
  return str->append('"') ||
         gtid_info_json_append_uint(str, gtid->domain_id) ||
         str->append('-') ||
         gtid_info_json_append_uint(str, gtid->server_id) ||
         str->append('-') ||
         gtid_info_json_append_uint(str, gtid->seq_no) ||
         str->append('"');
}


static bool
gtid_info_json_append_gtid_identity(String *str, const rpl_gtid *gtid)
{
  return gtid_info_json_append_key(str, "gtid", false) ||
         gtid_info_json_append_gtid_value(str, gtid) ||
         gtid_info_json_append_uint_member(str, "domain_id",
                                           gtid->domain_id, true) ||
         gtid_info_json_append_uint_member(str, "server_id",
                                           gtid->server_id, true) ||
         gtid_info_json_append_uint_member(str, "sequence_no",
                                           gtid->seq_no, true);
}


static int
gtid_at_parse_datetime(THD *thd, const char *ptr, size_t len,
                       my_time_t *out_time)
{
  MYSQL_TIME ltime;
  MYSQL_TIME_STATUS status;
  uint error_code= 0;

  bzero(&ltime, sizeof(ltime));
  my_time_status_init(&status);
  if (str_to_datetime_or_date(ptr, len, &ltime,
                              C_TIME_NO_ZERO_IN_DATE | C_TIME_NO_ZERO_DATE,
                              &status) ||
      ltime.time_type != MYSQL_TIMESTAMP_DATETIME ||
      ltime.second_part != 0)
  {
    my_error(ER_WRONG_VALUE, MYF(0), "datetime", ptr);
    return 1;
  }

  *out_time= thd->variables.time_zone->TIME_to_gmt_sec(&ltime, &error_code);
  if (error_code)
  {
    my_error(ER_WRONG_VALUE, MYF(0), "datetime", ptr);
    return 1;
  }
  return 0;
}


static void
gtid_at_update_state(Gtid_at_domain_vec *state, const rpl_gtid *gtid)
{
  for (size_t i= 0; i < state->size(); ++i)
  {
    if ((*state)[i].gtid.domain_id == gtid->domain_id)
    {
      (*state)[i].gtid= *gtid;
      return;
    }
  }

  Gtid_at_domain_state elem;
  elem.gtid= *gtid;
  state->push_back(elem);
}


static bool
gtid_at_append_position(String *out_str, Gtid_at_domain_vec *state)
{
  bool first= true;
  bool have_last= false;
  uint32 last_domain= 0;

  out_str->length(0);
  for (;;)
  {
    size_t best= state->size();
    uint32 best_domain= 0;
    for (size_t i= 0; i < state->size(); ++i)
    {
      uint32 domain= (*state)[i].gtid.domain_id;
      if ((!have_last || domain > last_domain) &&
          (best == state->size() || domain < best_domain))
      {
        best= i;
        best_domain= domain;
      }
    }
    if (best == state->size())
      break;

    if (!first && out_str->append(','))
      return true;
    first= false;
    if (gtid_info_json_append_uint(out_str, (*state)[best].gtid.domain_id) ||
        out_str->append('-') ||
        gtid_info_json_append_uint(out_str, (*state)[best].gtid.server_id) ||
        out_str->append('-') ||
        gtid_info_json_append_uint(out_str, (*state)[best].gtid.seq_no))
      return true;
    have_last= true;
    last_domain= (*state)[best].gtid.domain_id;
  }
  return false;
}


static bool
gtid_info_same_gtid(const rpl_gtid *a, const rpl_gtid *b)
{
  return a->domain_id == b->domain_id &&
         a->server_id == b->server_id &&
         a->seq_no == b->seq_no;
}


static bool
gtid_info_append_gtid_set(String *out_str, const Gtid_info_gtid_vec *gtids)
{
  out_str->length(0);
  for (size_t i= 0; i < gtids->size(); ++i)
  {
    if ((i && out_str->append(',')) ||
        gtid_info_json_append_uint(out_str, (*gtids)[i].domain_id) ||
        out_str->append('-') ||
        gtid_info_json_append_uint(out_str, (*gtids)[i].server_id) ||
        out_str->append('-') ||
        gtid_info_json_append_uint(out_str, (*gtids)[i].seq_no))
      return true;
  }
  return false;
}


static bool
gtid_info_gtid_less(const rpl_gtid *a, const rpl_gtid *b)
{
  if (a->domain_id != b->domain_id)
    return a->domain_id < b->domain_id;
  if (a->server_id != b->server_id)
    return a->server_id < b->server_id;
  return a->seq_no < b->seq_no;
}


static void
gtid_info_sort_gtids(Gtid_info_gtid_vec *gtids)
{
  /* The per-file vectors are small; insertion sort avoids extra dependencies. */
  for (size_t i= 1; i < gtids->size(); ++i)
  {
    rpl_gtid value= (*gtids)[i];
    size_t j= i;
    while (j && gtid_info_gtid_less(&value, &(*gtids)[j - 1]))
    {
      (*gtids)[j]= (*gtids)[j - 1];
      --j;
    }
    (*gtids)[j]= value;
  }
}


static bool
gtid_info_append_plain_gtid(String *out_str, const rpl_gtid *gtid)
{
  return gtid_info_json_append_uint(out_str, gtid->domain_id) ||
         out_str->append('-') ||
         gtid_info_json_append_uint(out_str, gtid->server_id) ||
         out_str->append('-') ||
         gtid_info_json_append_uint(out_str, gtid->seq_no);
}


static bool
gtid_info_append_gtid_gaps(String *out_str, Gtid_info_gtid_vec *gtids)
{
  bool first= true;
  gtid_info_sort_gtids(gtids);
  out_str->length(0);

  for (size_t i= 1; i < gtids->size(); ++i)
  {
    const rpl_gtid *previous= &(*gtids)[i - 1];
    const rpl_gtid *current= &(*gtids)[i];
    if (previous->domain_id != current->domain_id ||
        previous->server_id != current->server_id ||
        previous->seq_no >= current->seq_no ||
        current->seq_no - previous->seq_no <= 1)
      continue;

    for (uint64 seq= previous->seq_no + 1; seq < current->seq_no; ++seq)
    {
      rpl_gtid gap= *previous;
      gap.seq_no= seq;
      if ((!first && out_str->append(',')) ||
          gtid_info_append_plain_gtid(out_str, &gap))
        return true;
      first= false;
    }
  }
  return false;
}


static bool
gtid_info_append_gtid_ranges(String *out_str, Gtid_info_gtid_vec *gtids)
{
  bool first= true;
  gtid_info_sort_gtids(gtids);
  out_str->length(0);

  for (size_t start= 0; start < gtids->size();)
  {
    size_t end= start;
    while (end + 1 < gtids->size() &&
           (*gtids)[end].domain_id == (*gtids)[end + 1].domain_id &&
           (*gtids)[end].server_id == (*gtids)[end + 1].server_id &&
           ((*gtids)[end].seq_no == (*gtids)[end + 1].seq_no ||
            ((*gtids)[end].seq_no != ULONGLONG_MAX &&
             (*gtids)[end].seq_no + 1 == (*gtids)[end + 1].seq_no)))
      ++end;

    /* Ignore duplicate GTIDs at the end when deciding whether this is a range. */
    if ((!first && out_str->append(',')) ||
        gtid_info_append_plain_gtid(out_str, &(*gtids)[start]))
      return true;
    if ((*gtids)[end].seq_no != (*gtids)[start].seq_no &&
        (out_str->append(':') ||
         gtid_info_append_plain_gtid(out_str, &(*gtids)[end])))
      return true;
    first= false;
    start= end + 1;
  }
  return false;
}


static const char *
gtid_info_basename(const char *name)
{
  const char *base= strrchr(name, FN_LIBCHAR);
  return base ? base + 1 : name;
}


static bool
gtid_info_log_name_matches(const char *indexed_name,
                           const char *wanted, size_t wanted_len)
{
  size_t indexed_len= strlen(indexed_name);
  if (indexed_len == wanted_len && !memcmp(indexed_name, wanted, wanted_len))
    return true;

  const char *base= gtid_info_basename(indexed_name);
  return strlen(base) == wanted_len && !memcmp(base, wanted, wanted_len);
}


static int
gtid_info_collect_file_gtids(const char *log_name, Gtid_info_gtid_vec *gtids,
                             const Gtid_info_gtid_vec *wanted,
                             bool *has_wanted)
{
  Gtid_binlog_reader reader;
  int read_error;
  Log_event *ev= NULL;
  Log_event *fde_ev= NULL;
  Format_description_log_event init_fdle(BINLOG_VERSION);
  Format_description_log_event *fdle= &init_fdle;
  int ret= 0;

  if (has_wanted)
    *has_wanted= false;
  if (reader.open(current_thd, log_name))
    return 1;

  while ((ev= reader.read(current_thd, fdle, &read_error)))
  {
    Log_event_type typ= ev->get_type_code();
    if (typ == FORMAT_DESCRIPTION_EVENT)
    {
      delete fde_ev;
      fde_ev= ev;
      fdle= static_cast<Format_description_log_event *>(ev);
      ev= NULL;
      continue;
    }
    if (typ == START_ENCRYPTION_EVENT &&
        fdle->start_decryption(static_cast<Start_encryption_log_event *>(ev)))
    {
      ret= 1;
      break;
    }
    if (typ == GTID_EVENT)
    {
      Gtid_log_event *gev= static_cast<Gtid_log_event *>(ev);
      rpl_gtid gtid;
      gtid.domain_id= gev->domain_id;
      gtid.server_id= gev->server_id;
      gtid.seq_no= gev->seq_no;

      if (gtids)
        gtids->push_back(gtid);
      if (wanted && has_wanted)
      {
        for (size_t i= 0; i < wanted->size(); ++i)
        {
          if (gtid_info_same_gtid(&gtid, &(*wanted)[i]))
          {
            *has_wanted= true;
            break;
          }
        }
      }
    }
    delete ev;
    ev= NULL;
  }

  delete ev;
  delete fde_ev;
  return ret;
}


static int
binlog_gtids_for_file(const char *file_name, size_t file_name_len,
                      Gtid_info_gtid_vec *gtids)
{
  Gtid_binlog_file_iterator logs(current_thd);

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }
  if (!logs.valid())
    goto not_found;
  for (;;)
  {
    if (gtid_info_log_name_matches(logs.name(), file_name,
                                   file_name_len))
    {
      if (gtid_info_collect_file_gtids(logs.name(), gtids, NULL, NULL))
        return 1;
      return 0;
    }
    int error= logs.next();
    if (error == LOG_INFO_EOF)
      break;
    if (error)
      return 1;
  }

not_found:
  my_error(ER_KEY_NOT_FOUND, MYF(0), "binary log");
  return 1;
}


static int
binlog_gtid_set_to_string(const char *file_name, size_t file_name_len,
                          String *out_str)
{
  Gtid_info_gtid_vec gtids;
  if (binlog_gtids_for_file(file_name, file_name_len, &gtids))
    return 1;
  return gtid_info_append_gtid_set(out_str, &gtids);
}


static int
binlog_gtid_gaps_to_string(const char *file_name, size_t file_name_len,
                           String *out_str)
{
  Gtid_info_gtid_vec gtids;
  if (binlog_gtids_for_file(file_name, file_name_len, &gtids))
    return 1;
  return gtid_info_append_gtid_gaps(out_str, &gtids);
}


static int
binlog_gtid_ranges_to_string(const char *file_name, size_t file_name_len,
                             String *out_str)
{
  Gtid_info_gtid_vec gtids;
  if (binlog_gtids_for_file(file_name, file_name_len, &gtids))
    return 1;
  return gtid_info_append_gtid_ranges(out_str, &gtids);
}


static int
gtid_set_binlogs_to_json(const char *gtid_str, size_t gtid_len,
                         String *out_str)
{
  Gtid_binlog_file_iterator logs(current_thd);
  rpl_gtid *parsed;
  uint32 count;
  Gtid_info_gtid_vec wanted;
  bool first= true;

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }
  if (!(parsed= gtid_parse_string_to_list(gtid_str, gtid_len, &count)))
  {
    my_error(ER_INCORRECT_GTID_STATE, MYF(0));
    return 1;
  }
  for (uint32 i= 0; i < count; ++i)
    wanted.push_back(parsed[i]);
  my_free(parsed);

  out_str->length(0);
  if (out_str->append('['))
    return 1;
  if (!logs.valid() && !logs.eof())
    return 1;
  while (logs.valid())
  {
    bool matches;
    if (gtid_info_collect_file_gtids(logs.name(), NULL, &wanted, &matches))
      return 1;
    if (matches)
    {
      if ((!first && out_str->append(',')) ||
          gtid_info_json_append_quoted(out_str, logs.name(),
                                       strlen(logs.name())))
        return 1;
      first= false;
    }
    int error= logs.next();
    if (error != 0 && error != LOG_INFO_EOF)
      return 1;
  }
  return out_str->append(']');
}


static bool
gtid_info_json_append_group_stats(String *str,
                                  const Gtid_info_group_stats *stats)
{
  bool all_row_events_complete= stats->row_event_count > 0 &&
                                stats->row_event_count ==
                                  stats->complete_row_event_count;

  if (gtid_info_json_append_object_key(str, "compression", true) ||
      gtid_info_json_append_bool_member(str, "present",
                                        stats->compressed_event_count > 0,
                                        false) ||
      gtid_info_json_append_uint_member(str, "event_count",
                                        stats->compressed_event_count, true) ||
      gtid_info_json_append_uint_member(str, "row_event_count",
                                        stats->compressed_row_event_count,
                                        true) ||
      gtid_info_json_append_event_type_member(str, "event_types",
                                              stats->compressed_event_types,
                                              true) ||
      str->append('}'))
    return true;

  return gtid_info_json_append_object_key(str, "row_image_inference", true) ||
         gtid_info_json_append_bool_member(str, "has_row_events",
                                           stats->row_event_count > 0, false) ||
         gtid_info_json_append_uint_member(str, "row_event_count",
                                           stats->row_event_count, true) ||
         gtid_info_json_append_uint_member(str, "complete_row_events",
                                           stats->complete_row_event_count,
                                           true) ||
         gtid_info_json_append_uint_member(str, "partial_row_events",
                                           stats->partial_row_event_count,
                                           true) ||
         gtid_info_json_append_bool_member(str, "all_row_events_complete",
                                           all_row_events_complete, true) ||
         gtid_info_json_append_uint_member(str, "max_column_count",
                                           stats->max_column_count, true) ||
         gtid_info_json_append_uint_member(str, "before_columns_logged_total",
                                           stats->before_columns_logged_total,
                                           true) ||
         gtid_info_json_append_uint_member(str, "after_columns_logged_total",
                                           stats->after_columns_logged_total,
                                           true) ||
         str->append('}');
}


static void
gtid_info_note_event_stats(Log_event *ev, Gtid_info_group_stats *stats)
{
  Log_event_type typ= ev->get_type_code();

  if (typ == QUERY_COMPRESSED_EVENT || LOG_EVENT_IS_ROW_COMPRESSED(typ))
  {
    if (typ > UNKNOWN_EVENT && typ < ENUM_END_EVENT)
      stats->compressed_event_types[typ]= true;
    ++stats->compressed_event_count;
    if (LOG_EVENT_IS_ROW_COMPRESSED(typ))
      ++stats->compressed_row_event_count;
  }

  if (LOG_EVENT_IS_WRITE_ROW(typ) || LOG_EVENT_IS_UPDATE_ROW(typ) ||
      LOG_EVENT_IS_DELETE_ROW(typ))
  {
    Rows_log_event *rev= static_cast<Rows_log_event *>(ev);
    uint before_cols= bitmap_bits_set(rev->get_cols());
    uint after_cols= 0;
    bool complete= false;

    ++stats->row_event_count;
    set_if_bigger(stats->max_column_count, rev->get_width());

    if (LOG_EVENT_IS_WRITE_ROW(typ))
    {
      after_cols= before_cols;
      complete= bitmap_is_set_all(rev->get_cols()) ||
                rev->get_flags(Rows_log_event::COMPLETE_ROWS_F);
    }
    else if (LOG_EVENT_IS_UPDATE_ROW(typ))
    {
      after_cols= bitmap_bits_set(rev->get_cols_ai());
      complete= (bitmap_is_set_all(rev->get_cols()) &&
                 bitmap_is_set_all(rev->get_cols_ai())) ||
                rev->get_flags(Rows_log_event::COMPLETE_ROWS_F);
    }
    else
    {
      complete= bitmap_is_set_all(rev->get_cols()) ||
                rev->get_flags(Rows_log_event::COMPLETE_ROWS_F);
    }

    stats->before_columns_logged_total+= before_cols;
    stats->after_columns_logged_total+= after_cols;
    if (complete)
      ++stats->complete_row_event_count;
    else
      ++stats->partial_row_event_count;
  }
}


static bool
gtid_info_json_not_present(String *out_str, const rpl_gtid *gtid)
{
  out_str->length(0);
  return out_str->append('{') ||
         gtid_info_json_append_gtid_identity(out_str, gtid) ||
         gtid_info_json_append_bool_member(out_str, "present", false, true) ||
         out_str->append('}');
}


static bool
gtid_info_json_found(String *out_str, const rpl_gtid *gtid,
                     const char *log_name, const char *server_version,
                     my_off_t start_pos, my_off_t end_pos,
                     my_time_t commit_timestamp, my_time_t last_event_timestamp,
                     uint64 commit_id, uchar flags2, bool *seen,
                     uint event_count, const Gtid_info_group_stats *stats)
{
  out_str->length(0);
  if (out_str->append('{') ||
      gtid_info_json_append_gtid_identity(out_str, gtid) ||
      gtid_info_json_append_bool_member(out_str, "present", true, true) ||
      gtid_info_json_append_string_member(out_str, "binlog", log_name,
                                          strlen(log_name), true) ||
      gtid_info_json_append_string_member(out_str, "binlog_server_version",
                                          server_version,
                                          strlen(server_version), true) ||
      gtid_info_json_append_uint_member(out_str, "start_position",
                                        start_pos, true) ||
      gtid_info_json_append_uint_member(out_str, "end_position",
                                        end_pos, true) ||
      gtid_info_json_append_uint_member(out_str, "size", end_pos - start_pos,
                                        true) ||
      gtid_info_json_append_uint_member(out_str, "commit_timestamp",
                                        commit_timestamp, true) ||
      gtid_info_json_append_uint_member(out_str, "last_event_timestamp",
                                        last_event_timestamp, true) ||
      gtid_info_json_append_uint_member(out_str, "commit_id", commit_id,
                                        true) ||
      gtid_info_json_append_object_key(out_str, "flags", true) ||
      gtid_info_json_append_bool_member(out_str, "standalone",
                                        flags2 & Gtid_log_event::FL_STANDALONE,
                                        false) ||
      gtid_info_json_append_bool_member(out_str, "transactional",
                                        flags2 &
                                          Gtid_log_event::FL_TRANSACTIONAL,
                                        true) ||
      gtid_info_json_append_bool_member(out_str, "ddl",
                                        flags2 & Gtid_log_event::FL_DDL,
                                        true) ||
      gtid_info_json_append_bool_member(out_str, "allow_parallel",
                                        flags2 &
                                          Gtid_log_event::FL_ALLOW_PARALLEL,
                                        true) ||
      gtid_info_json_append_bool_member(out_str, "waited",
                                        flags2 & Gtid_log_event::FL_WAITED,
                                        true) ||
      gtid_info_json_append_bool_member(out_str, "prepared_xa",
                                        flags2 &
                                          Gtid_log_event::FL_PREPARED_XA,
                                        true) ||
      gtid_info_json_append_bool_member(out_str, "completed_xa",
                                        flags2 &
                                          Gtid_log_event::FL_COMPLETED_XA,
                                        true) ||
      out_str->append('}') ||
      gtid_info_json_append_uint_member(out_str, "event_count", event_count,
                                        true) ||
      gtid_info_json_append_event_type_member(out_str, "event_types", seen,
                                              true) ||
      gtid_info_json_append_group_stats(out_str, stats) ||
      out_str->append('}'))
    return true;
  return false;
}


static int
gtid_info_scan_file(const char *log_name, const rpl_gtid *wanted,
                    String *out_str, bool *found)
{
  Gtid_binlog_reader reader;
  int read_error;
  Log_event *ev= NULL;
  Log_event *fde_ev= NULL;
  Format_description_log_event init_fdle(BINLOG_VERSION);
  Format_description_log_event *fdle= &init_fdle;
  bool collecting= false;
  bool seen[ENUM_END_EVENT];
  my_off_t start_pos= 0, end_pos= 0;
  my_time_t commit_timestamp= 0;
  my_time_t last_event_timestamp= 0;
  uint64 commit_id= 0;
  uint event_count= 0;
  uchar flags2= 0;
  char server_version[ST_SERVER_VER_LEN];
  Gtid_info_group_stats stats;
  int ret= 0;

  *found= false;
  bzero(seen, sizeof(seen));
  bzero(&stats, sizeof(stats));
  server_version[0]= 0;
  if (reader.open(current_thd, log_name))
    return 1;

  while ((ev= reader.read(current_thd, fdle, &read_error)))
  {
    Log_event_type typ= ev->get_type_code();

    if (typ == FORMAT_DESCRIPTION_EVENT)
    {
      if (fde_ev)
        delete fde_ev;
      fde_ev= ev;
      fdle= static_cast<Format_description_log_event *>(ev);
      strmake(server_version, fdle->server_version, sizeof(server_version) - 1);
      ev= NULL;
      continue;
    }

    if (typ == START_ENCRYPTION_EVENT &&
        fdle->start_decryption(static_cast<Start_encryption_log_event *>(ev)))
    {
      ret= 1;
      break;
    }

    if (typ == ROTATE_EVENT || typ == STOP_EVENT)
    {
      if (collecting)
      {
        if (gtid_info_json_found(out_str, wanted, log_name, server_version,
                                 start_pos, end_pos, commit_timestamp,
                                 last_event_timestamp, commit_id, flags2, seen,
                                 event_count, &stats))
          ret= 1;
        *found= true;
      }
      delete ev;
      ev= NULL;
      break;
    }

    if (typ == GTID_EVENT)
    {
      Gtid_log_event *gev= static_cast<Gtid_log_event *>(ev);
      if (collecting)
      {
        if (gtid_info_json_found(out_str, wanted, log_name, server_version,
                                 start_pos, end_pos, commit_timestamp,
                                 last_event_timestamp, commit_id, flags2, seen,
                                 event_count, &stats))
          ret= 1;
        *found= true;
        break;
      }
      if (gev->domain_id == wanted->domain_id &&
          gev->server_id == wanted->server_id &&
          gev->seq_no == wanted->seq_no)
      {
        collecting= true;
        bzero(seen, sizeof(seen));
        bzero(&stats, sizeof(stats));
        seen[typ]= true;
        gtid_info_note_event_stats(ev, &stats);
        end_pos= reader.position();
        start_pos= end_pos - ev->data_written;
        commit_timestamp= ev->when;
        last_event_timestamp= ev->when;
        commit_id= gev->commit_id;
        event_count= 1;
        flags2= gev->flags2;
      }
    }
    else if (collecting)
    {
      if (typ > UNKNOWN_EVENT && typ < ENUM_END_EVENT)
        seen[typ]= true;
      gtid_info_note_event_stats(ev, &stats);
      end_pos= reader.position();
      last_event_timestamp= ev->when;
      ++event_count;
    }

    delete ev;
    ev= NULL;
  }

  if (!ret && collecting && !*found)
  {
    if (gtid_info_json_found(out_str, wanted, log_name, server_version,
                             start_pos, end_pos, commit_timestamp,
                             last_event_timestamp, commit_id, flags2, seen,
                             event_count, &stats))
      ret= 1;
    *found= true;
  }

  delete ev;
  delete fde_ev;
  return ret;
}


static int
gtid_info_full_scan(const rpl_gtid *wanted, String *out_str)
{
  Gtid_binlog_file_iterator logs(current_thd);
  int ret;
  bool found= false;

  if (!logs.valid())
  {
    if (logs.eof())
      return gtid_info_json_not_present(out_str, wanted);
    return 1;
  }

  for (;;)
  {
    if ((ret= gtid_info_scan_file(logs.name(), wanted, out_str, &found)) ||
        found)
      return ret;
    int error= logs.next();
    if (error == LOG_INFO_EOF)
      break;
    if (error)
      return 1;
  }

  return gtid_info_json_not_present(out_str, wanted);
}


static int
gtid_info_to_json(const char *gtid_str, size_t gtid_len, String *out_str)
{
  rpl_gtid *gtid_list;
  uint32 gtid_count;
  int ret;

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }

  if (!(gtid_list= gtid_parse_string_to_list(gtid_str, gtid_len, &gtid_count)))
  {
    my_error(ER_INCORRECT_GTID_STATE, MYF(0));
    return 1;
  }
  if (gtid_count != 1)
  {
    my_free(gtid_list);
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "GTID_INFO");
    return 1;
  }

  /*
    This plugin intentionally does a retained-binlog full scan to stay
    self-contained. A future optimized version should add a server helper near
    sql/sql_repl.cc's GTID lookup code, for example:

      int gtid_binlog_event_info(const rpl_gtid *gtid,
                                 Gtid_binlog_event_info *out);

    That helper should reuse get_binlog_list(), gtid_check_binlog_file(), and
    Gtid_index_reader_hot to return the candidate binlog and index-derived
    scan position, then perform the exact event scan from that position. The
    plugin should only format the returned metadata as JSON.
  */
  ret= gtid_info_full_scan(gtid_list, out_str);
  my_free(gtid_list);
  return ret;
}


static int
gtid_at_scan_file(const char *log_name, my_time_t target_time,
                  Gtid_at_domain_vec *state, bool *past_target)
{
  Gtid_binlog_reader reader;
  int read_error;
  Log_event *ev= NULL;
  Log_event *fde_ev= NULL;
  Format_description_log_event init_fdle(BINLOG_VERSION);
  Format_description_log_event *fdle= &init_fdle;
  int ret= 0;

  *past_target= false;
  if (reader.open(current_thd, log_name))
    return 1;

  while ((ev= reader.read(current_thd, fdle, &read_error)))
  {
    Log_event_type typ= ev->get_type_code();

    if (typ == FORMAT_DESCRIPTION_EVENT)
    {
      if (fde_ev)
        delete fde_ev;
      fde_ev= ev;
      fdle= static_cast<Format_description_log_event *>(ev);
      ev= NULL;
      continue;
    }

    if (typ == START_ENCRYPTION_EVENT &&
        fdle->start_decryption(static_cast<Start_encryption_log_event *>(ev)))
    {
      ret= 1;
      break;
    }

    if (typ == GTID_LIST_EVENT)
    {
      Gtid_list_log_event *glev= static_cast<Gtid_list_log_event *>(ev);
      if (glev->is_valid())
      {
        for (uint32 i= 0; i < glev->count; ++i)
          gtid_at_update_state(state, &glev->list[i]);
      }
    }
    else if (typ == GTID_EVENT)
    {
      Gtid_log_event *gev= static_cast<Gtid_log_event *>(ev);
      if (ev->when > target_time)
      {
        *past_target= true;
        delete ev;
        ev= NULL;
        break;
      }

      rpl_gtid gtid;
      gtid.domain_id= gev->domain_id;
      gtid.server_id= gev->server_id;
      gtid.seq_no= gev->seq_no;
      gtid_at_update_state(state, &gtid);
    }

    delete ev;
    ev= NULL;
  }

  delete ev;
  delete fde_ev;
  return ret;
}


static int
gtid_at_to_string(THD *thd, const char *datetime_str, size_t datetime_len,
                  String *out_str)
{
  Gtid_binlog_file_iterator logs(thd);
  Gtid_at_domain_vec state;
  my_time_t target_time;

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }

  if (gtid_at_parse_datetime(thd, datetime_str, datetime_len, &target_time))
    return 1;

  if (!logs.valid())
  {
    if (logs.eof())
    {
      out_str->length(0);
      return 0;
    }
    return 1;
  }

  for (;;)
  {
    bool past_target;
    if (gtid_at_scan_file(logs.name(), target_time, &state, &past_target))
      return 1;
    if (past_target)
      break;
    int error= logs.next();
    if (error == LOG_INFO_EOF)
      break;
    if (error)
      return 1;
  }

  return gtid_at_append_position(out_str, &state);
}
#endif


class Item_func_gtid_info final : public Item_str_func
{
public:
  Item_func_gtid_info(THD *thd, Item *arg)
    : Item_str_func(thd, arg)
  { }

  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info_for_i_s);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return false;
  }

  String *val_str(String *str) override
  {
    String gtid_buf;
    String *gtid;

    DBUG_ASSERT(fixed());
    gtid= args[0]->val_str(&gtid_buf);
    if (args[0]->null_value)
      goto null;

#ifdef HAVE_REPLICATION
    if (gtid_info_to_json(gtid->ptr(), gtid->length(), str))
      goto null;
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
#else
    str->copy("{\"present\":false}", 17, system_charset_info_for_i_s);
    null_value= false;
    return str;
#endif

null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("gtid_info") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_gtid_info>(thd, this);
  }
};


class Create_func_gtid_info final : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
#ifdef HAVE_REPLICATION
    if (unlikely(!mysql_bin_log.is_open()))
    {
      my_error(ER_NO_BINARY_LOGGING, MYF(0));
      return NULL;
    }
#endif
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_gtid_info(thd, arg1);
  }

  static Create_func_gtid_info s_singleton;
};

Create_func_gtid_info Create_func_gtid_info::s_singleton;


class Item_func_gtid_at final : public Item_str_func
{
public:
  Item_func_gtid_at(THD *thd, Item *arg)
    : Item_str_func(thd, arg)
  { }

  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info_for_i_s);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return false;
  }

  String *val_str(String *str) override
  {
    String datetime_buf;
    String *datetime;

    DBUG_ASSERT(fixed());
    datetime= args[0]->val_str(&datetime_buf);
    if (args[0]->null_value)
      goto null;

#ifdef HAVE_REPLICATION
    if (gtid_at_to_string(current_thd, datetime->ptr(), datetime->length(),
                          str))
      goto null;
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
#else
    str->length(0);
    null_value= false;
    return str;
#endif

null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("gtid_at") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_gtid_at>(thd, this);
  }
};


class Create_func_gtid_at final : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
#ifdef HAVE_REPLICATION
    if (unlikely(!mysql_bin_log.is_open()))
    {
      my_error(ER_NO_BINARY_LOGGING, MYF(0));
      return NULL;
    }
#endif
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_gtid_at(thd, arg1);
  }

  static Create_func_gtid_at s_singleton;
};

Create_func_gtid_at Create_func_gtid_at::s_singleton;


class Item_func_binlog_gtid_set final : public Item_str_func
{
public:
  Item_func_binlog_gtid_set(THD *thd, Item *arg) : Item_str_func(thd, arg) { }

  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info_for_i_s);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return false;
  }

  String *val_str(String *str) override
  {
    String name_buf;
    String *name;
    DBUG_ASSERT(fixed());
    name= args[0]->val_str(&name_buf);
    if (args[0]->null_value)
      goto null;
#ifdef HAVE_REPLICATION
    if (binlog_gtid_set_to_string(name->ptr(), name->length(), str))
      goto null;
#else
    str->length(0);
#endif
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("binlog_gtid_set") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_binlog_gtid_set>(thd, this); }
};


class Create_func_binlog_gtid_set final : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_binlog_gtid_set(thd, arg1);
  }
  static Create_func_binlog_gtid_set s_singleton;
};

Create_func_binlog_gtid_set Create_func_binlog_gtid_set::s_singleton;


class Item_func_binlog_gtid_set_gaps final : public Item_str_func
{
public:
  Item_func_binlog_gtid_set_gaps(THD *thd, Item *arg)
    : Item_str_func(thd, arg) { }

  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info_for_i_s);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return false;
  }

  String *val_str(String *str) override
  {
    String name_buf;
    String *name;
    DBUG_ASSERT(fixed());
    name= args[0]->val_str(&name_buf);
    if (args[0]->null_value)
      goto null;
#ifdef HAVE_REPLICATION
    if (binlog_gtid_gaps_to_string(name->ptr(), name->length(), str))
      goto null;
#else
    str->length(0);
#endif
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("binlog_gtid_set_gaps") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_binlog_gtid_set_gaps>(thd, this); }
};


class Create_func_binlog_gtid_set_gaps final : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_binlog_gtid_set_gaps(thd, arg1);
  }
  static Create_func_binlog_gtid_set_gaps s_singleton;
};

Create_func_binlog_gtid_set_gaps
  Create_func_binlog_gtid_set_gaps::s_singleton;


class Item_func_binlog_gtid_set_ranges final : public Item_str_func
{
public:
  Item_func_binlog_gtid_set_ranges(THD *thd, Item *arg)
    : Item_str_func(thd, arg) { }

  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info_for_i_s);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return false;
  }

  String *val_str(String *str) override
  {
    String name_buf;
    String *name;
    DBUG_ASSERT(fixed());
    name= args[0]->val_str(&name_buf);
    if (args[0]->null_value)
      goto null;
#ifdef HAVE_REPLICATION
    if (binlog_gtid_ranges_to_string(name->ptr(), name->length(), str))
      goto null;
#else
    str->length(0);
#endif
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("binlog_gtid_set_ranges") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_binlog_gtid_set_ranges>(thd, this); }
};


class Create_func_binlog_gtid_set_ranges final : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_binlog_gtid_set_ranges(thd, arg1);
  }
  static Create_func_binlog_gtid_set_ranges s_singleton;
};

Create_func_binlog_gtid_set_ranges
  Create_func_binlog_gtid_set_ranges::s_singleton;


class Item_func_gtid_set_binlogs final : public Item_str_func
{
public:
  Item_func_gtid_set_binlogs(THD *thd, Item *arg) : Item_str_func(thd, arg) { }

  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(system_charset_info_for_i_s);
    max_length= MAX_BLOB_WIDTH;
    set_maybe_null();
    return false;
  }

  String *val_str(String *str) override
  {
    String gtid_buf;
    String *gtid;
    DBUG_ASSERT(fixed());
    gtid= args[0]->val_str(&gtid_buf);
    if (args[0]->null_value)
      goto null;
#ifdef HAVE_REPLICATION
    if (gtid_set_binlogs_to_json(gtid->ptr(), gtid->length(), str))
      goto null;
#else
    str->copy(STRING_WITH_LEN("[]"), system_charset_info_for_i_s);
#endif
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("gtid_set_binlogs") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_gtid_set_binlogs>(thd, this); }
};


class Create_func_gtid_set_binlogs final : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_gtid_set_binlogs(thd, arg1);
  }
  static Create_func_gtid_set_binlogs s_singleton;
};

Create_func_gtid_set_binlogs Create_func_gtid_set_binlogs::s_singleton;


static const Native_func_registry gtid_info_func_array[] =
{
  { { STRING_WITH_LEN("GTID_INFO") }, &Create_func_gtid_info::s_singleton },
  { { STRING_WITH_LEN("GTID_FLASHBACK") },
    gtid_flashback_create_func() },
  { { STRING_WITH_LEN("GTID_FLASHBACK_TO") },
    gtid_flashback_to_create_func() },
  { { STRING_WITH_LEN("GTID_AT") }, &Create_func_gtid_at::s_singleton },
  { { STRING_WITH_LEN("BINLOG_GTID_SET") },
    &Create_func_binlog_gtid_set::s_singleton },
  { { STRING_WITH_LEN("BINLOG_GTID_SET_GAPS") },
    &Create_func_binlog_gtid_set_gaps::s_singleton },
  { { STRING_WITH_LEN("BINLOG_GTID_SET_RANGES") },
    &Create_func_binlog_gtid_set_ranges::s_singleton },
  { { STRING_WITH_LEN("GTID_SET_BINLOGS") },
    &Create_func_gtid_set_binlogs::s_singleton }
};


static int gtid_info_plugin_init(void *)
{
  if (native_functions_hash.append(gtid_info_func_array,
                                   array_elements(gtid_info_func_array)))
  {
    my_message(ER_PLUGIN_IS_NOT_LOADED,
               "Cannot register GTID_INFO native function", MYF(0));
    return 1;
  }
  return 0;
}


static int gtid_info_plugin_deinit(void *)
{
  (void) native_functions_hash.remove(gtid_info_func_array,
                                      array_elements(gtid_info_func_array));
  return 0;
}


static struct st_mysql_daemon gtid_info_plugin=
{ MYSQL_DAEMON_INTERFACE_VERSION };


maria_declare_plugin(gtid_info)
{
  MYSQL_DAEMON_PLUGIN,
  &gtid_info_plugin,
  "gtid_info",
  "lefred",
  "GTID binary log information function",
  PLUGIN_LICENSE_GPL,
  gtid_info_plugin_init,
  gtid_info_plugin_deinit,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
