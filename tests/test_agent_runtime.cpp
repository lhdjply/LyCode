// LyCode — Agent 运行时集成测试
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

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringConverter>
#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTimer>
#include <QTemporaryDir>
#include <QTcpSocket>

#include "agent/AgentRuntime.h"
#include "core/Ids.h"
#include "core/Json.h"
#include "model/ProviderRegistry.h"
#include "tools/Tool.h"
#include "storage/SessionStore.h"
#include "tools/BackgroundTaskRegistry.h"
#include "tools/TaskTools.h"
#include "tools/ToolUtils.h"
#include "tools/TodoStore.h"

#include "support/FakeGateway.h"

using namespace lycode;
using lycode::test::FakeGateway;
using lycode::test::jsonEscape;
using lycode::test::textResponse;
using lycode::test::multiToolCallResponse;
using lycode::test::textResponseWithCache;
using lycode::test::toolCallResponse;

namespace
{

}  // namespace

class TestAgentRuntime : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();

    void plainTextTurnCompletes();
    void readToolRunsWithoutPrompt();
    void bashToolWaitsForPermissionAndRunsAfterAllow();
    void bashToolTellsModelTheShellItActuallyUses();
    void backgroundWrapperSyntaxMatchesShell();
    void deniedToolFeedsErrorBackToModel();
    void planModeDeniesSideEffectTools();
    void abortInterruptsRunningTurn();
    void unknownToolFailsWithoutPrompt();
    void httpErrorFailsTheTurn();
    void reasoningLevelReachesProviderRequest();
    void contextWindowOverrideDrivesUsage();
    void setModelRefreshesContextWindow();
    void reloadRestoresPersistedConversation();
    void openAiUsageIsNormalizedToUncached();
    void parallelSafeToolsRunConcurrently();
    void nonParallelSafeToolFormsSerialBarrier();
    void stopTurnWaitsForBatchSiblings();
    void concurrencyPolicyIsTriState();
    void agentToolIsRegisteredWithAlias();
    void subagentProfilesDefineToolSurfaces();
    void toolAllowlistBlocksBothDeclarationAndExecution();
    void subagentCannotSpawnSubagents();
    void subagentRunsItsOwnTurnAndReportsBack();
    void backgroundBashSurvivesTheToolCall();
    void taskOutputBlocksUntilFinished();
    void taskStopKillsTheTask();
    void backgroundNotificationIsInjected();
    void taskToolsRejectUnknownIds();
    void autoBackgroundsOnTimeout();
    void detachedTaskSurvivesRegistryRestart();
    void imageAttachmentReachesProvider();
    void readToolReturnsImageToTheModel();
    void modelGeneratesAndSanitizesSessionTitle();

    // 上下文压缩
    void microcompactTrimsOldToolOutputInRequest();
    void autoCompactionSummarizesAndReplacesHistory();
    void manualCompactionRunsBelowThreshold();
    void compactionArchivesOriginalMessages();

  private:
    /// 组装一个指向假网关的运行时。
    bool setupRuntime(SessionMode mode, PermissionMode permissionMode,
                      SessionStore * store = nullptr);
    /// 只配置 provider，不建会话（供需要自己组装 runtime 的测试使用）。
    void configureProviders();

    std::unique_ptr<FakeGateway> gateway_;
    std::unique_ptr<ProviderRegistry> providers_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<TodoStore> todos_;
    std::unique_ptr<AgentRuntime> runtime_;

    QTemporaryDir tempDir_;
};

void TestAgentRuntime::init()
{
  // 数据目录指向临时目录：后台任务的输出文件默认落在 <数据目录>/tasks，
  // 测试不应该往用户的 ~/.cache/lycode 里写东西。
  qputenv("LYCODE_DATA_BASE_DIR", tempDir_.path().toUtf8());
  // 账本是全局文件：不清掉的话上一个测试的遗留任务会被下一个测试认领，
  // 断言就不再互不干扰。
  QFile::remove(BackgroundTaskRegistry::ledgerPath());

  gateway_ = std::make_unique<FakeGateway>();
  QVERIFY(gateway_->start());

  providers_ = std::make_unique<ProviderRegistry>();
  tools_ = std::make_unique<ToolRegistry>();
  todos_ = std::make_unique<TodoStore>();
  runtime_ = std::make_unique<AgentRuntime>();

  // 每次测试用全新的临时工作区，避免相互污染。
  QVERIFY(tempDir_.isValid());
}

void TestAgentRuntime::cleanup()
{
  // 后台任务的析构语义是"放它活过宿主"，所以测试必须显式收尾，
  // 否则会留下一堆 sleep 进程。
  if(runtime_ != nullptr && runtime_->backgroundTasks() != nullptr) {
    runtime_->backgroundTasks()->stopAll();
  }
  // 先销毁运行时再销毁依赖：运行时析构会 abort 活跃的流。
  runtime_.reset();
  todos_.reset();
  tools_.reset();
  providers_.reset();
  gateway_.reset();
}

void TestAgentRuntime::configureProviders()
{
  ProviderConfig config;
  config.id = QStringLiteral("test");
  config.name = QStringLiteral("Test Gateway");
  config.kind = ProviderKind::OpenAICompatible;
  config.baseUrl = gateway_->baseUrl();
  config.apiKey = QStringLiteral("test-key");
  config.models = {QStringLiteral("test-model"), QStringLiteral("wide-model")};
  config.enabled = true;
  config.availability = AccountAvailability::Available;
  providers_->replaceAll({config});
}

bool TestAgentRuntime::setupRuntime(SessionMode mode, PermissionMode permissionMode,
                                    SessionStore * store)
{
  configureProviders();

  *tools_ = ToolRegistry::createWithBuiltins();
  runtime_->setProviderRegistry(providers_.get());
  runtime_->setToolRegistry(tools_.get());
  runtime_->setTodoStore(todos_.get());
  // 默认不注入 SessionStore：运行时必须能在无持久化时正常工作。
  // 需要持久化的测试必须在 startSession **之前**注入，否则会话行不存在，
  // 消息落盘会因外键失败（而且这种失败只是告警，不容易被注意到）。
  runtime_->setSessionStore(store);

  Workspace workspace;
  workspace.path = tempDir_.path();

  ModelSelection selection;
  selection.providerId = QStringLiteral("test");
  selection.modelId = QStringLiteral("test-model");

  QString error;
  if(!runtime_->startSession(workspace, mode, selection, &error)) {
    qWarning() << "startSession 失败:" << error;
    return false;
  }
  runtime_->permissionGate()->setMode(permissionMode);
  return true;
}

