cmake_minimum_required (VERSION 3.0)

project(ZlibWin64Lib)

set(ZLIB_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include PARENT_SCOPE)

set(ZLIB_LIBRARIES ${CMAKE_CURRENT_SOURCE_DIR}/lib/zlib.lib PARENT_SCOPE)
