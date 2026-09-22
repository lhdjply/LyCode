// ZCode Qt — Agent 运行时集成测试
//
// 这里用**本地假网关**（QTcpServer 返回预置的 SSE 响应）真实驱动整条链路：
//
//   AgentRuntime → ProviderRegistry → OpenAI 兼容 Provider → SSE 解析
//                → 工具队列 → 权限门 → 真实工具 → 回填 → 下一轮模型步
//
// 之所以不用一个假的 ModelProvider：那样会跳过 Provider 与 SSE 层，
// 而这两层恰恰是最容易出错的地方（分帧、增量组装、收尾顺序）。
// 走真实 HTTP + SSE 才能验证它们与主循环的配合。
#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTcpSocket>

#include "agent/AgentRuntime.h"
#include "core/Ids.h"
#include "core/Json.h"
#include "model/ProviderRegistry.h"
#include "tools/Tool.h"
#include "tools/TodoStore.h"

#include "support/FakeGateway.h"

using namespace zcode;
using zcode::test::FakeGateway;
using zcode::test::jsonEscape;
using zcode::test::textResponse;
using zcode::test::toolCallResponse;

namespace {

}  // namespace

class TestAgentRuntime : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void plainTextTurnCompletes();
    void readToolRunsWithoutPrompt();
    void bashToolWaitsForPermissionAndRunsAfterAllow();
    void deniedToolFeedsErrorBackToModel();
    void planModeDeniesSideEffectTools();
    void abortInterruptsRunningTurn();
    void unknownToolFailsWithoutPrompt();
    void httpErrorFailsTheTurn();
    void reasoningLevelReachesProviderRequest();
    void contextWindowOverrideDrivesUsage();

private:
    /// 组装一个指向假网关的运行时。
    bool setupRuntime(SessionMode mode, PermissionMode permissionMode);

    std::unique_ptr<FakeGateway> gateway_;
    std::unique_ptr<ProviderRegistry> providers_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<TodoStore> todos_;
    std::unique_ptr<AgentRuntime> runtime_;

    QTemporaryDir tempDir_;
};

void TestAgentRuntime::init() {
    gateway_ = std::make_unique<FakeGateway>();
    QVERIFY(gateway_->start());

    providers_ = std::make_unique<ProviderRegistry>();
    tools_ = std::make_unique<ToolRegistry>();
    todos_ = std::make_unique<TodoStore>();
    runtime_ = std::make_unique<AgentRuntime>();

    // 每次测试用全新的临时工作区，避免相互污染。
    QVERIFY(tempDir_.isValid());
}

void TestAgentRuntime::cleanup() {
    // 先销毁运行时再销毁依赖：运行时析构会 abort 活跃的流。
    runtime_.reset();
    todos_.reset();
    tools_.reset();
    providers_.reset();
    gateway_.reset();
}

bool TestAgentRuntime::setupRuntime(SessionMode mode, PermissionMode permissionMode) {
    ProviderConfig config;
    config.id = QStringLiteral("test");
    config.name = QStringLiteral("Test Gateway");
    config.kind = ProviderKind::OpenAICompatible;
    config.baseUrl = gateway_->baseUrl();
    config.apiKey = QStringLiteral("test-key");
    config.models = {QStringLiteral("test-model")};
    config.enabled = true;
    config.availability = AccountAvailability::Available;
    providers_->replaceAll({config});

    *tools_ = ToolRegistry::createWithBuiltins();
    runtime_->setProviderRegistry(providers_.get());
    runtime_->setToolRegistry(tools_.get());
    runtime_->setTodoStore(todos_.get());
    // 故意不注入 SessionStore：运行时必须能在无持久化时正常工作。

    Workspace workspace;
    workspace.path = tempDir_.path();

    ModelSelection selection;
    selection.providerId = QStringLiteral("test");
    selection.modelId = QStringLiteral("test-model");

    QString error;
    if (!runtime_->startSession(workspace, mode, selection, &error)) {
        qWarning() << "startSession 失败:" << error;
        return false;
    }
    runtime_->permissionGate()->setMode(permissionMode);
    return true;
}

