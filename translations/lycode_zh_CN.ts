<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN" sourcelanguage="en_US">
<context>
    <name>agent::AgentRuntime</name>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="226"/>
        <source>Subagent</source>
        <translation>子代理</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="181"/>
        <source>A subagent cannot spawn another subagent.</source>
        <translation>子代理不能再派生子代理。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="186"/>
        <source>The subagent prompt cannot be empty.</source>
        <translation>子代理的 prompt 不能为空。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="290"/>
        <source>The subagent hit its model-step limit (%1) and was stopped.</source>
        <translation>子代理触及模型步数上限（%1）被中止。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="296"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="297"/>
        <source>The subagent failed.</source>
        <translation>子代理运行失败。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="305"/>
        <source>The subagent did not run.</source>
        <translation>子代理未执行。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="415"/>
        <source>The workspace path is empty.</source>
        <translation>工作区路径为空。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="460"/>
        <source>Session storage is not initialized.</source>
        <translation>会话存储未初始化。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="468"/>
        <source>Session not found: </source>
        <translation>找不到会话：</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="517"/>
        <source>Session closed</source>
        <translation>会话已关闭</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="565"/>
        <source>Input is empty.</source>
        <translation>输入为空。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="571"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="1441"/>
        <source>No active session.</source>
        <translation>没有活动会话。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="577"/>
        <source>A turn is already running. Wait for it to finish, or interrupt it.</source>
        <translation>正在运行中，请先等待当前回合结束或中断。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="584"/>
        <source>No usable model is configured.</source>
        <translation>未配置可用的模型。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="672"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="683"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="691"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="702"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="1030"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="1066"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="1149"/>
        <location filename="../src/agent/AgentRuntime.cpp" line="1931"/>
        <source>Interrupted by the user</source>
        <translation>用户中断了本次运行</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="711"/>
        <source>The subagent exceeded its model-step limit (%1) and was stopped.</source>
        <translation>子代理的模型步数超过上限（%1），已中止。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="713"/>
        <source>This turn exceeded the safety limit of %1 model steps and was stopped.</source>
        <translation>单次回合的模型步数超过安全上限（%1），已中止。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="421"/>
        <source>No model selected. Configure a model in Settings first.</source>
        <translation>未选择模型，请先在设置中配置模型。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="720"/>
        <source>The model registry is not initialized.</source>
        <translation>模型注册表未初始化。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="732"/>
        <source>The model this session uses is no longer available (it was deleted or is not fully configured). Pick another model in the picker at the bottom, then send again.</source>
        <translation>当前会话用的模型已不可用（已被删除或未配置完整）。请在底部模型下拉框里重新选一个模型后再发送。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="783"/>
        <source>Failed to create the model request.</source>
        <translation>模型请求创建失败。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1040"/>
        <source>The model request failed.</source>
        <translation>模型请求失败。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1052"/>
        <source>The model response has no matching message.</source>
        <translation>模型响应没有对应消息。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1447"/>
        <source>No usable model is configured, so no summary can be generated.</source>
        <translation>未配置可用的模型，无法生成压缩摘要。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1454"/>
        <source>Compaction is already in progress.</source>
        <translation>压缩已在进行中。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1462"/>
        <source>A turn is running. Compact after it finishes or after you interrupt it.</source>
        <translation>正在运行中，请等本轮结束或中断后再压缩。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1488"/>
        <source>Context is %1% full; old tool output will be trimmed on the way out.</source>
        <translation>上下文已用 %1%，旧工具输出将在下发时裁剪。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1510"/>
        <source>There is no history to compact (too few messages, or all of them are still inside the protected window).</source>
        <translation>没有可压缩的历史（消息太少或都还在保护窗口内）。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1519"/>
        <source>No usable model found.</source>
        <translation>找不到可用的模型。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1547"/>
        <source>Failed to create the summary request.</source>
        <translation>摘要请求创建失败。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1574"/>
        <source>The summary model returned nothing, so this compaction was skipped.</source>
        <translation>摘要模型没有返回内容，已跳过本次压缩。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1589"/>
        <source>Compacting the context (generating a summary)…</source>
        <translation>正在压缩上下文（生成摘要）…</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1611"/>
        <source>Compaction could not complete: archiving the history failed.</source>
        <translation>压缩未能完成：历史归档失败。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1649"/>
        <source>Context compacted: %1 earlier messages were archived into a summary; now about %2 tokens.</source>
        <translation>已压缩上下文：%1 条较早消息已归档为摘要，当前约 %2 tokens。</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1813"/>
        <source>Allow once</source>
        <translation>允许一次</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1824"/>
        <source>Always allow: </source>
        <translation>始终允许：</translation>
    </message>
    <message>
        <location filename="../src/agent/AgentRuntime.cpp" line="1837"/>
        <source>Deny</source>
        <translation>拒绝</translation>
    </message>
</context>
<context>
    <name>tools::AgentTool</name>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="35"/>
        <source>General purpose</source>
        <translation>通用</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="42"/>
        <source>Read-only research</source>
        <translation>只读调研</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="179"/>
        <source>Spawns a subagent. It has its own context, and its tool calls ask you for approval individually.</source>
        <translation>派生子代理。子代理拥有独立的上下文，其内部的工具调用会各自向你请求确认。</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="187"/>
        <source>(the subagent produced no output)</source>
        <translation>(子代理没有产出任何内容)</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="199"/>
        <source>&lt;warning&gt;The subagent hit its model-step limit and was stopped early; its conclusion may be incomplete.&lt;/warning&gt;</source>
        <translation>&lt;warning&gt;子代理触及步数上限被提前结束，其结论可能不完整。&lt;/warning&gt;</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="215"/>
        <source>A subagent cannot spawn another subagent. Do the task yourself.</source>
        <translation>子代理不能再派生子代理。请自己完成该任务。</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="224"/>
        <source>description cannot be empty.</source>
        <translation>description 不能为空。</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="229"/>
        <source>prompt cannot be empty.</source>
        <translation>prompt 不能为空。</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="238"/>
        <source>Background subagents are not implemented yet (run_in_background is unsupported); run it in the foreground instead.</source>
        <translation>后台子代理尚未实现（run_in_background 暂不支持），请改为前台运行。</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="253"/>
        <source>Unknown subagent_type: %1 (available: %2)</source>
        <translation>未知的 subagent_type：%1（可用：%2）</translation>
    </message>
    <message>
        <location filename="../src/tools/AgentTool.cpp" line="278"/>
        <source>The subagent failed: %1</source>
        <translation>子代理执行失败：%1</translation>
    </message>
</context>
<context>
    <name>tools::BackgroundTaskRegistry</name>
    <message>
        <location filename="../src/tools/BackgroundTaskRegistry.cpp" line="670"/>
        <source>
…[more output in output-path]</source>
        <translation>
…[更多输出见 output-path]</translation>
    </message>
</context>
<context>
    <name>tools::BashTool</name>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="173"/>
        <location filename="../src/tools/BashTool.cpp" line="228"/>
        <source>command cannot be empty</source>
        <translation>command 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="223"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="248"/>
        <source>Working directory does not exist: %1</source>
        <translation>工作目录不存在：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="268"/>
        <source>This environment does not support background tasks (run_in_background).</source>
        <translation>当前环境不支持后台任务（run_in_background）。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="277"/>
        <source>Could not create the background task output directory.</source>
        <translation>无法创建后台任务输出目录。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="313"/>
        <source>The background task could not start (program=%1, cwd=%2).</source>
        <translation>后台任务无法启动（program=%1，cwd=%2）。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="329"/>
        <source>Failed to register the background task.</source>
        <translation>后台任务登记失败。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="337"/>
        <source>The command was started in the background (detached from the host, survives restarts).
