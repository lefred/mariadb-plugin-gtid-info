# Copyright (c) 2026 MariaDB Corporation.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

cmake_minimum_required(VERSION 3.12)

project(disk_size_info)

set(PLUGIN_NAME "gtif_info")
set(PLUGIN_AUTHOR "lefred")
set(PLUGIN_VERSION "0.2.2")
set(PLUGIN_HEX_VERSION "0x000202")
set(PLUGIN_DESCRIPTION "MariaDB plugin that adds small utility SQL function")
set(PLUGIN_LICENSE "PLUGIN_LICENSE_GPL")

set(TYPE_PLUGIN_SOURCES
  gtid_info.cc gtid_flashback.cc gtid_binlog_reader.cc
)

#
# New MariaDB external plugin infrastructure.
# QUIET is important so older MariaDB versions can fall back to
# MYSQL_ADD_PLUGIN().
#
find_package(mariadb-plugin QUIET)

if(COMMAND MARIADB_ADD_PLUGIN)

    message(STATUS "Building ${PLUGIN_NAME} using MARIADB_ADD_PLUGIN")

    MARIADB_ADD_PLUGIN(${PLUGIN_NAME}
        ${TYPE_PLUGIN_SOURCES}
        MODULE_ONLY
        RECOMPILE_FOR_EMBEDDED
        AUTHOR "${PLUGIN_AUTHOR}"
        VERSION "${PLUGIN_VERSION}"
        DESCRIPTION "${PLUGIN_DESCRIPTION}"
    )

elseif(COMMAND MYSQL_ADD_PLUGIN)

    message(STATUS "Building ${PLUGIN_NAME} using legacy MYSQL_ADD_PLUGIN")

    MYSQL_ADD_PLUGIN(${PLUGIN_NAME}
        ${TYPE_PLUGIN_SOURCES}
        MODULE_ONLY
        RECOMPILE_FOR_EMBEDDED
    )

    #
    # MARIADB_ADD_PLUGIN provides these definitions automatically.
    # Legacy MYSQL_ADD_PLUGIN does not, so provide them explicitly.
    #
    target_compile_definitions(${PLUGIN_NAME} PRIVATE
        PLUGIN_AUTHOR="${PLUGIN_AUTHOR}"
        PLUGIN_DESCRIPTION="${PLUGIN_DESCRIPTION}"
        PLUGIN_VERSION="${PLUGIN_VERSION}"
        PLUGIN_HEX_VERSION=${PLUGIN_HEX_VERSION}
        PLUGIN_LICENSE=${PLUGIN_LICENSE}
    )

else()

    message(FATAL_ERROR
        "Cannot build ${PLUGIN_NAME}: neither MARIADB_ADD_PLUGIN nor "
        "MYSQL_ADD_PLUGIN is available."
    )

endif()

include(CPack)


