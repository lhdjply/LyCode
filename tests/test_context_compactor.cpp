// LyCode — 上下文压缩单元测试
//
// 压缩模块刻意做成纯函数，所以这一层可以直接断言"哪些消息被动过、合成消息
// 长什么样"，不需要启动运行时或假网关。运行时的触发时机与落盘在
// test_agent_runtime.cpp 里用真实模型流验证。
#include <QtTest>

#include <QJsonObject>

#include "agent/ContextCompactor.h"
#include "core/Ids.h"
#include "core/Types.h"

using namespace lycode;

namespace
{

Message makeMessage(MessageRole role, const QString & text)
{
  Message message;
  message.id = newMessageId();
  message.sessionId = QStringLiteral("session_test");
  message.role = role;
  message.status = MessageStatus::Complete;
  message.parts.append(Part::makeText(text));
  return message;
}

Message makeToolMessage(const QString & callId, const QString & name, const QString & output)
{
  Message message = makeMessage(MessageRole::User, {});
  message.modelOnly = true;
  Part part = Part::makeTool(name, callId);
  part.tool.state = ToolState::Success;
  part.tool.output = output;
  message.parts.append(part);
  return message;
}

/// 一轮正常的问答：user → assistant。
QList<Message> makeConversation(int turns, const QString & text = QStringLiteral("内容"))
{
  QList<Message> messages;
  for(int index = 0; index < turns; ++index) {
    messages.append(makeMessage(MessageRole::User,
                                QStringLiteral("%1-u%2").arg(text).arg(index)));
    messages.append(makeMessage(MessageRole::Assistant,
                                QStringLiteral("%1-a%2").arg(text).arg(index)));
  }
  return messages;
}

}  // namespace

class TestContextCompactor : public QObject
{
    Q_OBJECT

  private slots:
    void estimateCountsToolSchemasAndPrompt();
    void microcompactTrimsOldToolOutputOnly();
    void microcompactKeepsRecentToolOutput();
    void microcompactLeavesShortOutputsAlone();
    void microcompactKeepsToolCallPairing();
    void fullCompactionCutsOnUserBoundary();
    void fullCompactionKeepsCurrentUserMessage();
    void fullCompactionRefusesShortHistory();
    void fullCompactionArchivesHeadVerbatim();
    void summaryMessageRoundTripsThroughJson();
    void markerMessageIsLocalOnly();
    void transcriptKeepsToolNames();
};

void TestContextCompactor::estimateCountsToolSchemasAndPrompt()
{
  const QList<Message> messages = {makeMessage(MessageRole::User, QString(3000, QLatin1Char('a')))};

  const int bare = estimateContextTokens(messages, {}, {});
  QVERIFY(bare > 0);

  // 工具声明与系统提示词都占窗口：不计它们会在窗口快满时低估用量，
  // 压缩就永远不触发。
  ToolSpec spec;
  spec.name = QStringLiteral("Read");
  spec.description = QString(600, QLatin1Char('d'));
  const int withTool = estimateContextTokens(messages, {spec}, {});
  QVERIFY(withTool > bare);

  const int withPrompt = estimateContextTokens(messages, {spec}, QString(3000, QLatin1Char('p')));
  QVERIFY(withPrompt > withTool);
}

void TestContextCompactor::microcompactTrimsOldToolOutputOnly()
{
  CompactionPolicy policy;
  policy.keepRecentContextChars = 30000;   // 只保护最后一轮
  policy.toolOutputBudgetChars = 1000;

  const QString huge = QString(20000, QLatin1Char('x'));
  QList<Message> messages;
  messages.append(makeMessage(MessageRole::User, QStringLiteral("开始")));
  messages.append(makeMessage(MessageRole::Assistant, QStringLiteral("好的")));
  messages.append(makeToolMessage(QStringLiteral("call_1"), QStringLiteral("Bash"), huge));
  messages.append(makeMessage(MessageRole::User, QStringLiteral("继续")));
  // 最近的一轮也带工具输出，且它落在保护窗口内：必须原样保留。
  messages.append(makeToolMessage(QStringLiteral("call_2"), QStringLiteral("Bash"), huge));

  const QList<Message> projected = microcompactedMessages(messages, policy);

  QCOMPARE(projected.size(), messages.size());   // 不增删消息
  // 旧工具输出被换成裁剪标记。
  QVERIFY(!projected.at(2).toolParts().first().output.contains(QStringLiteral("xxxx")));
  QVERIFY(projected.at(2).toolParts().first().output.contains(QStringLiteral("裁剪")));
  // 标记里带上原名与原始体积，模型才知道"这里原本有东西"。
  QVERIFY(projected.at(2).toolParts().first().output.contains(QStringLiteral("Bash")));
  QVERIFY(projected.at(2).toolParts().first().output.contains(QStringLiteral("20000")));

  // 本地消息**不受影响**：工具卡片要继续显示完整输出。
  QCOMPARE(messages.at(2).toolParts().first().output, huge);

  // 保护窗口内的那条原样下发。
  QCOMPARE(projected.at(4).toolParts().first().output, huge);

  // 工具调用的身份信息必须原样保留，否则 provider 无法把结果配回调用。
  QCOMPARE(projected.at(2).toolParts().first().callId, QStringLiteral("call_1"));
  QCOMPARE(projected.at(2).toolParts().first().name, QStringLiteral("Bash"));
}

