cmake_minimum_required(VERSION 3.21)

# 版本号只有一个来源：仓库根目录的 VERSION 文件。
# 发布 CI 直接读同一个文件（.github/workflows/release.yaml 里的
# "Get PROJECT_VERSION from VERSION file"），所以界面上的版本、安装包名与
# release 产物名不会各说各话。
# string(STRIP) 顺带吃掉行尾的 \r：Windows 上若按 CRLF 检出，版本号会带进
# 一个回车，project(VERSION ...) 会直接解析失败。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LYCODE_VERSION LIMIT_COUNT 1)
string(STRIP "${LYCODE_VERSION}" LYCODE_VERSION)
if(LYCODE_VERSION STREQUAL "")
  message(FATAL_ERROR "无法从 ${CMAKE_CURRENT_SOURCE_DIR}/VERSION 读取版本号（文件缺失或为空）")
endif()

project(
  LyCode
  VERSION ${LYCODE_VERSION}
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
# LinguistTools 提供 qt_add_translations：.ts 源文件 → lrelease → .qm → 嵌进资源。
# 它是**必需**组件而不是可选：翻译是这个产品的功能之一，缺工具链应当构建失败，
# 而不是悄悄产出一个"选英文没反应"的包。
find_package(Qt6 6.4 REQUIRED COMPONENTS LinguistTools)

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

# ── 界面翻译 ────────────────────────────────────────────────────────────────
# 源码里的中文是**源文案**（tr() 标记可翻译），英文来自 translations/*.ts。
# 这里把 lrelease 产出的 .qm 嵌进 lycode_lib 的资源（:/i18n/lycode_<lang>.qm），
# 理由与图标相同：测试链接的是 lycode_lib，翻译跟着它才能被测试验证到；
# 同时不用操心 .qm 有没有被安装包带上（Windows 安装器尤其容易漏）。
#
# ⚠ 不设 QT_I18N_SOURCE_LANGUAGE 之类的全局量：源文案是中文，而 Qt 默认把源语言
#   当英文并据此生成"复数形式"的 .ts，那会产出一个与本项目无关的文件。
#   NO_GENERATE_PLURALS_TS_FILE 关掉它；中文没有复数变化，本项目也不需要复数。
qt_add_translations(
  lycode_lib
  TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/translations/lycode_zh_CN.ts"
  RESOURCE_PREFIX "/i18n"
  NO_GENERATE_PLURALS_TS_FILE
)

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
