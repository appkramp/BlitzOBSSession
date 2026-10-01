cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

if(APPLE)
  enable_language(OBJCXX)
elseif(NOT WIN32)
  # The network layer is written for NSURLSession and WinHTTP only.
  message(FATAL_ERROR "Blitz Session Stats builds for macOS and Windows only")
endif()

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(BSS_BUILD_TESTS "Build the core tests" ON)

include(compilerconfig)
include(defaults)
include(helpers)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)

# The logic that needs neither OBS nor the network: aggregation, the template's
# formulas, the vehicle catalogue, the protocol. Tested on its own.
add_library(bss-core STATIC)
target_sources(
  bss-core
  PRIVATE
    src/core/catalog.cpp
    src/core/catalog.h
    src/core/layout.cpp
    src/core/layout.h
    src/core/protocol.cpp
    src/core/protocol.h
    src/core/session.cpp
    src/core/session.h
    src/core/table.cpp
    src/core/table.h
)
target_include_directories(bss-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(bss-core PUBLIC Qt6::Core)
target_compile_features(bss-core PUBLIC cxx_std_17)
set_target_properties(bss-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api bss-core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/catalog-store.cpp
    src/catalog-store.h
    src/config.cpp
    src/config.h
    src/net/http.h
    src/net/websocket.h
    src/overlay-source.cpp
    src/plugin-main.cpp
    src/plugin.h
    src/render.cpp
    src/render.h
    src/stats-client.cpp
    src/stats-client.h
    src/ui/columns-dialog.cpp
    src/ui/columns-dialog.h
    src/ui/settings-dialog.cpp
    src/ui/settings-dialog.h
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(APPLE)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/net/net-mac.mm)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>")
elseif(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/net/net-win.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BSS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(bss-core-tests tests/core-tests.cpp)
  target_link_libraries(bss-core-tests PRIVATE bss-core)
  target_compile_definitions(bss-core-tests PRIVATE BSS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

  add_executable(bss-render-preview tests/render-preview.cpp src/render.cpp src/render.h)
  target_link_libraries(bss-render-preview PRIVATE bss-core Qt6::Gui Qt6::Svg)
  target_include_directories(bss-render-preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(bss-render-preview PRIVATE BSS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

  if(APPLE)
    # The Qt frameworks are the ones in .deps, not installed anywhere.
    get_filename_component(_qt_lib_dir "${Qt6_DIR}/../.." ABSOLUTE)
    set_target_properties(
      bss-core-tests
      bss-render-preview
      PROPERTIES
        BUILD_RPATH "${_qt_lib_dir}"
        XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "${_qt_lib_dir}"
        # Library validation would refuse the unsigned Qt from .deps.
        XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO
    )
  endif()
endif()