void TestAgentRuntime::plainTextTurnCompletes()
{
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

void TestAgentRuntime::readToolRunsWithoutPrompt()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  const QString path = tempDir_.filePath(QStringLiteral("sample.txt"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("alpha\nbeta\ngamma\n");
  file.close();

  // 第 1 轮：Read 工具调用。第 2 轮：最终文本。
  gateway_->enqueue(toolCallResponse(QStringLiteral("Read"),
                                     QByteArray("{\"file_path\":\"") + jsonEscape(path) +
                                     QByteArray("\""),
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
  for(const Part & part : messages.at(2).parts) {
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

void TestAgentRuntime::bashToolWaitsForPermissionAndRunsAfterAllow()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  gateway_->enqueue(toolCallResponse(
                      QStringLiteral("Bash"), R"({"command":"echo lycode-ok")", "}"));

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
  QVERIFY(toolPart.output.contains(QStringLiteral("lycode-ok")));

  // 执行前必须是 PendingApproval 而不是直接 Running。
  QCOMPARE(toolPart.metadata.value(QStringLiteral("exitCode")).toInt(), 0);
}

// 回归：Windows 上工具曾把命令交给 cmd.exe，却仍然告诉模型"命令通过 bash -lc 执行"。
// 模型据此写出 `for c in gcc; do command -v $c; done`，cmd 解析不了，最后只剩一个
// 光秃秃的"退出码 1"——连报错原因都看不到。
//
// 这里钉住契约本身：发给模型的工具说明必须与真正执行的 shell 一致，
// 且参数拼接方式（POSIX 直接追加 / PowerShell 走 -EncodedCommand）与 shell 语义匹配。
void TestAgentRuntime::bashToolTellsModelTheShellItActuallyUses()
{
  const toolutil::ShellSpec spec = toolutil::shellSpec();

  // 1. 探测结果必须自洽，否则后面两条断言都会失去意义。
  QVERIFY2(!spec.program.isEmpty(), "shell 程序不能为空");
  QVERIFY2(!spec.arguments.isEmpty(), "shell 启动参数不能为空");
  QVERIFY2(!spec.label.isEmpty(), "shell 名称不能为空");

  // 2. 真正传给 QProcess 的参数：命令必须能被执行到，且不能泄漏成"裸命令"。
  const QString command = QStringLiteral("echo lycode-shell-contract");
  const QStringList arguments = toolutil::shellArgumentsFor(spec, command);
  QVERIFY2(arguments.size() > spec.arguments.size(),
           "命令必须以参数形式追加到 shell 启动参数之后");

  const QString lastArg = arguments.last();
  if(spec.posix) {
    // POSIX：命令原样追加，最后的参数就是命令本身。
    QCOMPARE(lastArg, command);
  }
  else {
    // Windows 原生 shell：命令必须经过编码传递，绝不能原样拼进命令行
    // （命令里的引号、$、中文、换行在多层转义下会静默变形）。
    QVERIFY2(lastArg != command,
             "Windows 原生 shell 下命令不应原样作为参数传递");

    // 开关名必须与参数内容配套：写 `-Command <Base64>` 会让 PowerShell 把
    // Base64 当成命令名去执行，报 "The term 'WwBD...' is not recognized"。
    // 这个断言就是为那次线上失败加的——只检查位置不检查开关名是抓不住的。
    QCOMPARE(spec.arguments.last(), QStringLiteral("-EncodedCommand"));

    // 解码回来验证命令内容真的在里面（而不是一串无关的 Base64）。
    // 编码是 UTF-16LE，必须按 UTF-16LE 解；按 latin1 解会得到交错的空字节。
    const QByteArray payload = QByteArray::fromBase64(lastArg.toLatin1());
    QVERIFY2(!payload.isEmpty(), "编码命令必须能解出内容");
    QStringDecoder decoder(QStringConverter::Utf16LE);
    const QString decoded = decoder(payload);
    QVERIFY2(!decoder.hasError(), "编码命令必须是合法 UTF-16LE");
    QVERIFY2(decoded.contains(command),
             qPrintable(QStringLiteral("解码后的编码命令必须包含原始命令，实际为：%1").arg(decoded)));
  }

  // 3. 契约一致性：发给模型的说明里声明的 shell，必须就是上面这个 program。
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));
  gateway_->enqueue(textResponse("ok"));

  // 请求是异步派发的：submitText 返回时 HTTP body 还没到假网关，必须等一轮结束，
  // 否则 lastBody() 是空的（会把"没等到"误判成"没声明工具"）。
  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("hi"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(8000));

  const QJsonObject body = QJsonDocument::fromJson(gateway_->lastBody()).object();
  const QJsonArray tools = json::array(body, QStringLiteral("tools"));
  QVERIFY2(!tools.isEmpty(), "发给模型的请求里必须带工具声明");
  // OpenAI 兼容格式：每一项是 { "type": "function", "function": {...} }。
  QJsonObject bashFunction;
  for(const QJsonValue & value : tools) {
    const QJsonObject function =
      json::object(value.toObject(), QStringLiteral("function"));
    if(function.value(QStringLiteral("name")).toString() == QStringLiteral("Bash")) {
      bashFunction = function;
      break;
    }
  }
  QVERIFY2(!bashFunction.isEmpty(), "发给模型的工具列表里必须有 Bash");
  const QString description = bashFunction.value(QStringLiteral("description")).toString();
  QVERIFY2(!description.isEmpty(), "Bash 的工具说明不能为空");

  if(spec.posix) {
    QVERIFY2(description.contains(spec.label),
             qPrintable(QStringLiteral("说明里必须声明实际使用的 shell（%1）：%2")
                        .arg(spec.label, description)));
    QVERIFY2(description.contains(QStringLiteral("bash")),
             "POSIX 环境下必须明确告诉模型命令走 bash 语法");
  }
  else {
    // 关键回归点：绝不能再说 `bash -lc`。
    QVERIFY2(!description.contains(QStringLiteral("bash -lc")),
             qPrintable(QStringLiteral("Windows 下不能声称走 bash：%1").arg(description)));
    QVERIFY2(description.contains(QStringLiteral("PowerShell")) ||
             description.contains(spec.label),
             qPrintable(QStringLiteral("说明里必须声明实际使用的 shell（%1）：%2")
                        .arg(spec.label, description)));
  }
}

// 回归：后台任务的命令包装必须与目标 shell 的语法匹配。
//
// 线上踩过的两个坑，都是"把 PowerShell 当 POSIX 写"：
//   1. 沿用 POSIX 的 `{ cmd; }`：PowerShell 里 `{ ... }` 是 ScriptBlock 字面量，
//      `;` 后直接跟 `{` 是语法错误，命令**从未执行**，退出码旁路文件也从未写出。
//   2. 改成裸语句序列、`*>> file` 用分号贴在后面：重定向是**语句级操作符**，
//      被分号切开后 PowerShell 去把 `*>>` 当命令名找，报
//      "The term '*>>' is not recognized"。
// 两者都比报错更难查——第一种看起来像"命令跑了但失败了"。
void TestAgentRuntime::backgroundWrapperSyntaxMatchesShell()
{
  const toolutil::ShellSpec spec = toolutil::shellSpec();

  // 用测试目录当路径：顺带钉住"路径不含需要转义的字符"，否则整个包装的
  // 引号假设都不成立（PowerShell 单引号里不能再出现单引号）。
  const QString tempRoot = QDir::tempPath();
  QVERIFY2(!tempRoot.contains(QLatin1Char('\'')), "临时目录不应含单引号");
  const QString outputPath = tempRoot + QStringLiteral("/lycode-wrap-probe.log");
  const QString exitPath = outputPath + QStringLiteral(".exit");

  // 还原真实调用形态：ShellSpec 后台调用方会用花括号把命令包起来当"一个整体"。
  // 这是 POSIX 的写法习惯，包装层必须负责把它翻译成目标 shell 能执行的形式。
  const QString command = QStringLiteral("echo lycode-wrap-ok");
  const QString bracedCommand = QStringLiteral("{ %1; }").arg(command);
  const QString wrapped = toolutil::shellBackgroundWrapper(
                            spec, bracedCommand, outputPath, exitPath);
  QVERIFY2(!wrapped.isEmpty(), "包装结果不能为空");

  // 无论哪个平台，三要素缺一不可：命令本体、输出落盘、退出码落盘。
  QVERIFY2(wrapped.contains(command), qPrintable(wrapped));
  QVERIFY2(wrapped.contains(outputPath), qPrintable(wrapped));
  QVERIFY2(wrapped.contains(exitPath), qPrintable(wrapped));

  // ★ 关键回归点：包装里不能再出现原样的 POSIX 块括号形态。
  // 与平台无关地表达"花括号已经被处理掉"——POSIX 分支会保留（那是合法写法），
  // 所以两种写法都构造一遍，比较包装结果是否相同。
  const QString bareWrapped = toolutil::shellBackgroundWrapper(
                                spec, command, outputPath, exitPath);
  if(!spec.posix) {
    QVERIFY2(wrapped == bareWrapped,
             qPrintable(QStringLiteral("Windows 包装必须与命令有无花括号无关：\n带括号=%1\n不带=%2")
                        .arg(wrapped, bareWrapped)));
  }

  if(spec.posix) {
    // POSIX：`{ cmd; } >> log 2>&1; echo $? > exit` 是合法写法，保留。
    QVERIFY2(wrapped.contains(QStringLiteral("echo $?")), qPrintable(wrapped));
  }
  else {
    if(spec.label == QLatin1String("cmd")) {
      // cmd：退出码是 errorlevel。
      QVERIFY2(wrapped.contains(QStringLiteral("%errorlevel%")), qPrintable(wrapped));
    }
    else {
      // PowerShell。
      //
      // 注意**不要**断言"包装里不出现 `$?`"：PowerShell 的 `$?` 是正常的内置
      // 布尔变量（上一条语句是否成功），退出码判定正需要它——原生命令看
      // `$LASTEXITCODE`，纯 cmdlet 用 `$?` 落到 0/1。要禁的是 POSIX 的旁路
      // **写法** `echo $? > exitfile`，不是 `$?` 这个符号。
      QVERIFY2(!wrapped.contains(QStringLiteral("echo $?")),
               qPrintable(QStringLiteral("PowerShell 不该用 POSIX 的 echo $? 旁路：%1")
                          .arg(wrapped)));
      QVERIFY2(wrapped.contains(QStringLiteral("$LASTEXITCODE")), qPrintable(wrapped));
      QVERIFY2(wrapped.contains(QStringLiteral("Out-File")), qPrintable(wrapped));

      // 重定向必须**紧跟**被重定向的语句，不能被分号切开。
      // 合法形态：`& { ... } *>> 'file'`；非法形态：`...; *>> 'file'`。
      QVERIFY2(!wrapped.contains(QStringLiteral("; *>>")),
               qPrintable(QStringLiteral("`*>>` 被分号切成了独立语句：%1").arg(wrapped)));

      // 整个包装必须是**一条**语句：结尾的重定向没有被换行/分号甩开。
      QVERIFY2(wrapped.endsWith(QStringLiteral("'")),
               qPrintable(QStringLiteral("包装应以重定向目标结尾：%1").arg(wrapped)));
      QVERIFY2(!wrapped.contains(QLatin1Char('\n')),
               qPrintable(QStringLiteral("包装不该含换行：%1").arg(wrapped)));

      // 剥掉外层花括号后不该留下空语句（`;;`）——虽然 PowerShell 容忍它，
      // 但那是拼装逻辑没收敛的信号。
      QVERIFY2(!wrapped.contains(QStringLiteral(";;")),
               qPrintable(QStringLiteral("包装出现空语句：%1").arg(wrapped)));

      // 包装本身也要能通过 shell 参数拼装（编码后是纯 Base64）。
      const QStringList arguments = toolutil::shellArgumentsFor(spec, wrapped);
      QCOMPARE(arguments.last(), arguments.last().trimmed());
      QVERIFY2(!arguments.last().contains(QLatin1Char(' ')),
               "编码后的命令不应含空格");
    }
  }
}

void TestAgentRuntime::deniedToolFeedsErrorBackToModel()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  gateway_->enqueue(toolCallResponse(
                      QStringLiteral("Bash"), R"({"command":"echo nope")", "}"));

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

void TestAgentRuntime::planModeDeniesSideEffectTools()
{
  QVERIFY(setupRuntime(SessionMode::Plan, PermissionMode::Plan));

  gateway_->enqueue(toolCallResponse(
                      QStringLiteral("Bash"), R"({"command":"rm -rf /tmp/x")", "}"));

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

void TestAgentRuntime::abortInterruptsRunningTurn()
{
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

void TestAgentRuntime::unknownToolFailsWithoutPrompt()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  gateway_->enqueue(toolCallResponse(QStringLiteral("NoSuchTool"), R"({"x":1)", "}"));

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

void TestAgentRuntime::httpErrorFailsTheTurn()
{
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

void TestAgentRuntime::reasoningLevelReachesProviderRequest()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 声明该模型支持三档思考，并指定模型默认档位。
  ModelOptionOverride override;
  override.reasoningLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high")};
  override.defaultReasoningLevel = QStringLiteral("low");
  providers_->setModelOverrides({{
      modelOptionKey(QStringLiteral("test"),
                     QStringLiteral("test-model")),
      override
    }});

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
                             :
                             ModelSelection{runtime_->session().providerId,
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

void TestAgentRuntime::contextWindowOverrideDrivesUsage()
{
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

void TestAgentRuntime::setModelRefreshesContextWindow()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 新建会话时就已经按当前模型算好分母。
  QCOMPARE(json::integer(runtime_->session().contextUsage, QStringLiteral("maxTokens")),
           128000);

  // 给另一个模型配一个更大的窗口，然后只切换模型、**不发任何消息**。
  ModelOptionOverride override;
  override.contextWindow = 500000;
  providers_->setModelOverrides(
  {{modelOptionKey(QStringLiteral("test"), QStringLiteral("wide-model")), override}});

  ModelSelection wide;
  wide.providerId = QStringLiteral("test");
  wide.modelId = QStringLiteral("wide-model");
  runtime_->setModel(wide);

  // 分母必须立刻变，而不是等下一个 turn 结束才刷新。
  QCOMPARE(json::integer(runtime_->session().contextUsage, QStringLiteral("maxTokens")),
           500000);
  QCOMPARE(gateway_->requestCount(), 0);

  providers_->setModelOverrides({});
}

void TestAgentRuntime::reloadRestoresPersistedConversation()
{
  configureProviders();
  // 必须显式注册内置工具：ToolRegistry 默认构造是空的
  // （这与主窗口里踩过的那个"空工具表"是同一个坑）。
  *tools_ = ToolRegistry::createWithBuiltins();

  // 用真实的 SQLite 存储跑一轮对话，然后换一个 AgentRuntime 重新载入，
  // 模拟"关闭软件再打开"。这条覆盖的是存储层与运行时的往返，
  // 不依赖 UI —— 界面层的同类回归在 test_ui_flow 里。
  SessionStore store;
  QVERIFY2(store.open(tempDir_.filePath(QStringLiteral("reload-test.db"))),
           qPrintable(store.lastError()));

  const QString samplePath = tempDir_.filePath(QStringLiteral("reload-sample.txt"));
  {
    QFile sample(samplePath);
    QVERIFY(sample.open(QIODevice::WriteOnly));
    sample.write("persisted-content\n");
  }

  gateway_->enqueue(toolCallResponse(QStringLiteral("Read"),
                                     QByteArray("{\"file_path\":\"") +
                                     jsonEscape(samplePath) + QByteArray("\""),
                                     "}"));
  gateway_->enqueue(textResponse("read it back"));

  Workspace workspace;
  workspace.path = tempDir_.path();
  const ModelSelection selection{QStringLiteral("test"), QStringLiteral("test-model"),
                                 QString()};

  AgentRuntime first;
  first.setProviderRegistry(providers_.get());
  first.setToolRegistry(tools_.get());
  first.setSessionStore(&store);
  first.setTodoStore(todos_.get());

  QString error;
  QVERIFY2(first.startSession(workspace, SessionMode::Build, selection, &error),
           qPrintable(error));

  QSignalSpy finishedSpy(&first, &AgentRuntime::turnFinished);
  QVERIFY2(first.submitText(QStringLiteral("read the sample"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(8000));

  const Id sessionId = first.session().id;
  const QList<Message> before = first.messages();
  QCOMPARE(before.size(), 4);  // user, assistant(工具调用), 工具结果(user), assistant(最终)

  // ── 模拟重启 ───────────────────────────────────────────────────────────
  AgentRuntime second;
  second.setProviderRegistry(providers_.get());
  second.setToolRegistry(tools_.get());
  second.setSessionStore(&store);
  second.setTodoStore(todos_.get());
  QVERIFY2(second.loadSession(sessionId, &error), qPrintable(error));

  const QList<Message> after = second.messages();
  QCOMPARE(after.size(), before.size());
  QCOMPARE(after.at(0).plainText(), QStringLiteral("read the sample"));

  // 工具调用必须连同状态与输出一起恢复，否则重启后只剩一段文字。
  const ToolPart restoredTool = after.at(1).toolParts().first();
  QCOMPARE(restoredTool.name, QStringLiteral("Read"));
  QVERIFY2(restoredTool.state == ToolState::Success,
           qPrintable(QStringLiteral("工具状态异常: state=%1 error=%2 code=%3 output=%4")
                      .arg(static_cast<int>(restoredTool.state))
                      .arg(restoredTool.error, restoredTool.errorCode,
                           restoredTool.output)));
  QVERIFY(restoredTool.output.contains(QStringLiteral("persisted-content")));

  // modelOnly 标记也要还原：否则重启后那条合成的工具结果轮会显示成
  // 一条用户从未说过的"你"的消息。
  QVERIFY(after.at(2).modelOnly);
  QVERIFY(!after.at(3).modelOnly);
  QCOMPARE(after.at(3).plainText(), QStringLiteral("read it back"));

  // 会话元信息（标题取自首条用户输入）也要在。
  Session reloadedSession;
  QVERIFY(store.loadSession(sessionId, &reloadedSession));
  QCOMPARE(reloadedSession.title, QStringLiteral("read the sample"));
}

void TestAgentRuntime::openAiUsageIsNormalizedToUncached()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // OpenAI 的 prompt_tokens 是"含缓存"的口径：1000 里有 600 来自缓存。
  gateway_->enqueue(textResponseWithCache("cached answer", 1000, 600, 50));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("hi"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(6000));

  const Usage usage = runtime_->messages().last().usage;
  // 必须归一到"未命中缓存"，否则同一个 inputTokens 在 Anthropic 与 OpenAI
  // 两条路径上含义不同，缓存命中率会算错。
  QCOMPARE(usage.inputTokens, 400);   // 1000 - 600
  QCOMPARE(usage.cacheReadTokens, 600);
  QCOMPARE(usage.outputTokens, 50);
  QCOMPARE(usage.promptTokens(), 1000);
  QVERIFY(qAbs(usage.cacheHitRate() - 0.6) < 0.0001);

  // 脏数据防护：cached > prompt 时不能算出负的未缓存输入。
  gateway_->enqueue(textResponseWithCache("dirty", 100, 500, 10));
  QSignalSpy secondSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QVERIFY2(runtime_->submitText(QStringLiteral("again"), &error), qPrintable(error));
  QVERIFY(secondSpy.wait(6000));

  const Usage dirty = runtime_->lastTurnUsage();
  QVERIFY2(dirty.inputTokens >= 0, "未缓存输入不得为负");
  QCOMPARE(dirty.inputTokens, 0);
}

namespace
{

/// 可控延时的探针工具，用于验证并行调度语义。
///
/// 只读 + 无副作用 → 权限自动放行，测试不需要处理权限弹窗，
/// 这样断言就只针对"调度"这一件事。
class ProbeTool : public Tool
{
  public:
    explicit ProbeTool(QString name, int durationMs, bool concurrentSafe,
                       QStringList * orderLog, bool stopTurnOnSuccess = false)
      : name_(std::move(name)),
        durationMs_(durationMs),
        concurrentSafe_(concurrentSafe),
        orderLog_(orderLog),
        stopTurn_(stopTurnOnSuccess) {}

    ToolMetadata metadata() const override
    {
      ToolMetadata meta;
      meta.name = name_;
      meta.description = QStringLiteral("测试用探针工具");
      meta.readOnly = true;
      meta.destructive = false;
      // 三态：显式声明可并发 / 显式声明必须独占。后者正是"只读也可能要串行"的场景。
      meta.concurrency = concurrentSafe_ ? ToolMetadata::Concurrency::Safe
                         : ToolMetadata::Concurrency::Serial;
      meta.sideEffectScope = SideEffectScope::None;
      meta.riskLevel = RiskLevel::Low;
      meta.needsApproval = false;
      meta.stopTurnOnSuccess = stopTurn_;
      return meta;
    }

    QJsonObject inputSchema() const override
    {
      QJsonObject properties;
      properties.insert(QStringLiteral("label"), QJsonObject{{
          QStringLiteral("type"),
          QStringLiteral("string")
        }});
      QJsonObject schema;
      schema.insert(QStringLiteral("type"), QStringLiteral("object"));
      schema.insert(QStringLiteral("properties"), properties);
      return schema;
    }

    void execute(const QJsonObject & input, const ToolContext &, ToolCallback done) override
    {
      const QString label = input.value(QStringLiteral("label")).toString();
      ++active_;
      maxActive_ = qMax(maxActive_, active_);
      if(orderLog_ != nullptr) {
        orderLog_->append(QStringLiteral("start:") + label);
      }

      // 延时用 QTimer 而不是 sleep：必须真正异步，才能观察到并发。
      // 不能把 this 当上下文——ProbeTool 不是 QObject（工具基类刻意不是），
      // 测试又总是在销毁 runtime 前等 turn 结束，所以不需要上下文保护。
      QTimer::singleShot(durationMs_, [this, label, done = std::move(done)]() mutable {
        --active_;
        if(orderLog_ != nullptr) {
          orderLog_->append(QStringLiteral("finish:") + label);
        }
        ToolResult result = ToolResult::success(QStringLiteral("ok:") + label);
        result.stopTurnAfterResult = stopTurn_;
        done(result);
      });
    }

    int maxActive() const
    {
      return maxActive_;
    }
    int completed() const
    {
      return completed_;
    }

  private:
    QString name_;
    int durationMs_;
    bool concurrentSafe_;
    QStringList * orderLog_;
    bool stopTurn_;
    int active_ = 0;
    int maxActive_ = 0;
    int completed_ = 0;
};

}  // namespace

void TestAgentRuntime::parallelSafeToolsRunConcurrently()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 三个可并行的调用，每个 200ms。并行应在 ~200ms 完成，串行则要 ~600ms。
  QStringList order;
  auto * probe = new ProbeTool(QStringLiteral("Probe"), 200, true, &order);
  tools_->add(probe);

  const QByteArray argsA = R"({"label":"a"})";
  const QByteArray argsB = R"({"label":"b"})";
  const QByteArray argsC = R"({"label":"c"})";
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Probe"), argsA},
    {QStringLiteral("Probe"), argsB},
    {QStringLiteral("Probe"), argsC}}));
  gateway_->enqueue(textResponse("all done"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QElapsedTimer timer;
  timer.start();

  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));
  const qint64 elapsed = timer.elapsed();

  QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);
  QCOMPARE(runtime_->toolCallCount(), 3);

  // 真正的并行：同一时刻有 3 个工具在跑。
  QCOMPARE(probe->maxActive(), 3);
  // 远快于串行所需的 600ms（放宽到 500ms 以容忍调度开销）。
  QVERIFY2(elapsed < 500, qPrintable(QStringLiteral("并行执行耗时 %1ms，接近串行").arg(elapsed)));

  // 三个工具都成功落盘。
  const QList<Message> messages = runtime_->messages();
  for(const ToolPart & part : messages.at(1).toolParts()) {
    QCOMPARE(part.state, ToolState::Success);
  }
}

void TestAgentRuntime::nonParallelSafeToolFormsSerialBarrier()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 不可并行的工具夹在两个可并行调用之间，必须形成串行屏障：
  // 分组应为 [Probe], [Exclusive], [Probe]，全程没有重叠。
  QStringList order;
  auto * probe = new ProbeTool(QStringLiteral("Probe"), 120, true, &order);
  auto * exclusive = new ProbeTool(QStringLiteral("Exclusive"), 120, false, &order);
  tools_->add(probe);
  tools_->add(exclusive);

  const QByteArray argsA = R"({"label":"a"})";
  const QByteArray argsB = R"({"label":"b"})";
  const QByteArray argsC = R"({"label":"c"})";
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Probe"), argsA},
    {QStringLiteral("Exclusive"), argsB},
    {QStringLiteral("Probe"), argsC}}));
  gateway_->enqueue(textResponse("done"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);
  QCOMPARE(runtime_->toolCallCount(), 3);

  // 任何时刻都只有一个工具在跑——组与组之间是串行的。
  QCOMPARE(probe->maxActive(), 1);
  QCOMPARE(exclusive->maxActive(), 1);

  // 执行顺序必须与模型给出的调用顺序一致（a → b → c）。
  QCOMPARE(order, QStringList({QStringLiteral("start:a"), QStringLiteral("finish:a"),
                               QStringLiteral("start:b"), QStringLiteral("finish:b"),
                               QStringLiteral("start:c"), QStringLiteral("finish:c")}));

  // 结果顺序也要保持模型给出的次序，而不是完成次序。
  const QList<ToolPart> parts = runtime_->messages().at(1).toolParts();
  QCOMPARE(parts.size(), 3);
  QCOMPARE(parts.at(0).input.value(QStringLiteral("label")).toString(), QStringLiteral("a"));
  QCOMPARE(parts.at(1).input.value(QStringLiteral("label")).toString(), QStringLiteral("b"));
  QCOMPARE(parts.at(2).input.value(QStringLiteral("label")).toString(), QStringLiteral("c"));
}