task_id: %1
output_path: %2
pid: %3

Use TaskOutput to read progress or results, and TaskStop to stop it.</source>
        <translation>命令已在后台启动（已与宿主分离，可跨重启存活）。
task_id: %1
output_path: %2
pid: %3

用 TaskOutput 读取进度或结果，用 TaskStop 终止。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="450"/>
        <source>The command exceeded its timeout (%1ms) and is still running, so it was moved to the background.
task_id: %2
output_path: %3

Use TaskOutput to read progress and TaskStop to stop it.
Note: it inherited the foreground pipes, so it only lives as long as this process. Use run_in_background to survive a restart.</source>
        <translation>命令超过超时（%1ms）仍在运行，已自动转入后台。
task_id: %2
output_path: %3

用 TaskOutput 读取进度，用 TaskStop 终止。
注意：它继承的是前台管道，只在本进程存活期间继续运行；需要跨重启请用 run_in_background 显式启动。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="486"/>
        <source>Could not start the command process: %1</source>
        <translation>无法启动命令进程：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="550"/>
        <source>Command cancelled</source>
        <translation>命令已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="558"/>
        <source>The command timed out (%1 ms) and its process group was killed. Raise timeout explicitly if it needs longer (limit %2 ms).</source>
        <translation>命令超时（%1 ms），进程组已被终止。如需更长时间请显式提高 timeout（上限 %2 ms）。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="569"/>
        <source>The command ended abnormally (it did not exit normally; it may have segfaulted or been killed by a signal).</source>
        <translation>命令异常结束（未正常退出，可能是段错误或被信号杀死）。</translation>
    </message>
    <message>
        <location filename="../src/tools/BashTool.cpp" line="582"/>
        <source>The command exited with code %1</source>
        <translation>命令以退出码 %1 结束</translation>
    </message>
</context>
<context>
    <name>tools::EditTool</name>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="132"/>
        <source>file_path cannot be empty</source>
        <translation>file_path 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="136"/>
        <source>old_string cannot be empty</source>
        <translation>old_string 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="139"/>
        <source>new_string must be a string</source>
        <translation>new_string 必须是字符串</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="162"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="180"/>
        <source>Invalid path, or outside the workspace: %1</source>
        <translation>路径非法或超出工作区范围：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="189"/>
        <source>File does not exist: %1 (Edit cannot create files; use Write)</source>
        <translation>文件不存在：%1（Edit 不能创建新文件，请用 Write）</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="195"/>
        <source>%1 is not a regular file.</source>
        <translation>%1 不是普通文件。</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="204"/>
        <source>The file is too large (%1 bytes; Edit is limited to %2 bytes). Use Bash or Write.</source>
        <translation>文件过大（%1 字节，Edit 上限 %2 字节），请用 Bash 或 Write。</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="215"/>
        <source>Could not read the file: %1</source>
        <translation>无法读取文件：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="230"/>
        <source>old_string was not found in %1. Use Read to check the exact text first (indentation and whitespace must match exactly).</source>
        <translation>在 %1 中找不到 old_string。请先用 Read 确认原文（注意缩进与空白必须逐字一致）。</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="239"/>
        <source>old_string appears %2 times in %1, so it is ambiguous. Include more context to make the match unique, or pass replace_all=true to replace every occurrence.</source>
        <translation>old_string 在 %1 中出现了 %2 次，无法确定要替换哪一处。请扩大上下文使匹配唯一，或用 replace_all=true 全部替换。</translation>
    </message>
    <message>
        <location filename="../src/tools/EditTool.cpp" line="263"/>
        <source>Failed to write the file back: %1</source>
        <translation>写回文件失败：%1</translation>
    </message>
</context>
<context>
    <name>tools::GlobTool</name>
    <message>
        <location filename="../src/tools/GlobTool.cpp" line="194"/>
        <location filename="../src/tools/GlobTool.cpp" line="262"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/GlobTool.cpp" line="199"/>
        <source>pattern cannot be empty</source>
        <translation>pattern 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/GlobTool.cpp" line="210"/>
        <source>Invalid path, or outside the workspace: %1</source>
        <translation>path 非法或超出工作区范围：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GlobTool.cpp" line="225"/>
        <source>The search root does not exist or is not a directory: %1</source>
        <translation>搜索根目录不存在或不是目录：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GlobTool.cpp" line="239"/>
        <source>An absolute glob pattern must stay inside the search root: %1</source>
        <translation>绝对 glob 模式必须位于搜索根目录内：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GlobTool.cpp" line="308"/>
        <source>Skipped dependency/build directories (.git, node_modules, build*, .cache, and similar)</source>
        <translation>跳过了依赖/构建目录（.git、node_modules、build*、.cache 等）</translation>
    </message>
</context>
<context>
    <name>tools::GrepTool</name>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="630"/>
        <location filename="../src/tools/GrepTool.cpp" line="702"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="635"/>
        <source>pattern cannot be empty</source>
        <translation>pattern 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="643"/>
        <source>output_mode must be content / files_with_matches / count; got: %1</source>
        <translation>output_mode 只能是 content / files_with_matches / count，收到：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="655"/>
        <source>Invalid path, or outside the workspace: %1</source>
        <translation>path 非法或超出工作区范围：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="668"/>
        <source>The search path does not exist: %1</source>
        <translation>搜索路径不存在：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="688"/>
        <source>Invalid regular expression: %1</source>
        <translation>正则表达式非法：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="792"/>
        <source>The native backend does not support type filtering (no system ripgrep was found), so that argument was ignored</source>
        <translation>native 后端不支持 type 过滤（未能找到系统 ripgrep），已忽略该参数</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="848"/>
        <source>Could not start ripgrep: %1</source>
        <translation>无法启动 ripgrep：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="869"/>
        <source>The ripgrep search timed out and the process was killed.</source>
        <translation>ripgrep 搜索超时，已终止进程。</translation>
    </message>
    <message>
        <location filename="../src/tools/GrepTool.cpp" line="885"/>
        <source>ripgrep failed (exitCode=%1): %2</source>
        <translation>ripgrep 失败（exitCode=%1）：%2</translation>
    </message>
