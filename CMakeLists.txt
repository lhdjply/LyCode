cmake_minimum_required(VERSION 3.21)

project(
  LyCode
  VERSION 0.1.0
  DESCRIPTION "LyCode 桌面版 — Qt6 / C++ 原生实现"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      RelWithDebInfo
      CACHE STRING "构建类型" FORCE
  )
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Sql Concurrent)

qt_standard_project_setup()

# 必须在创建任何目标之前设置：该变量只在目标创建时被读取。
set(CMAKE_AUTORCC ON)

# GLOB_RECURSE 会递归子目录，因此新增 src/ 下的子目录不需要改这里。
# CONFIGURE_DEPENDS 让新增文件在下次构建时被纳入，避免"漏编译"。
file(
  GLOB_RECURSE LYCODE_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
# src/main.cpp 属于可执行文件，不能进库：否则 main() 会被编译两次而链接失败。
list(REMOVE_ITEM LYCODE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
file(
  GLOB_RECURSE LYCODE_HEADERS
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# OBJECT 库：不产出 .a，但让测试与可执行文件共享同一份编译结果。
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

# 图标资源挂在 lycode_lib 上（而不是可执行文件）：测试链接的是这个目标，
# 资源跟着它才能被测试验证到。
target_sources(lycode_lib PRIVATE assets/resources.qrc)

# Windows 下额外编译 .rc，让 exe 文件本身在资源管理器里有图标。
if(WIN32)
  target_sources(lycode_lib PRIVATE assets/icons/windows/win_resources.rc)
endif()

qt_add_executable(lycode src/main.cpp)

target_link_libraries(lycode PRIVATE lycode_lib)

set_target_properties(
  lycode
  PROPERTIES WIN32_EXECUTABLE TRUE
             MACOSX_BUNDLE TRUE
             MACOSX_BUNDLE_BUNDLE_NAME "LyCode"
             MACOSX_BUNDLE_GUI_IDENTIFIER "dev.lycode.qt"
)

option(LYCODE_BUILD_TESTS "构建单元测试" ON)

if(LYCODE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)

install(TARGETS lycode RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# 图标目录结构本身就是安装结构，整棵树照搬即可。
install(
  DIRECTORY assets/icons/linux/hicolor/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor"
)

# 桌面条目只在用 freedesktop 约定的平台安装（Linux/BSD）。
#
# 文件名必须是 lycode.desktop：main.cpp 里 setDesktopFileName("lycode")，
# Wayland 的 app id 与 X11 的 StartupWMClass 都靠它把运行中的窗口对上图标；
# Icon=lycode 也要求 hicolor 下有同名图标（见上面那条安装规则）。
#
# 这里用相对的 CMAKE_INSTALL_DATADIR 而不是 FULL_DATADIR：只有相对路径才会
# 跟着 `cmake --install --prefix` 与 DESTDIR 走（装到 ~/.local 依赖这一点）。
if(UNIX AND NOT APPLE)
  install(FILES assets/desktop/lycode.desktop
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
  )
endif()
