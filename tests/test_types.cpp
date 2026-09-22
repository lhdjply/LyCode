// ZCode Qt — 领域模型单元测试
//
// 重点验证：
//   1. 枚举转换的双向一致性（一旦漂移，持久化与 UI 渲染会出现难查的错位）
//   2. JSON 往返不丢字段
//   3. 未知枚举值降级而不是崩溃 / 丢数据
//   4. 策略函数（权限模式、会话模式、工具状态）的边界
#include <QtTest>

#include "core/Ids.h"
#include "core/Json.h"
#include "core/Types.h"

using namespace zcode;

class TestTypes : public QObject {
    Q_OBJECT

private slots:
    void idGenerationIsPrefixedAndUnique();
    void enumRoundTrips_data();
    void enumRoundTrips();
    void unknownTokenFallsBackWithoutLoss();
    void partJsonRoundTrip();
    void toolPartJsonRoundTrip();
    void timelinePartKeepsUnknownRawType();
    void messageJsonRoundTrip();
    void sessionJsonRoundTrip();
    void jsonHelpersFallBackOnTypeMismatch();
    void toolStateClassification();
    void sessionModePolicy();
    void usageAccumulates();
    void modelSelectionDisplayRoundTrip();
    void permissionDefaultOption();
    void workspaceIdentityKey();
    void modelOptionOverrideRoundTrip();
    void modelOptionOverrideAppliesToModelInfo();
};

void TestTypes::idGenerationIsPrefixedAndUnique() {
    const QString first = newSessionId();
    const QString second = newSessionId();

    QVERIFY(first.startsWith(QStringLiteral("session_")));
    QCOMPARE(first.size(), QStringLiteral("session_").size() + 24);
    QVERIFY(first != second);
}