</context>
<context>
    <name>tools::ReadTool</name>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="57"/>
        <source>The image is too large (%1 bytes; limit %2 bytes). Use Bash to handle it instead.</source>
        <translation>图片过大（%1 字节，上限 %2 字节），请改用 Bash 处理。</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="66"/>
        <source>Could not open the image file: %1</source>
        <translation>无法打开图片文件：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="94"/>
        <source>Read image %1 (%2, %3 bytes). The image is attached to this result; just look at it to answer, no external command is needed.</source>
        <translation>已读取图片 %1（%2，%3 字节）。图片内容已随本次结果附上，直接看图回答即可，不需要借助外部命令。</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="192"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="197"/>
        <source>file_path cannot be empty</source>
        <translation>file_path 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="206"/>
        <source>Invalid path, or outside the workspace: %1</source>
        <translation>路径非法或超出工作区范围：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="214"/>
        <source>File does not exist: %1</source>
        <translation>文件不存在：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="220"/>
        <source>%1 is a directory; Read only reads files. Use Glob/Grep or Bash ls.</source>
        <translation>%1 是目录，Read 只能读取文件；请用 Glob/Grep 或 Bash ls。</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="226"/>
        <source>%1 is not a regular file (it may be a device or FIFO).</source>
        <translation>%1 不是普通文件（可能是设备/FIFO）。</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="244"/>
        <source>Could not open the file: %1</source>
        <translation>无法打开文件：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/ReadTool.cpp" line="259"/>
        <source>%1 looks like a binary file (a NUL byte appears in the first 8 KiB), which Read does not support. Use Bash instead (for example `file`, `xxd`, `strings`).</source>
        <translation>%1 看起来是二进制文件（前 8KiB 内含 NUL 字节），Read 不支持；请改用 Bash（如 `file`、`xxd`、`strings`）。</translation>
    </message>
</context>
<context>
    <name>tools::SkillTool</name>
    <message>
        <location filename="../src/tools/SkillTool.cpp" line="64"/>
        <source>name cannot be empty.</source>
        <translation>name 不能为空。</translation>
    </message>
    <message>
        <location filename="../src/tools/SkillTool.cpp" line="69"/>
        <source>No skill directory is configured in this environment.</source>
        <translation>当前环境没有配置技能目录。</translation>
    </message>
    <message>
        <location filename="../src/tools/SkillTool.cpp" line="84"/>
        <source>Skill &quot;%1&quot; not found. Available: %2</source>
        <translation>找不到技能「%1」。可用：%2</translation>
    </message>
    <message>
        <location filename="../src/tools/SkillTool.cpp" line="85"/>
        <source>(none)</source>
        <translation>(无)</translation>
    </message>
</context>
<context>
    <name>tools::TaskTools</name>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="55"/>
        <source>(no output yet)</source>
        <translation>(目前没有输出)</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="142"/>
        <location filename="../src/tools/TaskTools.cpp" line="283"/>
        <source>task_id cannot be empty.</source>
        <translation>task_id 不能为空。</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="147"/>
        <location filename="../src/tools/TaskTools.cpp" line="288"/>
        <source>This environment does not support background tasks.</source>
        <translation>当前环境不支持后台任务。</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="156"/>
        <source>Background task not found: %1 (TaskOutput only reaches tasks started in this session)</source>
        <translation>找不到后台任务：%1（可用 TaskOutput 的任务仅限本次会话启动的）</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="296"/>
        <source>Background task not found: %1</source>
        <translation>找不到后台任务：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="305"/>
        <source>Task %1 has already finished (status=%2, exit_code=%3); there is nothing to stop.</source>
        <translation>任务 %1 已经结束（status=%2, exit_code=%3），无需停止。</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="316"/>
        <source>Failed to stop the task: %1</source>
        <translation>停止任务失败：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/TaskTools.cpp" line="325"/>
        <source>Sent a termination signal to task %1. Use TaskOutput to see its final status and exit code.</source>
        <translation>已向任务 %1 发送终止信号。用 TaskOutput 查看最终状态与退出码。</translation>
    </message>
</context>
<context>
    <name>tools::TodoTool</name>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="132"/>
        <source>todos[%1] is not an object</source>
        <translation>todos[%1] 不是对象</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="139"/>
        <source>todos[%1].content cannot be empty</source>
        <translation>todos[%1].content 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="144"/>
        <source>todos[%1].status is missing (allowed values: %2)</source>
        <translation>todos[%1].status 缺失（可选值：%2）</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="150"/>
        <source>todos[%1].status is invalid: %2 (allowed values: %3)</source>
        <translation>todos[%1].status 非法：%2（可选值：%3）</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="160"/>
        <source>todos[%1].priority is invalid: %2 (allowed values: %3)</source>
        <translation>todos[%1].priority 非法：%2（可选值：%3）</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="179"/>
        <source>The session todo store is unavailable (ToolContext::todoStore is null). This is a runtime wiring error, not a problem with the model arguments.</source>
        <translation>会话 todo 存储不可用（ToolContext::todoStore 为空）。这是运行时装配错误，不是模型参数问题。</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="246"/>
        <location filename="../src/tools/TodoTool.cpp" line="412"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="256"/>
        <source>The session id is empty, so todos cannot be read</source>
        <translation>会话 id 为空，无法读取 todo</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="383"/>
        <source>todos must be an array</source>
        <translation>todos 必须是数组</translation>
    </message>
    <message>
        <location filename="../src/tools/TodoTool.cpp" line="422"/>
        <source>The session id is empty, so todos cannot be written</source>
        <translation>会话 id 为空，无法写入 todo</translation>
    </message>
</context>
<context>
    <name>tools::Tool</name>
    <message>
        <location filename="../src/tools/Tool.cpp" line="349"/>
        <source>Tool %1 requires execute permission.</source>
        <translation>工具 %1 需要执行权限。</translation>
    </message>
    <message>
        <location filename="../src/tools/Tool.cpp" line="351"/>
        <source>Tool %1 will operate on: %2</source>
        <translation>工具 %1 将要操作：%2</translation>
    </message>
    <message>
        <location filename="../src/tools/Tool.cpp" line="399"/>
        <source>Missing required argument `%1`</source>
        <translation>缺少必填参数 `%1`</translation>
    </message>
    <message>
        <location filename="../src/tools/Tool.cpp" line="958"/>
        <source>This session is in read-only mode (plan/ask) and tool %1 has write side effects (sideEffectScope=%2), so it was denied. Ask the user to leave read-only mode first.</source>
        <translation>当前处于只读模式（plan/ask），工具 %1 具有写入副作用（sideEffectScope=%2），已被拒绝执行。请先请求用户退出只读模式。</translation>
    </message>
    <message>
        <location filename="../src/tools/Tool.cpp" line="978"/>
        <source>Bytes written do not match (expected %1, wrote %2)</source>
        <translation>写入字节数不符（期望 %1，实际 %2）</translation>
    </message>
    <message>
        <location filename="../src/tools/Tool.cpp" line="1093"/>
        <source>
…[output truncated: over the %1 byte budget]</source>
        <translation>
…[输出被截断：超过 %1 字节预算]</translation>
    </message>
</context>
<context>
    <name>tools::WriteTool</name>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="35"/>
        <source>%1 is a directory; Write cannot overwrite it.</source>
        <translation>%1 是目录，不能用 Write 覆盖。</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="42"/>
        <source>Could not read the original file (%1), so the overwrite was refused.</source>
        <translation>无法读取原文件（%1），拒绝覆盖。</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="123"/>
        <source>file_path cannot be empty</source>
        <translation>file_path 不能为空</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="128"/>
        <source>content must be a string</source>
        <translation>content 必须是字符串</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="147"/>
        <source>Execution cancelled</source>
        <translation>执行已取消</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="161"/>
        <source>Invalid path, or outside the workspace: %1</source>
        <translation>路径非法或超出工作区范围：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="180"/>
        <source>Could not create the parent directory: %1</source>
        <translation>无法创建父目录：%1</translation>
    </message>
    <message>
        <location filename="../src/tools/WriteTool.cpp" line="188"/>
        <source>Failed to write the file: %1</source>
        <translation>写入文件失败：%1</translation>
    </message>
