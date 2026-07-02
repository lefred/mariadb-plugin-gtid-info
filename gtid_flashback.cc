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
#include "sql_base.h"
#include "table.h"
#include "tztime.h"
#include <mysql/service_sql.h>
/*
  The server-side row event API exposes the table id and column bitmaps, but
  not the packed row payload nor table-map column metadata. GTID_FLASHBACK()
  needs both to build reverse DML. This needs to be contained in the plugin;
  a future server helper should expose a narrow read-only row-event decoding
  API instead of relying on class internals here. <TODO>
*/
#define private public
#define protected public
#include "log_event.h"
#include "rpl_utility.h"
#undef protected
#undef private
#include "compat56.h"
#include "my_decimal.h"
#include "mysqld.h"
#include "rpl_constants.h"
#include "rpl_gtid.h"
#include "gtid_flashback.h"


#ifdef HAVE_REPLICATION
struct Gtid_flashback_table
{
  ulonglong table_id;
  Table_map_log_event *map;
};

struct Gtid_flashback_event
{
  Log_event_type type;
  Table_map_log_event *map;
  Rows_log_event *rows;
};

typedef std::vector<Gtid_flashback_table> Gtid_flashback_table_vec;
typedef std::vector<Gtid_flashback_event> Gtid_flashback_event_vec;


int rpl_append_gtid_state(String *dest, bool use_binlog);


static bool
gtid_info_json_append_uint(String *str, ulonglong value)
{
  char buf[MY_INT64_NUM_DECIMAL_DIGITS + 1];
  size_t len= longlong10_to_str(value, buf, 10) - buf;
  return str->append(buf, len);
}


static bool
gtid_info_append_sql_quoted(String *str, const char *ptr, size_t len,
                            char quote)
{
  if (str->append(quote))
    return true;
  for (size_t i= 0; i < len; ++i)
  {
    char ch= ptr[i];
    switch (ch) {
    case '\0':
      if (str->append(STRING_WITH_LEN("\\0")))
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
    case '\b':
      if (str->append(STRING_WITH_LEN("\\b")))
        return true;
      break;
    case '\\':
      if (str->append(STRING_WITH_LEN("\\\\")))
        return true;
      break;
    case '\'':
      if (quote == '\'' && str->append(STRING_WITH_LEN("\\'")))
        return true;
      if (quote != '\'' && str->append(ch))
        return true;
      break;
    case '`':
      if (quote == '`' && str->append(STRING_WITH_LEN("``")))
        return true;
      if (quote != '`' && str->append(ch))
        return true;
      break;
    default:
      if (str->append(ch))
        return true;
    }
  }
  return str->append(quote);
}


static bool
gtid_info_append_sql_identifier(String *str, const char *ptr, size_t len)
{
  return gtid_info_append_sql_quoted(str, ptr, len, '`');
}


static bool
gtid_info_append_sql_table_name(String *str, Table_map_log_event *map)
{
  return gtid_info_append_sql_identifier(str, map->m_dbnam, map->m_dblen) ||
         str->append('.') ||
         gtid_info_append_sql_identifier(str, map->m_tblnam, map->m_tbllen);
}


static TABLE *
gtid_flashback_open_live_table(THD *thd, Table_map_log_event *map,
                               TABLE_LIST *tl, thr_lock_type lock_type)
{
  LEX_CSTRING db= { map->m_dbnam, map->m_dblen };
  LEX_CSTRING table_name= { map->m_tblnam, map->m_tbllen };

  tl->init_one_table(&db, &table_name, NULL, lock_type);
  tl->required_type= TABLE_TYPE_NORMAL;
  return open_ltable(thd, tl, lock_type, MYSQL_LOCK_IGNORE_TIMEOUT);
}


static bool
gtid_flashback_append_column_name(String *str, TABLE *table, uint col)
{
  if (!table || col >= table->s->fields || !table->field[col])
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0), "binlog column", "");
    return true;
  }
  return gtid_info_append_sql_identifier(str, table->field[col]->field_name.str,
                                         table->field[col]->field_name.length);
}


static bool
gtid_flashback_append_ll(String *str, longlong value)
{
  char buf[MY_INT64_NUM_DECIMAL_DIGITS + 2];
  size_t len= longlong10_to_str(value, buf, -10) - buf;
  return str->append(buf, len);
}


static bool
gtid_flashback_append_double(String *str, double value)
{
  char buf[64];
  size_t len= my_snprintf(buf, sizeof(buf), "%.17g", value);
  return str->append(buf, len);
}


static bool
gtid_flashback_append_unsupported(enum_field_types type)
{
  char msg[128];
  my_snprintf(msg, sizeof(msg), "GTID_FLASHBACK() for column type %u",
              static_cast<uint>(type));
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), msg);
  return true;
}


static bool
gtid_flashback_append_bit(String *str, const uchar *ptr, uint nbits)
{
  uint nbits8= ((nbits + 7) / 8) * 8;
  uint skip_bits= nbits8 - nbits;

  if (str->append(STRING_WITH_LEN("b'")))
    return true;
  for (uint bitnum= skip_bits; bitnum < nbits8; ++bitnum)
  {
    int is_set= (ptr[bitnum / 8] >> (7 - bitnum % 8)) & 0x01;
    if (str->append(is_set ? '1' : '0'))
      return true;
  }
  return str->append('\'');
}