void TestTypes::enumRoundTrips_data() {
    QTest::addColumn<int>("group");
    QTest::addColumn<int>("value");

    // group: 0=role 1=status 2=part 3=tool 4=sessionStatus 5=sessionMode
    //        6=sessionKind 7=permissionKind 8=risk 9=permissionMode
    //        10=permissionDecision 11=permissionOptionKind 12=provider
    //        13=timelineKind 14=ruleBehavior 15=availability 16=unavailableReason
    QTest::newRow("role-user") << 0 << static_cast<int>(MessageRole::User);
    QTest::newRow("role-assistant") << 0 << static_cast<int>(MessageRole::Assistant);
    QTest::newRow("role-system") << 0 << static_cast<int>(MessageRole::System);

    QTest::newRow("status-pending") << 1 << static_cast<int>(MessageStatus::Pending);
    QTest::newRow("status-streaming") << 1 << static_cast<int>(MessageStatus::Streaming);
    QTest::newRow("status-complete") << 1 << static_cast<int>(MessageStatus::Complete);
    QTest::newRow("status-interrupted") << 1 << static_cast<int>(MessageStatus::Interrupted);
    QTest::newRow("status-failed") << 1 << static_cast<int>(MessageStatus::Failed);

    QTest::newRow("part-text") << 2 << static_cast<int>(PartKind::Text);
    QTest::newRow("part-reasoning") << 2 << static_cast<int>(PartKind::Reasoning);
    QTest::newRow("part-tool") << 2 << static_cast<int>(PartKind::Tool);
    QTest::newRow("part-file") << 2 << static_cast<int>(PartKind::File);
    QTest::newRow("part-artifact") << 2 << static_cast<int>(PartKind::Artifact);
    QTest::newRow("part-subagent") << 2 << static_cast<int>(PartKind::Subagent);
    QTest::newRow("part-timeline") << 2 << static_cast<int>(PartKind::Timeline);
    QTest::newRow("part-step") << 2 << static_cast<int>(PartKind::Step);

    QTest::newRow("tool-input-streaming") << 3 << static_cast<int>(ToolState::InputStreaming);
    QTest::newRow("tool-pending-approval") << 3 << static_cast<int>(ToolState::PendingApproval);
    QTest::newRow("tool-running") << 3 << static_cast<int>(ToolState::Running);
    QTest::newRow("tool-success") << 3 << static_cast<int>(ToolState::Success);
    QTest::newRow("tool-error") << 3 << static_cast<int>(ToolState::Error);
    QTest::newRow("tool-cancelled") << 3 << static_cast<int>(ToolState::Cancelled);

    QTest::newRow("session-draft") << 4 << static_cast<int>(SessionStatus::Draft);
    QTest::newRow("session-prewarming") << 4 << static_cast<int>(SessionStatus::Prewarming);
    QTest::newRow("session-running") << 4 << static_cast<int>(SessionStatus::Running);
    QTest::newRow("session-completed-success") << 4
                                               << static_cast<int>(SessionStatus::CompletedSuccess);
    QTest::newRow("session-completed-interrupted")
        << 4 << static_cast<int>(SessionStatus::CompletedInterrupted);
    QTest::newRow("session-error") << 4 << static_cast<int>(SessionStatus::Error);

    QTest::newRow("mode-plan") << 5 << static_cast<int>(SessionMode::Plan);
    QTest::newRow("mode-build") << 5 << static_cast<int>(SessionMode::Build);
    QTest::newRow("mode-edit") << 5 << static_cast<int>(SessionMode::Edit);
    QTest::newRow("mode-yolo") << 5 << static_cast<int>(SessionMode::Yolo);
    QTest::newRow("mode-auto") << 5 << static_cast<int>(SessionMode::Auto);

    QTest::newRow("kind-interactive") << 6 << static_cast<int>(SessionKind::Interactive);
    QTest::newRow("kind-fork") << 6 << static_cast<int>(SessionKind::Fork);
    QTest::newRow("kind-side-chat") << 6 << static_cast<int>(SessionKind::SelectionSideChat);
    QTest::newRow("kind-workflow-parent") << 6 << static_cast<int>(SessionKind::WorkflowParent);
    QTest::newRow("kind-workflow-child") << 6 << static_cast<int>(SessionKind::WorkflowChild);
    QTest::newRow("kind-subagent-child") << 6 << static_cast<int>(SessionKind::SubagentChild);
    QTest::newRow("kind-nested-workflow-child")
        << 6 << static_cast<int>(SessionKind::NestedWorkflowChild);

    QTest::newRow("perm-kind-read") << 7 << static_cast<int>(PermissionKind::Read);
    QTest::newRow("perm-kind-write") << 7 << static_cast<int>(PermissionKind::Write);
    QTest::newRow("perm-kind-execute") << 7 << static_cast<int>(PermissionKind::Execute);
    QTest::newRow("perm-kind-network") << 7 << static_cast<int>(PermissionKind::Network);
    QTest::newRow("perm-kind-other") << 7 << static_cast<int>(PermissionKind::Other);

    QTest::newRow("risk-low") << 8 << static_cast<int>(RiskLevel::Low);
    QTest::newRow("risk-medium") << 8 << static_cast<int>(RiskLevel::Medium);
    QTest::newRow("risk-high") << 8 << static_cast<int>(RiskLevel::High);
    QTest::newRow("risk-critical") << 8 << static_cast<int>(RiskLevel::Critical);

    QTest::newRow("perm-mode-default") << 9 << static_cast<int>(PermissionMode::Default);
    QTest::newRow("perm-mode-accept-edits") << 9 << static_cast<int>(PermissionMode::AcceptEdits);
    QTest::newRow("perm-mode-plan") << 9 << static_cast<int>(PermissionMode::Plan);
    QTest::newRow("perm-mode-bypass") << 9
                                      << static_cast<int>(PermissionMode::BypassPermissions);

    QTest::newRow("decision-allow") << 10 << static_cast<int>(PermissionDecision::Allow);
    QTest::newRow("decision-deny") << 10 << static_cast<int>(PermissionDecision::Deny);
    QTest::newRow("decision-escalate") << 10 << static_cast<int>(PermissionDecision::Escalate);
    QTest::newRow("decision-modify") << 10 << static_cast<int>(PermissionDecision::Modify);

    QTest::newRow("option-allow-once") << 11 << static_cast<int>(PermissionOptionKind::AllowOnce);
    QTest::newRow("option-allow-always")
        << 11 << static_cast<int>(PermissionOptionKind::AllowAlways);
    QTest::newRow("option-deny") << 11 << static_cast<int>(PermissionOptionKind::Deny);
    QTest::newRow("option-custom") << 11 << static_cast<int>(PermissionOptionKind::Custom);

    QTest::newRow("provider-anthropic") << 12 << static_cast<int>(ProviderKind::Anthropic);
    QTest::newRow("provider-openai") << 12
                                     << static_cast<int>(ProviderKind::OpenAICompatible);

    QTest::newRow("timeline-compaction")
        << 13 << static_cast<int>(TimelineKind::ContextCompaction);
    QTest::newRow("timeline-goal") << 13 << static_cast<int>(TimelineKind::GoalVerification);
    QTest::newRow("timeline-fork") << 13 << static_cast<int>(TimelineKind::SessionFork);
    QTest::newRow("timeline-model-change") << 13 << static_cast<int>(TimelineKind::ModelChange);
    QTest::newRow("timeline-retry") << 13 << static_cast<int>(TimelineKind::Retry);
    QTest::newRow("timeline-checkpoint")
        << 13 << static_cast<int>(TimelineKind::CheckpointRestored);

    QTest::newRow("rule-allow") << 14 << static_cast<int>(PermissionRuleBehavior::Allow);
    QTest::newRow("rule-deny") << 14 << static_cast<int>(PermissionRuleBehavior::Deny);
    QTest::newRow("rule-ask") << 14 << static_cast<int>(PermissionRuleBehavior::Ask);

    QTest::newRow("avail-available") << 15 << static_cast<int>(AccountAvailability::Available);
    QTest::newRow("avail-pending") << 15 << static_cast<int>(AccountAvailability::Pending);
    QTest::newRow("avail-unavailable") << 15 << static_cast<int>(AccountAvailability::Unavailable);
    QTest::newRow("avail-unknown") << 15 << static_cast<int>(AccountAvailability::Unknown);

    QTest::newRow("reason-none") << 16 << static_cast<int>(AccountUnavailableReason::None);
    QTest::newRow("reason-not-authenticated")
        << 16 << static_cast<int>(AccountUnavailableReason::NotAuthenticated);
    QTest::newRow("reason-not-connected")
        << 16 << static_cast<int>(AccountUnavailableReason::NotConnected);
    QTest::newRow("reason-credential-failed")
        << 16 << static_cast<int>(AccountUnavailableReason::CredentialFailed);
    QTest::newRow("reason-not-entitled")
        << 16 << static_cast<int>(AccountUnavailableReason::NotEntitled);
}