</context>
<context>
    <name>ui::ChoiceCard</name>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="98"/>
        <source>Recommended</source>
        <translation>推荐</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="159"/>
        <location filename="../src/ui/ChoiceCard.cpp" line="255"/>
        <location filename="../src/ui/ChoiceCard.cpp" line="314"/>
        <source>Collapse the question card</source>
        <translation>收起问题卡片</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="166"/>
        <source>Dismiss all questions</source>
        <translation>放弃整组问题</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="184"/>
        <source>Type your answer</source>
        <translation>输入你的答案</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="218"/>
        <source>Previous question</source>
        <translation>上一题</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="229"/>
        <source>Next question</source>
        <translation>下一题</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="235"/>
        <source>Skip</source>
        <translation>跳过</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="241"/>
        <location filename="../src/ui/ChoiceCard.cpp" line="391"/>
        <source>Submit</source>
        <translation>提交</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="254"/>
        <source>Expand the question card</source>
        <translation>展开问题卡片</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="391"/>
        <source>Next</source>
        <translation>下一题</translation>
    </message>
    <message>
        <location filename="../src/ui/ChoiceCard.cpp" line="475"/>
        <source>%1: %2</source>
        <translation>%1：%2</translation>
    </message>
</context>
<context>
    <name>ui::ConversationView</name>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="32"/>
        <source>You</source>
        <translation>你</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="36"/>
        <source>System</source>
        <translation>系统</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="38"/>
        <source>Unknown</source>
        <translation>未知</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="45"/>
        <source>Waiting</source>
        <translation>等待中</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="47"/>
        <source>Generating</source>
        <translation>生成中</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="51"/>
        <source>Interrupted</source>
        <translation>已中断</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="53"/>
        <source>Failed</source>
        <translation>失败</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="267"/>
        <source>Reasoning</source>
        <translation>思考过程</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="325"/>
        <source>Image</source>
        <translation>图片</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="343"/>
        <source>Artifacts:</source>
        <translation>产物：</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="352"/>
        <source>Subagents:</source>
        <translation>子代理：</translation>
    </message>
    <message>
        <location filename="../src/ui/ConversationView.cpp" line="578"/>
        <source>Start a new session

Describe what you want to get done in this workspace. LyCode will read code, run commands, and make the changes.</source>
        <translation>开始一个新会话

在工作区里描述你想完成的任务，LyCode 会读取代码、执行命令并给出改动。</translation>
    </message>
</context>
<context>
    <name>ui::DiffView</name>
    <message>
        <location filename="../src/ui/DiffView.cpp" line="180"/>
        <source>&lt;div style=&quot;color:%1;&quot;&gt;… %2 more lines not shown (at most %3 lines are rendered at once)&lt;/div&gt;</source>
        <translation>&lt;div style=&quot;color:%1;&quot;&gt;… 还有 %2 行未显示（单次最多渲染 %3 行）&lt;/div&gt;</translation>
    </message>
</context>
<context>
    <name>ui::FileViewerDialog</name>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="243"/>
        <source>View</source>
        <translation>查看</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="262"/>
        <source>View contents</source>
        <translation>查看内容</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="291"/>
        <source>Cannot read the file: it does not exist or you lack permission.
%1</source>
        <translation>无法读取内容：文件不存在或没有权限。
%1</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="300"/>
        <source>File is too large (%1 MB) to display here.
Open it with the system application instead.</source>
        <translation>文件过大（%1 MB），不在窗口内展示。
请用系统程序打开。</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="314"/>
        <source>Close</source>
        <translation>关闭</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="320"/>
        <source>Open with system application</source>
        <translation>用系统程序打开</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="336"/>
        <source>The image could not be decoded.</source>
        <translation>图片无法解码。</translation>
    </message>
    <message>
        <location filename="../src/ui/FileViewerDialog.cpp" line="399"/>
        <source>%1  ·  %2 lines  ·  %3 KB</source>
        <translation>%1  ·  %2 行  ·  %3 KB</translation>
    </message>
</context>
<context>
    <name>ui::MainWindow</name>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="77"/>
        <location filename="../src/ui/MainWindow.cpp" line="89"/>
        <source>Ready</source>
        <translation>就绪</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="79"/>
        <source>Generating…</source>
        <translation>正在生成…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="81"/>
        <source>Running tools…</source>
        <translation>正在执行工具…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="83"/>
        <source>Waiting for your approval…</source>
        <translation>等待你确认…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="85"/>
        <location filename="../src/ui/MainWindow.cpp" line="1942"/>
        <source>Interrupting…</source>
        <translation>正在中断…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="87"/>
        <source>Error</source>
        <translation>出错</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="118"/>
        <source>Context %1 / %2 · auto-compacts at %3</source>
        <translation>上下文 %1 / %2 · 自动压缩于 %3</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="121"/>
        <source>Context %1 / %2</source>
        <translation>上下文 %1 / %2</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="136"/>
        <source>%1 / %2 tokens used</source>
        <translation>已用 %1 / %2 tokens</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="138"/>
        <source>· At %1 or more: old tool output is trimmed on the way out (no model call)</source>
        <translation>· 达到 %1 起：下发时裁剪旧工具输出（不调模型）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="142"/>
        <source>· At %1 or more: the model writes a summary that replaces earlier history</source>
        <translation>· 达到 %1 起：调用模型生成摘要，替换较早的历史</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="146"/>
        <source>· This session has been compacted; the original messages are kept in the archive</source>
        <translation>· 本会话已经压缩过上下文，原始消息保存在归档里</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="149"/>
        <source>Type /compact to compact now.</source>
        <translation>输入 /compact 可立即压缩。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="158"/>
        <source>Cached %1% · uncached %2 · cache read %3 · output %4</source>
        <translation>缓存 %1% · 未缓存 %2 · 缓存读 %3 · 输出 %4</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="168"/>
        <source>%1
  Uncached input  %2
  Cache read      %3
  Cache write     %4
  Output          %5
  Cache hit rate  %6%</source>
        <translation>%1
  未缓存输入  %2
  缓存读取    %3
  缓存写入    %4
  输出        %5
  缓存命中率  %6%</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="178"/>
        <source>Cache hit rate = cache read / (cache read + uncached input).
The denominator excludes cache writes: a first write never had a chance to hit.

%1

%2</source>
        <translation>缓存命中率 = 缓存读取 /（缓存读取 + 未缓存输入）。
分母不含缓存写入：首次写入的部分本来就没有机会命中。

%1

%2</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="180"/>
        <source>This session (cumulative)</source>
        <translation>本次会话累计</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="181"/>
        <source>Last turn</source>
        <translation>最近一轮</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="197"/>
        <source>Failed to load settings; defaults are in use: </source>
        <translation>配置载入失败，已使用默认设置：</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="221"/>
        <source>Session storage unavailable</source>
        <translation>会话存储不可用</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="222"/>
        <source>Cannot open the session database:
%1

History will not be saved during this run.</source>
        <translation>无法打开会话数据库：
%1

本次运行不会保存历史记录。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="256"/>
        <source>MCP server &quot;%1&quot; unavailable: %2</source>
        <translation>MCP 服务器「%1」不可用：%2</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="418"/>
        <source>Describe what you want to get done… (Enter to send, Shift+Enter for a newline, paste images)
