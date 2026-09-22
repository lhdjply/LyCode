// LyCode — MCP 集成测试
//
// 用 tests/support/fake_mcp_server.cpp 那个**真的子进程**做端到端验证：
// 走的是 stdio + 换行分隔 JSON-RPC，和接真实 MCP 服务器完全同一条路径。
//
// 重点覆盖容易出错的地方：
//   * 握手顺序（少发 initialized 通知会被服务器拒绝）
//   * stdout 上的非 JSON 日志行不能把客户端打挂
//   * 工具自己报告失败（isError）与协议错误要区分开
//   * 远端工具的权限元数据默认必须保守（不能因为服务器"自称只读"就免确认，
//     但只读提示可以放宽到免确认）
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include "mcp/McpManager.h"
#include "tools/Tool.h"

using namespace lycode;
using namespace lycode::mcp;

namespace {

/// 假服务器可执行文件与本测试在同一目录。
///
/// Windows 上文件名带 `.exe`。CreateProcess 在没有扩展名时会自动补 `.exe`，
/// 但依赖这个隐式行为不够明确——显式拼出来，两个平台都确定。
QString fakeServerPath() {
    QString name = QStringLiteral("fake_mcp_server");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

/// 退出码的可读形式。Windows 的异常退出码是 NTSTATUS（负的十进制），
/// 只有十六进制才对得上微软文档，所以两个都给。
QString exitCodeText(int exitCode) {
    if (exitCode < 0) {
        return QStringLiteral("%1 (0x%2)")
            .arg(exitCode)
            .arg(static_cast<quint32>(exitCode), 8, 16, QLatin1Char('0'));
    }
    return QString::number(exitCode);
}

/// 把 Windows 上"进程起来了又立刻没了"的经典退出码翻译成人话。
///
/// 这类失败在 Windows 上占比很高（构建目录里的 exe 找不到 Qt DLL、
/// 被杀软拦下），但只看一个十进制数字根本看不出该修什么。
QString explainExitCode(int exitCode) {
    switch (static_cast<quint32>(exitCode)) {
        case 0xC0000135u:
            return QStringLiteral("STATUS_DLL_NOT_FOUND：子进程缺依赖 DLL。"
                                  "Windows 上跑构建目录里的 exe 需要 Qt 的 bin 目录在 PATH 里"
                                  "（或对产物执行 windeployqt）。");
        case 0xC0000142u:
            return QStringLiteral("STATUS_DLL_INIT_FAILED：依赖 DLL 初始化失败。");
        case 0xC0000005u:
            return QStringLiteral("STATUS_ACCESS_VIOLATION：子进程崩溃。");
        case 0xC00000FDu:
            return QStringLiteral("STATUS_STACK_OVERFLOW：子进程栈溢出。");
        case 0xC0000409u:
            return QStringLiteral("STATUS_STACK_BUFFER_OVERRUN：子进程被系统安全机制终止。");
        default:
            return {};
    }
}

/// 把一段字节输出压成一行，便于塞进诊断报告。
QString oneLine(const QByteArray &bytes, int maxChars = 400) {
    const QString text = QString::fromUtf8(bytes).simplified();
    if (text.isEmpty()) {
        return QStringLiteral("(空)");
    }
    return text.size() > maxChars ? text.left(maxChars) + QStringLiteral("…") : text;
}

/// 主动探测：绕开 Manager/Client，直接拉起假服务器走一步 initialize。
///
/// 这一步把两类失败彻底分开，这是整个诊断的关键：
///   * 探测就失败 → 环境问题（文件不在、缺 DLL、被杀软拦、stdin/stdout 不通）；
///   * 探测成功但 Manager 握手失败 → 问题在客户端逻辑。
/// 没有它，Windows 上只能看到一句"握手超时"，无法判断该修哪边。
QString probeFakeServer(int timeoutMs = 5000) {
    QStringList lines;
    lines << QStringLiteral("主动探测: 直接运行 %1").arg(fakeServerPath());

    QProcess process;
    process.setProgram(fakeServerPath());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(timeoutMs)) {
        lines << QStringLiteral("  ✗ 无法启动");
        lines << QStringLiteral("    启动错误: %1").arg(process.errorString());
        lines << QStringLiteral("    进程状态: %1")
                     .arg(process.state() == QProcess::NotRunning
                              ? QStringLiteral("NotRunning")
                              : QStringLiteral("Starting/Running"));
        lines << QStringLiteral("  结论: 假服务器连启动都失败 → 先修环境，不是客户端逻辑问题");
        return lines.join(QLatin1Char('\n'));
    }
    lines << QStringLiteral("  ✓ 已启动");

    // initialize 是握手第一步，假服务器收到就回，不需要 initialized 通知。
    process.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n");
    process.waitForBytesWritten(1000);

    QByteArray out;
    QByteArray err;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (process.waitForReadyRead(200)) {
            out += process.readAllStandardOutput();
            err += process.readAllStandardError();
            if (out.contains("serverInfo")) {
                break;
            }
        }
        if (process.state() == QProcess::NotRunning) {
            break;
        }
    }
    out += process.readAllStandardOutput();
    err += process.readAllStandardError();