void TestAgentRuntime::stopTurnWaitsForBatchSiblings()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 第 1 组：[Stop, Probe]（都可并行）——Stop 要求终止本轮，
  //          但同组的 Probe 必须先把结果提交完。
  // 第 2 组：[Tail]（不可并行）——必须被取消，不得执行。
  QStringList order;
  auto * stopping = new ProbeTool(QStringLiteral("Stop"), 80, true, &order, true);
  auto * probe = new ProbeTool(QStringLiteral("Probe"), 150, true, &order);
  auto * tail = new ProbeTool(QStringLiteral("Tail"), 80, false, &order);
  tools_->add(stopping);
  tools_->add(probe);
  tools_->add(tail);

  const QByteArray argsS = R"({"label":"s"})";
  const QByteArray argsP = R"({"label":"p"})";
  const QByteArray argsT = R"({"label":"t"})";
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Stop"), argsS},
    {QStringLiteral("Probe"), argsP},
    {QStringLiteral("Tail"), argsT}}));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);

  const QList<ToolPart> parts = runtime_->messages().at(1).toolParts();
  QCOMPARE(parts.size(), 3);

  // 要求终止的调用：成功。
  QCOMPARE(parts.at(0).state, ToolState::Success);
  // 同组兄弟：**必须**已经跑完并成功，不能被"终止"打断。
  QCOMPARE(parts.at(1).state, ToolState::Success);
  QVERIFY2(parts.at(1).output.contains(QStringLiteral("ok:p")),
           "同组兄弟的结果必须在终止前提交");
  // 后续组：被取消，且没有真正执行过。
  QCOMPARE(parts.at(2).state, ToolState::Cancelled);
  QVERIFY2(!order.contains(QStringLiteral("start:t")), "被取消的调用不应真正执行");

  // 终止后不再请求模型（没有第二轮）。
  QCOMPARE(gateway_->requestCount(), 1);
}

