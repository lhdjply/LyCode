cmake_minimum_required(VERSION 3.21)

project(
  LyCode
  VERSION 0.1.0
  DESCRIPTION "LyCode 桌面版 — Qt6 / C++ 原生实现"
  LANGUAGES CXX
)

# ── 语言与构建类型 ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      RelWithDebInfo
      CACHE STRING "构建类型" FORCE
  )
endif()

# 导出 compile_commands.json，供 clangd / IDE 使用。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Qt ──────────────────────────────────────────────────────────────────────
# 仅依赖本机已有的模块：Widgets 而非 Quick/QML，避免引入未安装的依赖。
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Sql Concurrent)

qt_standard_project_setup()

# ── 依赖项 ──────────────────────────────────────────────────────────────────
# 源码用 GLOB + CONFIGURE_DEPENDS：本项目正处于快速扩张期，新文件需要自动纳入构建。
# CONFIGURE_DEPENDS 会在每次构建时重新求值 glob，避免"新增文件没被编译"的沉默失败。
file(
  GLOB_RECURSE LYCODE_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/agent/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mcp/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/skills/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
)

file(
  GLOB_RECURSE LYCODE_HEADERS
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/model/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/agent/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.h"
)

# ── 核心目标 ────────────────────────────────────────────────────────────────
add_library(lycode_lib OBJECT ${LYCODE_SOURCES} ${LYCODE_HEADERS})

target_include_directories(lycode_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(
  lycode_lib
  PUBLIC Qt6::Core
         Qt6::Gui
         Qt6::Widgets
         Qt6::Network
         Qt6::Sql
         Qt6::Concurrent
)

target_compile_definitions(
  lycode_lib
  PUBLIC LYCODE_QT_VERSION="${PROJECT_VERSION}"
         QT_DISABLE_DEPRECATED_BEFORE=0x060500
)

if(MSVC)
  target_compile_options(lycode_lib PRIVATE /W4 /permissive-)
else()
  target_compile_options(lycode_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── 可执行文件 ──────────────────────────────────────────────────────────────
qt_add_executable(lycode src/main.cpp)

target_link_libraries(lycode PRIVATE lycode_lib)

set_target_properties(
  lycode
  PROPERTIES WIN32_EXECUTABLE TRUE
             MACOSX_BUNDLE TRUE
             MACOSX_BUNDLE_BUNDLE_NAME "LyCode"
             MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lycode.qt"
)

# ── 测试 ────────────────────────────────────────────────────────────────────
option(LYCODE_BUILD_TESTS "构建单元测试" ON)

if(LYCODE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── 安装 ────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS lycode RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