static bool
gtid_flashback_append_value(String *str, table_def *td, uint col,
                            const uchar *value, bool is_null,
                            const uchar **next_value)
{
  enum_field_types type= td->type(col);
  uint16 meta= td->field_metadata(col);
  uint32 length;

  if (is_null)
  {
    *next_value= value;
    return str->append(STRING_WITH_LEN("NULL"));
  }

  length= td->calc_field_size(col, const_cast<uchar *>(value));
  if (length == ~(uint32) 0)
    return gtid_flashback_append_unsupported(type);

  *next_value= value + length;

  switch (type) {
  case MYSQL_TYPE_TINY:
    return gtid_flashback_append_ll(str, static_cast<signed char>(*value));
  case MYSQL_TYPE_SHORT:
    return gtid_flashback_append_ll(str, sint2korr(value));
  case MYSQL_TYPE_INT24:
    return gtid_flashback_append_ll(str, sint3korr(value));
  case MYSQL_TYPE_LONG:
    return gtid_flashback_append_ll(str, sint4korr(value));
#ifdef HAVE_LONG_LONG
  case MYSQL_TYPE_LONGLONG:
    return gtid_flashback_append_ll(str, sint8korr(value));
#endif
  case MYSQL_TYPE_FLOAT:
  {
    float f;
    float4get(f, value);
    return gtid_flashback_append_double(str, f);
  }
  case MYSQL_TYPE_DOUBLE:
  {
    double d;
    float8get(d, value);
    return gtid_flashback_append_double(str, d);
  }
  case MYSQL_TYPE_NEWDECIMAL:
  {
    uint precision= meta >> 8;
    uint decimals= meta & 0xFF;
    my_decimal dec(value, precision, decimals);
    int len= DECIMAL_MAX_STR_LENGTH;
    char buf[DECIMAL_MAX_STR_LENGTH + 1];
    decimal2string(&dec, buf, &len, 0, 0, 0);
    return str->append(buf, len);
  }
  case MYSQL_TYPE_BIT:
  {
    uint nbits= ((meta >> 8) * 8) + (meta & 0xFF);
    return gtid_flashback_append_bit(str, value, nbits);
  }
  case MYSQL_TYPE_TIMESTAMP:
  {
    char buf[MAX_DATE_STRING_REP_LENGTH];
    MYSQL_TIME ltime;
    current_thd->variables.time_zone->gmt_sec_to_TIME(&ltime, uint4korr(value));
    int len= my_datetime_to_str(&ltime, buf, 0);
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_TIMESTAMP2:
  {
    char buf[MAX_DATE_STRING_REP_LENGTH];
    struct my_timeval tm;
    MYSQL_TIME ltime;
    my_timestamp_from_binary(&tm, value, meta);
    current_thd->variables.time_zone->gmt_sec_to_TIME(&ltime, tm.tv_sec);
    ltime.second_part= tm.tv_usec;
    int len= my_datetime_to_str(&ltime, buf, meta);
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_DATETIME:
  {
    char buf[32];
    ulong d, t;
    uint64 i64= uint8korr(value);
    d= static_cast<ulong>(i64 / 1000000);
    t= static_cast<ulong>(i64 % 1000000);
    size_t len= my_snprintf(buf, sizeof(buf),
                            "%04d-%02d-%02d %02d:%02d:%02d",
                            static_cast<int>(d / 10000),
                            static_cast<int>((d % 10000) / 100),
                            static_cast<int>(d % 100),
                            static_cast<int>(t / 10000),
                            static_cast<int>((t % 10000) / 100),
                            static_cast<int>(t % 100));
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_DATETIME2:
  {
    char buf[MAX_DATE_STRING_REP_LENGTH];
    MYSQL_TIME ltime;
    longlong packed= my_datetime_packed_from_binary(value, meta);
    TIME_from_longlong_datetime_packed(&ltime, packed);
    int len= my_datetime_to_str(&ltime, buf, meta);
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_TIME:
  {
    char buf[32];
    int32 tmp= sint3korr(value);
    int32 i32= tmp >= 0 ? tmp : -tmp;
    const char *sign= tmp < 0 ? "-" : "";
    size_t len= my_snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d",
                            sign, i32 / 10000, (i32 % 10000) / 100,
                            i32 % 100);
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_TIME2:
  {
    char buf[MAX_DATE_STRING_REP_LENGTH];
    MYSQL_TIME ltime;
    longlong packed= my_time_packed_from_binary(value, meta);
    TIME_from_longlong_time_packed(&ltime, packed);
    int len= my_time_to_str(&ltime, buf, meta);
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATE:
  {
    char buf[16];
    uint32 tmp= uint3korr(value);
    uint day= tmp & 31;
    uint month= (tmp >> 5) & 15;
    uint year= tmp >> 9;
    size_t len= my_snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
                            year, month, day);
    return gtid_info_append_sql_quoted(str, buf, len, '\'');
  }
  case MYSQL_TYPE_YEAR:
    return gtid_flashback_append_ll(str, *value + 1900);
  case MYSQL_TYPE_NULL:
    return str->append(STRING_WITH_LEN("NULL"));
  case MYSQL_TYPE_ENUM:
    if ((meta & 0xFF) == 1)
      return gtid_flashback_append_ll(str, *value);
    if ((meta & 0xFF) == 2)
      return gtid_flashback_append_ll(str, uint2korr(value));
    return gtid_flashback_append_unsupported(type);
  case MYSQL_TYPE_SET:
    return gtid_flashback_append_bit(str, value, (meta & 0xFF) * 8);
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VARCHAR_COMPRESSED:
  {
    uint len_len= meta > 255 ? 2 : 1;
    uint str_len= len_len == 1 ? *value : uint2korr(value);
    return gtid_info_append_sql_quoted(str,
                                       reinterpret_cast<const char *>(value +
                                                                      len_len),
                                       str_len, '\'');
  }
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  {
    uint str_len= *value;
    return gtid_info_append_sql_quoted(str,
                                       reinterpret_cast<const char *>(value + 1),
                                       str_len, '\'');
  }
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_BLOB_COMPRESSED:
  case MYSQL_TYPE_GEOMETRY:
  {
    uint blob_len= 0;
    switch (meta) {
    case 1:
      blob_len= *value;
      break;
    case 2:
      blob_len= uint2korr(value);
      break;
    case 3:
      blob_len= uint3korr(value);
      break;
    case 4:
      blob_len= uint4korr(value);
      break;
    default:
      return gtid_flashback_append_unsupported(type);
    }
    return gtid_info_append_sql_quoted(str,
                                       reinterpret_cast<const char *>(value +
                                                                      meta),
                                       blob_len, '\'');
  }
  default:
    return gtid_flashback_append_unsupported(type);
  }
}


static table_def *
gtid_flashback_create_table_def(Table_map_log_event *map)
{
  return new table_def(map->m_coltype, map->m_colcnt, map->m_field_metadata,
                       map->m_field_metadata_size, map->m_null_bits,
                       map->m_flags, map->m_optional_metadata,
                       map->m_optional_metadata_len);
}


static bool
gtid_flashback_row_complete(Rows_log_event *rev)
{
  Log_event_type typ= rev->get_type_code();

  if (rev->get_flags(Rows_log_event::COMPLETE_ROWS_F))
    return true;
  if (LOG_EVENT_IS_UPDATE_ROW(typ))
    return bitmap_is_set_all(rev->get_cols()) &&
           bitmap_is_set_all(rev->get_cols_ai());
  return bitmap_is_set_all(rev->get_cols());
}


static bool
gtid_flashback_append_row_values(String *str, table_def *td,
                                 TABLE *table,
                                 MY_BITMAP const *cols,
                                 const uchar *value, const uchar *end,
                                 bool names, bool predicate,
                                 const uchar **next_value)
{
  const uchar *null_bits= value;
  uint null_bit_index= 0;
  bool first= true;

  value+= (bitmap_bits_set(cols) + 7) / 8;
  if (value > end)
  {
    my_error(ER_MASTER_FATAL_ERROR_READING_BINLOG, MYF(0),
             "Corrupt row event in GTID_FLASHBACK()");
    return true;
  }

  for (uint i= 0; i < td->size(); ++i)
  {
    const uchar *new_value;
    bool is_null;

    if (!bitmap_is_set(cols, i))
      continue;

    is_null= (null_bits[null_bit_index / 8] >>
              (null_bit_index % 8)) & 0x01;

    if (!first)
    {
      if (predicate ? str->append(STRING_WITH_LEN(" AND ")) :
                      str->append(','))
        return true;
    }
    first= false;

    if (names &&
        (gtid_flashback_append_column_name(str, table, i) ||
         (predicate ? str->append(STRING_WITH_LEN(" <=> ")) :
                      str->append('='))))
      return true;
    if (!names &&
        (str->append('@') ||
         gtid_info_json_append_uint(str, i + 1) ||
         (predicate ? str->append(STRING_WITH_LEN(" <=> ")) :
                      str->append('='))))
      return true;

    if (gtid_flashback_append_value(str, td, i, value, is_null, &new_value))
      return true;
    if (new_value > end)
    {
      my_error(ER_MASTER_FATAL_ERROR_READING_BINLOG, MYF(0),
               "Corrupt row event in GTID_FLASHBACK()");
      return true;
    }
    value= new_value;
    ++null_bit_index;
  }

  *next_value= value;
  return false;
}


static bool
gtid_flashback_append_column_list(String *str, TABLE *table, table_def *td,
                                  MY_BITMAP const *cols)
{
  bool first= true;
  if (str->append('('))
    return true;
  for (uint i= 0; i < td->size(); ++i)
  {
    if (!bitmap_is_set(cols, i))
      continue;
    if (!first && str->append(','))
      return true;
    first= false;
    if (gtid_flashback_append_column_name(str, table, i))
      return true;
  }
  return str->append(')');
}


static bool
gtid_flashback_append_where(String *str, table_def *td, TABLE *table,
                            MY_BITMAP const *cols,
                            const uchar *value, const uchar *end,
                            const uchar **next_value)
{
  return str->append(STRING_WITH_LEN(" WHERE ")) ||
         gtid_flashback_append_row_values(str, td, table, cols, value, end,
                                          true, true,
                                          next_value);
}


static bool
gtid_flashback_append_insert(String *str, Table_map_log_event *map,
                             table_def *td, TABLE *table, MY_BITMAP const *cols,
                             const uchar *value, const uchar *end,
                             const uchar **next_value)
{
  return str->append(STRING_WITH_LEN("INSERT INTO ")) ||
         gtid_info_append_sql_table_name(str, map) ||
         str->append(' ') ||
         gtid_flashback_append_column_list(str, table, td, cols) ||
         str->append(STRING_WITH_LEN(" VALUES (")) ||
         gtid_flashback_append_row_values(str, td, table, cols, value, end,
                                          false, false,
                                          next_value) ||
         str->append(STRING_WITH_LEN(");\n"));
}


static bool
gtid_flashback_append_delete(String *str, Table_map_log_event *map,
                             table_def *td, TABLE *table, MY_BITMAP const *cols,
                             const uchar *value, const uchar *end,
                             const uchar **next_value)
{
  return str->append(STRING_WITH_LEN("DELETE FROM ")) ||
         gtid_info_append_sql_table_name(str, map) ||
         gtid_flashback_append_where(str, td, table, cols, value, end,
                                     next_value) ||
         str->append(STRING_WITH_LEN(";\n"));
}


static bool
gtid_flashback_append_update(String *str, Table_map_log_event *map,
                             table_def *td, TABLE *table, Rows_log_event *rev,
                             const uchar *value, const uchar *end,
                             const uchar **next_value)
{
  const uchar *after;

  if (str->append(STRING_WITH_LEN("UPDATE ")) ||
      gtid_info_append_sql_table_name(str, map) ||
      str->append(STRING_WITH_LEN(" SET ")) ||
      gtid_flashback_append_row_values(str, td, table, rev->get_cols(), value,
                                       end, true, false, &after) ||
      gtid_flashback_append_where(str, td, table, rev->get_cols_ai(), after, end,
                                  next_value) ||
      str->append(STRING_WITH_LEN(";\n")))
    return true;
  return false;
}


static bool
gtid_flashback_append_reversed_event(THD *thd, String *str,
                                     const Gtid_flashback_event *fb_ev)
{
  Rows_log_event *rev= fb_ev->rows;
  Log_event_type typ= rev->get_type_code();
  const uchar *value= rev->m_rows_buf;
  const uchar *end= rev->m_rows_end;
  table_def *td;
  TABLE_LIST tl;
  TABLE *table;
  bool ret= false;

  if (!gtid_flashback_row_complete(rev))
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "GTID_FLASHBACK() for incomplete row images");
    return true;
  }

  if (!(td= gtid_flashback_create_table_def(fb_ev->map)))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), static_cast<int>(sizeof(table_def)));
    return true;
  }

  if (!(table= gtid_flashback_open_live_table(thd, fb_ev->map, &tl, TL_READ)))
  {
    delete td;
    return true;
  }
  if (table->s->fields < td->size())
  {
    close_thread_tables(thd);
    delete td;
    my_error(ER_BAD_FIELD_ERROR, MYF(0), "binlog column", "");
    return true;
  }

  while (!ret && value < end)
  {
    const uchar *next_value= value;
    if (LOG_EVENT_IS_WRITE_ROW(typ))
      ret= gtid_flashback_append_delete(str, fb_ev->map, td, table,
                                        rev->get_cols(),
                                        value, end, &next_value);
    else if (LOG_EVENT_IS_DELETE_ROW(typ))
      ret= gtid_flashback_append_insert(str, fb_ev->map, td, table,
                                        rev->get_cols(),
                                        value, end, &next_value);
    else if (LOG_EVENT_IS_UPDATE_ROW(typ))
      ret= gtid_flashback_append_update(str, fb_ev->map, td, table, rev,
                                        value, end,
                                        &next_value);
    else
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "GTID_FLASHBACK() for non-row events");
      ret= true;
    }
    value= next_value;
  }

  close_thread_tables(thd);
  delete td;
  return ret;
}


