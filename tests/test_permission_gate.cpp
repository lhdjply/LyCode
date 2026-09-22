// LyCode — 权限门单元测试
//
// 权限是唯一的安全边界，因此这里覆盖得比较密：每个模式 × 每种类别的默认行为、
// 规则匹配（含前缀与通配）、自动裁决与用户裁决的分界、以及取消/重复请求这类
// 容易造成状态机卡死的边界。
#include <QtTest>

#include "agent/PermissionGate.h"
#include "core/Ids.h"

using namespace lycode;

namespace {

/// 构造一个最小可用的权限请求。
PermissionRequest makeRequest(PermissionKind kind, const QString &toolName) {
    PermissionRequest request;
    request.id = newPermissionId();
    request.sessionId = QStringLiteral("session_test");
    request.callId = newToolCallId();
    request.toolName = toolName;
    request.kind = kind;
    request.riskLevel = RiskLevel::Medium;
    request.title = toolName;
    request.createdAtMs = nowMs();

    PermissionOption allow;
    allow.optionId = QStringLiteral("allowOnce");
    allow.kind = PermissionOptionKind::AllowOnce;
    allow.response.decision = PermissionDecision::Allow;
    request.options.append(allow);

    PermissionOption deny;
    deny.optionId = QStringLiteral("deny");
    deny.kind = PermissionOptionKind::Deny;
    deny.response.decision = PermissionDecision::Deny;
    request.options.append(deny);

    return request;
}

PermissionRule makeRule(const QString &tool, const QString &content, PermissionRuleBehavior behavior) {
    PermissionRule rule;
    rule.toolName = tool;
    rule.ruleContent = content;
    rule.behavior = behavior;
    return rule;
}

}  // namespace

class TestPermissionGate : public QObject {
    Q_OBJECT

private slots:
    void bypassModeAllowsWithoutPrompt();
    void readIsAutoAllowedInDefaultMode();
    void writePromptsInDefaultMode();
    void acceptEditsAllowsWriteButPromptsExecute();
    void planModeDeniesSideEffectsWithoutPrompt();
    void allowAlwaysRuleAppliesToLaterCalls();
    void denyRuleBlocks();
    void rulePrefixMatchesSubject();
    void ruleWithoutContentMatchesWholeCapability();
    void allowRuleWinsOverDenyRule();
    void cancelAllDeniesPendingRequests();
    void resolveUnknownRequestIsIdempotent();
    void duplicateRequestIdIsRejected();
    void resolveAppliesPermissionUpdates();
};

void TestPermissionGate::bypassModeAllowsWithoutPrompt() {
    PermissionGate gate;
    gate.setMode(PermissionMode::BypassPermissions);

    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);
    PermissionOutcome captured;
    bool called = false;

    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("rm -rf /tmp/x"),
                 [&](PermissionOutcome outcome) {
                     captured = outcome;
                     called = true;
                 });

    QVERIFY(called);
    QVERIFY(captured.automatic);
    QCOMPARE(captured.reasonCode, QStringLiteral("bypass_mode"));
    QVERIFY(captured.allowed());
    // 自动裁决不得打扰用户。
    QCOMPARE(requestedSpy.count(), 0);
}

void TestPermissionGate::readIsAutoAllowedInDefaultMode() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);

    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);
    PermissionOutcome captured;
    gate.request(makeRequest(PermissionKind::Read, QStringLiteral("Read")),
                 QStringLiteral("read"), QStringLiteral("/tmp/a.txt"),
                 [&](PermissionOutcome outcome) { captured = outcome; });

    QVERIFY(captured.allowed());
    QVERIFY(captured.automatic);
    QCOMPARE(captured.reasonCode, QStringLiteral("read_only"));
    QCOMPARE(requestedSpy.count(), 0);
}