void TestTypes::enumRoundTrips() {
    QFETCH(int, group);
    QFETCH(int, value);

    switch (group) {
        case 0: {
            const auto typed = static_cast<MessageRole>(value);
            QCOMPARE(messageRoleFromToken(toToken(typed)), typed);
            break;
        }
        case 1: {
            const auto typed = static_cast<MessageStatus>(value);
            QCOMPARE(messageStatusFromToken(toToken(typed)), typed);
            break;
        }
        case 2: {
            const auto typed = static_cast<PartKind>(value);
            QCOMPARE(partKindFromToken(toToken(typed)), typed);
            break;
        }
        case 3: {
            const auto typed = static_cast<ToolState>(value);
            QCOMPARE(toolStateFromToken(toToken(typed)), typed);
            break;
        }
        case 4: {
            const auto typed = static_cast<SessionStatus>(value);
            QCOMPARE(sessionStatusFromToken(toToken(typed)), typed);
            break;
        }
        case 5: {
            const auto typed = static_cast<SessionMode>(value);
            QCOMPARE(sessionModeFromToken(toToken(typed)), typed);
            break;
        }
        case 6: {
            const auto typed = static_cast<SessionKind>(value);
            QCOMPARE(sessionKindFromToken(toToken(typed)), typed);
            break;
        }
        case 7: {
            const auto typed = static_cast<PermissionKind>(value);
            QCOMPARE(permissionKindFromToken(toToken(typed)), typed);
            break;
        }
        case 8: {
            const auto typed = static_cast<RiskLevel>(value);
            QCOMPARE(riskLevelFromToken(toToken(typed)), typed);
            break;
        }
        case 9: {
            const auto typed = static_cast<PermissionMode>(value);
            QCOMPARE(permissionModeFromToken(toToken(typed)), typed);
            break;
        }
        case 10: {
            const auto typed = static_cast<PermissionDecision>(value);
            QCOMPARE(permissionDecisionFromToken(toToken(typed)), typed);
            break;
        }
        case 11: {
            const auto typed = static_cast<PermissionOptionKind>(value);
            QCOMPARE(permissionOptionKindFromToken(toToken(typed)), typed);
            break;
        }
        case 12: {
            const auto typed = static_cast<ProviderKind>(value);
            QCOMPARE(providerKindFromToken(toToken(typed)), typed);
            break;
        }
        case 13: {
            const auto typed = static_cast<TimelineKind>(value);
            QCOMPARE(timelineKindFromToken(toToken(typed)), typed);
            break;
        }
        case 14: {
            const auto typed = static_cast<PermissionRuleBehavior>(value);
            QCOMPARE(permissionRuleBehaviorFromToken(toToken(typed)), typed);
            break;
        }
        case 15: {
            const auto typed = static_cast<AccountAvailability>(value);
            QCOMPARE(accountAvailabilityFromToken(toToken(typed)), typed);
            break;
        }
        case 16: {
            const auto typed = static_cast<AccountUnavailableReason>(value);
            QCOMPARE(accountUnavailableReasonFromToken(toToken(typed)), typed);
            break;
        }
        default:
            QFAIL("未知的测试分组");
    }
}

