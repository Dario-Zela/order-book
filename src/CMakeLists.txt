# Hot-path code is header-only (templates: visitor parser, SPSC ring, listener
# plumbing). ob::lib carries the include dir; out-of-line TUs (mmap reader)
# join as they land.
add_library(ob_lib INTERFACE)
add_library(ob::lib ALIAS ob_lib)
target_include_directories(ob_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