void TestPermissionGate::writePromptsInDefaultMode() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);

    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);
    QSignalSpy resolvedSpy(&gate, &PermissionGate::resolved);

    bool called = false;
    PermissionOutcome captured;
    PermissionRequest request = makeRequest(PermissionKind::Write, QStringLiteral("Write"));

    gate.request(request, QStringLiteral("edit"), QStringLiteral("/tmp/a.txt"),
                 [&](PermissionOutcome outcome) {
                     captured = outcome;
                     called = true;
                 });

    // 必须弹窗，且回调此刻还不能被调用（否则调用方会在用户决定前继续执行）。
    QCOMPARE(requestedSpy.count(), 1);
    QVERIFY(!called);
    QVERIFY(gate.hasPending());
    QCOMPARE(gate.pendingCount(), 1);

    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    QVERIFY(gate.resolve(request.id, response));

    QVERIFY(called);
    QVERIFY(captured.allowed());
    QVERIFY(!captured.automatic);
    QCOMPARE(captured.reasonCode, QStringLiteral("user_choice"));
    QCOMPARE(resolvedSpy.count(), 1);
    QVERIFY(!gate.hasPending());
}

void TestPermissionGate::acceptEditsAllowsWriteButPromptsExecute() {
    PermissionGate gate;
    gate.setMode(PermissionMode::AcceptEdits);

    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);

    PermissionOutcome writeOutcome;
    gate.request(makeRequest(PermissionKind::Write, QStringLiteral("Edit")),
                 QStringLiteral("edit"), QStringLiteral("/tmp/a.txt"),
                 [&](PermissionOutcome outcome) { writeOutcome = outcome; });
    QVERIFY(writeOutcome.allowed());
    QVERIFY(writeOutcome.automatic);

    bool executeCalled = false;
    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("ls"),
                 [&](PermissionOutcome) { executeCalled = true; });

    // 执行类仍然需要确认。
    QCOMPARE(requestedSpy.count(), 1);
    QVERIFY(!executeCalled);
}

void TestPermissionGate::planModeDeniesSideEffectsWithoutPrompt() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Plan);

    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);

    PermissionOutcome captured;
    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("touch x"),
                 [&](PermissionOutcome outcome) { captured = outcome; });

    // plan 模式是模式的语义约束，不该让用户"批准"一次写入。
    QVERIFY(!captured.allowed());
    QVERIFY(captured.automatic);
    QCOMPARE(captured.reasonCode, QStringLiteral("plan_mode"));
    QCOMPARE(requestedSpy.count(), 0);

    // 只读调用在 plan 模式下仍然放行。
    PermissionOutcome readOutcome;
    gate.request(makeRequest(PermissionKind::Read, QStringLiteral("Read")),
                 QStringLiteral("read"), QStringLiteral("/tmp/a"),
                 [&](PermissionOutcome outcome) { readOutcome = outcome; });
    QVERIFY(readOutcome.allowed());
}

void TestPermissionGate::allowAlwaysRuleAppliesToLaterCalls() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);

    PermissionRequest first = makeRequest(PermissionKind::Execute, QStringLiteral("Bash"));
    bool firstCalled = false;
    gate.request(first, QStringLiteral("bash"), QStringLiteral("make test"),
                 [&](PermissionOutcome) { firstCalled = true; });
    QVERIFY(!firstCalled);

    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    response.permissionUpdates.append(
        makeRule(QStringLiteral("bash"), QStringLiteral("make test"), PermissionRuleBehavior::Allow));
    QVERIFY(gate.resolve(first.id, response));
    QVERIFY(firstCalled);
    QCOMPARE(gate.grantedRules().size(), 1);

    // 相同能力 + 相同前缀：不再打扰用户。
    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);
    PermissionOutcome second;
    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("make test -- --watch"),
                 [&](PermissionOutcome outcome) { second = outcome; });

    QVERIFY(second.allowed());
    QVERIFY(second.automatic);
    QCOMPARE(second.reasonCode, QStringLiteral("granted_rule"));
    QCOMPARE(requestedSpy.count(), 0);
}

void TestPermissionGate::denyRuleBlocks() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);
    gate.addRule(makeRule(QStringLiteral("bash"), QStringLiteral("rm -rf"),
                          PermissionRuleBehavior::Deny));

    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);
    PermissionOutcome captured;
    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("rm -rf /"),
                 [&](PermissionOutcome outcome) { captured = outcome; });

    QVERIFY(!captured.allowed());
    QVERIFY(captured.automatic);
    QCOMPARE(captured.reasonCode, QStringLiteral("denied_rule"));
    QCOMPARE(requestedSpy.count(), 0);
}

