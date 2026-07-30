cmake_minimum_required(VERSION 3.24)
project(order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OB_BUILD_TESTS "Build unit tests" ON)
option(OB_BUILD_BENCH "Build benchmarks" OFF)

# Scoped to our targets (ob::warnings) so FetchContent deps build clean.
add_library(ob_warnings INTERFACE)
add_library(ob::warnings ALIAS ob_warnings)
target_compile_options(ob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# Sanitizer toggles driven by presets
set(OB_SANITIZE "" CACHE STRING "Sanitizer set: address+undefined, thread, or empty")
if(OB_SANITIZE STREQUAL "address+undefined")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(OB_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

add_subdirectory(src)

if(OB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OB_BUILD_BENCH)
  add_subdirectory(src/bench)
endif()

option(OB_BUILD_FUZZ "Build fuzz targets" OFF)
if(OB_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
