# harp-vst3-host — minimal CLI VST3 host for automated, agent-driven testing.
# Builds the VST3 SDK (hosting parts + example plugins for self-validation).
cmake_minimum_required(VERSION 3.20)
project(harp-vst3-host VERSION 0.1.0 LANGUAGES CXX C)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS) # harp C sources use getenv/snprintf
endif()

set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES ON CACHE BOOL "" FORCE) # adelay = test subject
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE) # no ~/Library symlink spam
# Skip the SDK's post-build validator: it loads the plugin before our libusb DLL
# is copied beside it, and we validate the shell with harp-vst3-host + a real DAW.
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk vst3sdk EXCLUDE_FROM_ALL)

set(SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk)
add_executable(harp-vst3-host main.cpp
  ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
  ${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp)
if(APPLE)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm)
  set_source_files_properties(${SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
elseif(WIN32)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif(UNIX)
  target_sources(harp-vst3-host PRIVATE ${SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
target_link_libraries(harp-vst3-host PRIVATE sdk_hosting sdk_common base pluginterfaces)
if(APPLE)
  target_link_libraries(harp-vst3-host PRIVATE "-framework CoreFoundation" "-framework Foundation")
endif()
target_include_directories(harp-vst3-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../external/vst3sdk ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ---- ring unit + stress tests (no SDK, no hardware) ----
add_executable(ring-tests ring_tests.cpp)
target_include_directories(ring-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../shell)
enable_testing()
add_test(NAME rings COMMAND ring-tests)

# ---- the HARP shell plugin (shell/) — needs libusb ----
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
endif()
if(NOT LIBUSB_FOUND)
  # No pkg-config (typical on Windows/MSVC): vendored prebuilt under
  # external/libusb-win. Use the STATIC lib so the plugin is fully self-contained
  # — no libusb DLL to place beside the plugin, the host, AND the SDK's
  # moduleinfo/validator tools (all of which LoadLibrary() the plugin at build).
  set(_HARP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  find_path(LIBUSB_INCLUDE_DIRS NAMES libusb.h PATH_SUFFIXES libusb-1.0
    HINTS ${_HARP_ROOT}/external/libusb-win/include)
  find_library(LIBUSB_LIBRARIES NAMES libusb-1.0 usb-1.0
    HINTS ${_HARP_ROOT}/external/libusb-win/VS2022/MS64/static
          ${_HARP_ROOT}/external/libusb-win/MS64/static)
  if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
    set(LIBUSB_FOUND TRUE)
  endif()
endif()
if(LIBUSB_FOUND)
  set(HARP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  # Platform shim (harp/plat.h) compiled into the shell/AU like the core .c files.
  if(WIN32)
    set(HARP_PLAT_SRC ${HARP_ROOT}/core/src/plat_win.c)
    set(HARP_PLAT_LIBS winmm)
  else()
    set(HARP_PLAT_SRC ${HARP_ROOT}/core/src/plat_posix.c)
    set(HARP_PLAT_LIBS "")
  endif()
  # VST3 plugin entry point is per-platform (macmain/linuxmain/dllmain).
  if(APPLE)
    set(SHELL_MAIN ${SDK_ROOT}/public.sdk/source/main/macmain.cpp)
  elseif(WIN32)
    set(SHELL_MAIN ${SDK_ROOT}/public.sdk/source/main/dllmain.cpp)
  else()
    set(SHELL_MAIN ${SDK_ROOT}/public.sdk/source/main/linuxmain.cpp)
  endif()
  smtg_add_vst3plugin(harp-shell
    ${SHELL_MAIN}
    ${HARP_ROOT}/shell/plugin.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC}
  )
  target_include_directories(harp-shell PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(harp-shell PRIVATE HAVE_LIBUSB)
  target_link_directories(harp-shell PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(harp-shell PRIVATE sdk ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS})
  if(WIN32)
    # static libusb pulls in the Win device-enumeration + COM GUID APIs
    target_link_libraries(harp-shell PRIVATE setupapi ole32)
  endif()
  message(STATUS "harp-shell: building VST3 shell with libusb")

  # tsan-host (debt #17): the shell runtime, statically linked into a TSan
  # main image and driven directly (no plugin boundary — TSan must be in
  # the main image, not a dlopen'd bundle). Built only with -DHARP_TSAN=ON;
  # run against a real device via scripts/tsan-shell.sh.
  option(HARP_TSAN "Build tsan-host: ThreadSanitize the shell runtime" OFF)
  if(HARP_TSAN)
    add_executable(tsan-host
      ${CMAKE_CURRENT_SOURCE_DIR}/../tsan-host/main.cpp
      ${HARP_ROOT}/shell/runtime.cpp
      ${HARP_ROOT}/shell/runtime_registry.cpp
      ${HARP_ROOT}/host/usb_io.c
      ${HARP_ROOT}/host/client.c
      ${HARP_ROOT}/core/src/cbor.c
      ${HARP_ROOT}/core/src/sha256.c
      ${HARP_ROOT}/core/src/frame.c
      ${HARP_ROOT}/core/src/object.c
      ${HARP_ROOT}/core/src/store.c
      ${HARP_ROOT}/core/src/envelope.c
      ${HARP_ROOT}/core/src/link.c
      ${HARP_ROOT}/core/src/audio.c
      ${HARP_PLAT_SRC})
    target_include_directories(tsan-host PRIVATE
      ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
      ${LIBUSB_INCLUDE_DIRS})
    target_compile_definitions(tsan-host PRIVATE HAVE_LIBUSB)
    target_compile_options(tsan-host PRIVATE -fsanitize=thread -g -O1)
    target_link_options(tsan-host PRIVATE -fsanitize=thread)
    if(LIBUSB_LIBRARY_DIRS)
      target_link_directories(tsan-host PRIVATE ${LIBUSB_LIBRARY_DIRS})
    endif()
    target_link_libraries(tsan-host ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS})
    message(STATUS "tsan-host: ThreadSanitizer harness for the shell runtime")
  endif()

  # registry-tests (P4): the process-global runtime registry's keying +
  # refcount logic (shell/runtime_registry.*), unit-tested WITHOUT a device.
  # The registry never start()s a runtime, and an un-started HarpRuntime opens
  # no USB and spawns no threads (ctor = on-disk store only; stop()/dtor early-
  # return), so acquire()/release() exercise the table bookkeeping against real
  # HarpRuntime objects with zero hardware — no test hook in shell/* needed.
  # It still links runtime.cpp (the registry news/deletes it) and thus the C
  # core/host sources + libusb runtime.cpp references, so it lives here in the
  # LIBUSB_FOUND block beside tsan-host rather than in the harpcore-only
  # harp-tests. Runs in plain CI (ctest) — it is the cloud-side complement to
  # the session-share HARDWARE test, which proves the same sharing on the bus.
  add_executable(registry-tests
    ${HARP_ROOT}/tests/registry_tests.cpp
    ${HARP_ROOT}/shell/runtime.cpp
    ${HARP_ROOT}/shell/runtime_registry.cpp
    ${HARP_ROOT}/host/usb_io.c
    ${HARP_ROOT}/host/client.c
    ${HARP_ROOT}/core/src/cbor.c
    ${HARP_ROOT}/core/src/sha256.c
    ${HARP_ROOT}/core/src/frame.c
    ${HARP_ROOT}/core/src/object.c
    ${HARP_ROOT}/core/src/store.c
    ${HARP_ROOT}/core/src/envelope.c
    ${HARP_ROOT}/core/src/link.c
    ${HARP_ROOT}/core/src/audio.c
    ${HARP_PLAT_SRC})
  target_include_directories(registry-tests PRIVATE
    ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
    ${LIBUSB_INCLUDE_DIRS})
  target_compile_definitions(registry-tests PRIVATE HAVE_LIBUSB)
  target_link_directories(registry-tests PRIVATE ${LIBUSB_LIBRARY_DIRS})
  target_link_libraries(registry-tests ${LIBUSB_LIBRARIES} ${HARP_PLAT_LIBS})
  if(WIN32)
    target_link_libraries(registry-tests setupapi ole32) # static libusb deps
  endif()
  add_test(NAME registry COMMAND registry-tests)
  message(STATUS "registry-tests: runtime registry keying/refcount unit test")

  # SMTG emits the bundle under VST3/<config>/ ($<CONFIG> = multi-config safe).
  set(HARP_SHELL_BUNDLE ${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/harp-shell.vst3)
  if(APPLE)
    # SMTG signs at link, then moduleinfotool rewrites Resources and breaks the
    # seal ("code has no resources but signature indicates they must be present"
    # — Live refuses such bundles). Re-sign as the last step.
    add_custom_command(TARGET harp-shell POST_BUILD
      COMMAND codesign --force --deep -s - "${HARP_SHELL_BUNDLE}"
      COMMENT "Re-sealing harp-shell.vst3 signature")
  endif()

  # `cmake --build ... --target install-live` -> the OS VST3 folder. Uses
  # cmake -E (cross-platform) instead of rm/cp/mkdir. On Windows the system
  # VST3 dir lives under Program Files, so this may need an elevated shell.
  if(WIN32)
    set(HARP_VST3_DEST "$ENV{CommonProgramW6432}/VST3")
  elseif(APPLE)
    set(HARP_VST3_DEST "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
  else()
    set(HARP_VST3_DEST "$ENV{HOME}/.vst3")
  endif()
  if(NOT HARP_VST3_DEST OR HARP_VST3_DEST MATCHES "^/+VST3$")
    # the env var was unset — fall back to a build-local dir rather than risk
    # `rm -rf` against a bogus root path.
    set(HARP_VST3_DEST "${CMAKE_BINARY_DIR}/dist/VST3")
  endif()
  add_custom_target(install-live
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${HARP_VST3_DEST}/harp-shell.vst3"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HARP_VST3_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${HARP_SHELL_BUNDLE}" "${HARP_VST3_DEST}/harp-shell.vst3"
    DEPENDS harp-shell
    COMMENT "Installing harp-shell.vst3 to ${HARP_VST3_DEST}")
  if(APPLE)
    add_custom_command(TARGET install-live POST_BUILD
      COMMAND codesign --force --deep -s - "${HARP_VST3_DEST}/harp-shell.vst3")
  endif()
  # Back-compat alias referenced by docs/CI.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(install-linux DEPENDS install-live)
  endif()

  # ---- the Audio Unit shell (macOS): same runtime, second format ----
  if(APPLE)
    add_library(harp-au MODULE
      ${HARP_ROOT}/shell/au/harp_au.mm
      ${HARP_ROOT}/shell/runtime.cpp
      ${HARP_ROOT}/host/usb_io.c
      ${HARP_ROOT}/host/client.c
      ${HARP_ROOT}/core/src/cbor.c
      ${HARP_ROOT}/core/src/sha256.c
      ${HARP_ROOT}/core/src/frame.c
      ${HARP_ROOT}/core/src/object.c
      ${HARP_ROOT}/core/src/store.c
      ${HARP_ROOT}/core/src/envelope.c
      ${HARP_ROOT}/core/src/link.c
      ${HARP_ROOT}/core/src/audio.c
      ${HARP_PLAT_SRC})
    target_include_directories(harp-au PRIVATE
      ${HARP_ROOT}/core/include ${HARP_ROOT}/host ${HARP_ROOT}/shell
      ${LIBUSB_INCLUDE_DIRS})
    # os_workgroup_t must mean the same thing in runtime.cpp (C++) and
    # harp_au.mm (ObjC++): force the C-struct flavor of OS objects
    target_compile_definitions(harp-au PRIVATE HAVE_LIBUSB OS_OBJECT_USE_OBJC=0)
    target_link_directories(harp-au PRIVATE ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(harp-au PRIVATE ${LIBUSB_LIBRARIES}
      "-framework AudioToolbox" "-framework CoreFoundation" "-framework Foundation")
    set_target_properties(harp-au PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION "component"
      OUTPUT_NAME "harp-au"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/../au-host/au-info.plist.in)

    add_executable(au-host ${CMAKE_CURRENT_SOURCE_DIR}/../au-host/main.mm)
    target_include_directories(au-host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(au-host PRIVATE
      "-framework AudioToolbox" "-framework CoreFoundation")

    add_custom_target(install-au
      COMMAND rm -rf "$ENV{HOME}/Library/Audio/Plug-Ins/Components/harp-au.component"
      COMMAND cp -R "$<TARGET_BUNDLE_DIR:harp-au>"
              "$ENV{HOME}/Library/Audio/Plug-Ins/Components/"
      COMMAND codesign --force --deep -s -
              "$ENV{HOME}/Library/Audio/Plug-Ins/Components/harp-au.component"
      DEPENDS harp-au
      COMMENT "Installing harp-au.component")
  endif()
else()
  message(STATUS "harp-shell: libusb not found, shell not built")
endif()
