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
#include <QJsonArray>
#include <QJsonObject>
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
    QVERIFY2(readySpy.wait(15000), "MCP 服务器应当完成握手");
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
    manager.startAll({fakeConfig()});
    QVERIFY(readySpy.wait(15000));

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
    manager.startAll({fakeConfig()});
    QVERIFY(readySpy.wait(15000));

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
    QVERIFY2(failedSpy.wait(15000), "启动失败必须被报告，而不是一直等");
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
    manager.startAll({fakeConfig()});
    QVERIFY(readySpy.wait(15000));
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
