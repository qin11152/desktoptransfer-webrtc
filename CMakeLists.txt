cmake_minimum_required(VERSION 3.15)

project(twebrtc)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(WIN32)
    set(TWEBRTC_PLATFORM windows)
elseif(APPLE)
    set(TWEBRTC_PLATFORM macos)
elseif(UNIX)
    set(TWEBRTC_PLATFORM linux)
else()
    set(TWEBRTC_PLATFORM unknown)
endif()

set(THIRD_PARTY_ROOT ${CMAKE_SOURCE_DIR}/3rd)
set(THIRD_PARTY_INCLUDE_DIR ${THIRD_PARTY_ROOT}/include)
set(THIRD_PARTY_LIB_ROOT ${THIRD_PARTY_ROOT}/lib)
set(THIRD_PARTY_PLATFORM_LIB_DIR ${THIRD_PARTY_LIB_ROOT}/${TWEBRTC_PLATFORM})
set(PLATFORM_RUNTIME_OUTPUT_ROOT ${CMAKE_SOURCE_DIR}/bin/${TWEBRTC_PLATFORM})

if(EXISTS "${THIRD_PARTY_PLATFORM_LIB_DIR}")
    set(ACTIVE_THIRD_PARTY_LIB_DIR "${THIRD_PARTY_PLATFORM_LIB_DIR}")
elseif(TWEBRTC_PLATFORM STREQUAL "linux" AND EXISTS "${THIRD_PARTY_LIB_ROOT}/libwebrtc.a")
    set(ACTIVE_THIRD_PARTY_LIB_DIR "${THIRD_PARTY_LIB_ROOT}")
    message(WARNING "Using legacy third-party library layout from ${THIRD_PARTY_LIB_ROOT}; move Linux libraries into ${THIRD_PARTY_PLATFORM_LIB_DIR}.")
else()
    message(FATAL_ERROR "No third-party library directory found for platform ${TWEBRTC_PLATFORM}: ${THIRD_PARTY_PLATFORM_LIB_DIR}")
endif()

message(STATUS "Configuring ${PROJECT_NAME} for platform: ${TWEBRTC_PLATFORM}")

function(configure_platform_output target_name)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(config Debug Release RelWithDebInfo MinSizeRel)
            string(TOUPPER "${config}" config_upper)
            set_target_properties(${target_name} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${PLATFORM_RUNTIME_OUTPUT_ROOT}/${config}"
                LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${PLATFORM_RUNTIME_OUTPUT_ROOT}/${config}"
                ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${PLATFORM_RUNTIME_OUTPUT_ROOT}/${config}")
        endforeach()
    else()
        set(_output_config "${CMAKE_BUILD_TYPE}")
        if(NOT _output_config)
            set(_output_config Debug)
        endif()
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${PLATFORM_RUNTIME_OUTPUT_ROOT}/${_output_config}"
            LIBRARY_OUTPUT_DIRECTORY "${PLATFORM_RUNTIME_OUTPUT_ROOT}/${_output_config}"
            ARCHIVE_OUTPUT_DIRECTORY "${PLATFORM_RUNTIME_OUTPUT_ROOT}/${_output_config}")
    endif()
endfunction()

aux_source_directory(. srcs)
file(GLOB_RECURSE module CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/module/*.cpp)
file(GLOB_RECURSE ui CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/ui/*.cpp)

if(NOT TWEBRTC_PLATFORM STREQUAL "linux")
    list(FILTER module EXCLUDE REGEX ".*/pulse_loopback_audio_device_module\\.cpp$")
endif()

if(APPLE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@loader_path/lib")
elseif(UNIX)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "\${ORIGIN}/lib")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
# set(CMAKE_BUILD_TYPE "RelWithDebInfo")

add_executable(${PROJECT_NAME} ${srcs} ${module} ${ui})
add_executable(signaling_server tools/signaling_server.cpp)

configure_platform_output(${PROJECT_NAME})
configure_platform_output(signaling_server)

find_package(Qt5 REQUIRED COMPONENTS Gui Widgets Network WebSockets)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE_SIMPLE libpulse-simple)
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
endif()

unset(TWEBRTC_RELEASE_LIBRARY CACHE)
unset(TWEBRTC_DEBUG_LIBRARY CACHE)
find_library(TWEBRTC_RELEASE_LIBRARY NAMES webrtc PATHS "${ACTIVE_THIRD_PARTY_LIB_DIR}" NO_DEFAULT_PATH)
find_library(TWEBRTC_DEBUG_LIBRARY NAMES webrtc_d webrtc PATHS "${ACTIVE_THIRD_PARTY_LIB_DIR}" NO_DEFAULT_PATH)

if(NOT TWEBRTC_RELEASE_LIBRARY)
    message(FATAL_ERROR "Failed to find the release WebRTC library in ${ACTIVE_THIRD_PARTY_LIB_DIR}")
endif()

if(NOT TWEBRTC_DEBUG_LIBRARY)
    set(TWEBRTC_DEBUG_LIBRARY "${TWEBRTC_RELEASE_LIBRARY}")
    message(WARNING "Debug WebRTC library was not found in ${ACTIVE_THIRD_PARTY_LIB_DIR}; Debug builds will reuse the release library.")
endif()

set(WEBRTC_ABSL_DIR "${THIRD_PARTY_INCLUDE_DIR}/rtc/third_party/abseil-cpp"
    CACHE PATH "Path to a complete abseil-cpp headers tree used by WebRTC")