void TestPermissionGate::rulePrefixMatchesSubject() {
    QList<PermissionRule> rules;
    rules.append(makeRule(QStringLiteral("edit"), QStringLiteral("/home/u/project/src"),
                          PermissionRuleBehavior::Allow));

    QVERIFY(PermissionGate::matchRule(rules, QStringLiteral("edit"),
                                      QStringLiteral("/home/u/project/src/main.cpp")) != nullptr);
    // 前缀必须真正匹配前缀，不能是子串匹配。
    QVERIFY(PermissionGate::matchRule(rules, QStringLiteral("edit"),
                                      QStringLiteral("/home/u/project/other/main.cpp")) == nullptr);
    // 能力名不同就不匹配。
    QVERIFY(PermissionGate::matchRule(rules, QStringLiteral("bash"),
                                      QStringLiteral("/home/u/project/src/main.cpp")) == nullptr);
}

void TestPermissionGate::ruleWithoutContentMatchesWholeCapability() {
    QList<PermissionRule> rules;
    rules.append(makeRule(QStringLiteral("read"), QString(), PermissionRuleBehavior::Allow));

    QVERIFY(PermissionGate::matchRule(rules, QStringLiteral("read"),
                                      QStringLiteral("/anything/at/all")) != nullptr);
    QVERIFY(PermissionGate::matchRule(rules, QStringLiteral("bash"), QStringLiteral("ls")) ==
            nullptr);

    // `*` 与空内容等价。
    QList<PermissionRule> wildcard;
    wildcard.append(makeRule(QStringLiteral("bash"), QStringLiteral("*"),
                             PermissionRuleBehavior::Allow));
    QVERIFY(PermissionGate::matchRule(wildcard, QStringLiteral("bash"),
                                      QStringLiteral("anything")) != nullptr);

    // `prefix:*` 写法的尾部通配要被剥掉当作前缀。
    QList<PermissionRule> starPrefix;
    starPrefix.append(makeRule(QStringLiteral("bash"), QStringLiteral("make:*"),
                               PermissionRuleBehavior::Allow));
    QVERIFY(PermissionGate::matchRule(starPrefix, QStringLiteral("bash"),
                                      QStringLiteral("make install")) != nullptr);
    // ⚠ 关键是"前缀匹配要求命令**以此前缀开头**"：`make:*` 不该命中 `mymake install`。
    // （这条断言原本用另一个包管理器名来验——它不以该前缀开头，所以不匹配。
    //   换例子时必须保持"不以该前缀开头"这个性质；我先前换成 `maker install`，
    //   它恰好以 `make` 开头，于是断言反过来失败——测试当场抓住了这个错误。）
    QVERIFY(PermissionGate::matchRule(starPrefix, QStringLiteral("bash"),
                                      QStringLiteral("mymake install")) == nullptr);
}

void TestPermissionGate::allowRuleWinsOverDenyRule() {
    QList<PermissionRule> rules;
    // 先 deny 全部，再 allow 一条：这是用户最自然的操作顺序，必须按预期生效。
    rules.append(makeRule(QStringLiteral("bash"), QString(), PermissionRuleBehavior::Deny));
    rules.append(makeRule(QStringLiteral("bash"), QStringLiteral("make test"),
                          PermissionRuleBehavior::Allow));

    const PermissionRule *matched =
        PermissionGate::matchRule(rules, QStringLiteral("bash"), QStringLiteral("make test -- x"));
    QVERIFY(matched != nullptr);
    QCOMPARE(matched->behavior, PermissionRuleBehavior::Allow);

    // 没被 allow 覆盖的调用仍然命中 deny。
    const PermissionRule *denied =
        PermissionGate::matchRule(rules, QStringLiteral("bash"), QStringLiteral("rm -rf /"));
    QVERIFY(denied != nullptr);
    QCOMPARE(denied->behavior, PermissionRuleBehavior::Deny);
}