void TestAgentRuntime::plainTextTurnCompletes() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));
    gateway_->enqueue(textResponse("Hello there"));

    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QSignalSpy messageSpy(runtime_.get(), &AgentRuntime::messageAdded);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("hi"), &error), qPrintable(error));

    QVERIFY(finishedSpy.wait(5000));
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);

    // user + assistant 两条消息。
    QCOMPARE(messageSpy.count(), 2);
    const QList<Message> messages = runtime_->messages();
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).role, MessageRole::User);
    QCOMPARE(messages.at(1).role, MessageRole::Assistant);
    QCOMPARE(messages.at(1).parts.size(), 1);
    QCOMPARE(messages.at(1).parts.first().text.text, QStringLiteral("Hello there"));
    QCOMPARE(messages.at(1).status, MessageStatus::Complete);

    // usage 必须从流里被正确记账。
    QCOMPARE(messages.at(1).usage.inputTokens, 100);
    QCOMPARE(messages.at(1).usage.outputTokens, 20);

    QCOMPARE(runtime_->session().status, SessionStatus::CompletedSuccess);
    QCOMPARE(runtime_->modelStepCount(), 1);
    QCOMPARE(runtime_->toolCallCount(), 0);

    // 只发了一次 HTTP 请求：没有工具调用就不该再请求模型。
    QCOMPARE(gateway_->requestCount(), 1);
}

void TestAgentRuntime::readToolRunsWithoutPrompt() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    const QString path = tempDir_.filePath(QStringLiteral("sample.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("alpha\nbeta\ngamma\n");
    file.close();

    // 第 1 轮：Read 工具调用。第 2 轮：最终文本。
    gateway_->enqueue(toolCallResponse(QStringLiteral("Read"),
                                       "{\\\"file_path\\\":\\\"" + jsonEscape(path) + "\\\"",
                                       "}"));
    gateway_->enqueue(textResponse("I read the file"));

    QSignalSpy permissionSpy(runtime_.get(), &AgentRuntime::permissionRequested);
    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("read it"), &error), qPrintable(error));
    QVERIFY(finishedSpy.wait(8000));

    // Read 是只读工具：不得弹权限窗。
    QCOMPARE(permissionSpy.count(), 0);

    // 两轮模型步，两个 HTTP 请求。
    QCOMPARE(runtime_->modelStepCount(), 2);
    QCOMPARE(runtime_->toolCallCount(), 1);
    QCOMPARE(gateway_->requestCount(), 2);

    // 工具结果必须落成一条独立的 user 消息（全部 part 都是 Tool）。
    const QList<Message> messages = runtime_->messages();
    QCOMPARE(messages.size(), 4);  // user, assistant(tool), toolResults(user), assistant(final)
    QCOMPARE(messages.at(2).role, MessageRole::User);
    QVERIFY(!messages.at(2).parts.isEmpty());
    for (const Part &part : messages.at(2).parts) {
        QCOMPARE(part.kind, PartKind::Tool);
    }

    const ToolPart toolPart = messages.at(1).toolParts().first();
    QCOMPARE(toolPart.name, QStringLiteral("Read"));
    QCOMPARE(toolPart.state, ToolState::Success);
    QCOMPARE(toolPart.callId, QStringLiteral("call_1"));
    // 工具确实读到了文件内容。
    QVERIFY(toolPart.output.contains(QStringLiteral("alpha")));
    QVERIFY(toolPart.durationMs() >= 0);

    // 第二轮请求必须把工具结果带给模型。
    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);
}

void TestAgentRuntime::bashToolWaitsForPermissionAndRunsAfterAllow() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    gateway_->enqueue(toolCallResponse(
        QStringLiteral("Bash"), "{\\\"command\\\":\\\"echo zcode-ok\\\"", "}"));

    QSignalSpy permissionSpy(runtime_.get(), &AgentRuntime::permissionRequested);
    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("run it"), &error), qPrintable(error));

    // 必须先弹权限窗，且此时不该有第二个 HTTP 请求（工具还没执行）。
    QVERIFY(permissionSpy.wait(5000));
    QCOMPARE(permissionSpy.count(), 1);

    const auto request = permissionSpy.first().at(0).value<PermissionRequest>();
    QCOMPARE(request.toolName, QStringLiteral("Bash"));
    QCOMPARE(request.kind, PermissionKind::Execute);
    QCOMPARE(request.riskLevel, RiskLevel::High);
    QVERIFY(request.options.size() >= 2);
    QVERIFY(request.options.first().response.allowed());
    QCOMPARE(gateway_->requestCount(), 1);

    // 第二轮响应准备好，然后放行。
    gateway_->enqueue(textResponse("command finished"));

    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    QVERIFY(runtime_->resolvePermission(request.id, response));

    QVERIFY(finishedSpy.wait(8000));
    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);

    const QList<Message> messages = runtime_->messages();
    const ToolPart toolPart = messages.at(1).toolParts().first();
    QCOMPARE(toolPart.state, ToolState::Success);
    QVERIFY(toolPart.output.contains(QStringLiteral("zcode-ok")));

    // 执行前必须是 PendingApproval 而不是直接 Running。
    QCOMPARE(toolPart.metadata.value(QStringLiteral("exitCode")).toInt(), 0);
}

