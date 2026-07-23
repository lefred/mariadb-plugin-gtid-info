/* Copyright (c) 2026 MariaDB Corporation.
   Copyright (c) 2026 lefred.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "handler.h"
#include "handler_binlog_reader.h"
#include "log_event.h"
#include "log.h"
#include "mysqld.h"
#include "gtid_binlog_reader.h"


Gtid_binlog_reader::Gtid_binlog_reader()
  : m_file((File)-1), m_engine_reader(NULL), m_engine(false)
{
}


Gtid_binlog_reader::~Gtid_binlog_reader()
{
  if (m_engine_reader)
    delete m_engine_reader;
  if (m_file >= 0)
  {
    end_io_cache(&m_log);
    mysql_file_close(m_file, MYF(MY_WME));
  }
}


int
Gtid_binlog_reader::open(THD *thd, const char *log_name)
{
  if (opt_binlog_engine_hton)
  {
    m_engine= true;
    m_engine_reader= (*opt_binlog_engine_hton->get_binlog_reader)(false);
    if (!m_engine_reader)
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return 1;
    }
    /*
      Engine file lists may contain the configured binlog directory, while
      init_legacy_pos() accepts the engine's short logical filename.
    */
    const char *short_name= strrchr(log_name, FN_LIBCHAR);
    short_name= short_name ? short_name + 1 : log_name;
    if (m_engine_reader->init_legacy_pos(thd, short_name, 0))
      return 1;
    m_engine_reader->enable_single_file();
    return 0;
  }

  const char *errmsg;
  if ((m_file= open_binlog(&m_log, log_name, &errmsg)) < 0)
    return 1;
  return 0;
}


Log_event *
Gtid_binlog_reader::read(THD *thd, Format_description_log_event *fdle,
                         int *read_error)
{
  if (!m_engine)
    return Log_event::read_log_event(&m_log, read_error, fdle,
                                     opt_master_verify_checksum);

  m_packet.length(0);
  *read_error= m_engine_reader->read_log_event(
    &m_packet, 0,
    (size_t) thd->variables.max_allowed_packet + MAX_LOG_EVENT_HEADER);
  if (*read_error)
    return NULL;

  const char *errmsg;
  Log_event *ev= Log_event::read_log_event(
    reinterpret_cast<const uchar *>(m_packet.ptr()),
    static_cast<uint>(m_packet.length()), &errmsg, fdle, false, false);
  if (!ev)
    *read_error= LOG_READ_BOGUS;
  return ev;
}


my_off_t
Gtid_binlog_reader::position() const
{
  return m_engine ? static_cast<my_off_t>(m_engine_reader->cur_file_pos) :
                    my_b_tell(const_cast<IO_CACHE *>(&m_log));
}


Gtid_binlog_file_iterator::Gtid_binlog_file_iterator(THD *thd)
  : m_engine_entry(NULL), m_error(0), m_engine(opt_binlog_engine_hton != NULL)
{
  m_engine_active_name[0]= 0;
  if (m_engine)
  {
    uint64_t active_file_no, active_pos;
    (*opt_binlog_engine_hton->binlog_status)(&active_file_no, &active_pos);
    (*opt_binlog_engine_hton->get_filename)(m_engine_active_name,
                                            active_file_no);
    m_engine_entry=
      (*opt_binlog_engine_hton->get_binlog_file_list)(thd->mem_root);
    if (!m_engine_entry)
      m_error= LOG_INFO_EOF;
  }
  else
    m_error= mysql_bin_log.find_log_pos(&m_linfo, NullS, true);
}


const char *
Gtid_binlog_file_iterator::name() const
{
  return m_engine ? m_engine_entry->name.str : m_linfo.log_file_name;
}


int
Gtid_binlog_file_iterator::next()
{
  if (m_error)
    return m_error;
  if (m_engine)
  {
    const char *base= strrchr(m_engine_entry->name.str, FN_LIBCHAR);
    base= base ? base + 1 : m_engine_entry->name.str;
    if (!strcmp(base, m_engine_active_name))
    {
      m_engine_entry= NULL;
      m_error= LOG_INFO_EOF;
      return m_error;
    }
    m_engine_entry= m_engine_entry->next;
    m_error= m_engine_entry ? 0 : LOG_INFO_EOF;
  }
  else
    m_error= mysql_bin_log.find_next_log(&m_linfo, true);
  return m_error;
}