static Table_map_log_event *
gtid_flashback_find_map(Gtid_flashback_table_vec *tables, ulonglong table_id)
{
  for (size_t i= tables->size(); i > 0; --i)
  {
    if ((*tables)[i - 1].table_id == table_id)
      return (*tables)[i - 1].map;
  }
  return NULL;
}


static void
gtid_flashback_free_collected(Gtid_flashback_table_vec *tables,
                              Gtid_flashback_event_vec *events)
{
  for (size_t i= 0; i < events->size(); ++i)
    delete (*events)[i].rows;
  events->clear();

  for (size_t i= 0; i < tables->size(); ++i)
    delete (*tables)[i].map;
  tables->clear();
}


static bool
gtid_flashback_emit_collected(THD *thd, String *out_str,
                              Gtid_flashback_event_vec *events)
{
  out_str->length(0);
  if (out_str->append(STRING_WITH_LEN("SET FOREIGN_KEY_CHECKS=0;\n")))
    return true;

  for (size_t i= events->size(); i > 0; --i)
  {
    if (gtid_flashback_append_reversed_event(thd, out_str, &(*events)[i - 1]))
      return true;
  }

  return out_str->append(STRING_WITH_LEN("SET FOREIGN_KEY_CHECKS=1;\n"));
}


static int
gtid_flashback_scan_file(THD *thd, const char *log_name, const rpl_gtid *wanted,
                         String *out_str, bool *found)
{
  const char *errmsg;
  IO_CACHE log;
  File file;
  int read_error;
  Log_event *ev= NULL;
  Log_event *fde_ev= NULL;
  Format_description_log_event init_fdle(BINLOG_VERSION);
  Format_description_log_event *fdle= &init_fdle;
  bool collecting= false;
  bool have_rows= false;
  Gtid_flashback_table_vec tables;
  Gtid_flashback_event_vec events;
  int ret= 0;

  *found= false;
  if ((file= open_binlog(&log, log_name, &errmsg)) < 0)
    return 1;

  while ((ev= Log_event::read_log_event(&log, &read_error, fdle,
                                        opt_master_verify_checksum)))
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

    if (typ == ROTATE_EVENT || typ == STOP_EVENT)
      break;

    if (typ == GTID_EVENT)
    {
      Gtid_log_event *gev= static_cast<Gtid_log_event *>(ev);
      if (collecting)
      {
        *found= true;
        ret= have_rows ? gtid_flashback_emit_collected(thd, out_str, &events) : 0;
        if (!have_rows)
          my_error(ER_NOT_SUPPORTED_YET, MYF(0),
                   "GTID_FLASHBACK() for GTID groups without row DML");
        break;
      }
      if (gev->domain_id == wanted->domain_id &&
          gev->server_id == wanted->server_id &&
          gev->seq_no == wanted->seq_no)
        collecting= true;
    }
    else if (collecting && typ == TABLE_MAP_EVENT)
    {
      Gtid_flashback_table table;
      table.map= static_cast<Table_map_log_event *>(ev);
      table.table_id= table.map->get_table_id();
      tables.push_back(table);
      ev= NULL;
    }
    else if (collecting &&
             (LOG_EVENT_IS_WRITE_ROW(typ) || LOG_EVENT_IS_UPDATE_ROW(typ) ||
              LOG_EVENT_IS_DELETE_ROW(typ)))
    {
      Rows_log_event *rev= static_cast<Rows_log_event *>(ev);
      Table_map_log_event *map= gtid_flashback_find_map(&tables,
                                                        rev->get_table_id());
      if (!map)
      {
        my_error(ER_MASTER_FATAL_ERROR_READING_BINLOG, MYF(0),
                 "Row event without table-map in GTID_FLASHBACK()");
        ret= 1;
        break;
      }
      Gtid_flashback_event fb_ev;
      fb_ev.type= typ;
      fb_ev.map= map;
      fb_ev.rows= rev;
      events.push_back(fb_ev);
      have_rows= true;
      ev= NULL;
    }
    delete ev;
    ev= NULL;
  }

  if (!ret && collecting && !*found)
  {
    *found= true;
    ret= have_rows ? gtid_flashback_emit_collected(thd, out_str, &events) : 0;
    if (!have_rows)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "GTID_FLASHBACK() for GTID groups without row DML");
      ret= 1;
    }
  }

  delete ev;
  delete fde_ev;
  gtid_flashback_free_collected(&tables, &events);
  end_io_cache(&log);
  mysql_file_close(file, MYF(MY_WME));
  return ret;
}