void TestTypes::unknownTokenFallsBackWithoutLoss() {
    // 未知枚举字面量必须降级到安全默认值，而不是抛异常。
    QCOMPARE(toolStateFromToken(QStringLiteral("someFutureState")), ToolState::InputStreaming);
    QCOMPARE(sessionModeFromToken(QStringLiteral("turbo")), SessionMode::Build);
    QCOMPARE(permissionDecisionFromToken(QStringLiteral("maybe")), PermissionDecision::Deny);

    // 空字符串不触发告警路径，直接取默认。
    QCOMPARE(riskLevelFromToken(QString()), RiskLevel::Medium);
}

void TestTypes::partJsonRoundTrip() {
    const Part text = Part::makeText(QStringLiteral("hello"));
    QCOMPARE(Part::fromJson(text.toJson()).text.text, QStringLiteral("hello"));

    const Part reasoning = Part::makeReasoning(QStringLiteral("thinking"));
    const Part restoredReasoning = Part::fromJson(reasoning.toJson());
    QCOMPARE(restoredReasoning.kind, PartKind::Reasoning);
    QCOMPARE(restoredReasoning.reasoning.text, QStringLiteral("thinking"));

    const Part step = Part::makeStep(2, QStringLiteral("Step 2"));
    const Part restoredStep = Part::fromJson(step.toJson());
    QCOMPARE(restoredStep.kind, PartKind::Step);
    QCOMPARE(restoredStep.step.index, 2);
    QCOMPARE(restoredStep.step.title, QStringLiteral("Step 2"));

    // 图片不单独建类型，走 FilePart + mime 判别。
    Part image;
    image.id = newPartId();
    image.kind = PartKind::File;
    image.file.mimeType = QStringLiteral("image/png");
    image.file.base64 = QStringLiteral("AAAA");
    QVERIFY(image.file.isImage());
    const Part restoredImage = Part::fromJson(image.toJson());
    QVERIFY(restoredImage.file.isImage());
    QCOMPARE(restoredImage.file.base64, QStringLiteral("AAAA"));
}

void TestTypes::toolPartJsonRoundTrip() {
    Part tool = Part::makeTool(QStringLiteral("Bash"), QStringLiteral("call_1"));
    tool.tool.state = ToolState::Running;
    tool.tool.input.insert(QStringLiteral("command"), QStringLiteral("ls"));
    tool.tool.title = QStringLiteral("Bash: ls");
    tool.tool.startedAtMs = 1000;
    tool.tool.endedAtMs = 1500;
    tool.tool.progress.insert(QStringLiteral("bytes"), 42);

    const Part restored = Part::fromJson(tool.toJson());
    QCOMPARE(restored.kind, PartKind::Tool);
    QCOMPARE(restored.tool.name, QStringLiteral("Bash"));
    QCOMPARE(restored.tool.callId, QStringLiteral("call_1"));
    QCOMPARE(restored.tool.state, ToolState::Running);
    QCOMPARE(restored.tool.input.value(QStringLiteral("command")).toString(), QStringLiteral("ls"));
    QCOMPARE(restored.tool.title, QStringLiteral("Bash: ls"));
    QCOMPARE(restored.tool.durationMs(), 500LL);
    QCOMPARE(restored.tool.progress.value(QStringLiteral("bytes")).toInt(), 42);
}