Type /compact to compact the context, /help for commands</source>
        <translation>描述你想完成的任务…（Enter 发送，Shift+Enter 换行，可粘贴图片）
输入 /compact 压缩上下文，/help 查看命令</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="436"/>
        <source>Session mode: plan is read-only, build is the default, edit edits, yolo skips approvals</source>
        <translation>会话模式：plan 只读规划 / build 默认 / edit 编辑 / yolo 免确认</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="438"/>
        <source>plan · read-only</source>
        <translation>plan 只读规划</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="440"/>
        <source>build · default</source>
        <translation>build 默认</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="442"/>
        <source>edit · edit</source>
        <translation>edit 编辑</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="443"/>
        <source>yolo · no approvals</source>
        <translation>yolo 免确认</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="448"/>
        <source>Image</source>
        <translation>图片</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="450"/>
        <source>Attach an image (you can also paste with Ctrl+V or drag one in)</source>
        <translation>附加图片（也可以直接 Ctrl+V 粘贴或拖入）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="461"/>
        <source>Choose the model for this session</source>
        <translation>选择本会话使用的模型</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="470"/>
        <source>Reasoning level: higher means more thorough reasoning and more tokens</source>
        <translation>思考等级：越高推理越充分，消耗的 token 也越多</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="475"/>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="481"/>
        <source>Send</source>
        <translation>发送</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="528"/>
        <source>Context 000.0 / 2000.0M</source>
        <translation>上下文 000.0 / 2000.0M</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="547"/>
        <source>Cached 100% · uncached 000.0M · cache read 000.0M · output 000.0M</source>
        <translation>缓存 100% · 未缓存 000.0M · 缓存读 000.0M · 输出 000.0M</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="555"/>
        <source>File</source>
        <translation>文件</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="557"/>
        <source>New session</source>
        <translation>新建会话</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="561"/>
        <source>Open Workspace…</source>
        <translation>打开工作区…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="566"/>
        <source>Settings…</source>
        <translation>设置…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="572"/>
        <source>Quit</source>
        <translation>退出</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="576"/>
        <source>View</source>
        <translation>视图</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="607"/>
        <source>Increase font size</source>
        <translation>增大字号</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="615"/>
        <source>Decrease font size</source>
        <translation>减小字号</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="623"/>
        <source>Help</source>
        <translation>帮助</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="624"/>
        <location filename="../src/ui/MainWindow.cpp" line="627"/>
        <source>About LyCode</source>
        <translation>关于 LyCode</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="628"/>
        <source>&lt;b&gt;LyCode&lt;/b&gt; %1&lt;br/&gt;&lt;br/&gt;An AI coding workbench — native Qt6 / C++.&lt;br/&gt;Rewritten from the LyCode design spec and agent semantics.</source>
        <translation>&lt;b&gt;LyCode&lt;/b&gt; %1&lt;br/&gt;&lt;br/&gt;AI 编程工作台 —— Qt6 / C++ 原生实现。&lt;br/&gt;基于 LyCode 的设计规范与 Agent 语义重写。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="811"/>
        <source> (no tool support)</source>
        <translation>（不支持工具）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="817"/>
        <source>(no model configured — add one in Settings)</source>
        <translation>（未配置模型，请在设置中添加）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="872"/>
        <source>Reasoning </source>
        <translation>思考 </translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1042"/>
        <source>Session is running</source>
        <translation>会话正在运行</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1043"/>
        <source>The current session is still running. Starting a new session will interrupt it. Continue?</source>
        <translation>当前会话仍在运行。新建会话会中断它，是否继续？</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1055"/>
        <source>No workspace yet — use &quot;Open Workspace…&quot; to pick a directory first.</source>
        <translation>还没有工作区：先用「打开工作区…」选择一个目录。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1063"/>
        <source>Model required</source>
        <translation>需要配置模型</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1064"/>
        <source>No usable model yet. Add a model under Settings → Model and fill in its model list.</source>
        <translation>还没有可用的模型。请在「设置 → 模型」中添加一个模型并填写模型列表。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2052"/>
        <source>No usable model right now — check the Base URL and API key.</source>
        <translation>当前没有可用的模型，请检查 Base URL 与 API Key。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1075"/>
        <source>Cannot create session</source>
        <translation>无法新建会话</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1171"/>
        <source>Delete session</source>
        <translation>删除会话</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1172"/>
        <source>Delete this session and all of its messages? This cannot be undone.</source>
        <translation>确定要删除这个会话及其全部消息吗？此操作不可撤销。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1188"/>
        <location filename="../src/ui/MainWindow.cpp" line="1327"/>
        <source>Delete failed: </source>
        <translation>删除失败：</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1193"/>
        <source>Session deleted.</source>
        <translation>会话已删除。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1226"/>
        <source>That workspace is not in the list.</source>
        <translation>该工作区不在列表里。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1233"/>
        <source>Removed from the list: %1 (sessions and files are untouched)</source>
        <translation>已从列表移除：%1（会话与文件都还在）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1266"/>
        <source>The last workspace was removed. Use &quot;Open Workspace…&quot; to pick a directory and continue.</source>
        <translation>已移除最后一个工作区。用「打开工作区…」选择目录即可继续。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1282"/>
        <source>Removed from the list: %1 (workspace files are untouched)</source>
        <translation>已从列表移除：%1（工作区文件未被改动）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1297"/>
        <source>This workspace has no sessions to delete.</source>
        <translation>该工作区没有可删除的会话。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1304"/>
        <source>Delete workspace sessions</source>
        <translation>删除工作区的会话</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1305"/>
        <source>This deletes all %2 sessions under &quot;%1&quot; and their messages.

This cannot be undone.
The workspace directory itself and the files inside it will **not** be deleted.</source>
        <translation>将删除「%1」下的全部 %2 个会话及其消息。

