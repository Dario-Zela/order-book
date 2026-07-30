# Hot-path code is header-only (templates: visitor parser, SPSC ring, listener
# plumbing). ob::lib carries the include dir plus the out-of-line TUs.
add_library(ob_lib STATIC
  itch/mmap_file.cpp
  net/udp.cpp
)
add_library(ob::lib ALIAS ob_lib)
target_include_directories(ob_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_lib PRIVATE ob::warnings)

add_subdirectory(tools)