static int
gtid_flashback_full_scan(THD *thd, const rpl_gtid *wanted, String *out_str)
{
  LOG_INFO linfo;
  int error;
  int ret;
  bool found= false;

  if ((error= mysql_bin_log.find_log_pos(&linfo, NullS, true)))
  {
    if (error == LOG_INFO_EOF)
    {
      my_error(ER_KEY_NOT_FOUND, MYF(0), "GTID");
      return 1;
    }
    return 1;
  }

  for (;;)
  {
    if ((ret= gtid_flashback_scan_file(thd, linfo.log_file_name, wanted, out_str,
                                       &found)) || found)
      return ret;
    error= mysql_bin_log.find_next_log(&linfo, true);
    if (error == LOG_INFO_EOF)
      break;
    if (error)
      return 1;
  }

  my_error(ER_KEY_NOT_FOUND, MYF(0), "GTID");
  return 1;
}


static bool
gtid_flashback_same_domain(const rpl_gtid *a, const rpl_gtid *b)
{
  return a->domain_id == b->domain_id;
}


static bool
gtid_flashback_same_gtid(const rpl_gtid *a, const rpl_gtid *b)
{
  return a->domain_id == b->domain_id &&
         a->server_id == b->server_id &&
         a->seq_no == b->seq_no;
}