void TestContextCompactor::microcompactKeepsRecentToolOutput()
{
  CompactionPolicy policy;
  policy.keepRecentContextChars = 100000;   // 保护窗口覆盖全部消息
  policy.toolOutputBudgetChars = 1000;

  const QString huge = QString(20000, QLatin1Char('y'));
  QList<Message> messages;
  messages.append(makeToolMessage(QStringLiteral("call_1"), QStringLiteral("Bash"), huge));

  const QList<Message> projected = microcompactedMessages(messages, policy);
  QCOMPARE(projected.at(0).toolParts().first().output, huge);
}

void TestContextCompactor::microcompactLeavesShortOutputsAlone()
{
  CompactionPolicy policy;
  policy.keepRecentContextChars = 1;
  policy.toolOutputBudgetChars = 1000;

  const QString shortOutput = QStringLiteral("ok");
  QList<Message> messages;
  messages.append(makeMessage(MessageRole::User, QStringLiteral("开始")));
  messages.append(makeToolMessage(QStringLiteral("call_1"), QStringLiteral("Bash"), shortOutput));

  const QList<Message> projected = microcompactedMessages(messages, policy);
  // 本来就小，裁了省不下什么，保留原样更有用。
  QCOMPARE(projected.at(1).toolParts().first().output, shortOutput);
}

void TestContextCompactor::microcompactKeepsToolCallPairing()
{
  CompactionPolicy policy;
  policy.keepRecentContextChars = 1;
  policy.toolOutputBudgetChars = 10;

  // assistant 发起工具调用 → user 消息带回结果。两边的 callId 必须一致。
  Message assistant = makeMessage(MessageRole::Assistant, QStringLiteral("我查一下"));
  Part call = Part::makeTool(QStringLiteral("Read"), QStringLiteral("call_9"));
  call.tool.state = ToolState::Success;
  call.tool.output = QString(500, QLatin1Char('z'));
  assistant.parts.append(call);

  QList<Message> messages;
  messages.append(assistant);
  messages.append(makeToolMessage(QStringLiteral("call_9"), QStringLiteral("Read"),
                                  QString(500, QLatin1Char('w'))));

  const QList<Message> projected = microcompactedMessages(messages, policy);
  QCOMPARE(projected.size(), 2);
  // assistant 侧的 tool_use 原样保留（它的 output 本来就为空）。
  QCOMPARE(projected.at(0).toolParts().first().callId, QStringLiteral("call_9"));
  QVERIFY(projected.at(0).toolParts().first().name == QStringLiteral("Read"));
  // 结果侧只换 output，callId 不变——配对被保住。
  QCOMPARE(projected.at(1).toolParts().first().callId, QStringLiteral("call_9"));
}

void TestContextCompactor::fullCompactionCutsOnUserBoundary()
{
  CompactionPolicy policy;
  policy.keepTailMessages = 4;
  policy.minCompactionMessages = 4;

  const QList<Message> messages = makeConversation(6, QString(200, QLatin1Char('m')));
  const FullCompaction plan = buildFullCompaction(messages, policy);

  QVERIFY(plan.applied);
  // 裁剪点必须落在 user 消息上（此处即 u4），否则会拆开 assistant/tool 配对。
  QCOMPARE(plan.tailMessages.first().role, MessageRole::User);
  QCOMPARE(plan.headMessages.size(), plan.replacedMessageCount);
  QCOMPARE(plan.headMessages.size() + plan.tailMessages.size(), messages.size());
  QVERIFY(plan.tailMessages.size() >= policy.keepTailMessages);
  QVERIFY(!plan.transcript.trimmed().isEmpty());
}