void TestTypes::timelinePartKeepsUnknownRawType() {
    // 未知的时间线类型必须保留原始字面量，不能静默丢失信息。
    QJsonObject timelinePayload;
    timelinePayload.insert(QStringLiteral("timelineType"), QStringLiteral("future_kind"));
    timelinePayload.insert(QStringLiteral("display"), QStringLiteral("separator"));
    timelinePayload.insert(QStringLiteral("summary"), QStringLiteral("something happened"));

    QJsonObject partJson;
    partJson.insert(QStringLiteral("id"), QStringLiteral("part_x"));
    partJson.insert(QStringLiteral("kind"), QStringLiteral("timeline"));
    partJson.insert(QStringLiteral("timeline"), timelinePayload);

    const Part part = Part::fromJson(partJson);
    QCOMPARE(part.kind, PartKind::Timeline);
    QCOMPARE(part.timeline.kind, TimelineKind::Unknown);
    QCOMPARE(part.timeline.rawType, QStringLiteral("future_kind"));

    // 再序列化时 rawType 必须原样写出。
    const QJsonObject roundTripped =
        part.toJson().value(QStringLiteral("timeline")).toObject();
    QCOMPARE(roundTripped.value(QStringLiteral("timelineType")).toString(),
             QStringLiteral("future_kind"));
}

void TestTypes::messageJsonRoundTrip() {
    Message message;
    message.id = newMessageId();
    message.sessionId = newSessionId();
    message.role = MessageRole::Assistant;
    message.status = MessageStatus::Streaming;
    message.parts.append(Part::makeReasoning(QStringLiteral("thinking")));
    message.parts.append(Part::makeText(QStringLiteral("hi")));
    message.parts.append(Part::makeTool(QStringLiteral("Read"), QString()));
    message.modelId = QStringLiteral("test-model");
    message.createdAtMs = 1700000000000LL;
    message.usage.inputTokens = 100;
    message.usage.outputTokens = 20;
    message.parentMessageId = QStringLiteral("msg_parent");

    const Message restored = Message::fromJson(message.toJson());
    QCOMPARE(restored.id, message.id);
    QCOMPARE(restored.role, MessageRole::Assistant);
    QCOMPARE(restored.status, MessageStatus::Streaming);
    QCOMPARE(restored.parts.size(), 3);
    QCOMPARE(restored.parts.at(1).text.text, QStringLiteral("hi"));
    QCOMPARE(restored.modelId, QStringLiteral("test-model"));
    QCOMPARE(restored.createdAtMs, 1700000000000LL);
    QCOMPARE(restored.plainText(), QStringLiteral("hi"));
    QCOMPARE(restored.toolParts().size(), 1);
    QCOMPARE(restored.usage.inputTokens, 100);
    QCOMPARE(restored.parentMessageId, QStringLiteral("msg_parent"));
    QVERIFY(!restored.isTerminal());
}

void TestTypes::sessionJsonRoundTrip() {
    Session session;
    session.id = newSessionId();
    session.title = QStringLiteral("修复构建");
    session.workspace.path = QStringLiteral("/tmp/project");
    session.workspace.identity = QStringLiteral("identity-1");
    session.mode = SessionMode::Plan;
    session.status = SessionStatus::Running;
    session.kind = SessionKind::SubagentChild;
    session.parentSessionId = QStringLiteral("session_parent");
    session.pendingPermissionCount = 2;

    const Session restored = Session::fromJson(session.toJson());
    QCOMPARE(restored.id, session.id);
    QCOMPARE(restored.title, session.title);
    QCOMPARE(restored.workspace.path, session.workspace.path);
    QCOMPARE(restored.workspace.identity, session.workspace.identity);
    QCOMPARE(restored.mode, SessionMode::Plan);
    QCOMPARE(restored.status, SessionStatus::Running);
    QCOMPARE(restored.kind, SessionKind::SubagentChild);
    QCOMPARE(restored.parentSessionId, QStringLiteral("session_parent"));
    QVERIFY(restored.isSubagent());
    QVERIFY(restored.hasParent());
    QVERIFY(restored.isWaitingOnUser());
}