static int
gtid_flashback_get_current_domain_gtid(const rpl_gtid *target,
                                       rpl_gtid *upper)
{
  String current_pos;
  rpl_gtid *gtid_list;
  uint32 gtid_count;
  int ret= 1;

  current_pos.length(0);
  if (rpl_append_gtid_state(&current_pos, true))
    return 1;

  if (!(gtid_list= gtid_parse_string_to_list(current_pos.ptr(),
                                             current_pos.length(),
                                             &gtid_count)))
  {
    my_error(ER_INCORRECT_GTID_STATE, MYF(0));
    return 1;
  }

  for (uint32 i= 0; i < gtid_count; ++i)
  {
    if (gtid_list[i].domain_id == target->domain_id)
    {
      *upper= gtid_list[i];
      ret= 0;
      break;
    }
  }

  my_free(gtid_list);
  if (ret)
    my_error(ER_KEY_NOT_FOUND, MYF(0), "GTID domain in current position");
  return ret;
}


static bool
gtid_flashback_gtid_in_suffix(const rpl_gtid *gtid, const rpl_gtid *target,
                              const rpl_gtid *upper)
{
  return gtid_flashback_same_domain(gtid, target) &&
         gtid->seq_no > target->seq_no &&
         gtid->seq_no <= upper->seq_no;
}