此操作不可撤销。
工作区目录本身和其中的文件**不会**被删除。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1337"/>
        <source>Deleted %2 sessions under &quot;%1&quot; (workspace files are untouched).</source>
        <translation>已删除「%1」的 %2 个会话（工作区文件未受影响）。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1346"/>
        <source>Select workspace directory</source>
        <translation>选择工作区目录</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1360"/>
        <source>Directory does not exist: </source>
        <translation>目录不存在：</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1398"/>
        <source>Switched to workspace: </source>
        <translation>已切换工作区：</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1472"/>
        <source>Done.</source>
        <translation>完成。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1482"/>
        <source>Interrupted.</source>
        <translation>已中断。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1487"/>
        <source>This turn was not executed.</source>
        <translation>本次回合未执行。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1514"/>
        <source>The subagent finished; see the sidebar list for its full run.</source>
        <translation>子代理已完成，可在左侧列表查看它的完整过程。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1516"/>
        <source>The subagent failed; see that session in the sidebar list.</source>
        <translation>子代理执行失败，详见左侧列表中的该会话。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1534"/>
        <source>· Background tasks %1</source>
        <translation>· 后台任务 %1</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1538"/>
        <source>%1 background task(s) running. Use TaskOutput to read their output, or TaskStop on a tool card to stop them.</source>
        <translation>有 %1 个后台任务正在运行。用 TaskOutput 查看它们的输出，或在工具卡片里用 TaskStop 终止。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1571"/>
        <source>Connected to %1 MCP server(s); the tools they provide are registered as regular tools the model can call.</source>
        <translation>已连接 %1 个 MCP 服务器；它们提供的工具已注册为普通工具，模型可以直接调用。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1655"/>
        <source>Attach image</source>
        <translation>附加图片</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1656"/>
        <source>Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;All files (*)</source>
        <translation>图片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;所有文件 (*)</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1668"/>
        <source>Cannot read: %1</source>
        <translation>无法读取：%1</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1690"/>
        <source>Only image attachments are supported; got %1.</source>
        <translation>只支持图片附件，收到的是 %1。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1698"/>
        <source>Image too large (%1 MB); the limit is 8 MB.</source>
        <translation>图片过大（%1 MB），上限 8 MB。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1705"/>
        <source>Pasted image</source>
        <translation>粘贴的图片</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1769"/>
        <source>Remove this image</source>
        <translation>移除这张图片</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1841"/>
        <source>No session yet, so there is nothing to compact.</source>
        <translation>还没有会话，无法压缩上下文。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1854"/>
        <source>Compacting the context…</source>
        <translation>正在压缩上下文…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1860"/>
        <source>Commands: /compact compacts the context, /help shows this note.</source>
        <translation>命令：/compact 压缩上下文，/help 查看这条说明。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1867"/>
        <source>Unknown command: %1 (available: /compact, /help)</source>
        <translation>未知命令：%1（可用 /compact、/help）</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="1895"/>
        <location filename="../src/ui/MainWindow.cpp" line="2047"/>
        <source>Connecting to MCP servers…</source>
        <translation>正在连接 MCP 服务器…</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2056"/>
        <source>Settings saved.</source>
        <translation>设置已保存。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2066"/>
        <source>Interface language changed. Reopen the app for it to take full effect.</source>
        <translation>界面语言已切换。重新打开应用后完全生效。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2082"/>
        <source>Interface language</source>
        <translation>界面语言</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2083"/>
        <source>The interface language has been changed. Restart now to apply it?</source>
        <translation>界面语言已更改。是否立即重启以应用？</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2096"/>
        <source>Could not restart automatically. Please close and reopen the app.</source>
        <translation>无法自动重启，请手动关闭并重新打开应用。</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2108"/>
        <source>Failed to save settings: </source>
        <translation>设置保存失败：</translation>
    </message>
    <message>
        <location filename="../src/ui/MainWindow.cpp" line="2161"/>
        <source>pasted-image.png</source>
        <translation>粘贴的图片.png</translation>
    </message>
</context>
<context>
    <name>ui::PermissionDialog</name>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="80"/>
        <source>Low</source>
        <translation>低</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="82"/>
        <location filename="../src/ui/PermissionDialog.cpp" line="88"/>
        <source>Medium</source>
        <translation>中</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="84"/>
        <source>High</source>
        <translation>高</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="86"/>
        <source>Critical</source>
        <translation>严重</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="112"/>
        <source>Read</source>
        <translation>读取</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="114"/>
        <source>Write</source>
        <translation>写入</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="116"/>
        <source>Execute</source>
        <translation>执行</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="118"/>
        <source>Network</source>
        <translation>网络</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="120"/>
        <location filename="../src/ui/PermissionDialog.cpp" line="122"/>
        <source>Other</source>
        <translation>其它</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="131"/>
        <source>Allow once</source>
        <translation>允许一次</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="133"/>
        <source>Always allow</source>
        <translation>始终允许</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="135"/>
        <source>Deny</source>
        <translation>拒绝</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="137"/>
        <source>Custom</source>
        <translation>自定义</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="139"/>
        <source>Continue</source>
        <translation>继续</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="304"/>
        <source>
… content too long, truncated (%1 characters total)</source>
        <translation>
… 内容过长，已截断（共 %1 字符）</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="327"/>
        <source>The user closed the confirmation dialog</source>
        <translation>用户关闭了确认窗口</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="331"/>
        <source>Permission request — </source>
        <translation>权限确认 — </translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="359"/>
        <source>The permission request has no options</source>
        <translation>权限请求没有可选项</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="427"/>
        <source>Risk level: </source>
        <translation>风险等级：</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="433"/>
        <source>Permission type: </source>
        <translation>权限类别：</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="443"/>
        <source>Tool that made this request</source>
        <translation>发起本次请求的工具</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="453"/>
        <source>(no arguments)</source>
        <translation>（无入参）</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="464"/>
        <source>Command</source>
        <translation>命令</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="472"/>
        <source>Command about to run (selectable)</source>
        <translation>即将执行的命令（可选中复制）</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="482"/>
        <source>Other arguments</source>
        <translation>其它入参</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="492"/>
        <source>Arguments</source>
        <translation>入参</translation>
    </message>
    <message>
        <location filename="../src/ui/PermissionDialog.cpp" line="618"/>
        <source>Operation awaiting approval</source>
        <translation>需要确认的操作</translation>
    </message>
</context>
<context>
    <name>ui::SettingsDialog</name>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="150"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="631"/>
        <source>OpenAI-compatible</source>
        <translation>OpenAI 兼容</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="157"/>
        <source>Available</source>
        <translation>可用</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="159"/>
        <source>Checking</source>
        <translation>等待中</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="161"/>
        <source>Unavailable</source>
        <translation>不可用</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="163"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="165"/>
        <source>Unknown</source>
        <translation>未知</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="174"/>
        <source>Not signed in</source>
        <translation>未登录</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="176"/>
        <source>Not connected</source>
        <translation>未连接</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="178"/>
        <source>Credentials expired</source>
        <translation>凭据失效</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="180"/>
        <source>No permission</source>
        <translation>无权限</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="191"/>
        <source>plan · read-only</source>
        <translation>plan · 只读规划</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="193"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="201"/>
        <source>build · default</source>
        <translation>build · 默认</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="195"/>
        <source>edit · edit</source>
        <translation>edit · 编辑</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="197"/>
        <source>yolo · no approvals</source>
        <translation>yolo · 免确认</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="199"/>
        <source>auto · internal</source>
        <translation>auto · 内部态</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="260"/>
        <source>Comma-separated level ids, for example %1.
Valid values: %2
Leave empty to use the default (whatever the model reports).</source>
        <translation>逗号分隔的档位 id，例如 %1。
