# TODO
# - backend selection via command line, rather than simply detecting headers.

cmake_minimum_required(VERSION 3.1 FATAL_ERROR)
project(cubeb
  VERSION 0.0.0)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

if(POLICY CMP0063)
  cmake_policy(SET CMP0063 NEW)
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DGTEST_HAS_TR1_TUPLE=0)
set(gtest_force_shared_crt ON CACHE BOOL "")
add_subdirectory(googletest)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

set(CMAKE_CXX_WARNING_LEVEL 4)
if(NOT MSVC)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
endif()

add_library(cubeb
  src/cubeb.c
  src/cubeb_resampler.cpp
  src/cubeb_panner.cpp
   $<TARGET_OBJECTS:speex>)
target_include_directories(cubeb PUBLIC include)
target_include_directories(cubeb PRIVATE src)
target_compile_definitions(cubeb PRIVATE OUTSIDE_SPEEX)
target_compile_definitions(cubeb PRIVATE FLOATING_POINT)
target_compile_definitions(cubeb PRIVATE EXPORT=)
target_compile_definitions(cubeb PRIVATE RANDOM_PREFIX=speex)

include(GenerateExportHeader)
generate_export_header(cubeb EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/exports/cubeb_export.h)
target_include_directories(cubeb PUBLIC ${CMAKE_BINARY_DIR}/exports)

add_library(speex OBJECT
  src/speex/resample.c)
set_target_properties(speex PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_compile_definitions(speex PRIVATE OUTSIDE_SPEEX)
target_compile_definitions(speex PRIVATE FLOATING_POINT)
target_compile_definitions(speex PRIVATE EXPORT=)
target_compile_definitions(speex PRIVATE RANDOM_PREFIX=speex)

include(CheckIncludeFiles)

check_include_files(AudioUnit/AudioUnit.h USE_AUDIOUNIT)
if(USE_AUDIOUNIT)
  target_sources(cubeb PRIVATE
    src/cubeb_audiounit.cpp
    src/cubeb_osx_run_loop.cpp)
  target_compile_definitions(cubeb PRIVATE USE_AUDIOUNIT)
  target_link_libraries(cubeb PRIVATE "-framework AudioUnit" "-framework CoreAudio" "-framework CoreServices")
endif()

check_include_files(pulse/pulseaudio.h USE_PULSE)
if(USE_PULSE)
  target_sources(cubeb PRIVATE
    src/cubeb_pulse.c)
  target_compile_definitions(cubeb PRIVATE USE_PULSE)
  target_link_libraries(cubeb PRIVATE pulse dl)
endif()

check_include_files(alsa/asoundlib.h USE_ALSA)
if(USE_ALSA)
  target_sources(cubeb PRIVATE
    src/cubeb_alsa.c)
  target_compile_definitions(cubeb PRIVATE USE_ALSA)
  target_link_libraries(cubeb PRIVATE asound dl pthread)
endif()

check_include_files(jack/jack.h USE_JACK)
if(USE_JACK)
  target_sources(cubeb PRIVATE
    src/cubeb_jack.cpp)
  target_compile_definitions(cubeb PRIVATE USE_JACK)
  target_link_libraries(cubeb PRIVATE jack dl pthread)
endif()

check_include_files(audioclient.h USE_WASAPI)
if(USE_WASAPI)
  target_sources(cubeb PRIVATE
    src/cubeb_wasapi.cpp)
  target_compile_definitions(cubeb PRIVATE USE_WASAPI)
endif()

check_include_files("windows.h;mmsystem.h" USE_WINMM)
if(USE_WINMM)
  target_sources(cubeb PRIVATE
    src/cubeb_winmm.c)
  target_compile_definitions(cubeb PRIVATE USE_WINMM)
  target_link_libraries(cubeb PRIVATE winmm)
endif()

check_include_files(SLES/OpenSLES.h USE_OPENSL)
if(USE_OPENSL)
  target_sources(cubeb PRIVATE
    src/cubeb_opensl.c)
  target_compile_definitions(cubeb PRIVATE USE_OPENSL)
  target_link_libraries(cubeb PRIVATE OpenSLES)
endif()

check_include_files(android/log.h USE_AUDIOTRACK)
if(USE_AUDIOTRACK)
  target_sources(cubeb PRIVATE
    src/cubeb_audiotrack.c)
  target_compile_definitions(cubeb PRIVATE USE_AUDIOTRACK)
  target_link_libraries(cubeb PRIVATE log)
endif()

check_include_files(sndio.h USE_SNDIO)
if(USE_SNDIO)
  target_sources(cubeb PRIVATE
    src/cubeb_sndio.c)
  target_compile_definitions(cubeb PRIVATE USE_SNDIO)
  target_link_libraries(cubeb PRIVATE sndio)
endif()

check_include_files(kai.h USE_KAI)
if(USE_KAI)
  target_sources(cubeb PRIVATE
    src/cubeb_kai.c)
  target_compile_definitions(cubeb PRIVATE USE_KAI)
  target_link_libraries(cubeb PRIVATE kai)
endif()

find_package(Doxygen)
if(DOXYGEN_FOUND)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile @ONLY)
  add_custom_target(doc ALL
    ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs
    COMMENT "Generating API documentation with Doxygen" VERBATIM)