static int
gtid_flashback_finish_range_group(bool collecting, bool have_rows,
                                  bool *any_group)
{
  if (!collecting)
    return 0;
  if (!have_rows)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "GTID_FLASHBACK_TO() for GTID groups without row DML");
    return 1;
  }
  *any_group= true;
  return 0;
}


static int
gtid_flashback_to_scan_file(const char *log_name, const rpl_gtid *target,
                            const rpl_gtid *upper,
                            Gtid_flashback_table_vec *tables,
                            Gtid_flashback_event_vec *events,
                            bool *found_anchor, bool *after_anchor,
                            bool *any_group, bool *reached_upper, bool *done)
{
  const char *errmsg;
  IO_CACHE log;
  File file;
  int read_error;
  Log_event *ev= NULL;
  Log_event *fde_ev= NULL;
  Format_description_log_event init_fdle(BINLOG_VERSION);
  Format_description_log_event *fdle= &init_fdle;
  bool collecting= false;
  bool have_rows= false;
  int ret= 0;

  if ((file= open_binlog(&log, log_name, &errmsg)) < 0)
    return 1;

  while ((ev= Log_event::read_log_event(&log, &read_error, fdle,
                                        opt_master_verify_checksum)))
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

    if (typ == ROTATE_EVENT || typ == STOP_EVENT)
      break;

    if (typ == GTID_EVENT)
    {
      Gtid_log_event *gev= static_cast<Gtid_log_event *>(ev);
      rpl_gtid current_gtid;

      current_gtid.domain_id= gev->domain_id;
      current_gtid.server_id= gev->server_id;
      current_gtid.seq_no= gev->seq_no;

      if ((ret= gtid_flashback_finish_range_group(collecting, have_rows,
                                                  any_group)))
        break;
      collecting= false;
      have_rows= false;

      if (gtid_flashback_same_gtid(&current_gtid, target))
      {
        *found_anchor= true;
        *after_anchor= true;
      }
      else if (*after_anchor &&
               gtid_flashback_gtid_in_suffix(&current_gtid, target, upper))
      {
        collecting= true;
        if (current_gtid.seq_no == upper->seq_no)
          *reached_upper= true;
      }
      else if (*after_anchor &&
               gtid_flashback_same_domain(&current_gtid, target) &&
               current_gtid.seq_no > upper->seq_no)
      {
        *done= true;
        break;
      }
    }
    else if (collecting && typ == TABLE_MAP_EVENT)
    {
      Gtid_flashback_table table;
      table.map= static_cast<Table_map_log_event *>(ev);
      table.table_id= table.map->get_table_id();
      tables->push_back(table);
      ev= NULL;
    }
    else if (collecting &&
             (LOG_EVENT_IS_WRITE_ROW(typ) || LOG_EVENT_IS_UPDATE_ROW(typ) ||
              LOG_EVENT_IS_DELETE_ROW(typ)))
    {
      Rows_log_event *rev= static_cast<Rows_log_event *>(ev);
      Table_map_log_event *map= gtid_flashback_find_map(tables,
                                                        rev->get_table_id());
      if (!map)
      {
        my_error(ER_MASTER_FATAL_ERROR_READING_BINLOG, MYF(0),
                 "Row event without table-map in GTID_FLASHBACK_TO()");
        ret= 1;
        break;
      }
      Gtid_flashback_event fb_ev;
      fb_ev.type= typ;
      fb_ev.map= map;
      fb_ev.rows= rev;
      events->push_back(fb_ev);
      have_rows= true;
      ev= NULL;
    }
    delete ev;
    ev= NULL;
  }

  if (!ret)
    ret= gtid_flashback_finish_range_group(collecting, have_rows, any_group);

  delete ev;
  delete fde_ev;
  end_io_cache(&log);
  mysql_file_close(file, MYF(MY_WME));
  return ret;
}


