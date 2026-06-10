cmake_minimum_required(VERSION 3.16)
project(harp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

add_library(harpcore STATIC
  core/src/cbor.c
  core/src/sha256.c
  core/src/frame.c
  core/src/object.c
  core/src/store.c
  core/src/envelope.c
  core/src/link.c
)
target_include_directories(harpcore PUBLIC core/include)

add_executable(harp-deviced device/harp-deviced.c)
target_link_libraries(harp-deviced harpcore m)

add_executable(harp-probe host/harp-probe.c)
target_link_libraries(harp-probe harpcore m)

enable_testing()
add_executable(harp-tests tests/harp_tests.c)
target_link_libraries(harp-tests harpcore m)
add_test(NAME core COMMAND harp-tests)