合法取值：%2
留空 = 沿用默认（由模型自报的档位决定）。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="350"/>
        <source>(unnamed)</source>
        <translation>（未命名）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="357"/>
        <source>No Base URL set</source>
        <translation>未设置 Base URL</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="359"/>
        <source> · disabled</source>
        <translation> · 已停用</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="372"/>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="395"/>
        <source>Appearance</source>
        <translation>外观</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="397"/>
        <source>Session</source>
        <translation>会话</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="476"/>
        <source>Follow system</source>
        <translation>跟随系统</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="478"/>
        <source>Light</source>
        <translation>浅色</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="479"/>
        <source>Dark</source>
        <translation>深色</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="497"/>
        <source>简体中文 (zh-CN)</source>
        <translation>简体中文 (zh-CN)</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="505"/>
        <source>Theme</source>
        <translation>主题模式</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="506"/>
        <source>UI font size</source>
        <translation>界面字号</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="507"/>
        <source>Code font size</source>
        <translation>代码字号</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="508"/>
        <source>Language</source>
        <translation>语言</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="510"/>
        <source>Switches the interface language. English is the source text; Chinese comes from the bundled translation file.</source>
        <translation>切换界面语言。英文是源文案，中文来自随安装包一起发布的翻译文件。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="582"/>
        <source>Add</source>
        <translation>新增</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="584"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1603"/>
        <source>Delete</source>
        <translation>删除</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="589"/>
        <source>Move up</source>
        <translation>上移</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="590"/>
        <source>Move down</source>
        <translation>下移</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="622"/>
        <source>For example: Company gateway</source>
        <translation>例如：公司网关</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="624"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1398"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1574"/>
        <source>Name</source>
        <translation>名称</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="633"/>
        <source>Protocol</source>
        <translation>协议</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="652"/>
        <source>Show</source>
        <translation>显示</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="653"/>
        <source>Reveals the key you just typed; a saved key is never filled back in</source>
        <translation>临时显示本次输入的密钥；已保存的密钥不会回填</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="670"/>
        <source>Model list</source>
        <translation>模型列表</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="672"/>
        <source>One model id per line; blank lines are ignored.</source>
        <translation>每行一个模型 id，空行会被忽略。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="682"/>
        <source>Pick from list…</source>
        <translation>从列表选择…</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="686"/>
        <source>Pick from the built-in model catalog, including context window and reasoning levels</source>
        <translation>从内置模型目录选择，自动带上上下文窗口与思考档位</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="695"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1417"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1575"/>
        <source>Enabled</source>
        <translation>启用</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="705"/>
        <source>Model capabilities</source>
        <translation>模型能力</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="746"/>
        <source>Fill in the model list above first</source>
        <translation>请先在上方填写模型列表</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="766"/>
        <source>Account status (a runtime field; read-only here)</source>
        <translation>账号状态（运行时字段，本对话框只读展示）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="880"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="897"/>
        <source>Not configured</source>
        <translation>未配置</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="881"/>
        <source>Configured; leave empty to keep it unchanged</source>
        <translation>已配置，留空表示不修改</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="886"/>
        <source>Availability: </source>
        <translation>可用性：</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="890"/>
        <source>Reason: </source>
        <translation>原因：</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="956"/>
        <source>Name cannot be empty</source>
        <translation>名称不能为空</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="959"/>
        <source>Base URL cannot be empty</source>
        <translation>Base URL 不能为空</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1053"/>
        <source>Models</source>
        <translation>模型</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1053"/>
        <source>Context window</source>
        <translation>上下文窗口</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1053"/>
        <source>Max output</source>
        <translation>最大输出</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1054"/>
        <source>Reasoning levels</source>
        <translation>思考档位</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1054"/>
        <source>Default level</source>
        <translation>默认档位</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1124"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1137"/>
        <source>Default</source>
        <translation>默认</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1126"/>
        <source>0 keeps the built-in default (usually 128000; 200000 for Claude models)</source>
        <translation>留 0 表示沿用内置默认（通常 128000，Claude 系列 200000）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1138"/>
        <source>0 keeps the built-in default (usually 8192)</source>
        <translation>留 0 表示沿用内置默认（通常 8192）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1155"/>
        <source>Reasoning level selected by default in new sessions</source>
        <translation>新建会话时默认选中的思考档位</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1202"/>
        <source>(unspecified)</source>
        <translation>（不指定）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1288"/>
        <source>Unknown reasoning level id: %1 (model %2)</source>
        <translation>未知档位 id：%1（模型 %2）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1387"/>
        <source>Add MCP server</source>
        <translation>添加 MCP 服务器</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1388"/>
        <source>Edit MCP server</source>
        <translation>编辑 MCP 服务器</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1396"/>
        <source>For example github (it becomes the tool name prefix)</source>
        <translation>例如 github（会出现在工具名前缀里）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1401"/>
        <source>For example npx / python3 / /abs/path/server</source>
        <translation>例如 npx / python3 / /abs/path/server</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1403"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1574"/>
        <source>Command</source>
        <translation>命令</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1406"/>
        <source>One argument per line, for example
-y
@modelcontextprotocol/server-github</source>
        <translation>每行一个参数，例如
-y
@modelcontextprotocol/server-github</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1409"/>
        <location filename="../src/ui/SettingsDialog.cpp" line="1575"/>
        <source>Arguments</source>
        <translation>参数</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1412"/>
        <source>One KEY=VALUE per line; layered on top of the inherited environment</source>
        <translation>每行一个 KEY=VALUE；会叠加在继承的环境之上</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1415"/>
        <source>Environment</source>
        <translation>环境变量</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1423"/>
        <source>Tool permissions are conservative by default: a tool the server does not mark read-only asks for your approval on every call.
A server&apos;s &quot;read-only&quot; claim is only trusted in the permissive direction — it is the server&apos;s word, not a verified fact.</source>
        <translation>工具的权限默认是保守的：服务器没声明只读的工具，每次调用都会请你确认。
「服务器自述只读」只在放宽方向采信——那是它的说法，不是可信信息。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1462"/>
        <source>Malformed environment variable</source>
        <translation>环境变量格式错误</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1463"/>
        <source>This line is missing &apos;=&apos;: %1
It should be written as KEY=VALUE.</source>
        <translation>这一行缺少 &apos;=&apos;：%1
