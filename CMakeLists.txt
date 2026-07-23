# Copyright (c) 2026 MariaDB Corporation.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

MYSQL_ADD_PLUGIN(gtid_info gtid_info.cc gtid_flashback.cc gtid_binlog_reader.cc
  RECOMPILE_FOR_EMBEDDED
  MODULE_ONLY COMPONENT Server)