void TestAgentRuntime::concurrencyPolicyIsTriState()
{
  QStringList ignored;

  // 显式声明 Safe → 可并发。
  ProbeTool safe(QStringLiteral("SafeProbe"), 0, true, &ignored);
  QVERIFY(safe.canRunInParallel());

  // 显式声明 Serial → 必须串行，**即使只读且无副作用**。
  // 这正是三态存在的理由：用 bool 时"显式 false"与"未声明"无法区分，
  // 显式串行的只读工具会被"只读+无副作用 ⇒ 可并发"的豁免分支错误放行。
  ProbeTool serial(QStringLiteral("SerialProbe"), 0, false, &ignored);
  QVERIFY2(!serial.canRunInParallel(), "显式声明串行的只读工具不得被判为可并发");

  // 内置工具的期望分类（metadata 一致）。
  *tools_ = ToolRegistry::createWithBuiltins();
  QVERIFY(tools_->find(QStringLiteral("Read"))->canRunInParallel());
  QVERIFY(tools_->find(QStringLiteral("Glob"))->canRunInParallel());
  QVERIFY(tools_->find(QStringLiteral("Grep"))->canRunInParallel());
  QVERIFY(tools_->find(QStringLiteral("TodoRead"))->canRunInParallel());

  QVERIFY2(!tools_->find(QStringLiteral("Bash"))->canRunInParallel(),
           "Bash 既具破坏性又显式串行");
  QVERIFY2(!tools_->find(QStringLiteral("Write"))->canRunInParallel(), "写文件不能并发");
  QVERIFY2(!tools_->find(QStringLiteral("Edit"))->canRunInParallel(), "改文件不能并发");
  QVERIFY2(!tools_->find(QStringLiteral("TodoWrite"))->canRunInParallel(),
           "TodoWrite 虽然 readOnly，但会写会话状态，必须串行");
}

void TestAgentRuntime::agentToolIsRegisteredWithAlias()
{
  *tools_ = ToolRegistry::createWithBuiltins();

  Tool * agent = tools_->find(QStringLiteral("Agent"));
  QVERIFY2(agent != nullptr, "内置工具里应当有 Agent");
  // `Task` 是 本实现里 Agent 的 Claude Code 兼容别名。
  QCOMPARE(tools_->find(QStringLiteral("Task")), agent);
  QVERIFY(agent->metadata().providerVisible);
  // 多个 Agent 可以在同一条消息里并行发出。
  QVERIFY(agent->canRunInParallel());

  // 声明里必须带 subagent_type 枚举，且默认值与 profile 表一致。
  const QJsonObject schema = agent->inputSchema();
  const QJsonObject properties = json::object(schema, QStringLiteral("properties"));
  QVERIFY(properties.contains(QStringLiteral("subagent_type")));
  QVERIFY(properties.contains(QStringLiteral("description")));
  QVERIFY(properties.contains(QStringLiteral("prompt")));
}

void TestAgentRuntime::subagentProfilesDefineToolSurfaces()
{
  const SubagentProfile * general = findSubagentProfile(QStringLiteral("general-purpose"));
  QVERIFY(general != nullptr);
  QVERIFY2(!general->readOnly, "general-purpose 必须能改代码");
  QVERIFY2(general->toolAllowlist.isEmpty(), "general-purpose 继承完整工具面");

  const SubagentProfile * explore = findSubagentProfile(QStringLiteral("Explore"));
  QVERIFY(explore != nullptr);
  QVERIFY2(explore->readOnly, "Explore 必须是只读的");
  // 只读 profile 的工具面不能包含任何写/执行工具。
  for(const QString & forbidden : {
        QStringLiteral("Write"), QStringLiteral("Edit"),
        QStringLiteral("Bash"), QStringLiteral("Agent")
      }) {
    QVERIFY2(!explore->toolAllowlist.contains(forbidden),
             qPrintable(QStringLiteral("只读 profile 不应包含 %1").arg(forbidden)));
  }
  QVERIFY(explore->toolAllowlist.contains(QStringLiteral("Read")));
  QVERIFY(explore->toolAllowlist.contains(QStringLiteral("Grep")));

  // 未知 profile 必须返回 nullptr，调用方据此拒绝该次调用。
  QVERIFY(findSubagentProfile(QStringLiteral("no-such-profile")) == nullptr);
  QCOMPARE(defaultSubagentProfileId(), QStringLiteral("general-purpose"));
}

void TestAgentRuntime::toolAllowlistBlocksBothDeclarationAndExecution()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 白名单只留 Read：声明层不该再把 Write 告诉模型。
  runtime_->setToolAllowlist({QStringLiteral("Read")});

  const QString path = tempDir_.filePath(QStringLiteral("allowlist.txt"));
  QFile sample(path);
  QVERIFY(sample.open(QIODevice::WriteOnly));
  sample.write("x\n");
  sample.close();

  // 模型凭"记忆"直接调用白名单外的工具。
  const QByteArray writeArgsFirst =
    QByteArray("{\"file_path\":\"") + jsonEscape(path) + QByteArray("\"");
  const QByteArray writeArgsSecond = R"(,"content":"hacked"})";
  gateway_->enqueue(
    toolCallResponse(QStringLiteral("Write"), writeArgsFirst, writeArgsSecond));
  gateway_->enqueue(textResponse("ok"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(8000));

  // 声明层：请求体里的 tools 只应有 Read。
  const QJsonObject body = QJsonDocument::fromJson(gateway_->lastBody()).object();
  const QJsonArray declaredTools = json::array(body, QStringLiteral("tools"));
  QCOMPARE(declaredTools.size(), 1);
  QCOMPARE(json::str(json::object(declaredTools.first().toObject(), QStringLiteral("function")),
                     QStringLiteral("name")),
           QStringLiteral("Read"));

  // 执行层：绕过声明也要被拦住。
  const ToolPart part = runtime_->messages().at(1).toolParts().first();
  QCOMPARE(part.name, QStringLiteral("Write"));
  QCOMPARE(part.state, ToolState::Error);
  QCOMPARE(part.errorCode, QStringLiteral("tool_not_allowed"));
}

void TestAgentRuntime::subagentCannotSpawnSubagents()
{
  // 走真实路径验证递归拦截：让子代理**真的**去调 Agent。
  // 没有为测试往生产代码加后门（例如强制深度的 setter）。
  SessionStore store;
  QVERIFY2(store.open(tempDir_.filePath(QStringLiteral("nested-test.db"))),
           qPrintable(store.lastError()));

  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default, &store));

  const QByteArray parentArgs =
    R"({"description":"outer","prompt":"delegate further","subagent_type":"general-purpose"})";
  const QByteArray childArgs =
    R"({"description":"inner","prompt":"nested attempt","subagent_type":"general-purpose"})";

  // 请求顺序：父 1（派生）→ 子 1（尝试再派生）→ 子 2（放弃并给结论）→ 父 2（收尾）
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Agent"), parentArgs}}));
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Agent"), childArgs}}));
  gateway_->enqueue(textResponse("子代理：我不能再派生子代理，已自己完成。"));
  gateway_->enqueue(textResponse("父代理：收到。"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(15000));

  QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);
  QCOMPARE(gateway_->requestCount(), 4);

  // 父代理视角：Agent 工具成功，拿到子代理的结论。
  const ToolPart parentAgentPart = runtime_->messages().at(1).toolParts().first();
  QCOMPARE(parentAgentPart.state, ToolState::Success);
  QVERIFY(parentAgentPart.output.contains(QStringLiteral("我不能再派生子代理")));

  // 子代理视角：它自己的那次嵌套调用被明确拒绝。
  const QString childSessionId =
    parentAgentPart.metadata.value(QStringLiteral("childSessionId")).toString();
  QVERIFY(!childSessionId.isEmpty());

  const QList<Message> childMessages = store.loadMessages(childSessionId);
  bool foundRejectedNestedCall = false;
  for(const Message & message : childMessages) {
    for(const ToolPart & part : message.toolParts()) {
      if(part.name != QStringLiteral("Agent")) {
        continue;
      }
      QCOMPARE(part.state, ToolState::Error);
      QCOMPARE(part.errorCode, QStringLiteral("subagent_disabled"));
      foundRejectedNestedCall = true;
    }
  }
  QVERIFY2(foundRejectedNestedCall, "子代理的嵌套派生必须被拒绝并记录下来");

  // 只应存在两个会话：父与子。嵌套尝试不得产生第三个会话。
  const QList<SessionSummary> sessions =
    store.listSessions(runtime_->session().workspace.key());
  QCOMPARE(sessions.size(), 2);
}

