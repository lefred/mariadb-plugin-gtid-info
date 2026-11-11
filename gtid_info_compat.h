/* Copyright (c) 2026 lefred.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

/*
  Compatibility shims across MariaDB 12.3+ server builds.
*/
#ifndef GTID_INFO_COMPAT_INCLUDED
#define GTID_INFO_COMPAT_INCLUDED

#include <mysql_version.h>

#if MYSQL_VERSION_ID < 120300
#error "gtid_info requires MariaDB 12.3 or later"
#endif

namespace gtid_info_compat {

/*
  table_def::calc_field_size() gained a bounds argument
  ("size_t available") in later 12.3.4+ builds. Earlier builds only take
  (col, master_data). Pick whichever exists.
*/
template <typename TD>
static auto calc_field_size_impl(const TD *td, uint col, uchar *data,
                                 size_t available, int)
  -> decltype(td->calc_field_size(col, data, available))
{
  return td->calc_field_size(col, data, available);
}

template <typename TD>
static uint32 calc_field_size_impl(const TD *td, uint col, uchar *data,
                                   size_t, long)
{
  return td->calc_field_size(col, data);
}

template <typename TD>
static inline uint32 calc_field_size(const TD *td, uint col, uchar *data,
                                     size_t available)
{
  return calc_field_size_impl(td, col, data, available, 0);
}

} // namespace gtid_info_compat

#endif