void TestAgentRuntime::deniedToolFeedsErrorBackToModel() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    gateway_->enqueue(toolCallResponse(
        QStringLiteral("Bash"), "{\\\"command\\\":\\\"echo nope\\\"", "}"));

    QSignalSpy permissionSpy(runtime_.get(), &AgentRuntime::permissionRequested);
    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("run it"), &error), qPrintable(error));
    QVERIFY(permissionSpy.wait(5000));

    const auto request = permissionSpy.first().at(0).value<PermissionRequest>();
    gateway_->enqueue(textResponse("understood, skipping"));

    PermissionResponse response;
    response.decision = PermissionDecision::Deny;
    response.reason = QStringLiteral("不要执行这个命令");
    QVERIFY(runtime_->resolvePermission(request.id, response));

    QVERIFY(finishedSpy.wait(8000));
    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);

    const ToolPart toolPart = runtime_->messages().at(1).toolParts().first();
    // 拒绝以"工具失败"的形式回灌给模型，而不是静默跳过——
    // 否则模型会以为自己执行成功了。
    QCOMPARE(toolPart.state, ToolState::Error);
    QCOMPARE(toolPart.errorCode, QStringLiteral("permission_denied"));
    QVERIFY(toolPart.error.contains(QStringLiteral("不要执行这个命令")));
}

void TestAgentRuntime::planModeDeniesSideEffectTools() {
    QVERIFY(setupRuntime(SessionMode::Plan, PermissionMode::Plan));

    gateway_->enqueue(toolCallResponse(
        QStringLiteral("Bash"), "{\\\"command\\\":\\\"rm -rf /tmp/x\\\"", "}"));

    QSignalSpy permissionSpy(runtime_.get(), &AgentRuntime::permissionRequested);
    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("delete it"), &error), qPrintable(error));
    QVERIFY(finishedSpy.wait(8000));

    // plan 模式是模式语义：直接拒绝，不弹窗。
    QCOMPARE(permissionSpy.count(), 0);

    const ToolPart toolPart = runtime_->messages().at(1).toolParts().first();
    QCOMPARE(toolPart.state, ToolState::Error);
    QCOMPARE(toolPart.errorCode, QStringLiteral("plan_mode_denied"));

    // 模型仍被再问一次（把拒绝原因告诉它），所以是两轮模型步。
    QCOMPARE(runtime_->modelStepCount(), 2);
    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);
}

void TestAgentRuntime::abortInterruptsRunningTurn() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    // 一个永远不结束的流：服务端写完头就不发 body 结束标记。
    // 这里直接用一段没有 [DONE] 且 finish_reason 缺失的响应体，
    // 让 Provider 一直等后续数据。
    gateway_->enqueue(QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"));

    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
    QVERIFY(runtime_->isRunning());

    runtime_->abort();

    // 注意：abort() 可能同步触发 provider 的失败事件（"请求已取消"），
    // 因此 turnFinished 可能在 wait() 之前就已发出。QSignalSpy::wait() 只等待
    // **新的**发射，所以必须先检查已有计数，否则会误判为超时。
    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 5000);
    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Interrupted);
    QCOMPARE(runtime_->session().status, SessionStatus::CompletedInterrupted);
    QVERIFY(!runtime_->isRunning());

    // abort 是幂等的：再调一次不应产生第二个结束事件。
    runtime_->abort();
    QTest::qWait(100);
    QCOMPARE(finishedSpy.count(), 1);
}

void TestAgentRuntime::unknownToolFailsWithoutPrompt() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    gateway_->enqueue(toolCallResponse(QStringLiteral("NoSuchTool"), "{\\\"x\\\":1", "}"));

    QSignalSpy permissionSpy(runtime_.get(), &AgentRuntime::permissionRequested);
    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
    QVERIFY(finishedSpy.wait(8000));

    // 执行不了的调用不该打扰用户。
    QCOMPARE(permissionSpy.count(), 0);

    const ToolPart toolPart = runtime_->messages().at(1).toolParts().first();
    QCOMPARE(toolPart.state, ToolState::Error);
    QCOMPARE(toolPart.errorCode, QStringLiteral("tool_not_found"));
}