function(webrtc_is_complete_absl_tree dir result_var)
    if(EXISTS "${dir}/absl/numeric/int128.h"
    #    AND EXISTS "${dir}/absl/numeric/int128_have_intrinsic.inc"
    #    AND EXISTS "${dir}/absl/numeric/int128_no_intrinsic.inc"
       )
        set(${result_var} TRUE PARENT_SCOPE)
    else()
        set(${result_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(_webrtc_absl_candidates
    "${WEBRTC_ABSL_DIR}"
    "${THIRD_PARTY_INCLUDE_DIR}/rtc/third_party/abseil-cpp"
    )

set(_webrtc_absl_resolved "")
foreach(_webrtc_absl_candidate IN LISTS _webrtc_absl_candidates)
    webrtc_is_complete_absl_tree("${_webrtc_absl_candidate}" _webrtc_absl_ok)
    if(_webrtc_absl_ok)
        set(_webrtc_absl_resolved "${_webrtc_absl_candidate}")
        break()
    endif()
endforeach()

if(_webrtc_absl_resolved STREQUAL "")
    message(FATAL_ERROR
        "A complete Abseil headers tree was not found. Set WEBRTC_ABSL_DIR to a directory containing absl/numeric/int128.h, int128_have_intrinsic.inc, and int128_no_intrinsic.inc.")
endif()

set(WEBRTC_ABSL_DIR "${_webrtc_absl_resolved}" CACHE PATH
    "Path to a complete abseil-cpp headers tree used by WebRTC" FORCE)

# target_include_directories(${PROJECT_NAME} PRIVATE /home/qin/rtc/webrtc-checkout/src/third_party/jsoncpp/source/include)
# target_include_directories(${PROJECT_NAME} PRIVATE /home/qin/rtc/webrtc-checkout/src)
# target_include_directories(${PROJECT_NAME} PRIVATE /home/qin/rtc/webrtc-checkout/src/third_party/abseil-cpp)

# target_include_directories(${PROJECT_NAME} PRIVATE /home/qin/rtc/webrtc-checkout/src/third_party/jsoncpp/source/include)
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE ${WEBRTC_ABSL_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${THIRD_PARTY_INCLUDE_DIR}/rtc)
target_include_directories(${PROJECT_NAME} PRIVATE ${THIRD_PARTY_INCLUDE_DIR})

if(TWEBRTC_PLATFORM STREQUAL "linux")
    target_compile_definitions(${PROJECT_NAME} PRIVATE TWEBRTC_PLATFORM_LINUX)
elseif(TWEBRTC_PLATFORM STREQUAL "windows")
    target_compile_definitions(${PROJECT_NAME} PRIVATE TWEBRTC_PLATFORM_WINDOWS)
elseif(TWEBRTC_PLATFORM STREQUAL "macos")
    target_compile_definitions(${PROJECT_NAME} PRIVATE TWEBRTC_PLATFORM_MACOS)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    debug ${TWEBRTC_DEBUG_LIBRARY}
    optimized ${TWEBRTC_RELEASE_LIBRARY}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    Qt5::WebSockets
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ws2_32
        crypt32
        iphlpapi
    )
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEBRTC_POSIX WEBRTC_USE_X11)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pthread
        dl
        ${X11_LIBRARIES}
        Xfixes
        Xdamage
        Xext
        Xrandr
        Xcomposite
        Xtst
        glib-2.0
        gobject-2.0
        gio-2.0
        drm
        gbm
    )
    target_include_directories(${PROJECT_NAME} PRIVATE ${X11_INCLUDE_DIR})

    if(PULSE_SIMPLE_FOUND)
        message(STATUS "PulseAudio simple API detected: enabling custom system-audio capture module")
        target_compile_definitions(${PROJECT_NAME} PRIVATE TWEBRTC_USE_PULSE_SIMPLE_CAPTURE)
        target_include_directories(${PROJECT_NAME} PRIVATE ${PULSE_SIMPLE_INCLUDE_DIRS})
        target_link_directories(${PROJECT_NAME} PRIVATE ${PULSE_SIMPLE_LIBRARY_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PULSE_SIMPLE_LIBRARIES})
        target_compile_options(${PROJECT_NAME} PRIVATE ${PULSE_SIMPLE_CFLAGS_OTHER})
        target_link_options(${PROJECT_NAME} PRIVATE ${PULSE_SIMPLE_LDFLAGS_OTHER})
    else()
        message(WARNING "PulseAudio simple development files were not found; falling back to WebRTC's built-in ADM.")
    endif()

    if(PIPEWIRE_FOUND)
        message(STATUS "PipeWire detected: enabling Wayland desktop capture support")
        target_compile_definitions(${PROJECT_NAME} PRIVATE WEBRTC_USE_PIPEWIRE)
        target_include_directories(${PROJECT_NAME} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_directories(${PROJECT_NAME} PRIVATE ${PIPEWIRE_LIBRARY_DIRS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${PIPEWIRE_LIBRARIES})
        target_compile_options(${PROJECT_NAME} PRIVATE ${PIPEWIRE_CFLAGS_OTHER})
        target_link_options(${PROJECT_NAME} PRIVATE ${PIPEWIRE_LDFLAGS_OTHER})
    else()
        message(WARNING "PipeWire development files were not found; Wayland desktop capture will be unavailable.")
    endif()
endif()

target_link_libraries(signaling_server PRIVATE
    Qt5::Core
    Qt5::Network
    Qt5::WebSockets
)