void TestTypes::jsonHelpersFallBackOnTypeMismatch() {
    QJsonObject obj;
    obj.insert(QStringLiteral("number"), QStringLiteral("not-a-number"));
    obj.insert(QStringLiteral("text"), 42);
    obj.insert(QStringLiteral("flag"), QStringLiteral("yes"));

    // 类型不符必须回退到默认值，而不是抛异常或返回垃圾数据。
    QCOMPARE(json::integer(obj, QStringLiteral("number"), -1), -1);
    QCOMPARE(json::str(obj, QStringLiteral("text"), QStringLiteral("fallback")),
             QStringLiteral("fallback"));
    QCOMPARE(json::boolean(obj, QStringLiteral("flag"), true), true);

    // 缺失字段同样回退。
    QCOMPARE(json::integer(obj, QStringLiteral("missing"), 7), 7);
    QVERIFY(!json::has(obj, QStringLiteral("missing")));

    // 截断保留标记，且不超长。
    const QString truncated = json::truncate(QString(100, QLatin1Char('a')), 50);
    QVERIFY(truncated.size() <= 50);
    QVERIFY(truncated.endsWith(QStringLiteral("[truncated]")));
}

void TestTypes::toolStateClassification() {
    QVERIFY(toolStateIsActive(ToolState::InputStreaming));
    QVERIFY(toolStateIsActive(ToolState::PendingApproval));
    QVERIFY(toolStateIsActive(ToolState::Running));
    QVERIFY(!toolStateIsActive(ToolState::Success));

    QVERIFY(toolStateIsTerminal(ToolState::Success));
    QVERIFY(toolStateIsTerminal(ToolState::Error));
    QVERIFY(toolStateIsTerminal(ToolState::Cancelled));
    QVERIFY(!toolStateIsTerminal(ToolState::PendingApproval));

    // 中断时必须把未终态的工具全部关闭，否则 UI 永远显示"运行中"。
    Message message;
    message.parts.append(Part::makeTool(QStringLiteral("Bash"), QStringLiteral("call_a")));
    message.parts.append(Part::makeTool(QStringLiteral("Read"), QStringLiteral("call_b")));
    message.parts[0].tool.state = ToolState::Running;
    message.parts[1].tool.state = ToolState::Success;
    message.cancelPendingTools();

    QCOMPARE(message.parts.at(0).tool.state, ToolState::Cancelled);
    QVERIFY(message.parts.at(0).tool.endedAtMs > 0);
    // 已终态的工具不能被改写。
    QCOMPARE(message.parts.at(1).tool.state, ToolState::Success);
}

void TestTypes::sessionModePolicy() {
    // Plan 是唯一只读模式。
    QVERIFY(!sessionModeAllowsWrites(SessionMode::Plan));
    QVERIFY(sessionModeAllowsWrites(SessionMode::Build));
    QVERIFY(sessionModeAllowsWrites(SessionMode::Edit));
    QVERIFY(sessionModeAllowsWrites(SessionMode::Yolo));

    // 模式到权限模式的默认映射。
    QCOMPARE(defaultPermissionModeFor(SessionMode::Plan), PermissionMode::Plan);
    QCOMPARE(defaultPermissionModeFor(SessionMode::Build), PermissionMode::Default);
    QCOMPARE(defaultPermissionModeFor(SessionMode::Edit), PermissionMode::AcceptEdits);
    QCOMPARE(defaultPermissionModeFor(SessionMode::Yolo), PermissionMode::BypassPermissions);

    // Default：只读放行，其余都要问。
    QVERIFY(!permissionModeRequiresPrompt(PermissionMode::Default, PermissionKind::Read));
    QVERIFY(permissionModeRequiresPrompt(PermissionMode::Default, PermissionKind::Write));
    QVERIFY(permissionModeRequiresPrompt(PermissionMode::Default, PermissionKind::Execute));

    // AcceptEdits：额外放行写入，执行仍要问。
    QVERIFY(!permissionModeRequiresPrompt(PermissionMode::AcceptEdits, PermissionKind::Write));
    QVERIFY(permissionModeRequiresPrompt(PermissionMode::AcceptEdits, PermissionKind::Execute));

    // Bypass：全部放行。
    QVERIFY(!permissionModeRequiresPrompt(PermissionMode::BypassPermissions,
                                          PermissionKind::Execute));
}

