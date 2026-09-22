find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

# 每个测试是一个独立可执行文件：这样单个测试崩溃不会带走其它测试的结果，
# 也让 CTest 能把失败精确定位到某一个模块。
function(zcode_add_test name)
  add_executable(${name} ${name}.cpp)
  # 测试共享的辅助设施（假网关等）放在 support/ 下，以头文件形式被包含。
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${name} PRIVATE zcode_lib Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  # 测试需要 QApplication（有 Widgets 依赖），用 offscreen 平台避免要求显示服务。
  # 注意 set_tests_properties 必须在 add_test 之后调用。
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )
endfunction()

# 假 MCP 服务器：测试用的协议完整子进程。它必须是一个**独立可执行文件**
# （MCP 的 stdio 传输本来就是进程间对话），所以不放进 zcode_lib。
add_executable(fake_mcp_server support/fake_mcp_server.cpp)
target_link_libraries(fake_mcp_server PRIVATE Qt6::Core)
# 和测试二进制放同一个目录，测试用 applicationDirPath() 就能找到它。
set_target_properties(fake_mcp_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

zcode_add_test(test_types)
zcode_add_test(test_permission_gate)
zcode_add_test(test_markdown)
zcode_add_test(test_agent_runtime)
zcode_add_test(test_diff_view
test_diff_view.cpp)

zcode_add_test(test_syntax_highlighter)

zcode_add_test(test_file_viewer)

zcode_add_test(test_ui_flow)
zcode_add_test(test_app_config)
zcode_add_test(test_mcp)
zcode_add_test(test_skills)