    const bool answered = out.contains("serverInfo");
    lines << QStringLiteral("  收到 initialize 响应: %1")
                 .arg(answered ? QStringLiteral("是") : QStringLiteral("否"));
    lines << QStringLiteral("  stdout: %1").arg(oneLine(out));
    lines << QStringLiteral("  stderr: %1").arg(oneLine(err));

    process.closeWriteChannel();  // EOF：getline 结束，服务器自行退出
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(1000);
    }
    const int code = process.exitCode();
    const QString hint = explainExitCode(code);
    lines << QStringLiteral("  退出码: %1%2")
                 .arg(exitCodeText(code),
                      hint.isEmpty() ? QString() : QStringLiteral(" —— ") + hint);
    lines << QStringLiteral("  退出状态: %1")
                 .arg(process.exitStatus() == QProcess::NormalExit
                          ? QStringLiteral("NormalExit")
                          : QStringLiteral("CrashExit"));
    lines << (answered
                  ? QStringLiteral("  结论: 假服务器本身正常 → 问题在客户端/Manager 侧，"
                                   "看下面的进程诊断")
                  : QStringLiteral("  结论: 连主动探测都拿不到响应 → 先修环境（见上），"
                                   "不是客户端逻辑问题"));
    return lines.join(QLatin1Char('\n'));
}

/// 把 Manager 侧与子进程侧的诊断拼成一份可读报告。
QString mcpDiagnostics(Manager &manager, const QString &serverId) {
    const QString path = fakeServerPath();
    const QFileInfo info(path);
    QStringList lines;
    lines << QStringLiteral("──────── MCP 失败诊断 ────────");
    lines << QStringLiteral("假服务器路径: %1").arg(path);
    lines << QStringLiteral("  存在: %1；可执行: %2；大小: %3 字节；最后修改: %4")
                 .arg(info.exists() ? QStringLiteral("是") : QStringLiteral("否"),
                      info.isExecutable() ? QStringLiteral("是") : QStringLiteral("否"))
                 .arg(info.size())
                 .arg(info.lastModified().toString(Qt::ISODate));
    lines << QStringLiteral("Manager 侧状态: %1（ready=%2 pending=%3）")
                 .arg(serverStateToken(manager.state(serverId)))
                 .arg(manager.readyCount())
                 .arg(manager.pendingCount());
    lines << QStringLiteral("Manager 记录的失败原因: %1")
                 .arg(manager.lastError(serverId).isEmpty()
                          ? QStringLiteral("(无)")
                          : manager.lastError(serverId));
    lines << QStringLiteral("--- 客户端进程诊断 ---");
    lines << manager.diagnostics(serverId);
    lines << QStringLiteral("--- 主动探测（绕开客户端）---");
    lines << probeFakeServer();
    lines << QStringLiteral("──────────────────────────────");
    return lines.join(QLatin1Char('\n'));
}