void TestContextCompactor::fullCompactionKeepsCurrentUserMessage()
{
  CompactionPolicy policy;
  policy.keepTailMessages = 2;
  policy.minCompactionMessages = 4;

  // 最后一条是用户刚说的话——压缩完模型必须还看得到它。
  QList<Message> messages = makeConversation(5);
  messages.append(makeMessage(MessageRole::User, QStringLiteral("这句话不能被压掉")));
  const FullCompaction plan = buildFullCompaction(messages, policy);

  QVERIFY(plan.applied);
  QVERIFY(plan.tailMessages.last().plainText().contains(QStringLiteral("这句话不能被压掉")));
  // 摘要素材里不能包含它（否则等于压缩后又把同一句话喂了一遍）。
  QVERIFY(!plan.transcript.contains(QStringLiteral("这句话不能被压掉")));
}

void TestContextCompactor::fullCompactionRefusesShortHistory()
{
  CompactionPolicy policy;
  policy.minCompactionMessages = 8;

  const QList<Message> messages = makeConversation(2);
  const FullCompaction plan = buildFullCompaction(messages, policy);

  // 太短：摘要未必比原文便宜，不值得花这次调用。
  QVERIFY(!plan.applied);
  QVERIFY(plan.headMessages.isEmpty());
  QVERIFY(plan.tailMessages.isEmpty());
}

void TestContextCompactor::fullCompactionArchivesHeadVerbatim()
{
  CompactionPolicy policy;
  policy.keepTailMessages = 2;
  policy.minCompactionMessages = 4;

  const QList<Message> messages = makeConversation(5, QString(50, QLatin1Char('q')));
  const FullCompaction plan = buildFullCompaction(messages, policy);
  QVERIFY(plan.applied);

  // 归档的必须是原文（压缩只是把它移出活动历史，不是删除）。
  QVERIFY(plan.headMessages.first().plainText().contains(QStringLiteral("q-u0")));
  QVERIFY(plan.transcript.contains(QStringLiteral("q-u0")));
}

void TestContextCompactor::summaryMessageRoundTripsThroughJson()
{
  const QString summary = QStringLiteral("1. 目标：修好构建\n2. 改动：src/main.cpp");
  const Message summaryMessage =
    makeCompactionSummaryMessage(QStringLiteral("session_x"), summary, QStringLiteral("m1"));

  // 会下发给 provider：必须是 User 角色（System 会被两家 provider 跳过）。
  QCOMPARE(summaryMessage.role, MessageRole::User);
  QVERIFY(summaryMessage.modelOnly);
  QVERIFY(summaryMessage.plainText().contains(summary));

  // 元数据要能穿过持久化，否则重开会话就认不回摘要了。
  const Message restored = Message::fromJson(summaryMessage.toJson());
  QCOMPARE(restored.plainText(), summaryMessage.plainText());
  QVERIFY(restored.modelOnly);
  QCOMPARE(restoreContextSummary({restored}), summary);
}

void TestContextCompactor::markerMessageIsLocalOnly()
{
  const Message marker = makeCompactionMarkerMessage(QStringLiteral("session_x"),
                                                     QStringLiteral("摘要正文"), 7);

  // System 角色：不下发给 provider（否则等于给模型看一条它不该看的注解）。
  QCOMPARE(marker.role, MessageRole::System);
  QCOMPARE(marker.parts.size(), 1);
  QCOMPARE(marker.parts.first().kind, PartKind::Timeline);
  QCOMPARE(marker.parts.first().timeline.kind, TimelineKind::ContextCompaction);
  QVERIFY(marker.parts.first().timeline.summary.contains(QStringLiteral("7")));
  // 分隔行不该被当成压缩摘要本身（否则 restoreContextSummary 会认错）。
  QVERIFY(restoreContextSummary({marker}).isEmpty());
}

void TestContextCompactor::transcriptKeepsToolNames()
{
  QList<Message> messages;
  messages.append(makeMessage(MessageRole::User, QStringLiteral("看看这个文件")));
  messages.append(makeToolMessage(QStringLiteral("call_1"), QStringLiteral("Read"),
                                  QStringLiteral("文件内容")));

  const QString transcript = renderTranscriptForSummary(messages, 100000);
  QVERIFY(transcript.contains(QStringLiteral("用户")));
  QVERIFY(transcript.contains(QStringLiteral("看看这个文件")));
  // 工具名要保留：摘要里"改了哪个文件"这类信息全靠它。
  QVERIFY(transcript.contains(QStringLiteral("Read")));
  QVERIFY(transcript.contains(QStringLiteral("文件内容")));

  // 超长时保留尾部而不是头部。
  const QString clipped = renderTranscriptForSummary(messages, 10);
  QVERIFY(clipped.startsWith(QStringLiteral("…")));
  QVERIFY(clipped.size() < transcript.size());
}

QTEST_MAIN(TestContextCompactor)
#include "test_context_compactor.moc"