void TestTypes::usageAccumulates() {
    Usage total;
    Usage first;
    first.inputTokens = 10;
    first.outputTokens = 5;
    first.cacheReadTokens = 3;

    Usage second;
    second.inputTokens = 20;
    second.outputTokens = 7;

    total += first;
    total += second;

    QCOMPARE(total.inputTokens, 30);
    QCOMPARE(total.outputTokens, 12);
    QCOMPARE(total.cacheReadTokens, 3);
    QCOMPARE(total.effectiveTotal(), 42);

    // provider 只给总量时以总量为准。
    Usage onlyTotal;
    onlyTotal.totalTokens = 99;
    QCOMPARE(onlyTotal.effectiveTotal(), 99);

    // JSON 往返覆盖 cache 子对象。
    const Usage restored = Usage::fromJson(total.toJson());
    QCOMPARE(restored.inputTokens, 30);
    QCOMPARE(restored.cacheReadTokens, 3);
}

void TestTypes::modelSelectionDisplayRoundTrip() {
    ModelSelection selection;
    selection.providerId = QStringLiteral("anthropic");
    selection.modelId = QStringLiteral("claude-sonnet-4");
    QCOMPARE(selection.displayValue(), QStringLiteral("anthropic/claude-sonnet-4"));

    selection.reasoningLevel = QStringLiteral("high");
    QCOMPARE(selection.displayValue(), QStringLiteral("anthropic/claude-sonnet-4$high"));

    const ModelSelection parsed = ModelSelection::parseDisplayValue(selection.displayValue());
    QCOMPARE(parsed.providerId, QStringLiteral("anthropic"));
    QCOMPARE(parsed.modelId, QStringLiteral("claude-sonnet-4"));
    QCOMPARE(parsed.reasoningLevel, QStringLiteral("high"));
    QVERIFY(parsed.isValid());

    // 形状不符时返回无效选择，而不是猜测。
    QVERIFY(!ModelSelection::parseDisplayValue(QStringLiteral("no-separator")).isValid());
    QVERIFY(!ModelSelection::parseDisplayValue(QString()).isValid());
    QVERIFY(!ModelSelection::parseDisplayValue(QStringLiteral("/leading")).isValid());
    QVERIFY(!ModelSelection::parseDisplayValue(QStringLiteral("trailing/")).isValid());
}

void TestTypes::permissionDefaultOption() {
    PermissionRequest request;

    // 没有选项时不得崩溃。
    QCOMPARE(request.defaultOption(), nullptr);

    PermissionOption deny;
    deny.optionId = QStringLiteral("deny");
    deny.kind = PermissionOptionKind::Deny;
    deny.response.decision = PermissionDecision::Deny;

    PermissionOption allowOnce;
    allowOnce.optionId = QStringLiteral("allowOnce");
    allowOnce.kind = PermissionOptionKind::AllowOnce;
    allowOnce.response.decision = PermissionDecision::Allow;

    request.options = {deny, allowOnce};
    // 默认选项应优先取允许项，而不是顺序上的第一项。
    QCOMPARE(request.defaultOption()->optionId, QStringLiteral("allowOnce"));

    request.options = {deny};
    QCOMPARE(request.defaultOption()->optionId, QStringLiteral("deny"));

    // 规则键必须区分行为，否则 allow 与 deny 规则会互相覆盖。
    PermissionRule allowRule;
    allowRule.toolName = QStringLiteral("Bash");
    allowRule.behavior = PermissionRuleBehavior::Allow;

    PermissionRule denyRule = allowRule;
    denyRule.behavior = PermissionRuleBehavior::Deny;
    QVERIFY(allowRule.key() != denyRule.key());
}