应当写成 KEY=VALUE。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1481"/>
        <source>Model catalog unavailable</source>
        <translation>模型目录不可用</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1483"/>
        <source>The built-in model catalog is empty.</source>
        <translation>内置模型目录为空。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1501"/>
        <source>Pick a model from the list</source>
        <translation>从列表选择模型</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="396"/>
        <source>Model</source>
        <translation>模型</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="563"/>
        <source>Configured models</source>
        <translation>已配置模型</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="583"/>
        <source>Add a model</source>
        <translation>新增一个模型</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="587"/>
        <source>Delete the selected model (applies when you click OK)</source>
        <translation>删除选中的模型（点「确定」后生效）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="614"/>
        <source>No models configured yet. Click &quot;Add&quot; in the lower left to start.</source>
        <translation>尚未配置模型，点击左下角「新增」开始。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="697"/>
        <source>Disabled models do not appear in the model picker</source>
        <translation>停用后该模型不会出现在模型选择器中</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="713"/>
        <source>Overrides the metadata the model reports. Leave 0 or empty to keep the built-in default. Context window and output limit are in tokens.</source>
        <translation>覆盖模型自报的元信息；留 0 / 留空表示沿用内置默认。窗口与输出上限的单位是 token。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1007"/>
        <source>New model</source>
        <translation>新模型</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1502"/>
        <source>Models from the built-in catalog (selected ones are added to the current model list):</source>
        <translation>内置目录里的模型（选择后会加入当前模型列表）：</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1562"/>
        <source>MCP servers</source>
        <translation>MCP 服务器</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1566"/>
        <source>At startup these servers are launched as child processes. The tools they provide are registered as regular tools the model can call. Reopen the app for changes to reconnect (the child processes are terminated on exit).</source>
        <translation>应用启动时按下面的配置把这些服务器作为子进程拉起，它们提供的工具会注册成普通工具供模型调用。改完需要重新打开应用才会重连（退出时会终止这些子进程）。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1588"/>
        <source>Add…</source>
        <translation>添加…</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1595"/>
        <source>Edit…</source>
        <translation>编辑…</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1618"/>
        <source>Skill directories</source>
        <translation>Skills 目录</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1622"/>
        <source>Looks for &lt;name&gt;/SKILL.md or &lt;name&gt;.md in these directories. Empty means the default directories: the user-level &lt;data dir&gt;/skills and the current workspace&apos;s .lycode/skills.</source>
        <translation>在这些目录下发现 &lt;名称&gt;/SKILL.md 或 &lt;名称&gt;.md。留空表示使用默认目录：用户级 &lt;数据目录&gt;/skills，以及当前工作区的 .lycode/skills。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1640"/>
        <source>Add directory…</source>
        <translation>添加目录…</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1645"/>
        <source>Remove</source>
        <translation>移除</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1651"/>
        <source>Restore default directories</source>
        <translation>恢复默认目录</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1675"/>
        <source>Yes</source>
        <translation>是</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1676"/>
        <source>No</source>
        <translation>否</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1700"/>
        <source>Duplicate name</source>
        <translation>名称重复</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1701"/>
        <source>A server named &quot;%1&quot; already exists.</source>
        <translation>已经有一个叫「%1」的服务器了。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1727"/>
        <source>Default directories (click &quot;Add directory…&quot; to use a custom list)</source>
        <translation>默认目录（要改成自定义列表就点「添加目录…」）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1738"/>
        <source>No skills found.</source>
        <translation>当前没有发现任何技能。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1745"/>
        <source>Found %1 skill(s): %2</source>
        <translation>已发现 %1 个技能：%2</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1753"/>
        <source>Select skill directory</source>
        <translation>选择技能目录</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1813"/>
        <source>Persist sessions</source>
        <translation>持久化会话</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1819"/>
        <source>Generate session titles with the model</source>
        <translation>用模型生成会话标题</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1824"/>
        <source>When off, the title is the first 40 characters of your first input, with no extra model call</source>
        <translation>关闭后标题取首条输入的前 40 字符，不额外消耗模型调用</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1827"/>
        <source>When off, new sessions would exist only in memory (this version still persists them; the switch is kept for future evolution)</source>
        <translation>关闭后新建会话仅存在于内存中（当前版本仍会落盘，保留开关以便后续演进）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1830"/>
        <source>Default session mode</source>
        <translation>默认会话模式</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1843"/>
        <source>Recent workspaces</source>
        <translation>最近工作区</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1846"/>
        <source>Clear the list</source>
        <translation>清空记录</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1847"/>
        <source>Clear the recent workspace list (applies when you click OK)</source>
        <translation>清空最近工作区列表（点「确定」后生效）</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1860"/>
        <source>No recent workspaces yet.</source>
        <translation>暂无最近工作区记录。</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1922"/>
        <source>OK</source>
        <translation>确定</translation>
    </message>
    <message>
        <location filename="../src/ui/SettingsDialog.cpp" line="1926"/>
        <source>Cancel</source>
        <translation>取消</translation>
    </message>
</context>
<context>
    <name>ui::SidebarPanel</name>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="51"/>
        <source>%1 awaiting approval</source>
        <translation>%1 待确认</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="58"/>
        <source>Subagent · running</source>
        <translation>子代理 · 运行中</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="60"/>
        <source>Subagent · error</source>
        <translation>子代理 · 出错</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="62"/>
        <source>Subagent</source>
        <translation>子代理</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="68"/>
        <source>Running</source>
        <translation>运行中</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="70"/>
        <source>Error</source>
        <translation>出错</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="102"/>
        <source>Open Workspace…</source>
        <translation>打开工作区…</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="104"/>
        <source>Pick an existing directory to add to the workspace list</source>
        <translation>选择已有目录，把它加进工作区列表</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="184"/>
        <source>Remove from the recent list</source>
        <translation>从最近列表移除</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="185"/>
        <source>Delete all sessions in this workspace…</source>
        <translation>删除该工作区的全部会话…</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="196"/>
        <source>Delete session</source>
        <translation>删除会话</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="204"/>
        <source>No sessions yet.</source>
        <translation>还没有会话。</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="211"/>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="261"/>
        <source>Untitled session</source>
        <translation>未命名会话</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="435"/>
        <source>New session in &quot;%1&quot;</source>
        <translation>在「%1」里新建会话</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="617"/>
        <source>This workspace has no sessions yet.
Use the + on the workspace row to create one.</source>
        <translation>这个工作区还没有会话。
点工作区行右侧的 + 新建一个。</translation>
    </message>
    <message>
        <location filename="../src/ui/SidebarPanel.cpp" line="619"/>
        <source>No workspace yet.
Use &quot;Open Workspace…&quot; above to pick a directory.</source>
        <translation>还没有工作区。
点上面的「打开工作区…」选择一个目录。</translation>
    </message>
</context>
<context>
    <name>ui::ToolCallWidget</name>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="103"/>
        <source>Expand or collapse details</source>
        <translation>展开或收起详情</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="114"/>
        <source>Tool status</source>
        <translation>工具状态</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="141"/>
        <source>Arguments</source>
        <translation>入参</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="148"/>
        <location filename="../src/ui/ToolCallWidget.cpp" line="335"/>
        <source>Output</source>
        <translation>输出</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="157"/>
        <source>Changes</source>
        <translation>改动</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="244"/>
        <source>Receiving arguments</source>
        <translation>接收参数中</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="246"/>
        <source>Awaiting approval</source>
        <translation>等待确认</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="248"/>
        <source>Running</source>
        <translation>执行中</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="250"/>
        <source>Succeeded</source>
        <translation>成功</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="252"/>
        <source>Failed</source>
        <translation>失败</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="254"/>
        <source>Cancelled</source>
        <translation>已取消</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="256"/>
        <source>Unknown</source>
        <translation>未知</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="266"/>
        <source>(unnamed tool)</source>
        <translation>(未命名工具)</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="333"/>
        <source>Error</source>
        <translation>错误</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="351"/>
        <source>Receiving call arguments…</source>
        <translation>正在接收调用参数…</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="354"/>
        <source>It runs only after you approve.</source>
        <translation>等待你确认后才会执行。</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="357"/>
        <source>Running…</source>
        <translation>正在执行…</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="360"/>
        <source>This call was cancelled and produced no output.</source>
        <translation>该调用已取消，未产生输出。</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="364"/>
        <source>Succeeded with no output.</source>
        <translation>执行成功，无输出。</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="367"/>
        <source>Failed with no error details.</source>
        <translation>执行失败，无错误详情。</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="422"/>
        <source>Changes  +%1  −%2</source>
        <translation>改动  +%1  −%2</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="442"/>
        <source>View file  %1  ▸</source>
        <translation>查看文件  %1  ▸</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="444"/>
        <source>Click to view the full content: %1</source>
        <translation>点击查看完整内容：%1</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="474"/>
        <source>Click to view the original image (%1)</source>
        <translation>点击查看原图（%1）</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="475"/>
        <location filename="../src/ui/ToolCallWidget.cpp" line="484"/>
        <source>Image</source>
        <translation>图片</translation>
    </message>
    <message>
        <location filename="../src/ui/ToolCallWidget.cpp" line="493"/>
        <source>The image could not be decoded: %1</source>
        <translation>图片无法解码：%1</translation>
    </message>
</context>
</TS>