/// 等 ready / failed 先到者，返回是否就绪。
///
/// 不能只 `QSignalSpy::wait(ready)`：failed 不会唤醒它，于是"命令不存在"
/// 这种立刻就能判定的事也要干等满 15 秒，报告里还只写"握手超时"。
bool waitForReadyOrFailed(QSignalSpy &readySpy, QSignalSpy &failedSpy,
                          int timeoutMs = 15000) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (readySpy.count() > 0) {
            return true;
        }
        if (failedSpy.count() > 0) {
            return false;
        }
        QTest::qWait(20);
    }
    return readySpy.count() > 0;
}

/// 只在失败时生成报告：把完整诊断无条件写进测试日志（QWARN），
/// 返回给 QVERIFY2 的只是一句指路，避免同一份十几行报告在日志里出现两次。
///
/// 为什么坚持写日志而不是只靠 QVERIFY2 的消息：部分 ctest 配置会把断言消息
/// 截断，而失败诊断是这条测试唯一的价值所在，不能丢。
QString failureReportIf(bool ok, Manager &manager, const QString &serverId) {
    if (ok) {
        return {};
    }
    qWarning().noquote() << mcpDiagnostics(manager, serverId);
    return QStringLiteral("（详细诊断见上方「MCP 失败诊断」这段日志）");
}

ServerConfig fakeConfig(const QString &id = QStringLiteral("fake")) {
    ServerConfig config;
    config.id = id;
    config.command = fakeServerPath();
    return config;
}

}  // namespace

class TestMcp : public QObject {
    Q_OBJECT

private slots:
    void handshakesAndDiscoversTools();
    void registersRemoteToolsWithConservativePermissions();
    void callsToolAndSeparatesToolErrorFromProtocolError();
    void reportsBadCommandInsteadOfHanging();
    void disabledServerIsSkipped();
    void toolNamesAreNamespacedAndSanitized();
};

void TestMcp::handshakesAndDiscoversTools() {
    Manager manager;
    QSignalSpy readySpy(&manager, &Manager::serverReady);
    QSignalSpy failedSpy(&manager, &Manager::serverFailed);

    manager.startAll({fakeConfig()});
    const bool ready = waitForReadyOrFailed(readySpy, failedSpy);
    QVERIFY2(ready, qPrintable(QStringLiteral("MCP 服务器应当完成握手\n%1")
                                   .arg(failureReportIf(ready, manager,
                                                        QStringLiteral("fake")))));
    QCOMPARE(failedSpy.count(), 0);

    QCOMPARE(manager.readyCount(), 1);
    QCOMPARE(manager.state(QStringLiteral("fake")), ServerState::Ready);
    QCOMPARE(manager.toolCount(QStringLiteral("fake")), 2);

    // 非 JSON 的日志行出现在 initialize 响应**之前**。如果客户端把它当成
    // 致命错误，握手就会失败——这里通过就说明跳过了。
    // （服务器端还验证了 initialized 通知确实收到，否则 tools/list 会被拒。）
}