void TestTypes::workspaceIdentityKey() {
    Workspace workspace;
    workspace.path = QStringLiteral("/tmp/project");

    // identity 缺失或全空白时必须回退到路径。
    QCOMPARE(workspace.key(), QStringLiteral("/tmp/project"));
    workspace.identity = QStringLiteral("   ");
    QCOMPARE(workspace.key(), QStringLiteral("/tmp/project"));
    workspace.identity = QStringLiteral(" remote:host:/tmp/project ");
    QCOMPARE(workspace.key(), QStringLiteral("remote:host:/tmp/project"));
    QCOMPARE(workspace.displayName(), QStringLiteral("project"));
    QVERIFY(!workspace.isRemote());

    workspace.remoteSessionId = QStringLiteral("remote-1");
    QVERIFY(workspace.isRemote());

    // JSON 往返要带上派生的 workspaceKey。
    const QJsonObject serialized = workspace.toJson();
    QCOMPARE(serialized.value(QStringLiteral("workspaceKey")).toString(),
             QStringLiteral("remote:host:/tmp/project"));
}

void TestTypes::modelOptionOverrideRoundTrip() {
    ModelOptionOverride override;
    // 全空等价于"没有覆盖"，这样"留 0 表示用默认"的语义才成立。
    QVERIFY(override.isEmpty());
    QVERIFY(override.toJson().isEmpty());

    override.contextWindow = 200000;
    override.maxOutputTokens = 32768;
    override.reasoningLevels = {QStringLiteral("off"), QStringLiteral("high")};
    override.defaultReasoningLevel = QStringLiteral("high");
    QVERIFY(!override.isEmpty());

    const QJsonObject serialized = override.toJson();
    QCOMPARE(serialized.value(QStringLiteral("contextWindow")).toInt(), 200000);

    const ModelOptionOverride restored = ModelOptionOverride::fromJson(serialized);
    QCOMPARE(restored.contextWindow, 200000);
    QCOMPARE(restored.maxOutputTokens, 32768);
    QCOMPARE(restored.reasoningLevels.size(), 2);
    QCOMPARE(restored.defaultReasoningLevel, QStringLiteral("high"));

    // 键的格式必须与 ModelSelection::displayValue() 的前半段一致，
    // 否则设置页写入的键与运行时查询的键对不上。
    ModelSelection selection;
    selection.providerId = QStringLiteral("p1");
    selection.modelId = QStringLiteral("m1");
    QCOMPARE(modelOptionKey(selection.providerId, selection.modelId),
             QStringLiteral("p1/m1"));
    QVERIFY(selection.displayValue().startsWith(modelOptionKey(selection.providerId,
                                                              selection.modelId)));
}

void TestTypes::modelOptionOverrideAppliesToModelInfo() {
    ModelInfo info;
    info.providerId = QStringLiteral("p1");
    info.modelId = QStringLiteral("m1");
    info.contextWindow = 128000;
    info.maxOutputTokens = 8192;
    info.supportsReasoning = false;

    // 空覆盖不得改动任何字段。
    ModelOptionOverride empty;
    ModelInfo untouched = info;
    empty.applyTo(&untouched);
    QCOMPARE(untouched.contextWindow, 128000);
    QCOMPARE(untouched.maxOutputTokens, 8192);
    QVERIFY(!untouched.supportsReasoning);

    ModelOptionOverride override;
    override.contextWindow = 200000;
    override.maxOutputTokens = 32768;
    override.reasoningLevels = {QStringLiteral("off"), QStringLiteral("low")};

    override.applyTo(&info);
    QCOMPARE(info.contextWindow, 200000);
    QCOMPARE(info.maxOutputTokens, 32768);
    QCOMPARE(info.reasoningLevels.size(), 2);
    // 覆盖了档位列表就必须同步"是否支持思考"，否则 UI 会拿旧标志做判断。
    QVERIFY(info.supportsReasoning);
}

QTEST_MAIN(TestTypes)
#include "test_types.moc"