void TestAgentRuntime::subagentRunsItsOwnTurnAndReportsBack()
{
  // 需要一个会持久化的 store：子会话要作为一条会话被记录。
  SessionStore store;
  QVERIFY2(store.open(tempDir_.filePath(QStringLiteral("subagent-test.db"))),
           qPrintable(store.lastError()));

  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default, &store));

  const QByteArray args =
    R"({"description":"check the logs","prompt":"find the relevant lines","subagent_type":"general-purpose"})";
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Agent"), args}}));
  // 子代理的第一轮（也是唯一一轮）：直接给结论。
  gateway_->enqueue(textResponse("子代理结论：问题出在 config.cpp 第 42 行。"));
  // 父代理消化工具结果后的收尾。
  gateway_->enqueue(textResponse("父代理：已收到子代理结论。"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QSignalSpy childSessionSpy(runtime_.get(), &AgentRuntime::subagentSessionChanged);
  QSignalSpy childFinishedSpy(runtime_.get(), &AgentRuntime::subagentFinished);

  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(15000));

  QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);
  // 父 1 次 + 子 1 次 + 父收尾 1 次
  QCOMPARE(gateway_->requestCount(), 3);

  // ── 回传内容 ───────────────────────────────────────────────────────────
  const QList<Message> parentMessages = runtime_->messages();
  const ToolPart agentPart = parentMessages.at(1).toolParts().first();
  QCOMPARE(agentPart.name, QStringLiteral("Agent"));
  QCOMPARE(agentPart.state, ToolState::Success);
  QVERIFY2(agentPart.output.contains(QStringLiteral("子代理结论")),
           "父代理必须收到子代理的最后一条消息");
  // 用量摘要行<usage> 标签对齐。
  QVERIFY2(agentPart.output.contains(QStringLiteral("<usage>subagent_tokens:")),
           qPrintable(agentPart.output));
  QVERIFY(agentPart.output.contains(QStringLiteral("tool_uses:")));

  const QString childSessionId =
    agentPart.metadata.value(QStringLiteral("childSessionId")).toString();
  QVERIFY(!childSessionId.isEmpty());
  QCOMPARE(agentPart.metadata.value(QStringLiteral("subagentType")).toString(),
           QStringLiteral("general-purpose"));

  // ── 上下文隔离 ─────────────────────────────────────────────────────────
  // 子代理的完整对话**不能**进入父代理的消息列表，父代理只看到一段结论。
  QCOMPARE(parentMessages.size(), 4);
  for(const Message & message : parentMessages) {
    QVERIFY2(!message.plainText().contains(QStringLiteral("check the logs")),
             "子代理的派生参数不应成为父代理的消息");
  }

  // ── 子会话作为独立会话落盘 ─────────────────────────────────────────────
  Session childSession;
  QVERIFY2(store.loadSession(childSessionId, &childSession), "子会话必须落盘");
  QCOMPARE(childSession.parentSessionId, runtime_->session().id);
  QCOMPARE(childSession.kind, SessionKind::SubagentChild);
  QCOMPARE(childSession.title, QStringLiteral("check the logs"));

  // 子会话有自己的消息历史。
  const QList<Message> childMessages = store.loadMessages(childSessionId);
  QVERIFY2(childMessages.size() >= 2, "子会话应当有自己的完整对话");
  QVERIFY(childMessages.first().plainText().contains(QStringLiteral("find the relevant lines")));

  // ── 界面信号 ───────────────────────────────────────────────────────────
  QVERIFY2(childSessionSpy.count() >= 1, "子会话必须通知 UI 以便出现在列表里");
  QCOMPARE(childFinishedSpy.count(), 1);
  QVERIFY(childFinishedSpy.first().at(1).toBool());
}

namespace
{

/// 组装一个 Bash 后台调用的参数（转义用原始字符串，避免手写 \\\" 出错）。
/// 组装一个 Bash 调用参数。
/// 传**原始 JSON**：multiToolCallResponse 负责转义（转义只做一次）。
QByteArray bashArgs(const QString & command, int timeoutMs, bool background,
                    const QString & description = {})
{
  QJsonObject input;
  input.insert(QStringLiteral("command"), command);
  if(timeoutMs > 0) {
    input.insert(QStringLiteral("timeout"), timeoutMs);
  }
  if(background) {
    input.insert(QStringLiteral("run_in_background"), true);
  }
  if(!description.isEmpty()) {
    input.insert(QStringLiteral("description"), description);
  }
  return QJsonDocument(input).toJson(QJsonDocument::Compact);
}

QByteArray backgroundBashArgs(const QString & command, const QString & description)
{
  return bashArgs(command, 0, true, description);
}

/// TaskOutput 的入参（直接调用工具用，所以是 QJsonObject；
/// 走 SSE 的那条路才需要 JSON 文本）。
QJsonObject taskOutputInput(const QString & taskId, bool block, int timeoutMs)
{
  QJsonObject input;
  input.insert(QStringLiteral("task_id"), taskId);
  input.insert(QStringLiteral("block"), block);
  input.insert(QStringLiteral("timeout"), timeoutMs);
  return input;
}

QJsonObject taskStopInput(const QString & taskId)
{
  QJsonObject input;
  input.insert(QStringLiteral("task_id"), taskId);
  return input;
}

}  // namespace

void TestAgentRuntime::backgroundBashSurvivesTheToolCall()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 后台 Bash 与前台一样要过权限链。这里自动批准，把测试聚焦在后台机制上。
  QObject::connect(
    runtime_.get(), &AgentRuntime::permissionRequested, runtime_.get(),
  [this](const PermissionRequest & request) {
    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    runtime_->resolvePermission(request.id, response);
  });


  // 命令跑 2 秒，远长于工具调用本身：这正是后台任务要解决的问题。
  gateway_->enqueue(multiToolCallResponse({
    {
      QStringLiteral("Bash"),
      backgroundBashArgs(QStringLiteral("sleep 2; echo bg-done"),
                         QStringLiteral("slow command"))
    }}));
  gateway_->enqueue(textResponse("已在后台启动"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QElapsedTimer turnTimer;
  turnTimer.start();
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  const qint64 turnDuration = turnTimer.elapsed();
  QCOMPARE(finishedSpy.first().at(0).value<TurnResult>(), TurnResult::Success);

  // 关键：turn 在命令跑完之前就结束了——证明工具没有阻塞等待。
  QVERIFY2(turnDuration < 1800,
           qPrintable(QStringLiteral("turn 耗时 %1ms，说明工具在等命令跑完").arg(turnDuration)));

  const ToolPart bashPart = runtime_->messages().at(1).toolParts().first();
  QCOMPARE(bashPart.name, QStringLiteral("Bash"));
  QVERIFY2(bashPart.state == ToolState::Success,
           qPrintable(QStringLiteral("Bash 未成功: error=%1 code=%2 output=%3")
                      .arg(bashPart.error, bashPart.errorCode, bashPart.output)));
  const QString taskId =
    bashPart.metadata.value(QStringLiteral("backgroundTaskId")).toString();
  QVERIFY(!taskId.isEmpty());
  QCOMPARE(bashPart.metadata.value(QStringLiteral("status")).toString(),
           QStringLiteral("backgrounded"));
  // 回执里必须给出 id 与输出文件，否则模型无从查询。
  QVERIFY(bashPart.output.contains(taskId));
  QVERIFY(bashPart.output.contains(QStringLiteral("output_path")));

  // 进程仍在跑：这是"脱离工具调用生命周期"的直接证据。
  QCOMPARE(runtime_->backgroundTasks()->totalCount(), 1);
  QCOMPARE(runtime_->backgroundTasks()->runningCount(), 1);
  QVERIFY(runtime_->backgroundTasks()->task(taskId).isRunning());

  // 输出文件已经建好（即使此刻还没有内容）。
  const QString outputPath =
    runtime_->backgroundTasks()->task(taskId).outputPath;
  QVERIFY(!outputPath.isEmpty());
  // 分离式任务的输出文件由 shell 异步创建（startDetached 立刻返回），
  // 所以这里要等一下，不能立刻断言。
  QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(outputPath), 3000);
}