void TestMcp::registersRemoteToolsWithConservativePermissions() {
    Manager manager;
    QSignalSpy readySpy(&manager, &Manager::serverReady);
    QSignalSpy failedSpy(&manager, &Manager::serverFailed);
    manager.startAll({fakeConfig()});
    const bool ready = waitForReadyOrFailed(readySpy, failedSpy);
    QVERIFY2(ready, qPrintable(QStringLiteral("MCP 服务器应当完成握手\n%1")
                                   .arg(failureReportIf(ready, manager,
                                                        QStringLiteral("fake")))));

    ToolRegistry registry;
    const int added = manager.registerToolsInto(registry);
    QCOMPARE(added, 2);

    // 工具名带服务器前缀：避免不同服务器的同名工具互相覆盖，
    // 也让模型一眼看出这是远端工具。
    Tool *echo = registry.find(QStringLiteral("mcp__fake__echo"));
    Tool *mutate = registry.find(QStringLiteral("mcp__fake__mutate"));
    QVERIFY2(echo != nullptr, "只读工具应当注册");
    QVERIFY2(mutate != nullptr, "写入工具应当注册");

    // 服务器声明了 readOnlyHint → 可以免确认。
    QVERIFY2(echo->metadata().readOnly, "readOnlyHint 应当被采信");
    QVERIFY2(!echo->metadata().needsApproval, "只读远端工具不该每次都弹确认");

    // 没声明 → 保守：需要确认，独占执行，作用域按 System。
    QVERIFY2(!mutate->metadata().readOnly, "未声明只读的远端工具必须按可写处理");
    QVERIFY2(mutate->metadata().needsApproval, "未声明的远端工具必须要求确认");
    QVERIFY2(mutate->metadata().sideEffectScope == SideEffectScope::System,
             "未声明的远端工具按 System 处理");
    QVERIFY2(!mutate->canRunInParallel(), "远端工具无法判断是否安全并发，一律独占");

    // 入参 schema 原样透传，不做修补。
    const QJsonObject schema = echo->inputSchema();
    QCOMPARE(schema.value(QStringLiteral("type")).toString(), QStringLiteral("object"));

    // 第二次注册应当是空操作（幂等），否则会反复覆盖同名工具。
    QCOMPARE(manager.registerToolsInto(registry), 0);
}

void TestMcp::callsToolAndSeparatesToolErrorFromProtocolError() {
    Manager manager;
    QSignalSpy readySpy(&manager, &Manager::serverReady);
    QSignalSpy failedSpy(&manager, &Manager::serverFailed);
    manager.startAll({fakeConfig()});
    const bool ready = waitForReadyOrFailed(readySpy, failedSpy);
    QVERIFY2(ready, qPrintable(QStringLiteral("MCP 服务器应当完成握手\n%1")
                                   .arg(failureReportIf(ready, manager,
                                                        QStringLiteral("fake")))));

    ToolRegistry registry;
    manager.registerToolsInto(registry);

    ToolContext context;
    context.sessionId = QStringLiteral("session_test");
    context.workspace = Workspace{QDir::tempPath(), {}, {}};

    // ① 成功调用。
    Tool *echo = registry.find(QStringLiteral("mcp__fake__echo"));
    QVERIFY(echo != nullptr);
    ToolResult echoResult;
    bool echoDone = false;
    echo->execute(QJsonObject{{QStringLiteral("text"), QStringLiteral("你好")}}, context,
                  [&](ToolResult result) {
                      echoDone = true;
                      echoResult = result;
                  });
    QTRY_VERIFY_WITH_TIMEOUT(echoDone, 15000);
    QVERIFY2(echoResult.ok, qPrintable(echoResult.error));
    QVERIFY2(echoResult.output.contains(QStringLiteral("echo: 你好")),
             qPrintable(echoResult.output));
    // 结果里带上服务器与工具名，便于排查"这个输出来自哪"。
    QCOMPARE(echoResult.metadata.value(QStringLiteral("mcpServer")).toString(),
             QStringLiteral("fake"));
    QCOMPARE(echoResult.metadata.value(QStringLiteral("mcpTool")).toString(),
             QStringLiteral("echo"));

    // ② 工具**自己**报告失败：MCP 用 isError 表达，不是 JSON-RPC error。
    //    我们必须把它翻译成 ToolResult 失败，而不是"调用成功但内容写着拒绝"。
    Tool *mutate = registry.find(QStringLiteral("mcp__fake__mutate"));
    QVERIFY(mutate != nullptr);
    ToolResult mutateResult;
    bool mutateDone = false;
    mutate->execute(QJsonObject{{QStringLiteral("text"), QStringLiteral("x")}}, context,
                    [&](ToolResult result) {
                        mutateDone = true;
                        mutateResult = result;
                    });
    QTRY_VERIFY_WITH_TIMEOUT(mutateDone, 15000);
    QVERIFY2(!mutateResult.ok, "isError=true 必须映射成失败");
    // 文本进的是 error 而不是 output（ToolResult::failure 的约定）。
    QVERIFY2(mutateResult.error.contains(QStringLiteral("拒绝修改")),
             qPrintable(QStringLiteral("error=%1 output=%2")
                            .arg(mutateResult.error, mutateResult.output)));
}

