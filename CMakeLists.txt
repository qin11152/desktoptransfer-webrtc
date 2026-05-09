cmake_minimum_required(VERSION 3.15)

project(twebrtc)

aux_source_directory(. srcs)
file(GLOB_RECURSE module ${CMAKE_SOURCE_DIR}/module/*.cpp)
file(GLOB_RECURSE ui ${CMAKE_SOURCE_DIR}/ui/*.cpp)

#设置可执行文件输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE})
SET(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
SET(CMAKE_INSTALL_RPATH "\${ORIGIN}/lib")

if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
	set(APPLICATION_DIR_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${Player_BUILD_TYPE})
else()
	set(APPLICATION_DIR_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
# set(CMAKE_BUILD_TYPE "RelWithDebInfo")

add_executable(${PROJECT_NAME} ${srcs} ${module} ${ui})
add_executable(signaling_server tools/signaling_server.cpp)

find_package(Qt5 REQUIRED COMPONENTS Gui Widgets Network WebSockets)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
endif()

target_link_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/3rd/lib)

set(WEBRTC_ABSL_DIR "${CMAKE_SOURCE_DIR}/3rd/include/rtc/third_party/abseil-cpp"
    CACHE PATH "Path to a complete abseil-cpp headers tree used by WebRTC")

function(webrtc_is_complete_absl_tree dir result_var)
    if(EXISTS "${dir}/absl/numeric/int128.h"
       AND EXISTS "${dir}/absl/numeric/int128_have_intrinsic.inc"
       AND EXISTS "${dir}/absl/numeric/int128_no_intrinsic.inc")
        set(${result_var} TRUE PARENT_SCOPE)
    else()
        set(${result_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(_webrtc_absl_candidates
    "${WEBRTC_ABSL_DIR}"
    "${CMAKE_SOURCE_DIR}/3rd/include/rtc/third_party/abseil-cpp"
    "/home/qin/sdk/rtc/webrtc-checkout/src/third_party/abseil-cpp")

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
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/3rd/include/rtc)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/3rd/include)

#查看是debug还是release


if("Debug" STREQUAL "${CMAKE_BUILD_TYPE}")
    message(STATUS "Debug mode: Adding debug libraries")
    target_link_libraries(${PROJECT_NAME} PRIVATE webrtc_d)
else()
    message(STATUS "Release mode: Adding release libraries")
    target_link_libraries(${PROJECT_NAME} PRIVATE webrtc)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    stdc++
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

set_target_properties(signaling_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}
)