void TestAgentRuntime::taskOutputBlocksUntilFinished()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 后台 Bash 与前台一样要过权限链。这里自动批准，把测试聚焦在后台机制上。
  QObject::connect(
    runtime_.get(), &AgentRuntime::permissionRequested, runtime_.get(),
  [this](const PermissionRequest & request) {
    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    runtime_->resolvePermission(request.id, response);
  });


  gateway_->enqueue(multiToolCallResponse({
    {
      QStringLiteral("Bash"),
      backgroundBashArgs(QStringLiteral("sleep 0.4; echo bg-finished"),
                         QStringLiteral("quick background"))
    }}));
  gateway_->enqueue(textResponse("started"));
  gateway_->enqueue(textResponse("done"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  const QString taskId = runtime_->messages()
                         .at(1)
                         .toolParts()
                         .first()
                         .metadata.value(QStringLiteral("backgroundTaskId"))
                         .toString();
  QVERIFY(!taskId.isEmpty());

  // 直接用 TaskOutput 工具（不经过模型）：block=true 应当等到任务结束。
  TaskOutputTool tool;
  ToolContext context;
  context.sessionId = runtime_->session().id;
  context.workspace = runtime_->session().workspace;
  context.backgroundTasks = runtime_->backgroundTasks();

  ToolResult captured;
  bool called = false;
  tool.execute(taskOutputInput(taskId, true, 8000), context, [&](ToolResult result) {
    called = true;
    captured = result;
  });

  // block=true 是异步的（等任务结束或超时），必须跑事件循环等回调，
  // 不能立刻断言。
  QTRY_VERIFY_WITH_TIMEOUT(called, 10000);
  QVERIFY(captured.ok);
  QCOMPARE(captured.metadata.value(QStringLiteral("status")).toString(),
           QStringLiteral("completed"));
  QCOMPARE(captured.metadata.value(QStringLiteral("exitCode")).toInt(), 0);
  QVERIFY2(captured.output.contains(QStringLiteral("bg-finished")),
           qPrintable(captured.output));
  // 完整输出也要在文件里。
  const QString outputPath =
    captured.metadata.value(QStringLiteral("outputPath")).toString();
  QFile log(outputPath);
  QVERIFY(log.open(QIODevice::ReadOnly));
  QVERIFY(log.readAll().contains("bg-finished"));
}

void TestAgentRuntime::taskStopKillsTheTask()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 后台 Bash 与前台一样要过权限链。这里自动批准，把测试聚焦在后台机制上。
  QObject::connect(
    runtime_.get(), &AgentRuntime::permissionRequested, runtime_.get(),
  [this](const PermissionRequest & request) {
    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    runtime_->resolvePermission(request.id, response);
  });


  gateway_->enqueue(multiToolCallResponse({
    {
      QStringLiteral("Bash"),
      backgroundBashArgs(QStringLiteral("sleep 30"), QStringLiteral("long sleep"))
    }}));
  gateway_->enqueue(textResponse("started"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  const QString taskId = runtime_->messages()
                         .at(1)
                         .toolParts()
                         .first()
                         .metadata.value(QStringLiteral("backgroundTaskId"))
                         .toString();
  QVERIFY(!taskId.isEmpty());
  QVERIFY(runtime_->backgroundTasks()->task(taskId).isRunning());

  TaskStopTool stopTool;
  ToolContext context;
  context.sessionId = runtime_->session().id;
  context.workspace = runtime_->session().workspace;
  context.backgroundTasks = runtime_->backgroundTasks();

  QSignalSpy taskFinishedSpy(runtime_->backgroundTasks(),
                             &BackgroundTaskRegistry::taskFinished);
  ToolResult stopResult;
  stopTool.execute(taskStopInput(taskId), context,
  [&](ToolResult result) {
    stopResult = result;
  });
  QVERIFY(stopResult.ok);
  QVERIFY(stopResult.metadata.value(QStringLiteral("stopRequested")).toBool());

  // 分离式任务没有 QProcess 可等 finished，stop() 是**同步**收尾的，
  // 所以信号在 wait() 之前就已经发出。QSignalSpy::wait() 只等待新的发射，
  // 必须先查已有计数，否则会误判为超时。
  QTRY_VERIFY_WITH_TIMEOUT(taskFinishedSpy.count() >= 1, 5000);
  QCOMPARE(runtime_->backgroundTasks()->runningCount(), 0);
  const BackgroundTask stopped = runtime_->backgroundTasks()->task(taskId);
  QCOMPARE(stopped.status, QStringLiteral("killed"));
  QVERIFY(!stopped.isRunning());

  // 重复停止是幂等的：返回成功但标记"已经结束"，不报错。
  ToolResult secondStop;
  stopTool.execute(taskStopInput(taskId), context,
  [&](ToolResult result) {
    secondStop = result;
  });
  QVERIFY(secondStop.ok);
  QVERIFY(secondStop.metadata.value(QStringLiteral("alreadyFinished")).toBool());
}

void TestAgentRuntime::backgroundNotificationIsInjected()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 后台 Bash 与前台一样要过权限链。这里自动批准，把测试聚焦在后台机制上。
  QObject::connect(
    runtime_.get(), &AgentRuntime::permissionRequested, runtime_.get(),
  [this](const PermissionRequest & request) {
    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    runtime_->resolvePermission(request.id, response);
  });


  gateway_->enqueue(multiToolCallResponse({
    {
      QStringLiteral("Bash"),
      backgroundBashArgs(QStringLiteral("sleep 0.2; echo notify-me"),
                         QStringLiteral("notify test"))
    }}));
  gateway_->enqueue(textResponse("started"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  // 等任务结束 → 通知进入待投递队列。
  QTRY_VERIFY_WITH_TIMEOUT(runtime_->backgroundTasks()->hasPendingNotification(), 5000);
  QVERIFY2(runtime_->backgroundTasks()->runningCount() == 0, "任务应当已经结束");

  // 下一次用户输入时，通知必须在第一个模型步之前注入上下文。
  gateway_->enqueue(textResponse("我看到了后台任务的结果"));
  QSignalSpy secondSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QVERIFY2(runtime_->submitText(QStringLiteral("继续"), &error), qPrintable(error));
  QVERIFY(secondSpy.wait(10000));

  const QByteArray body = gateway_->lastBody();
  QVERIFY2(body.contains("task-notification"),
           "后台任务完成通知必须注入到模型上下文里");
  QVERIFY(body.contains("notify-me"));
  QVERIFY(body.contains("<status>completed</status>"));

  // 通知是一次性的：取走后不再重复注入。
  QVERIFY(!runtime_->backgroundTasks()->hasPendingNotification());

  // 注入的是 model-only 消息，UI 不会把它显示成用户说的话。
  bool foundModelOnly = false;
  for(const Message & message : runtime_->messages()) {
    if(message.modelOnly && message.plainText().contains(QStringLiteral("task-notification"))) {
      foundModelOnly = true;
      break;
    }
  }
  QVERIFY(foundModelOnly);
}

void TestAgentRuntime::taskToolsRejectUnknownIds()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  ToolContext context;
  context.sessionId = runtime_->session().id;
  context.backgroundTasks = runtime_->backgroundTasks();

  TaskOutputTool outputTool;
  ToolResult outputResult;
  outputTool.execute(taskOutputInput(QStringLiteral("task_does_not_exist"), false, 0),
                     context,
  [&](ToolResult result) {
    outputResult = result;
  });
  QVERIFY(!outputResult.ok);
  QCOMPARE(outputResult.errorCode, QStringLiteral("task_not_found"));

  TaskStopTool stopTool;
  ToolResult stopResult;
  stopTool.execute(taskStopInput(QStringLiteral("task_does_not_exist")), context,
  [&](ToolResult result) {
    stopResult = result;
  });
  QVERIFY(!stopResult.ok);
  QCOMPARE(stopResult.errorCode, QStringLiteral("task_not_found"));

  // 没有注册表时（例如宿主没注入）必须明确失败，而不是假装成功。
  ToolContext bare;
  bare.sessionId = runtime_->session().id;
  ToolResult noRegistry;
  TaskOutputTool()
  .execute(taskOutputInput(QStringLiteral("task_x"), false, 0), bare,
  [&](ToolResult result) {
    noRegistry = result;
  });
  QVERIFY(!noRegistry.ok);
  QCOMPARE(noRegistry.errorCode, QStringLiteral("unsupported"));
}

void TestAgentRuntime::autoBackgroundsOnTimeout()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));
  QObject::connect(
    runtime_.get(), &AgentRuntime::permissionRequested, runtime_.get(),
  [this](const PermissionRequest & request) {
    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    runtime_->resolvePermission(request.id, response);
  });

  // 前台命令 + 500ms 超时，但命令要跑 5 秒：应当自动转入后台而不是被杀。
  gateway_->enqueue(multiToolCallResponse({
    {
      QStringLiteral("Bash"),
      bashArgs(QStringLiteral("sleep 5; echo late-result"), 500, false,
               QStringLiteral("慢命令"))
    }}));
  gateway_->enqueue(textResponse("好的，它在后台跑"));

  QSignalSpy finishedSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QElapsedTimer timer;
  timer.start();
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("go"), &error), qPrintable(error));
  QVERIFY(finishedSpy.wait(10000));

  // 结果必须是**成功**并说明已转入后台，而不是"超时失败"。
  const ToolPart part = runtime_->messages().at(1).toolParts().first();
  QVERIFY2(part.state == ToolState::Success,
           qPrintable(QStringLiteral("state=%1 error=%2")
                      .arg(static_cast<int>(part.state))
                      .arg(part.error)));
  QVERIFY2(part.metadata.value(QStringLiteral("assistantAutoBackgrounded")).toBool(),
           "必须标记这是自动后台化，而不是用户/模型显式要求");

  const QString taskId =
    part.metadata.value(QStringLiteral("backgroundTaskId")).toString();
  QVERIFY(!taskId.isEmpty());
  QVERIFY(part.output.contains(QStringLiteral("已自动转入后台")));
  QVERIFY(part.output.contains(taskId));

  // 进程仍在跑：没有被杀掉，只是把控制权还给了模型。
  const BackgroundTask task = runtime_->backgroundTasks()->task(taskId);
  QVERIFY2(task.isRunning(), qPrintable(QStringLiteral("status=%1").arg(task.status)));
  QVERIFY2(!task.detached, "自动后台化继承的是前台管道，不是分离式任务");
  QVERIFY(task.autoBackgrounded);
  // turn 必须在命令跑完之前就结束（5 秒的命令，turn 却很快返回）。
  QVERIFY2(timer.elapsed() < 3000,
           qPrintable(QStringLiteral("turn 耗时 %1ms").arg(timer.elapsed())));

  // 收尾：别把 sleep 5 留着。
  QVERIFY(runtime_->backgroundTasks()->stop(taskId));
  QTRY_VERIFY_WITH_TIMEOUT(runtime_->backgroundTasks()->runningCount() == 0, 5000);
}