void TestMcp::reportsBadCommandInsteadOfHanging() {
    Manager manager;
    QSignalSpy failedSpy(&manager, &Manager::serverFailed);
    QSignalSpy readySpy(&manager, &Manager::serverReady);

    ServerConfig broken;
    broken.id = QStringLiteral("broken");
    broken.command = QStringLiteral("/nonexistent/definitely-not-a-real-mcp-server");

    manager.startAll({broken});
    // 坏命令正常是"立刻失败"，所以这里等 failed 而不是死等 15 秒。
    const bool ready = waitForReadyOrFailed(readySpy, failedSpy);
    QVERIFY2(!ready && failedSpy.count() > 0,
             qPrintable(QStringLiteral("启动失败必须被报告，而不是一直等\n%1")
                            .arg(failureReportIf(!ready && failedSpy.count() > 0, manager,
                                                 QStringLiteral("broken")))));
    QCOMPARE(readySpy.count(), 0);
    QCOMPARE(manager.readyCount(), 0);

    // 错误信息要能定位：至少包含失败的服务器 id。
    const QString reason = manager.lastError(QStringLiteral("broken"));
    QVERIFY2(!reason.isEmpty(), "必须有可读的失败原因");
    QCOMPARE(manager.state(QStringLiteral("broken")), ServerState::Failed);
}

void TestMcp::disabledServerIsSkipped() {
    Manager manager;
    QSignalSpy readySpy(&manager, &Manager::serverReady);
    QSignalSpy failedSpy(&manager, &Manager::serverFailed);

    ServerConfig config = fakeConfig();
    config.enabled = false;
    manager.startAll({config});

    // 给事件循环一点时间，确认什么都没发生（禁用不等于失败）。
    QTest::qWait(500);
    QCOMPARE(readySpy.count(), 0);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(manager.readyCount(), 0);
}

void TestMcp::toolNamesAreNamespacedAndSanitized() {
    QCOMPARE(Manager::qualifiedToolName(QStringLiteral("github"), QStringLiteral("list_issues")),
             QStringLiteral("mcp__github__list_issues"));
    QCOMPARE(Manager::toolNamePrefix(QStringLiteral("fs")), QStringLiteral("mcp__fs__"));

    // 服务器返回的名字可能带空格或斜杠，直接进声明会被不少 provider 拒绝。
    Manager manager;
    QSignalSpy readySpy(&manager, &Manager::serverReady);
    QSignalSpy failedSpy(&manager, &Manager::serverFailed);
    manager.startAll({fakeConfig()});
    const bool ready = waitForReadyOrFailed(readySpy, failedSpy);
    QVERIFY2(ready, qPrintable(QStringLiteral("MCP 服务器应当完成握手\n%1")
                                   .arg(failureReportIf(ready, manager,
                                                        QStringLiteral("fake")))));
    ToolRegistry registry;
    manager.registerToolsInto(registry);

    // 假服务器的工具名是干净的，这里断言的是"注册进去的名字都符合我们的约束"。
    for (const QString &name : registry.names()) {
        if (!name.startsWith(QStringLiteral("mcp__"))) {
            continue;
        }
        QVERIFY2(!name.contains(QLatin1Char(' ')), qPrintable(name));
        QVERIFY2(!name.contains(QLatin1Char('/')), qPrintable(name));
    }
}

QTEST_MAIN(TestMcp)

#include "test_mcp.moc"
