cmake_minimum_required(VERSION 3.16)
project(harp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Warnings + math lib: MSVC speaks neither -Wall/-Wextra nor a separate libm
# (math lives in the CRT). Keep the GNU/Clang flags for every other toolchain.
if(MSVC)
  add_compile_options(/W4 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  set(HARP_MATH_LIB "")
else()
  add_compile_options(-Wall -Wextra)
  set(HARP_MATH_LIB m)
endif()

# Platform shim (harp/plat.h): monotonic clock, hi-res sleep, UTC breakdown.
# Compiled into the host/shell targets — kept out of dependency-free harpcore.
if(WIN32)
  set(HARP_PLAT_SRC core/src/plat_win.c)
  set(HARP_PLAT_LIBS winmm)
else()
  set(HARP_PLAT_SRC core/src/plat_posix.c)
  set(HARP_PLAT_LIBS "")
endif()

# Fuzz tree (spec §16 / T9): cmake -B build-fuzz -DHARP_FUZZ=ON.
# ASan goes tree-wide (every binary in this tree is sanitized — the abuse
# test runs the sanitized daemon); the libFuzzer engine is added per
# fuzz target below, on toolchains that ship it.
option(HARP_FUZZ "Build fuzz targets for the wire-facing parsers" OFF)
if(HARP_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "HARP_FUZZ needs clang")
  endif()
  add_compile_options(-fsanitize=address -g -O1)
  add_link_options(-fsanitize=address)
endif()

add_library(harpcore STATIC
  core/src/cbor.c
  core/src/sha256.c
  core/src/frame.c
  core/src/object.c
  core/src/store.c
  core/src/envelope.c
  core/src/link.c
  core/src/audio.c
)
target_include_directories(harpcore PUBLIC core/include)

# harp-deviced is the reference device daemon — it runs on the Pi (FunctionFS
# gadget) or any POSIX host (TCP simulator). On Windows it builds as a localhost
# fake-hardware target for the §8.7 IP e2e (win.yml), compiled with MinGW so its
# gnu11 dialect + pthreads (winpthreads) and clock_gettime are native: the POSIX-only
# AF_UNIX front panel is gated out, and the accept/audio paths use recv/send
# (host/sock_io.c) since Winsock SOCKETs reject fd read/write.
# harpdevice: the reusable device-protocol library — the §5/§9/§11 session loop +
# content-addressed state, with NO CPU-synth coupling. session.c/state.c reach the
# engine only through the seam (g_params[] + engine_*, device.h) and the front panel
# only through front_panel_set — so a downstream daemon (e.g. a GPU synth) links this
# lib and supplies its own engine + panel. harp-deviced is just one such daemon.
add_library(harpdevice STATIC
  device/state.c        # engine <-> content-addressed objects
  device/session.c      # protocol handlers + session loop
  device/audio_loop.c   # the §8.7/§8.3 render+emit loop (render_output seam)
)
target_include_directories(harpdevice PUBLIC device host)
target_link_libraries(harpdevice PUBLIC harpcore ${HARP_MATH_LIB})

add_executable(harp-deviced
  device/harp-deviced.c # main + transports
  device/engine.c       # the refdev synth = the engine seam impl (g_params[] + engine_*)
  host/rtp.c            # §8.7 RTP packetization (--rtp-out emit)
)
target_include_directories(harp-deviced PRIVATE host)
target_link_libraries(harp-deviced harpdevice)
if(WIN32)
  # On Windows the device daemon is a MinGW build (winpthreads + winsock); the MSVC
  # target is the plugin/host, not this. panel.c (AF_UNIX) is gated out — POSIX-only;
  # session.c's reconcile symbols come from the tiny panel_stub_win.c. sock_io = the
  # recv/send accept path (harp_io_fd's _read/_write don't work on Winsock SOCKETs).
  target_sources(harp-deviced PRIVATE device/panel_stub_win.c host/sock_io.c)
  target_link_libraries(harp-deviced ws2_32)
else()
  find_package(Threads REQUIRED)
  target_sources(harp-deviced PRIVATE device/panel.c) # front-panel unix-socket API
  target_link_libraries(harp-deviced Threads::Threads)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(harp-deviced PRIVATE device/ffs.c device/ffs_link.c) # USB gadget transport (§4.3)
  endif()
endif()

add_executable(harp-probe host/harp-probe.c host/client.c host/mdns.c ${HARP_PLAT_SRC})
target_link_libraries(harp-probe harpcore ${HARP_MATH_LIB} ${HARP_PLAT_LIBS})
if(WIN32)
  target_link_libraries(harp-probe ws2_32) # Winsock for the TCP dev transport
endif()
# §4.4.3 mDNS discovery (`harp-probe discover`): dns_sd is native on macOS (libSystem) and
# provided by avahi-compat's libdns_sd on Linux. Where neither header nor lib is present
# (bare CI), the discover command compiles to a clear stub — no build break.
include(CheckIncludeFile)
check_include_file(dns_sd.h HARP_HAVE_DNS_SD_HDR)
if(HARP_HAVE_DNS_SD_HDR)
  if(APPLE)
    target_compile_definitions(harp-probe PRIVATE HAVE_DNS_SD)
    message(STATUS "harp-probe: dns_sd (macOS native), mDNS discover enabled")
  else()
    find_library(HARP_DNSSD_LIB dns_sd)
    if(HARP_DNSSD_LIB)
      target_compile_definitions(harp-probe PRIVATE HAVE_DNS_SD)
      target_link_libraries(harp-probe ${HARP_DNSSD_LIB})
      message(STATUS "harp-probe: dns_sd (${HARP_DNSSD_LIB}), mDNS discover enabled")
    endif()
  endif()
endif()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBUSB libusb-1.0)
  pkg_check_modules(SAMPLERATE samplerate)
endif()
if(NOT LIBUSB_FOUND)
  # No pkg-config (typical on Windows/MSVC): try a vendored prebuilt drop under
  # external/libusb-win, then any system install. Sets the same LIBUSB_* vars.
  # harp-probe (CLI) links the import lib and ships libusb-1.0.dll next to the
  # exe; the VST3 shell statically links libusb (tools/vst3-host) so the plugin
  # bundle is self-contained — that's why it hints a different MS64 sub-dir.
  find_path(LIBUSB_INCLUDE_DIRS
    NAMES libusb.h
    PATH_SUFFIXES libusb-1.0
    HINTS ${CMAKE_SOURCE_DIR}/external/libusb-win/include)
  find_library(LIBUSB_LIBRARIES
    NAMES libusb-1.0 usb-1.0
    HINTS ${CMAKE_SOURCE_DIR}/external/libusb-win/VS2022/MS64/dll
          ${CMAKE_SOURCE_DIR}/external/libusb-win/MS64/dll)
  find_file(HARP_LIBUSB_DLL
    NAMES libusb-1.0.dll
    HINTS ${CMAKE_SOURCE_DIR}/external/libusb-win/VS2022/MS64/dll
          ${CMAKE_SOURCE_DIR}/external/libusb-win/MS64/dll)
  if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
    set(LIBUSB_FOUND TRUE)
  endif()
endif()
if(LIBUSB_FOUND)
  target_sources(harp-probe PRIVATE host/usb_io.c)
  target_compile_definitions(harp-probe PRIVATE HAVE_LIBUSB)
  target_include_directories(harp-probe PRIVATE ${LIBUSB_INCLUDE_DIRS})
  if(LIBUSB_LIBRARY_DIRS)
    target_link_directories(harp-probe PRIVATE ${LIBUSB_LIBRARY_DIRS})
  endif()
  target_link_libraries(harp-probe ${LIBUSB_LIBRARIES})
  message(STATUS "harp-probe: libusb found, USB transport enabled")
  # Windows: drop the libusb DLL next to the exe so it runs from the build tree.
  if(WIN32 AND HARP_LIBUSB_DLL)
    add_custom_command(TARGET harp-probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${HARP_LIBUSB_DLL}" "$<TARGET_FILE_DIR:harp-probe>")
  endif()
endif()

enable_testing()
add_executable(harp-tests tests/harp_tests.c)
target_link_libraries(harp-tests harpcore ${HARP_MATH_LIB})
add_test(NAME core COMMAND harp-tests)
set_tests_properties(core PROPERTIES LABELS unit)

# harp-engine-logic-tests: the determinism-critical PURE policies factored out of
# engine.c (device/voice_alloc.h §9.5 allocation, device/arp_select.h §9.7 arp
# note selection) — header-only, no device.h/daemon deps, so they unit-test on
# every OS without a board. engine.c calls these same functions, so this is the
# real logic; the chord/groove golden hashes pin it byte-identical on hardware.
add_executable(harp-engine-logic-tests tests/harp_engine_logic_tests.c)
target_include_directories(harp-engine-logic-tests PRIVATE device host)
add_test(NAME engine_logic COMMAND harp-engine-logic-tests)
set_tests_properties(engine_logic PROPERTIES LABELS unit)

# harp-ffs-link-tests: the §4.3 FunctionFS framing I/O (device/ffs_link.c) over a
# socketpair — the USB transport's mockable part, run in CI with no kernel gadget
# (the descriptors / UDC / ep0 loop in ffs.c stay Pi-only). ffs_link.c is portable
# (no functionfs.h), so this builds on Linux AND macOS; POSIX only (socketpair).
if(NOT WIN32)
  add_executable(harp-ffs-link-tests tests/harp_ffs_link_tests.c device/ffs_link.c)
  target_include_directories(harp-ffs-link-tests PRIVATE device core/include)
  add_test(NAME ffs_link COMMAND harp-ffs-link-tests)
  set_tests_properties(ffs_link PROPERTIES LABELS unit)

  # harp-eth-latefr-test: §8.2 host-paced late-frame discard — audio.start(key 7),
  # accepts the device's host-paced connect-back, sends a rewound SSI frame, asserts it
  # is discarded + counted (audio_late_frames). POSIX-only (raw server socket); it needs
  # NO libsamplerate, so it lives here, not in the SAMPLERATE_FOUND eth-tools block.
  add_executable(harp-eth-latefr-test tools/eth-latefr-test.c
    host/client.c host/sock_io.c ${HARP_PLAT_SRC})
  target_include_directories(harp-eth-latefr-test PRIVATE host)
  target_link_libraries(harp-eth-latefr-test harpcore
    ${HARP_MATH_LIB} ${HARP_PLAT_LIBS} Threads::Threads)
endif()

# harp-device-tests: unit tests for the params-blob codec (§10 / P3 closer
# step 3). Links the REAL pure codec from device/state.c — not a fork — so a
# daemon serializer regression is caught. state.c shares its TU with engine/
# store-coupled functions and pulls device.h (pthread/POSIX), so it can't live
# in the cross-platform, harpcore-only harp-tests; this is a separate POSIX-only
# device-linked test. It reuses fuzz/state_stubs.c — the SAME real g_params[] id
# table + abort-stubs the fuzz-state target uses — so there's one place to track
# id drift, not two. device/state.c pulls pthread, but winpthreads provides it
# under MinGW (harp-deviced already cross-compiles state.c on Windows), so this
# runs on Linux, macOS, AND the MinGW Windows build — exactly so a MinGW-vs-clang
# codec divergence in the params-blob parser the Windows daemon ships can't slip
# by. Guard is NOT MSVC (not NOT WIN32): ci.yml's shell-windows configures this
# root tree with MSVC and runs ctest, but state.c isn't MSVC-built there — so the
# target must be absent under MSVC or ctest would fail on a never-built executable.
if(NOT MSVC)
  find_package(Threads REQUIRED)
  add_executable(harp-device-tests
    tests/harp_device_tests.c
    device/state.c
    fuzz/state_stubs.c
  )
  target_include_directories(harp-device-tests PRIVATE device)
  target_link_libraries(harp-device-tests harpcore ${HARP_MATH_LIB} Threads::Threads)
  add_test(NAME device_codec COMMAND harp-device-tests)
  set_tests_properties(device_codec PROPERTIES LABELS unit)
endif()

# harp-freerun-tests: the §7.3/§8.7 free-running receiver — clock recovery +
# libsamplerate ASRC (host/freerun.c). Needs libsamplerate; skipped (with a
# notice) where it is absent so a bare build still configures.
if(SAMPLERATE_FOUND)
  add_executable(harp-freerun-tests tests/harp_freerun_tests.c host/freerun.c)
  target_include_directories(harp-freerun-tests PRIVATE host ${SAMPLERATE_INCLUDE_DIRS})
  if(SAMPLERATE_LIBRARY_DIRS)
    target_link_directories(harp-freerun-tests PRIVATE ${SAMPLERATE_LIBRARY_DIRS})
  endif()
  target_link_libraries(harp-freerun-tests ${SAMPLERATE_LIBRARIES} ${HARP_MATH_LIB})
  add_test(NAME freerun COMMAND harp-freerun-tests)

  # harp-rtp-tests: the §8.7 RTP codec (host/rtp.c) + end-to-end through the
  # free-running receiver (pack -> unpack -> recover -> resample).
  add_executable(harp-rtp-tests tests/harp_rtp_tests.c host/rtp.c host/freerun.c)
  target_include_directories(harp-rtp-tests PRIVATE host ${SAMPLERATE_INCLUDE_DIRS})
  if(SAMPLERATE_LIBRARY_DIRS)
    target_link_directories(harp-rtp-tests PRIVATE ${SAMPLERATE_LIBRARY_DIRS})
  endif()
  target_link_libraries(harp-rtp-tests ${SAMPLERATE_LIBRARIES} ${HARP_MATH_LIB})
  add_test(NAME rtp COMMAND harp-rtp-tests)

  # harp-rtp-demo: runnable §8.7 RTP-over-UDP path (loopback or KR260 -> Mac).
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(harp-rtp-demo tools/rtp-demo.c host/rtp_udp.c host/rtp.c host/freerun.c)
    target_include_directories(harp-rtp-demo PRIVATE host ${SAMPLERATE_INCLUDE_DIRS})
    if(SAMPLERATE_LIBRARY_DIRS)
      target_link_directories(harp-rtp-demo PRIVATE ${SAMPLERATE_LIBRARY_DIRS})
    endif()
    target_link_libraries(harp-rtp-demo ${SAMPLERATE_LIBRARIES} ${HARP_MATH_LIB} Threads::Threads)

    # harp-eth-rtp-test: end-to-end §8.7 Ethernet audio plane — dial the device's
    # TCP framed link, hello, audio.start(key 6 = our RTP port), recover the
    # device's RTP stream + report SINAD. The Ethernet analog of golden-test.sh
    # (drive it against a TCP-serving harp-deviced).
    add_executable(harp-eth-rtp-test tools/eth-rtp-test.c
      host/client.c host/sock_io.c host/rtp_udp.c host/rtp.c host/freerun.c ${HARP_PLAT_SRC})
    target_include_directories(harp-eth-rtp-test PRIVATE host ${SAMPLERATE_INCLUDE_DIRS})
    if(SAMPLERATE_LIBRARY_DIRS)
      target_link_directories(harp-eth-rtp-test PRIVATE ${SAMPLERATE_LIBRARY_DIRS})
    endif()
    target_link_libraries(harp-eth-rtp-test harpcore ${SAMPLERATE_LIBRARIES}
      ${HARP_MATH_LIB} ${HARP_PLAT_LIBS} Threads::Threads)

    # harp-eth-bitexact-test: the §8.7 BIT-EXACT (host-locked) path — plays the
    # device's RTP 1:1 (no resampler, hence no libsamplerate) and feeds back
    # audio.trim so the device emits at the host's rate. Proves bit-exact.
    add_executable(harp-eth-bitexact-test tools/eth-bitexact-test.c
      host/client.c host/sock_io.c host/rtp.c ${HARP_PLAT_SRC})
    target_include_directories(harp-eth-bitexact-test PRIVATE host)
    target_link_libraries(harp-eth-bitexact-test harpcore
      ${HARP_MATH_LIB} ${HARP_PLAT_LIBS} Threads::Threads)

    # harp-freerun-mt-tests: concurrent SPSC stress under ThreadSanitizer — proves
    # freerun is race-free for the runtime's rx-thread-push / audio-thread-pull
    # topology. TSan-instrumented (this target only) so it aborts on any data race.
    add_executable(harp-freerun-mt-tests tests/harp_freerun_mt_tests.c host/freerun.c)
    target_include_directories(harp-freerun-mt-tests PRIVATE host ${SAMPLERATE_INCLUDE_DIRS})
    target_compile_options(harp-freerun-mt-tests PRIVATE -fsanitize=thread -g -O1)
    target_link_options(harp-freerun-mt-tests PRIVATE -fsanitize=thread)
    if(SAMPLERATE_LIBRARY_DIRS)
      target_link_directories(harp-freerun-mt-tests PRIVATE ${SAMPLERATE_LIBRARY_DIRS})
    endif()
    target_link_libraries(harp-freerun-mt-tests ${SAMPLERATE_LIBRARIES} ${HARP_MATH_LIB} Threads::Threads)
    add_test(NAME freerun_mt COMMAND harp-freerun-mt-tests)
  endif()
else()
  message(STATUS "libsamplerate not found -> harp-freerun-tests skipped")
endif()

# Fuzz targets. Apple clang ships no libFuzzer runtime -> a standalone
# corpus-replay driver stands in there; coverage-guided runs happen on
# Linux (CI). gen-corpus seeds both with valid wire bytes.
if(HARP_FUZZ)
  if(APPLE)
    set(FUZZ_DRIVER fuzz/standalone_driver.c)
  else()
    set(FUZZ_DRIVER "")
    target_compile_options(harpcore PRIVATE -fsanitize=fuzzer-no-link)
  endif()
  foreach(t cbor envelope object link audio)
    add_executable(fuzz-${t} fuzz/fuzz_${t}.c ${FUZZ_DRIVER})
    target_link_libraries(fuzz-${t} harpcore m)
    if(NOT APPLE)
      target_compile_options(fuzz-${t} PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz-${t} PRIVATE -fsanitize=fuzzer)
    endif()
  endforeach()

  # fuzz-state: the params-blob codec (§10 / P3 closer). Unlike the harpcore
  # parsers above, refdev_parse_params_blob lives in device/state.c, so this
  # target compiles that TU directly (the real production code, not a copy)
  # plus fuzz/state_stubs.c, which supplies the one datum the codec reads
  # (g_params) and abort-stubs for the engine/session symbols state.c's other
  # functions reference — keeping the audio threads / pthread state out of a
  # pure-parser fuzzer. Needs the device/ include dir for device.h.
  add_executable(fuzz-state fuzz/fuzz_state.c device/state.c fuzz/state_stubs.c ${FUZZ_DRIVER})
  target_link_libraries(fuzz-state harpcore m)
  target_include_directories(fuzz-state PRIVATE device)
  if(NOT APPLE)
    target_compile_options(fuzz-state PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz-state PRIVATE -fsanitize=fuzzer)
  endif()

  add_executable(gen-corpus fuzz/gen_corpus.c)
  target_link_libraries(gen-corpus harpcore m)
endif()