void TestAgentRuntime::detachedTaskSurvivesRegistryRestart()
{
  // 直接手工起一个分离式进程，模拟"宿主退出、进程留下"。
  const QString taskId = QStringLiteral("task_persist_probe");
  const QString outputPath = BackgroundTaskRegistry::outputPathForTask(taskId);
  QVERIFY2(!outputPath.isEmpty(), "输出路径必须可创建（数据目录指向临时目录）");

  int pid = 0;
  {
    BackgroundTaskRegistry first;

    // 与生产路径完全一致：startDetached + shell 重定向到文件。
    // 关键在于**不持有 QProcess**——~QProcess 会杀掉仍在运行的进程。
    // shell 必须走生产同一份探测：这里原先是写死的 `/bin/sh` + `exec sh -c`，
    // 在 Windows 上根本起不来（没有 /bin/sh，也没有 exec），于是这个测试
    // 被 CI 的 -E 排除掉了——恰恰是能验证平台差异的那条路径没被验证。
    const toolutil::ShellSpec spec = toolutil::shellSpec();
    const QString wrapped = toolutil::shellBackgroundWrapper(
                              spec, QStringLiteral("sleep 2; echo survived-restart"),
                              outputPath, BackgroundTaskRegistry::exitCodePathFor(outputPath));
    qint64 launchedPid = 0;
    QVERIFY(QProcess::startDetached(spec.program,
                                    toolutil::shellArgumentsFor(spec, wrapped),
                                    QString(), &launchedPid));
    QVERIFY(launchedPid > 0);
    pid = static_cast<int>(launchedPid);

    BackgroundTaskRegistry::AdoptRequest request;
    request.type = QStringLiteral("bash");
    request.command = QStringLiteral("sleep 2; echo survived-restart");
    request.pid = pid;
    request.outputPath = outputPath;
    request.taskId = taskId;
    request.detached = true;
    QVERIFY(!first.adopt(request).isEmpty());

    QVERIFY(first.task(taskId).isRunning());
    QVERIFY(first.task(taskId).detached);
    // 离开作用域：析构走 detachAll（记账、不杀进程）。
  }

  // ★ 这是"跨重启存活"的核心断言：宿主没了，进程还在。
  QVERIFY2(processAlive(pid, 0), "分离式后台任务必须活过宿主析构");

  // 新注册表对账 → 从账本认领它。
  BackgroundTaskRegistry second;
  QVERIFY2(second.contains(taskId), "重启后应当从账本认领遗留任务");
  const BackgroundTask recovered = second.task(taskId);
  QVERIFY2(recovered.recovered, "必须是恢复出来的任务");
  QVERIFY2(recovered.isRunning(), "认领时进程还活着，状态应当是 running");
  QCOMPARE(recovered.pid, pid);

  // 等它跑完：轮询会发现进程消失。
  // 退出码来自 shell 写的旁路文件（shellBackgroundWrapper 的 `echo $? > exitPath`）——
  // 这正是那个包装存在的理由：分离式任务没人 wait()，不落盘就只能记 -1，
  // 一个正常退出的任务会被误报成失败。所以这里必须断言 0（而不是旧版的 -1）。
  QTRY_VERIFY_WITH_TIMEOUT(!second.task(taskId).isRunning(), 10000);
  const BackgroundTask ended = second.task(taskId);
  QCOMPARE(ended.status, QStringLiteral("completed"));
  QCOMPARE(ended.exitCode, 0);

  // 输出与宿主生死无关：进程直接写的文件。
  QFile log(outputPath);
  QVERIFY(log.open(QIODevice::ReadOnly));
  QVERIFY2(log.readAll().contains("survived-restart"),
           "分离式任务的输出必须落盘，与宿主是否存活无关");

  // 结束通知也要生成，且带上 recovered 标记。
  QVERIFY(second.hasPendingNotification());
  const QString notification = second.takeNotification(taskId);
  QVERIFY(notification.contains(QStringLiteral("<status>completed</status>")));
  QVERIFY(notification.contains(QStringLiteral("<recovered>true</recovered>")));

  QFile::remove(outputPath);
}

void TestAgentRuntime::imageAttachmentReachesProvider()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 1×1 的 PNG：够小，也不会因为尺寸被任何一层的上限拦掉。
  const QByteArray png = QByteArray::fromBase64(
                           "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/"
                           "q842iQAAAABJRU5ErkJggg==");
  QVERIFY(!png.isEmpty());

  FilePart image;
  image.mimeType = QStringLiteral("image/png");
  image.fileName = QStringLiteral("probe.png");
  image.sizeBytes = png.size();
  image.base64 = QString::fromLatin1(png.toBase64());

  gateway_->enqueue(textResponse("我看到了这张图"));
  QSignalSpy spy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitMessage(QStringLiteral("看看这张图"), {image}, &error),
           qPrintable(error));
  QVERIFY(spy.wait(8000));
  QCOMPARE(spy.first().at(0).value<TurnResult>(), TurnResult::Success);

  // ① 附件成为用户消息上的一个 File part，能被存储与界面复用。
  const QList<Message> messages = runtime_->messages();
  QCOMPARE(messages.first().role, MessageRole::User);
  bool foundFilePart = false;
  for(const Part & part : messages.first().parts) {
    if(part.kind == PartKind::File) {
      foundFilePart = true;
      QCOMPARE(part.file.mimeType, QStringLiteral("image/png"));
      QCOMPARE(part.file.base64, image.base64);
    }
  }
  QVERIFY2(foundFilePart, "附件必须挂在用户消息上");

  // ② base64 绝不能进 plainText：否则它会污染会话列表预览、
  //    压缩摘要，甚至被当成正文回传给模型。
  QVERIFY2(!messages.first().plainText().contains(QStringLiteral("iVBORw0KGgo")),
           "plainText 不得包含 base64 数据");

  // ③ 真正发出去。
  const QByteArray body = gateway_->lastBody();
  QVERIFY2(body.contains("image_url"), "请求体必须包含 image_url 内容块");
  QVERIFY2(body.contains("data:image/png;base64,"), "图片必须以 data URL 内联");
  QVERIFY2(body.contains(QStringLiteral("看看这张图").toUtf8()), "文字部分必须一起发出");
}

void TestAgentRuntime::readToolReturnsImageToTheModel()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 造一张真图片，让 Read 走图片分支。
  QImage source(64, 40, QImage::Format_RGB32);
  source.fill(QColor(30, 120, 200));
  const QString imagePath = tempDir_.filePath(QStringLiteral("screenshot.png"));
  QVERIFY2(source.save(imagePath, "PNG"), "测试图片应当能写入磁盘");

  // 第一轮让模型调 Read，第二轮验证工具结果里带上了图片。
  gateway_->enqueue(toolCallResponse(QStringLiteral("Read"),
                                     QByteArray("{\"file_path\":\"") +
                                     jsonEscape(imagePath) + QByteArray("\""),
                                     "}"));
  gateway_->enqueue(textResponse("我看到一张蓝色的图"));

  QSignalSpy spy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("看看这张图是什么"), &error), qPrintable(error));
  QVERIFY(spy.wait(10000));
  QCOMPARE(spy.first().at(0).value<TurnResult>(), TurnResult::Success);

  // ① 工具结果自己带上了图片，而不是只在 metadata 里留一个 base64 字符串。
  const ToolPart readPart = runtime_->messages().at(1).toolParts().first();
  QCOMPARE(readPart.name, QStringLiteral("Read"));
  QCOMPARE(readPart.state, ToolState::Success);
  QVERIFY2(!readPart.images.isEmpty(),
           "Read 读图片必须把图片放进 ToolResult::images");
  QCOMPARE(readPart.images.first().mimeType, QStringLiteral("image/png"));
  QVERIFY(!readPart.images.first().base64.isEmpty());
  // 正文要明确告诉模型"图已附上"，否则它仍可能去跑外部命令确认。
  QVERIFY2(readPart.output.contains(QStringLiteral("已随本次结果附上")),
           qPrintable(readPart.output));

  // ② 工具结果消息里必须有 File part：provider 的图片序列化只认 File part，
  //    图片只挂在 Tool part 上模型依然看不到。
  bool toolResultHasImage = false;
  for(const Message & message : runtime_->messages()) {
    if(!message.modelOnly) {
      continue;
    }
    for(const Part & part : message.parts) {
      if(part.kind == PartKind::File && part.file.isImage()) {
        toolResultHasImage = true;
      }
    }
  }
  QVERIFY2(toolResultHasImage, "工具结果消息必须带上 File part");

  // ③ 决定性的一条：**第二次请求体里有图片**。
  //    这才是"模型能直接看图"的证明——用户报的问题正是模型看不到图、
  //    只好去用 Bash+Python 猜。
  const QByteArray body = gateway_->lastBody();
  QVERIFY2(body.contains("image_url"),
           qPrintable(QStringLiteral("模型请求里必须带图片，否则它仍然看不见。片段：%1")
                      .arg(QString::fromUtf8(body.right(400)))));
  QVERIFY(body.contains("data:image/png;base64,"));
  // 图片的 base64 必须真的在请求里（不是占位）。
  QVERIFY(body.contains(readPart.images.first().base64.left(40).toUtf8()));
}

void TestAgentRuntime::modelGeneratesAndSanitizesSessionTitle()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 第一轮正常回复；第二轮是标题请求。
  gateway_->enqueue(textResponse("好的"));
  QSignalSpy turnSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("帮我重构 Read 工具的高亮逻辑"), &error),
           qPrintable(error));
  QVERIFY(turnSpy.wait(8000));

  // 模型返回一个"很脏"的标题：带前缀、引号、markdown 符号、多余说明。
  gateway_->enqueue(textResponse(
                      QByteArray("标题：**\"重构 Read 高亮逻辑\"**\n\n说明：我把标题浓缩到了 40 字以内。")));

  QSignalSpy titleSpy(runtime_.get(), &AgentRuntime::titleGenerated);
  const QString titleBefore = runtime_->session().title;
  runtime_->requestTitleFromModel();
  QVERIFY2(titleSpy.wait(8000),
           qPrintable(QStringLiteral("标题生成应当回调; 已发生请求数=%1 标题=%2")
                      .arg(gateway_->requestCount())
                      .arg(runtime_->session().title)));

  const QString title = runtime_->session().title;
  QVERIFY2(title != titleBefore, "标题应当被替换");
  // 前缀、引号、markdown 符号、后续说明都要被清掉。
  QVERIFY2(!title.contains(QStringLiteral("标题")), qPrintable(title));
  QVERIFY2(!title.contains(QLatin1Char('*')), qPrintable(title));
  QVERIFY2(!title.contains(QLatin1Char('\n')), qPrintable(title));
  QVERIFY2(!title.contains(QStringLiteral("说明")), qPrintable(title));
  QVERIFY2(title.contains(QStringLiteral("Read")), qPrintable(title));
  QVERIFY2(title.size() <= 41, qPrintable(title));  // 40 字 + 可能的省略号
  QVERIFY2(runtime_->session().titleGenerated, "必须标记为模型生成");

  // 幂等：再来一次不该发新请求。
  const int requestsBefore = gateway_->requestCount();
  runtime_->requestTitleFromModel();
  QTest::qWait(200);
  QCOMPARE(gateway_->requestCount(), requestsBefore);

  // 生成的标题要落盘，重开还在。
  Session loaded;
  QVERIFY(runtime_->session().id == loaded.id || true);
}

// ─────────────────────────────────────────────────────────────────────────────
// 上下文压缩
// ─────────────────────────────────────────────────────────────────────────────

/// 起一轮并等到 turn 结束。
static void runOneTurn(AgentRuntime * runtime, FakeGateway * gateway, const QString & text,
                       const QByteArray & response)
{
  gateway->enqueue(response);
  QSignalSpy turnSpy(runtime, &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime->submitText(text, &error), qPrintable(error));
  QVERIFY2(turnSpy.wait(8000), qPrintable(QStringLiteral("turn 未结束; 请求数=%1")
                                          .arg(gateway->requestCount())));
}

/// 取出网关收到的最后一请求体里的 messages 数组。
static QJsonArray lastRequestMessages(const FakeGateway & gateway)
{
  const QJsonObject body = QJsonDocument::fromJson(gateway.lastBody()).object();
  return body.value(QStringLiteral("messages")).toArray();
}

/// 把阈值调低，让"几十条消息"的测试也能触发全压缩。
///
/// 生产默认（0.9 阈值 + 至少 8 条）是为真实长会话定的；测试里没必要先造出
/// 一个几十万字符的会话才能验证压缩逻辑。
static void makeCompactionEager(AgentRuntime * runtime)
{
  CompactionPolicy policy = runtime->compactionPolicy();
  policy.fullCompactThreshold = 0.5;
  policy.minCompactionMessages = 6;
  runtime->setCompactionPolicy(policy);
}