void TestAgentRuntime::httpErrorFailsTheTurn() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    gateway_->enqueue(QByteArray("{\"error\":{\"message\":\"invalid api key\"}}"), 401);

    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QSignalSpy failedSpy(runtime_.get(), &AgentRuntime::failed);

    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("hi"), &error), qPrintable(error));
    QVERIFY(finishedSpy.wait(5000));

    QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Failed);
    QCOMPARE(runtime_->session().status, SessionStatus::Error);
    QCOMPARE(failedSpy.count(), 1);

    // 失败信息里应包含 HTTP 状态，便于用户判断是鉴权还是网络问题。
    const QString message = failedSpy.first().at(0).toString();
    QVERIFY(message.contains(QStringLiteral("401")));

    // 消息本身也要被标记为失败，UI 才会显示错误。
    const QList<Message> messages = runtime_->messages();
    QCOMPARE(messages.last().status, MessageStatus::Failed);
    QVERIFY(!messages.last().errorMessage.isEmpty());
}

void TestAgentRuntime::reasoningLevelReachesProviderRequest() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    // 声明该模型支持三档思考，并指定模型默认档位。
    ModelOptionOverride override;
    override.reasoningLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high")};
    override.defaultReasoningLevel = QStringLiteral("low");
    providers_->setModelOverrides({{modelOptionKey(QStringLiteral("test"),
                                                  QStringLiteral("test-model")),
                                   override}});

    // ── 第一轮：用默认档位（low），应当带上 reasoning_effort ────────────────
    gateway_->enqueue(textResponse("ok"));

    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("hi"), &error), qPrintable(error));
    QVERIFY(finishedSpy.wait(6000));

    QJsonObject body = QJsonDocument::fromJson(gateway_->lastBody()).object();
    QCOMPARE(body.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("low"));

    // ── 第二轮：把档位切到 high，请求里应变成 high ─────────────────────────
    ModelSelection selection = runtime_->session().providerId.isEmpty()
                                   ? ModelSelection{}
                                   : ModelSelection{runtime_->session().providerId,
                                                    runtime_->session().modelId,
                                                    QStringLiteral("high")};
    QVERIFY(selection.isValid());
    runtime_->setModel(selection);

    gateway_->enqueue(textResponse("ok again"));
    QSignalSpy secondSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QVERIFY2(runtime_->submitText(QStringLiteral("again"), &error), qPrintable(error));
    QVERIFY(secondSpy.wait(6000));

    body = QJsonDocument::fromJson(gateway_->lastBody()).object();
    QCOMPARE(body.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));

    // ── 第三轮：切到 off，必须完全不传该字段 ───────────────────────────────
    // 部分兼容网关见到未知/空值字段会直接 400，所以"关闭"必须是"不传"。
    ModelSelection offSelection{runtime_->session().providerId, runtime_->session().modelId,
                               QStringLiteral("off")};
    runtime_->setModel(offSelection);

    gateway_->enqueue(textResponse("done"));
    QSignalSpy thirdSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QVERIFY2(runtime_->submitText(QStringLiteral("stop thinking"), &error), qPrintable(error));
    QVERIFY(thirdSpy.wait(6000));

    body = QJsonDocument::fromJson(gateway_->lastBody()).object();
    QVERIFY2(!body.contains(QStringLiteral("reasoning_effort")),
             "关闭思考时不应下发 reasoning_effort");

    // 没有声明思考档位的模型也不该下发。
    providers_->setModelOverrides({});
}

void TestAgentRuntime::contextWindowOverrideDrivesUsage() {
    QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

    gateway_->enqueue(textResponse("hello"));

    QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QString error;
    QVERIFY2(runtime_->submitText(QStringLiteral("hi"), &error), qPrintable(error));
    QVERIFY(finishedSpy.wait(6000));

    // 默认（provider 自报）：128000。
    QCOMPARE(json::integer(runtime_->session().contextUsage,
                           QStringLiteral("maxTokens")),
             128000);

    // 覆盖成 32000：用量分母必须跟着变，否则压缩阈值判断会离实际很远。
    ModelOptionOverride override;
    override.contextWindow = 32000;
    providers_->setModelOverrides(
        {{modelOptionKey(QStringLiteral("test"), QStringLiteral("test-model")), override}});

    gateway_->enqueue(textResponse("hello again"));
    QSignalSpy secondSpy(runtime_.get(), &AgentRuntime::turnFinished);
    QVERIFY2(runtime_->submitText(QStringLiteral("hi again"), &error), qPrintable(error));
    QVERIFY(secondSpy.wait(6000));

    QCOMPARE(json::integer(runtime_->session().contextUsage,
                           QStringLiteral("maxTokens")),
             32000);
    // 用量百分比必须按新分母算：分子大于 0，百分比就应该明显大于 0。
    const double percent = json::number(runtime_->session().contextUsage,
                                        QStringLiteral("percent"));
    QVERIFY(percent > 0.0);
    QVERIFY(percent <= 100.0);

    providers_->setModelOverrides({});
}

QTEST_MAIN(TestAgentRuntime)
#include "test_agent_runtime.moc"