static int
gtid_flashback_to_full_scan(THD *thd, const rpl_gtid *target,
                            const rpl_gtid *upper, String *out_str)
{
  LOG_INFO linfo;
  int error;
  int ret;
  bool found_anchor= false;
  bool after_anchor= false;
  bool any_group= false;
  bool reached_upper= false;
  bool done= false;
  Gtid_flashback_table_vec tables;
  Gtid_flashback_event_vec events;

  if ((error= mysql_bin_log.find_log_pos(&linfo, NullS, true)))
  {
    if (error == LOG_INFO_EOF)
      my_error(ER_KEY_NOT_FOUND, MYF(0), "GTID");
    return 1;
  }

  for (;;)
  {
    ret= gtid_flashback_to_scan_file(linfo.log_file_name, target, upper,
                                     &tables, &events, &found_anchor,
                                     &after_anchor, &any_group,
                                     &reached_upper, &done);
    if (ret || done)
      break;
    error= mysql_bin_log.find_next_log(&linfo, true);
    if (error == LOG_INFO_EOF)
      break;
    if (error)
    {
      ret= 1;
      break;
    }
  }

  if (!ret && !found_anchor)
  {
    my_error(ER_KEY_NOT_FOUND, MYF(0), "GTID");
    ret= 1;
  }
  if (!ret && !reached_upper)
  {
    my_error(ER_KEY_NOT_FOUND, MYF(0), "current GTID");
    ret= 1;
  }
  if (!ret)
  {
    if (any_group)
      ret= gtid_flashback_emit_collected(thd, out_str, &events);
    else
    {
      out_str->length(0);
      ret= out_str->append(STRING_WITH_LEN("SET FOREIGN_KEY_CHECKS=0;\n")) ||
           out_str->append(STRING_WITH_LEN("SET FOREIGN_KEY_CHECKS=1;\n"));
    }
  }

  gtid_flashback_free_collected(&tables, &events);
  return ret;
}


static int
gtid_flashback_execute_statement(MYSQL *mysql, const char *sql, size_t len,
                                 ulonglong *affected_rows)
{
  if (mysql_real_query(mysql, sql, len))
  {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return 1;
  }
  if (MYSQL_RES *res= mysql_store_result(mysql))
    mysql_free_result(res);
  else
  {
    ulonglong stmt_affected= mysql_affected_rows(mysql);
    if (stmt_affected != static_cast<ulonglong>(~0ULL))
      *affected_rows+= stmt_affected;
  }
  return 0;
}


static int
gtid_flashback_execute_sql(String *sql)
{
  MYSQL *mysql;
  const char *ptr= sql->ptr();
  const char *end= ptr + sql->length();
  ulonglong affected_rows= 0;
  int ret= 0;

  if (!(mysql= mysql_init(NULL)))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), static_cast<int>(sizeof(MYSQL)));
    return 1;
  }
  if (!mysql_real_connect_local(mysql))
  {
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    mysql_close(mysql);
    return 1;
  }

  while (ptr < end)
  {
    const char *stmt_end= static_cast<const char *>(memchr(ptr, ';', end - ptr));
    const char *stmt_stop= stmt_end ? stmt_end : end;

    while (ptr < stmt_stop && my_isspace(system_charset_info, *ptr))
      ++ptr;
    if (ptr < stmt_stop &&
        gtid_flashback_execute_statement(mysql, ptr, stmt_stop - ptr,
                                         &affected_rows))
    {
      ret= 1;
      break;
    }
    ptr= stmt_end ? stmt_end + 1 : end;
  }
  mysql_close(mysql);
  if (!ret)
  {
    String summary;
    if (summary.append(STRING_WITH_LEN("Query OK, ")) ||
        gtid_info_json_append_uint(&summary, affected_rows))
      return 1;
    if (affected_rows == 1)
    {
      if (summary.append(STRING_WITH_LEN(" row affected\n")))
        return 1;
    }
    else if (summary.append(STRING_WITH_LEN(" rows affected\n")))
      return 1;
    if (summary.append(sql->ptr(), sql->length()))
      return 1;
    sql->copy(summary);
  }
  return ret;
}


static int
gtid_flashback_to_sql(THD *thd, const char *gtid_str, size_t gtid_len,
                      bool execute, String *out_str)
{
  rpl_gtid *gtid_list;
  uint32 gtid_count;
  int ret;

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }

  if (opt_binlog_engine_hton)
  {
    my_error(ER_NOT_AVAILABLE_WITH_ENGINE_BINLOG, MYF(0), "GTID_FLASHBACK()");
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
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "GTID_FLASHBACK");
    return 1;
  }

  ret= gtid_flashback_full_scan(thd, gtid_list, out_str);
  if (!ret && execute)
    ret= gtid_flashback_execute_sql(out_str);
  my_free(gtid_list);
  return ret;
}