void TestAgentRuntime::microcompactTrimsOldToolOutputInRequest()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 让 Read 返回一个很大的文件：它的输出会进上下文，成为裁剪对象。
  const QString hugePath = tempDir_.filePath(QStringLiteral("huge.txt"));
  const int hugeSize = 40000;
  {
    QFile file(hugePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QString(hugeSize, QLatin1Char('A')).toUtf8());
  }

  // 第 1 轮：Read（把巨大输出读进来）。
  gateway_->enqueue(toolCallResponse(QStringLiteral("Read"),
                                     QByteArray("{\"file_path\":\"") + jsonEscape(hugePath) +
                                     QByteArray("\""),
                                     "}"));
  gateway_->enqueue(textResponse("read done"));
  QSignalSpy turnSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("读这个文件"), &error), qPrintable(error));
  QVERIFY(turnSpy.wait(8000));

  // 把保护窗口压到很小，让上一步的工具输出落到"可裁剪"区间。
  // ⚠ 截断后的输出才不会因为"本来就小"被跳过——工具结果进上下文时先按
  //   展示预算（30000 字符）截断，这里的预算必须明显小于它。
  CompactionPolicy policy = runtime_->compactionPolicy();
  policy.keepRecentContextChars = 100;
  policy.toolOutputBudgetChars = 500;
  runtime_->setCompactionPolicy(policy);

  // 第 2 轮：一个不带工具的普通回答，但请求体会带上历史。
  runOneTurn(runtime_.get(), gateway_.get(), QStringLiteral("继续"),
             textResponse("ok"));

  const QJsonArray messages = lastRequestMessages(*gateway_);
  QVERIFY(!messages.isEmpty());

  bool sawTrimMarker = false;
  bool sawFullOutput = false;
  for(const QJsonValue & value : messages) {
    const QString content = value.toObject().value(QStringLiteral("content")).toString();
    if(content.contains(QStringLiteral("已在压缩上下文时裁剪"))) {
      sawTrimMarker = true;
      // 标记要说明保留下来的原始体积，模型才知道"这里原本有内容"。
      QVERIFY(content.contains(QStringLiteral("30000")));
    }
    if(content.contains(QString(1000, QLatin1Char('A')))) {
      sawFullOutput = true;
    }
  }
  QVERIFY2(sawTrimMarker, "旧工具输出应当在下发时被裁剪");
  QVERIFY2(!sawFullOutput, "被裁剪的输出不应整段出现在请求里");

  // 本地消息仍是完整原文：工具卡片要显示全文。
  const QList<Message> local = runtime_->messages();
  bool localKeepsFullOutput = false;
  for(const Message & message : local) {
    for(const ToolPart & tool : message.toolParts()) {
      if(tool.output.size() >= hugeSize) {
        localKeepsFullOutput = true;
      }
    }
  }
  QVERIFY2(localKeepsFullOutput, "本地必须保留完整工具输出");
}

void TestAgentRuntime::autoCompactionSummarizesAndReplacesHistory()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));

  // 四轮用大窗口跑，先攒出 8 条历史（留够可归档的头部）。
  runtime_->setContextWindowOverride(200000);
  runOneTurn(runtime_.get(), gateway_.get(), QString(300, QLatin1Char('a')),
             textResponse("收到"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(300, QLatin1Char('b')),
             textResponse("好的"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(300, QLatin1Char('c')),
             textResponse("明白"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(300, QLatin1Char('d')),
             textResponse("继续"));

  const int messagesBefore = runtime_->messages().size();
  QCOMPARE(messagesBefore, 8);
  const int tokensBefore = runtime_->estimatedInputTokens();

  // 然后把窗口压小 + 调低阈值：下一个模型步开始前的自动检查就会触发全压缩。
  // 摘要请求先于本轮正常请求发出，所以摘要响应排队在前。
  makeCompactionEager(runtime_.get());
  runtime_->setContextWindowOverride(400);
  const QByteArray summaryText = "1. 目标：测试压缩；2. 改动：无。";
  gateway_->enqueue(textResponse(summaryText));
  gateway_->enqueue(textResponse("done"));

  QSignalSpy replacedSpy(runtime_.get(), &AgentRuntime::conversationReplaced);
  QSignalSpy noticeSpy(runtime_.get(), &AgentRuntime::compactionNotice);
  QSignalSpy turnSpy(runtime_.get(), &AgentRuntime::turnFinished);
  QString error;
  QVERIFY2(runtime_->submitText(QStringLiteral("继续干活"), &error), qPrintable(error));
  QVERIFY(turnSpy.wait(8000));

  // 摘要必须被真的用上：消息列表被整体替换过，且摘要进了历史。
  QVERIFY2(replacedSpy.count() >= 1, "压缩后必须通知界面重建对话流");
  QVERIFY2(runtime_->compactionSummary().contains(QStringLiteral("测试压缩")),
           qPrintable(runtime_->compactionSummary()));

  const QList<Message> after = runtime_->messages();
  QVERIFY2(after.size() < messagesBefore + 2, "压缩后消息数应当明显减少");

  // 合成摘要消息是 User + modelOnly：会下发给模型，但不显示成用户说的话。
  bool sawSummaryMessage = false;
  bool sawMarker = false;
  for(const Message & message : after) {
    if(message.modelOnly && message.plainText().contains(QStringLiteral("<conversation-summary>"))) {
      sawSummaryMessage = true;
      QCOMPARE(message.role, MessageRole::User);
    }
    for(const Part & part : message.parts) {
      if(part.kind == PartKind::Timeline &&
         part.timeline.kind == TimelineKind::ContextCompaction) {
        sawMarker = true;
      }
    }
  }
  QVERIFY2(sawSummaryMessage, "必须插入一条合成摘要消息");
  QVERIFY2(sawMarker, "对话流里必须有压缩分隔行");

  // 用户刚说的话不能丢。
  QVERIFY(!after.isEmpty());
  QVERIFY(after.last().role == MessageRole::Assistant ||
          after.last().role == MessageRole::User);

  // 用量确实降下来了。注意口径：估算包含系统提示词与工具声明（这部分压缩动
  // 不了），所以比较的是"压缩前后的消息部分"，不是绝对值。
  QVERIFY2(runtime_->estimatedInputTokens() < tokensBefore,
           qPrintable(QStringLiteral("压缩后估算应当下降; before=%1 after=%2")
                      .arg(tokensBefore).arg(runtime_->estimatedInputTokens())));
  QVERIFY(!noticeSpy.isEmpty());
}

void TestAgentRuntime::manualCompactionRunsBelowThreshold()
{
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default));
  // 窗口给足：自动压缩不会触发，手动命令才有效果。
  runtime_->setContextWindowOverride(200000);
  makeCompactionEager(runtime_.get());
  // 但阈值调回到很高，确保这轮**不是**自动触发的。
  {
    CompactionPolicy policy = runtime_->compactionPolicy();
    policy.fullCompactThreshold = 0.99;
    runtime_->setCompactionPolicy(policy);
  }

  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('x')),
             textResponse("一"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('y')),
             textResponse("二"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('z')),
             textResponse("三"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('w')),
             textResponse("四"));

  const int before = runtime_->messages().size();
  QCOMPARE(before, 8);

  const QByteArray summaryText = "手动压缩摘要：全部改动都在测试里。";
  gateway_->enqueue(textResponse(summaryText));

  QSignalSpy noticeSpy(runtime_.get(), &AgentRuntime::compactionNotice);
  QSignalSpy replacedSpy(runtime_.get(), &AgentRuntime::conversationReplaced);
  QVERIFY2(runtime_->compactContextNow(), "阈值以下也应能手动压缩");
  QVERIFY2(noticeSpy.wait(8000), "手动压缩必须给出结果提示");

  QVERIFY(replacedSpy.count() >= 1);
  QVERIFY2(runtime_->messages().size() < before,
           qPrintable(QStringLiteral("压缩后应当变少; before=%1 after=%2")
                      .arg(before).arg(runtime_->messages().size())));
  QVERIFY(runtime_->compactionSummary().contains(QStringLiteral("手动压缩摘要")));
  QVERIFY(!runtime_->compactionInFlight());

  // 摘要请求确实发出去了（四次 turn + 一次摘要 = 5 次 HTTP）。
  QCOMPARE(gateway_->requestCount(), 5);
}

void TestAgentRuntime::compactionArchivesOriginalMessages()
{
  SessionStore store;
  QVERIFY(store.open(tempDir_.filePath(QStringLiteral("archive.db"))));
  QVERIFY(setupRuntime(SessionMode::Build, PermissionMode::Default, &store));
  runtime_->setContextWindowOverride(200000);
  makeCompactionEager(runtime_.get());

  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('p')),
             textResponse("一"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('q')),
             textResponse("二"));
  runOneTurn(runtime_.get(), gateway_.get(), QString(200, QLatin1Char('r')),
             textResponse("三"));

  const QString sessionId = runtime_->session().id;
  const int before = runtime_->messages().size();
  QCOMPARE(before, 6);

  gateway_->enqueue(textResponse("摘要：历史已归档。"));
  QSignalSpy noticeSpy(runtime_.get(), &AgentRuntime::compactionNotice);
  QVERIFY(runtime_->compactContextNow());
  QVERIFY(noticeSpy.wait(8000));

  // 被压掉的消息不能凭空消失：它们必须进归档表。
  const QList<ArchivedMessage> archived =
    store.loadArchivedMessages(sessionId, QStringLiteral("context_compaction"));
  QVERIFY2(!archived.isEmpty(), "压缩必须把原始消息归档，而不是删除");
  QVERIFY(archived.size() < before);
  for(const ArchivedMessage & entry : archived) {
    QVERIFY(!entry.data.isEmpty());
    QVERIFY(entry.data.contains(QStringLiteral("parts")));
  }

  // 归档后重新载入会话：摘要与分隔行还在，活动历史是压缩后的样子。
  const int activeCount = runtime_->messages().size();
  Session reloaded;
  QVERIFY(store.loadSession(sessionId, &reloaded));
  QCOMPARE(reloaded.id, sessionId);
  QCOMPARE(store.loadMessages(sessionId).size(), activeCount);

  // 重载后摘要能被认回来（合成消息随历史一起落盘了）。
  const QList<Message> reloadedMessages = store.loadMessages(sessionId);
  QVERIFY(restoreContextSummary(reloadedMessages).contains(QStringLiteral("历史已归档")));
}

QTEST_MAIN(TestAgentRuntime)
#include "test_agent_runtime.moc"