void TestPermissionGate::cancelAllDeniesPendingRequests() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);

    QList<PermissionOutcome> outcomes;
    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("cmd1"),
                 [&](PermissionOutcome outcome) { outcomes.append(outcome); });
    gate.request(makeRequest(PermissionKind::Execute, QStringLiteral("Bash")),
                 QStringLiteral("bash"), QStringLiteral("cmd2"),
                 [&](PermissionOutcome outcome) { outcomes.append(outcome); });

    QCOMPARE(gate.pendingCount(), 2);

    gate.cancelAll(QStringLiteral("用户中断了本次运行"));

    // 悬挂的回调会让状态机卡死，所以取消必须回调且必须恰好一次。
    QCOMPARE(outcomes.size(), 2);
    for (const PermissionOutcome &outcome : outcomes) {
        QVERIFY(!outcome.allowed());
        QCOMPARE(outcome.reasonCode, QStringLiteral("cancelled"));
        QCOMPARE(outcome.response.reason, QStringLiteral("用户中断了本次运行"));
    }
    QVERIFY(!gate.hasPending());

    // 再次取消是幂等的。
    gate.cancelAll(QStringLiteral("again"));
    QCOMPARE(outcomes.size(), 2);
}

void TestPermissionGate::resolveUnknownRequestIsIdempotent() {
    PermissionGate gate;
    PermissionResponse response;
    response.decision = PermissionDecision::Allow;

    // UI 可能因为重绘重复提交同一个裁决，这不该被当成错误。
    QVERIFY(!gate.resolve(QStringLiteral("perm_does_not_exist"), response));
}

void TestPermissionGate::duplicateRequestIdIsRejected() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);

    PermissionRequest request = makeRequest(PermissionKind::Execute, QStringLiteral("Bash"));
    int firstCalls = 0;
    gate.request(request, QStringLiteral("bash"), QStringLiteral("cmd"),
                 [&](PermissionOutcome) { ++firstCalls; });

    int secondCalls = 0;
    PermissionOutcome secondOutcome;
    gate.request(request, QStringLiteral("bash"), QStringLiteral("cmd"),
                 [&](PermissionOutcome outcome) {
                     ++secondCalls;
                     secondOutcome = outcome;
                 });

    // 同 id 的第二次请求必须立即被拒绝，否则会弹两个窗、产生两条回调。
    QCOMPARE(secondCalls, 1);
    QCOMPARE(secondOutcome.reasonCode, QStringLiteral("duplicate_request"));
    QVERIFY(!secondOutcome.allowed());
    QCOMPARE(firstCalls, 0);
    QCOMPARE(gate.pendingCount(), 1);
}

void TestPermissionGate::resolveAppliesPermissionUpdates() {
    PermissionGate gate;
    gate.setMode(PermissionMode::Default);

    PermissionRequest request = makeRequest(PermissionKind::Write, QStringLiteral("Write"));
    gate.request(request, QStringLiteral("edit"), QStringLiteral("/tmp/a.txt"),
                 [](PermissionOutcome) {});

    PermissionResponse response;
    response.decision = PermissionDecision::Allow;
    response.permissionUpdates.append(
        makeRule(QStringLiteral("edit"), QStringLiteral("/tmp"), PermissionRuleBehavior::Allow));

    QVERIFY(gate.resolve(request.id, response));
    QCOMPARE(gate.grantedRules().size(), 1);

    // 规则变更只应在提交裁决时生效，之后同类调用直接命中规则。
    QSignalSpy requestedSpy(&gate, &PermissionGate::requested);
    PermissionOutcome followUp;
    gate.request(makeRequest(PermissionKind::Write, QStringLiteral("Edit")),
                 QStringLiteral("edit"), QStringLiteral("/tmp/b.txt"),
                 [&](PermissionOutcome outcome) { followUp = outcome; });
    QVERIFY(followUp.allowed());
    QCOMPARE(requestedSpy.count(), 0);

    // reset 清空规则（切换会话时调用）。
    gate.reset();
    QVERIFY(gate.grantedRules().isEmpty());
}

QTEST_MAIN(TestPermissionGate)
#include "test_permission_gate.moc"