static int
gtid_flashback_suffix_to_sql(THD *thd, const char *gtid_str, size_t gtid_len,
                             bool execute, String *out_str)
{
  rpl_gtid *gtid_list;
  rpl_gtid upper;
  uint32 gtid_count;
  int ret;

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }

  if (opt_binlog_engine_hton)
  {
    my_error(ER_NOT_AVAILABLE_WITH_ENGINE_BINLOG, MYF(0),
             "GTID_FLASHBACK_TO()");
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
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "GTID_FLASHBACK_TO");
    return 1;
  }

  if ((ret= gtid_flashback_get_current_domain_gtid(gtid_list, &upper)))
  {
    my_free(gtid_list);
    return ret;
  }

  if (upper.seq_no <= gtid_list->seq_no)
  {
    out_str->length(0);
    ret= out_str->append(STRING_WITH_LEN("SET FOREIGN_KEY_CHECKS=0;\n")) ||
         out_str->append(STRING_WITH_LEN("SET FOREIGN_KEY_CHECKS=1;\n"));
  }
  else
    ret= gtid_flashback_to_full_scan(thd, gtid_list, &upper, out_str);

  if (!ret && execute)
    ret= gtid_flashback_execute_sql(out_str);
  my_free(gtid_list);
  return ret;
}


#endif


class Item_func_gtid_flashback final : public Item_str_func
{
public:
  Item_func_gtid_flashback(THD *thd, Item *arg)
    : Item_str_func(thd, arg)
  { }
  Item_func_gtid_flashback(THD *thd, Item *arg1, Item *arg2)
    : Item_str_func(thd, arg1, arg2)
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
    bool execute= false;

    DBUG_ASSERT(fixed());
    gtid= args[0]->val_str(&gtid_buf);
    if (args[0]->null_value)
      goto null;
    if (arg_count == 2)
    {
      execute= args[1]->val_bool();
      if (args[1]->null_value)
        goto null;
    }

#ifdef HAVE_REPLICATION
    if (gtid_flashback_to_sql(current_thd, gtid->ptr(), gtid->length(),
                              execute, str))
      goto null;
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
#else
    my_error(ER_FEATURE_DISABLED, MYF(0), "GTID_FLASHBACK", "replication");
    goto null;
#endif

null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("gtid_flashback") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_gtid_flashback>(thd, this);
  }
};


class Create_func_gtid_flashback final : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name,
                    List<Item> *item_list) override
  {
    Item *arg1;
    Item *arg2= NULL;

    if (item_list->elements != 1 && item_list->elements != 2)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      return NULL;
    }

    List_iterator_fast<Item> li(*item_list);
    arg1= li++;
    if (item_list->elements == 2)
      arg2= li++;

#ifdef HAVE_REPLICATION
    if (unlikely(!mysql_bin_log.is_open()))
    {
      my_error(ER_NO_BINARY_LOGGING, MYF(0));
      return NULL;
    }
#endif
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    if (arg2)
      return new (thd->mem_root) Item_func_gtid_flashback(thd, arg1, arg2);
    return new (thd->mem_root) Item_func_gtid_flashback(thd, arg1);
  }

  static Create_func_gtid_flashback s_singleton;
};

Create_func_gtid_flashback Create_func_gtid_flashback::s_singleton;


class Item_func_gtid_flashback_to final : public Item_str_func
{
public:
  Item_func_gtid_flashback_to(THD *thd, Item *arg)
    : Item_str_func(thd, arg)
  { }
  Item_func_gtid_flashback_to(THD *thd, Item *arg1, Item *arg2)
    : Item_str_func(thd, arg1, arg2)
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
    bool execute= false;

    DBUG_ASSERT(fixed());
    gtid= args[0]->val_str(&gtid_buf);
    if (args[0]->null_value)
      goto null;
    if (arg_count == 2)
    {
      execute= args[1]->val_bool();
      if (args[1]->null_value)
        goto null;
    }

#ifdef HAVE_REPLICATION
    if (gtid_flashback_suffix_to_sql(current_thd, gtid->ptr(), gtid->length(),
                                     execute, str))
      goto null;
    str->set_charset(system_charset_info_for_i_s);
    null_value= false;
    return str;
#else
    my_error(ER_FEATURE_DISABLED, MYF(0), "GTID_FLASHBACK_TO", "replication");
    goto null;
#endif

null:
    null_value= true;
    return NULL;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("gtid_flashback_to") };
    return name;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_gtid_flashback_to>(thd, this);
  }
};


class Create_func_gtid_flashback_to final : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name,
                    List<Item> *item_list) override
  {
    Item *arg1;
    Item *arg2= NULL;

    if (item_list->elements != 1 && item_list->elements != 2)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      return NULL;
    }

    List_iterator_fast<Item> li(*item_list);
    arg1= li++;
    if (item_list->elements == 2)
      arg2= li++;

#ifdef HAVE_REPLICATION
    if (unlikely(!mysql_bin_log.is_open()))
    {
      my_error(ER_NO_BINARY_LOGGING, MYF(0));
      return NULL;
    }
#endif
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    if (arg2)
      return new (thd->mem_root) Item_func_gtid_flashback_to(thd, arg1, arg2);
    return new (thd->mem_root) Item_func_gtid_flashback_to(thd, arg1);
  }

  static Create_func_gtid_flashback_to s_singleton;
};

Create_func_gtid_flashback_to Create_func_gtid_flashback_to::s_singleton;




Create_func *
gtid_flashback_create_func()
{
  return &Create_func_gtid_flashback::s_singleton;
}


Create_func *
gtid_flashback_to_create_func()
{
  return &Create_func_gtid_flashback_to::s_singleton;
}
