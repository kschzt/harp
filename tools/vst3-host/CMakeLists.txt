# harp-vst3-host — minimal CLI VST3 host for automated, agent-driven testing.
# Builds the VST3 SDK (hosting parts + example plugins for self-validation).
cmake_minimum_required(VERSION 3.20)
project(harp-vst3-host CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES ON CACHE BOOL "" FORCE) # adelay = test subject
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE) # no ~/Library symlink spam

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk vst3sdk EXCLUDE_FROM_ALL)

set(SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk)
add_executable(harp-vst3-host main.cpp
  ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
  ${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp)
if(APPLE)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm)
  set_source_files_properties(${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(UNIX)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
target_link_libraries(harp-vst3-host PRIVATE sdk_hosting sdk_common base pluginterfaces)
if(APPLE)
  target_link_libraries(harp-vst3-host PRIVATE "-framework CoreFoundation" "-framework Foundation")
endif()
target_include_directories(harp-vst3-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk)
