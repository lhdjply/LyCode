find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

# 每个测试是一个独立可执行文件：这样单个测试崩溃不会带走其它测试的结果，
# 也让 CTest 能把失败精确定位到某一个模块。
function(lycode_add_test name)
  add_executable(${name} ${name}.cpp)
  # 测试共享的辅助设施（假网关等）放在 support/ 下，以头文件形式被包含。
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${name} PRIVATE lycode_lib Qt6::Test)
  # 同时写 stdout 和一份文件日志。
  # QtTest 的 stdout 在 Windows CI 上出现过"只报一句 Failed、看不到任何输出"，
  # 断言消息和诊断全部丢失。文件日志是最后一道保险：ctest 抓不到时它一定在，
  # 失败时由 CI 的测试步骤直接把失败测试的日志打进 CI 日志。`-o -,txt` 表示标准
  # 输出，可重复指定。
  add_test(NAME ${name}
           COMMAND ${name} -o -,txt -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.log,txt")
  # 测试需要 QApplication（有 Widgets 依赖），用 offscreen 平台避免要求显示服务。
  # 同时强制打开 lycode.* 的 debug 级：文件日志要能看到协议原始数据与每一步握手，
  # 否则出问题时日志里只剩生命周期那几行，还是查不出所以然。
  # 注意 set_tests_properties 必须在 add_test 之后调用。
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=lycode.*=true"
  )
endfunction()

# 假 MCP 服务器：测试用的协议完整子进程。它必须是一个**独立可执行文件**
# （MCP 的 stdio 传输本来就是进程间对话），所以不放进 lycode_lib。
add_executable(fake_mcp_server support/fake_mcp_server.cpp)
target_link_libraries(fake_mcp_server PRIVATE Qt6::Core)

# ⚠ 这里刻意**不设** RUNTIME_OUTPUT_DIRECTORY。
#
# test_mcp 是用 applicationDirPath() 找它的，前提是两者在同一目录。两个目标都是
# 本目录下的普通 add_executable，默认输出位置天然一致，本来就同目录。
#
# 一旦给 fake_mcp_server 单独指定 RUNTIME_OUTPUT_DIRECTORY，多配置生成器会给
# **它**追加配置子目录，而测试可执行文件仍走默认位置，两者就分家了。Windows 上
# 的默认位置还特别容易踩：qt_standard_project_setup() 在 WIN32 分支里会把
# CMAKE_RUNTIME_OUTPUT_DIRECTORY 设成顶层 build 目录（目的是让 exe 旁边的
# Qt DLL 能被找到），所以实际布局变成：
#   test_mcp.exe        在 build/RelWithDebInfo/
#   fake_mcp_server.exe 在 build/tests/RelWithDebInfo/
# 客户端于是报 "Process failed to start: The system cannot find the file
# specified"，4 个握手测试全挂。Linux 上 qt_standard_project_setup() 不走那段
# 分支，两个目标都在 build/tests/，所以这个坑只在 Windows 暴露。

lycode_add_test(test_model_catalog)
lycode_add_test(test_types)
lycode_add_test(test_context_compactor)
lycode_add_test(test_translator)
lycode_add_test(test_permission_gate)
lycode_add_test(test_markdown)
lycode_add_test(test_agent_runtime)
lycode_add_test(test_diff_view
test_diff_view.cpp)

lycode_add_test(test_syntax_highlighter)

lycode_add_test(test_file_viewer)

lycode_add_test(test_ui_flow)
lycode_add_test(test_app_config)
lycode_add_test(test_mcp)
lycode_add_test(test_skills)