endif()

enable_testing()

add_executable(test_sanity test/test_sanity.cpp)
target_include_directories(test_sanity PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_sanity PRIVATE cubeb gtest_main)
add_test(sanity test_sanity)

add_executable(test_tone test/test_tone.cpp)
target_include_directories(test_tone PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_tone PRIVATE cubeb gtest_main)
add_test(tone test_tone)

add_executable(test_audio test/test_audio.cpp)
target_include_directories(test_audio PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_audio PRIVATE cubeb gtest_main)
add_test(audio test_audio)

add_executable(test_record test/test_record.cpp)
target_include_directories(test_record PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_record PRIVATE cubeb gtest_main)
add_test(record test_record)

add_executable(test_devices test/test_devices.cpp)
target_include_directories(test_devices PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_devices PRIVATE cubeb gtest_main)
add_test(devices test_devices)

add_executable(test_resampler test/test_resampler.cpp src/cubeb_resampler.cpp $<TARGET_OBJECTS:speex>)
target_include_directories(test_resampler PRIVATE ${gtest_SOURCE_DIR}/include)
target_include_directories(test_resampler PRIVATE src)
target_compile_definitions(test_resampler PRIVATE OUTSIDE_SPEEX)
target_compile_definitions(test_resampler PRIVATE FLOATING_POINT)
target_compile_definitions(test_resampler PRIVATE EXPORT=)
target_compile_definitions(test_resampler PRIVATE RANDOM_PREFIX=speex)
target_link_libraries(test_resampler PRIVATE cubeb gtest_main)
add_test(resampler test_resampler)

add_executable(test_duplex test/test_duplex.cpp)
target_include_directories(test_duplex PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_duplex PRIVATE cubeb gtest_main)
add_test(duplex test_duplex)

add_executable(test_latency test/test_latency.cpp)
target_include_directories(test_latency PRIVATE ${gtest_SOURCE_DIR}/include)
target_link_libraries(test_latency PRIVATE cubeb gtest_main)
add_test(latency test_latency)

add_executable(test_ring_array test/test_ring_array.cpp)
target_include_directories(test_ring_array PRIVATE ${gtest_SOURCE_DIR}/include)
target_include_directories(test_ring_array PRIVATE src)
target_link_libraries(test_ring_array PRIVATE cubeb gtest_main)
add_test(ring_array test_ring_array)

add_executable(test_utils test/test_utils.cpp)
target_include_directories(test_utils PRIVATE ${gtest_SOURCE_DIR}/include)
target_include_directories(test_utils PRIVATE src)
target_link_libraries(test_utils PRIVATE cubeb gtest_main)
add_test(utils test_utils)

add_executable(test_ring_buffer test/test_ring_buffer.cpp)
target_include_directories(test_ring_buffer PRIVATE ${gtest_SOURCE_DIR}/include)
target_include_directories(test_ring_buffer PRIVATE src)
target_link_libraries(test_ring_buffer PRIVATE cubeb gtest_main)
add_test(ring_buffer test_ring_buffer)
