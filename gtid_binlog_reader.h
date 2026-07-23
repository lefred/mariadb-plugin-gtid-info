/* Common reader for file and storage-engine binary logs. */
#ifndef GTID_BINLOG_READER_INCLUDED
#define GTID_BINLOG_READER_INCLUDED

#include "log.h"

class Format_description_log_event;
class handler_binlog_reader;
class Log_event;

class Gtid_binlog_reader
{
  IO_CACHE m_log;
  File m_file;
  handler_binlog_reader *m_engine_reader;
  String m_packet;
  bool m_engine;

public:
  Gtid_binlog_reader();
  ~Gtid_binlog_reader();

  int open(THD *thd, const char *log_name);
  Log_event *read(THD *thd, Format_description_log_event *fdle,
                  int *read_error);
  my_off_t position() const;
};


class Gtid_binlog_file_iterator
{
  LOG_INFO m_linfo;
  binlog_file_entry *m_engine_entry;
  char m_engine_active_name[FN_REFLEN];
  int m_error;
  bool m_engine;

public:
  explicit Gtid_binlog_file_iterator(THD *thd);

  bool valid() const { return m_error == 0; }
  bool eof() const { return m_error == LOG_INFO_EOF; }
  const char *name() const;
  int next();
};

#endif
