// ZCode Qt — 界面级端到端测试
//
// 这是最接近"用户真实操作"的一层验证：构造真正的 MainWindow，通过公开的
// Qt 控件 API 输入文本、点击发送、等待权限弹窗、点击"允许一次"，
// 最后用 QWidget::grab() 把界面与弹窗截图存盘作为证据。
//
// 为什么不先用 xdotool 驱动真机窗口：它依赖窗口管理器配合激活窗口，
// 在部分桌面环境下会 BadMatch 失败；而 grab() 走 Qt 自己的渲染路径，
// 结果确定、可在 CI 里跑，并且能断言控件树而不是只比对像素。
//
// 覆盖的链路（一次跑通）：
//   MainWindow → AgentRuntime → Provider → SSE → 工具队列
//              → 权限门 → 权限弹窗 → 真实 Bash 执行 → 回填 → 最终回复
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFontMetrics>
#include <QLabel>
#include <QPlainTextEdit>
#include <QMenu>
#include <QListWidget>
#include <QMenuBar>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QHeaderView>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTableWidget>
#include <QPushButton>
#include <QTemporaryDir>

#include "support/FakeGateway.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "ui/PermissionDialog.h"
#include "ui/AppConfig.h"
#include "ui/SettingsDialog.h"
#include "ui/SidebarPanel.h"
#include "ui/ToolCallWidget.h"

using namespace zcode;
using namespace zcode::ui;
using zcode::test::FakeGateway;
using zcode::test::jsonEscape;
using zcode::test::multiToolCallResponse;
using zcode::test::textResponse;
using zcode::test::textResponseWithCache;
using zcode::test::toolCallResponse;

namespace {

/// 轮询等待条件成立。界面更新是异步的（网络 + 事件循环），不能用固定 sleep。
template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 8000, int stepMs = 50) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) {
            return true;
        }
        QTest::qWait(stepMs);
    }
    return predicate();
}

}  // namespace

class TestUiFlow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void drivesFullToolAndPermissionFlow();

private:
    /// 截图输出目录（构建目录下的 ui-screenshots）。
    static QString screenshotDir();

    std::unique_ptr<FakeGateway> gateway_;
    std::unique_ptr<QTemporaryDir> dataDir_;
};

QString TestUiFlow::screenshotDir() {
    const QString directory =
        QCoreApplication::applicationDirPath() + QStringLiteral("/ui-screenshots");
    QDir().mkpath(directory);
    return directory;
}

void TestUiFlow::initTestCase() {
    gateway_ = std::make_unique<FakeGateway>();
    QVERIFY2(gateway_->start(), "假网关未能监听本地端口");

    // 每次跑用全新的数据目录，避免上一轮的会话被恢复而干扰断言。
    dataDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dataDir_->isValid());

    const QString configDirectory = dataDir_->path() + QStringLiteral("/qt");
    QVERIFY(QDir().mkpath(configDirectory));

    QJsonObject provider;
    provider.insert(QStringLiteral("id"), QStringLiteral("smoke"));
    provider.insert(QStringLiteral("name"), QStringLiteral("Smoke Gateway"));
    provider.insert(QStringLiteral("kind"), QStringLiteral("openai_compatible"));
    provider.insert(QStringLiteral("baseUrl"), gateway_->baseUrl());
    provider.insert(QStringLiteral("apiKey"), QStringLiteral("smoke-key"));
    provider.insert(QStringLiteral("models"), QJsonArray::fromStringList({QStringLiteral("smoke-model")}));
    provider.insert(QStringLiteral("enabled"), true);
    provider.insert(QStringLiteral("availability"), QStringLiteral("available"));

    QJsonObject settings;
    settings.insert(QStringLiteral("themeMode"), QStringLiteral("dark"));
    settings.insert(QStringLiteral("uiFontSize"), 14);
    settings.insert(QStringLiteral("codeFontSize"), 14);
    settings.insert(QStringLiteral("language"), QStringLiteral("zh-CN"));
    settings.insert(QStringLiteral("providers"), QJsonArray{provider});
    settings.insert(QStringLiteral("lastModel"), QStringLiteral("smoke/smoke-model"));

    // 声明该模型的能力覆盖：上下文窗口与思考档位。用于验证
    // ① 工具条上的思考等级选择器出现并选中默认档位
    // ② 上下文用量的分母用的是覆盖值而不是内置默认值
    QJsonObject reasoningLevels;
    reasoningLevels.insert(QStringLiteral("contextWindow"), 64000);
    reasoningLevels.insert(QStringLiteral("reasoningLevels"),
                           QJsonArray::fromStringList({QStringLiteral("off"),
                                                       QStringLiteral("low"),
                                                       QStringLiteral("high")}));
    reasoningLevels.insert(QStringLiteral("defaultReasoningLevel"), QStringLiteral("high"));

    QJsonObject overrides;
    overrides.insert(QStringLiteral("smoke/smoke-model"), reasoningLevels);
    settings.insert(QStringLiteral("modelOverrides"), overrides);
    settings.insert(QStringLiteral("recentWorkspaces"), QJsonArray());
    settings.insert(QStringLiteral("defaultSessionMode"), QStringLiteral("build"));
    settings.insert(QStringLiteral("persistSessions"), true);

    QFile file(configDirectory + QStringLiteral("/settings.json"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
    file.close();

    // MainWindow 在构造时读这个环境变量决定数据根，所以必须在构造之前设置。
    qputenv("ZCODE_DATA_BASE_DIR", dataDir_->path().toUtf8());
}

void TestUiFlow::cleanupTestCase() {
    qunsetenv("ZCODE_DATA_BASE_DIR");
    dataDir_.reset();
    gateway_.reset();
}

void TestUiFlow::drivesFullToolAndPermissionFlow() {
    // ── 第 1 轮：模型要求执行 Bash；第 2 轮：模型给出最终回复 ──────────────
    gateway_->enqueue(toolCallResponse(QStringLiteral("Bash"),
                                       "{\\\"command\\\":\\\"echo hello-from-zcode\\\"", "}"));
    // 第二轮故意带缓存用量：prompt 1000 里有 750 命中缓存。
    // 用于端到端验证"SSE → provider 口径归一 → 用量累加 → 状态栏文案"整条链路。
    gateway_->enqueue(textResponseWithCache(
        "命令已执行，输出是 hello-from-zcode。\\n\\n### 结论\\n\\n- 工具调用成功\\n- "
        "权限确认生效\\n\\n```bash\\necho hello-from-zcode\\n```",
        1000, 750, 40));

    MainWindow window;
    window.resize(1280, 820);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *composer = window.findChild<QPlainTextEdit *>(QStringLiteral("composer"));
    auto *sendButton = window.findChild<QPushButton *>(QStringLiteral("sendButton"));
    auto *conversation = window.findChild<ConversationView *>(QStringLiteral("conversation"));
    QVERIFY(composer != nullptr);
    QVERIFY(sendButton != nullptr);
    QVERIFY(conversation != nullptr);

    // ── 侧边栏工作区行必须是完整可见的 ─────────────────────────────────────
    // 回归防护：曾经把 QVBoxLayout 塞进 QPushButton 承载"名称 + 路径"两行，
    // 而按钮的 sizeHint 只按自身文本算，导致路径那行被裁掉
    // （用户可见症状："左上角新建会话上面显示不全"）。
    auto *sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
    QVERIFY(sidebar != nullptr);

    auto *workspaceButton = sidebar->findChild<QPushButton *>();
    QVERIFY(workspaceButton != nullptr);

    // 按钮高度必须容得下它自己的文本（被裁的直接判据）。
    QVERIFY2(workspaceButton->height() >= workspaceButton->sizeHint().height(),
             qPrintable(QStringLiteral("工作区按钮被裁：height=%1 sizeHint=%2")
                            .arg(workspaceButton->height())
                            .arg(workspaceButton->sizeHint().height())));
    QVERIFY(!workspaceButton->text().isEmpty());

    // 路径标签必须真的占位并显示内容。
    QLabel *pathLabel = nullptr;
    for (QLabel *label : sidebar->findChildren<QLabel *>()) {
        if (label->toolTip().startsWith(QLatin1Char('/'))) {
            pathLabel = label;
            break;
        }
    }
    QVERIFY2(pathLabel != nullptr, "侧边栏应当有一行显示工作区路径的标签");
    QVERIFY2(pathLabel->isVisible() && pathLabel->height() > 0,
             "工作区路径标签必须可见且有高度");
    QVERIFY2(!pathLabel->text().isEmpty(), "工作区路径标签不应为空");
    // 完整路径必须在 tooltip 里，即使显示被省略。
    QVERIFY(pathLabel->toolTip().startsWith(QLatin1Char('/')));

    // ── 思考等级选择器 ─────────────────────────────────────────────────────
    auto *reasoningCombo = window.findChild<QComboBox *>(QStringLiteral("reasoningCombo"));
    QVERIFY2(reasoningCombo != nullptr, "工具条上应当有思考等级选择器");
    QVERIFY2(reasoningCombo->isVisible(),
             "模型声明了思考档位时，选择器必须可见（否则用户无法设置思考等级）");
    QCOMPARE(reasoningCombo->count(), 3);
    QCOMPARE(reasoningCombo->currentData().toString(), QStringLiteral("high"));

    // ── 三个选择器必须在输入区，而不是顶部工具条 ───────────────────────────
    // 要求：模式在输入框左下；模型与思考在发送按钮左边。
    auto *composerFrame = window.findChild<QWidget *>(QStringLiteral("composerFrame"));
    QVERIFY2(composerFrame != nullptr, "输入区应当有 composerFrame");
    auto *modeCombo = window.findChild<QComboBox *>(QStringLiteral("modeCombo"));
    auto *modelCombo = window.findChild<QComboBox *>(QStringLiteral("modelCombo"));
    QVERIFY(modeCombo != nullptr);
    QVERIFY(modelCombo != nullptr);

    for (QWidget *widget : {static_cast<QWidget *>(modeCombo),
                            static_cast<QWidget *>(modelCombo),
                            static_cast<QWidget *>(reasoningCombo),
                            static_cast<QWidget *>(sendButton)}) {
        QVERIFY2(composerFrame->isAncestorOf(widget),
                 "模式/模型/思考/发送都必须位于输入区内");
    }

    // 同一行上的水平顺序：模式 → 模型 → 思考 → 发送。
    const auto windowX = [&window](QWidget *widget) {
        return widget->mapTo(&window, QPoint(0, 0)).x();
    };
    const auto windowY = [&window](QWidget *widget) {
        return widget->mapTo(&window, QPoint(0, 0)).y();
    };
    QVERIFY2(windowX(modeCombo) < windowX(modelCombo),
             "模式应当排在模型左边（模式在输入框左下，模型在发送按钮一侧）");
    QVERIFY(windowX(modelCombo) < windowX(reasoningCombo));
    QVERIFY2(windowX(reasoningCombo) < windowX(sendButton),
             "模型与思考必须在发送按钮左边");
    QVERIFY2(qAbs(windowY(modeCombo) - windowY(sendButton)) < modeCombo->height(),
             "模式应与发送按钮在同一行（输入框下方那一行）");
    QVERIFY2(windowY(modeCombo) > windowY(window.findChild<QPlainTextEdit *>(
                                     QStringLiteral("composer"))),
             "模式应当在输入框的下方，而不是上方");

    // 启动时会自动建一个会话（或恢复最近会话），等它稳定下来。
    QVERIFY(waitFor([&]() { return sendButton->isEnabled(); }));

    // ── 上下文用量的分母必须来自覆盖值 ─────────────────────────────────────
    // 会话一建立就应显示窗口大小：否则用户改完"上下文窗口"看不到任何反馈。
    auto *contextLabel = window.findChild<QLabel *>(QStringLiteral("contextLabel"));
    QVERIFY(contextLabel != nullptr);
    QVERIFY2(contextLabel->text().contains(QStringLiteral("64.0k")),
             qPrintable(QStringLiteral("上下文分母应为覆盖后的 64000，实际文案：") +
                        contextLabel->text()));

    // ── 输入并发送 ─────────────────────────────────────────────────────────
    composer->setPlainText(QStringLiteral("run echo test"));
    QVERIFY(sendButton->isEnabled());
    sendButton->click();

    // 用户消息与 assistant 占位消息应当立刻出现。
    QVERIFY(waitFor([&]() { return conversation->messageCount() >= 2; }));
    QVERIFY(waitFor([&]() { return !sendButton->isEnabled(); }));  // 运行中禁止再发送

    // ── 等待权限弹窗 ───────────────────────────────────────────────────────
    auto *dialog = waitFor([&]() {
                       return window.findChild<PermissionDialog *>() != nullptr;
                   })
                       ? window.findChild<PermissionDialog *>()
                       : nullptr;
    QVERIFY2(dialog != nullptr, "Bash 调用必须先弹出权限确认对话框");

    // 弹窗内容：工具名、命令、"允许一次"选项都要在。
    const QList<QPushButton *> dialogButtons = dialog->findChildren<QPushButton *>();
    QPushButton *allowOnce = nullptr;
    for (QPushButton *button : dialogButtons) {
        if (button->property("optionKind").toString() == QStringLiteral("allowOnce")) {
            allowOnce = button;
            break;
        }
    }
    QVERIFY2(allowOnce != nullptr, "权限弹窗必须提供 allowOnce 选项按钮");

    const QString permissionShot =
        screenshotDir() + QStringLiteral("/02-permission-dialog.png");
    QVERIFY2(dialog->grab().save(permissionShot), qPrintable(permissionShot));

    // ── 放行 ───────────────────────────────────────────────────────────────
    allowOnce->click();

    // ── 等待整轮结束 ───────────────────────────────────────────────────────
    // 可见消息应为 3 条：user + assistant(工具调用) + assistant(最终文本)。
    // 合成的工具结果轮是 model-only 消息，**不应**作为一条"你"的消息出现
    // （工具输出已经渲染在工具卡片里）。
    QVERIFY2(waitFor([&]() { return conversation->messageCount() >= 3; }, 12000),
             "两次模型步的结果都应当出现在对话流里");
    QCOMPARE(conversation->messageCount(), 3);
    QVERIFY(waitFor([&]() { return sendButton->isEnabled(); }, 12000));

    // 工具卡片必须存在且已成功。
    const QList<ToolCallWidget *> cards = conversation->findChildren<ToolCallWidget *>();
    QVERIFY2(!cards.isEmpty(), "对话流里应当渲染出工具调用卡片");
    QCOMPARE(cards.first()->callId(), QStringLiteral("call_1"));

    // 两个模型请求：一轮带工具调用，一轮收尾。
    QCOMPARE(gateway_->requestCount(), 2);

    // ── 状态栏的用量明细 ───────────────────────────────────────────────────
    // 第一轮：prompt 50 / completion 10，无缓存
    // 第二轮：prompt 1000 里 750 命中缓存 → 未缓存 250、缓存读 750、输出 40
    // 累计：未缓存 300、缓存读 750、输出 50 → 命中率 750/(300+750) = 71%
    auto *usageLabel = window.findChild<QLabel *>(QStringLiteral("usageLabel"));
    QVERIFY2(usageLabel != nullptr, "状态栏应当有用量明细标签");
    QCOMPARE(usageLabel->text(),
             QStringLiteral("缓存 71% · 未缓存 300 · 缓存读 750 · 输出 50"));

    // 悬浮说明要给出精确数字与口径，避免缩写的歧义。
    QVERIFY(usageLabel->toolTip().contains(QStringLiteral("缓存命中率")));
    QVERIFY2(usageLabel->toolTip().contains(QStringLiteral("分母不含缓存写入")),
             "tooltip 应当解释命中率的分母口径");
    QVERIFY(usageLabel->toolTip().contains(QStringLiteral("最近一轮")));

    const QPixmap windowShot = window.grab();
    const QString chatShot = screenshotDir() + QStringLiteral("/03-conversation.png");
    QVERIFY2(windowShot.save(chatShot), qPrintable(chatShot));

    // 侧边栏单独存一张，便于核对工作区行的排版。
    // ⚠ 必须从**整窗**截图裁剪，不能用 sidebar->grab()：单独渲染子控件时
    // 拿不到祖先级样式表的解析结果，控件会退回系统浅色配色，得到一张
    // 与真实外观不符的误导性图片（实测踩到）。
    const QString sidebarShot = screenshotDir() + QStringLiteral("/01-sidebar.png");
    QVERIFY2(windowShot.copy(0, 0, 280, windowShot.height()).save(sidebarShot),
             qPrintable(sidebarShot));

    // ── 展开工具卡片再截一张，确认入参/输出区渲染 ──────────────────────────
    cards.first()->setExpanded(true);
    QTest::qWait(200);
    const QString toolShot = screenshotDir() + QStringLiteral("/04-tool-card-expanded.png");
    QVERIFY2(cards.first()->grab().save(toolShot), qPrintable(toolShot));

    // ── 子代理：独立会话进列表，但不抢走当前会话 ───────────────────────────
    auto *sessionList = sidebar->findChild<QListWidget *>();
    QVERIFY(sessionList != nullptr);
    const int sessionsBefore = sessionList->count();
    const int parentMessagesBefore = conversation->messageCount();

    const QByteArray agentArgs =
        R"({"description":"核对日志","prompt":"看看有没有异常","subagent_type":"general-purpose"})";
    gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Agent"), agentArgs}}));
    gateway_->enqueue(textResponse("子代理结论：没有异常。"));
    gateway_->enqueue(textResponse("父代理：收到子代理结论。"));

    composer->setPlainText(QStringLiteral("派个 agent 去核对"));
    QVERIFY(sendButton->isEnabled());
    sendButton->click();

    QVERIFY2(waitFor([&]() { return conversation->messageCount() >= parentMessagesBefore + 3; },
                     15000),
             "父会话应当继续累积消息（子代理的对话不能替换掉它）");
    QVERIFY(waitFor([&]() { return sendButton->isEnabled(); }, 15000));
    // 子会话必须出现在列表里。
    QVERIFY2(waitFor([&]() { return sessionList->count() > sessionsBefore; }, 8000),
             "子代理会话应当出现在侧边栏列表里");

    // 列表里能一眼看出哪条是子代理。
    bool foundSubagentRow = false;
    for (int row = 0; row < sessionList->count(); ++row) {
        if (sessionList->item(row)->text().contains(QStringLiteral("子代理"))) {
            foundSubagentRow = true;
            break;
        }
    }
    QVERIFY2(foundSubagentRow, "子代理会话应当在列表里标注出来");

    // 关键：当前会话**没有**被子代理顶替。父会话的消息继续累积，
    // 而不是被替换成子代理那两条。
    QCOMPARE(conversation->messageCount(), parentMessagesBefore + 3);
    QVERIFY2(conversation->findChildren<ToolCallWidget *>().size() >= 2,
             "父会话里应当同时有 Bash 与 Agent 两张工具卡片");

    const QString subagentShot =
        screenshotDir() + QStringLiteral("/08-subagent-session.png");
    QVERIFY2(window.grab().save(subagentShot), qPrintable(subagentShot));

    // ── 回归：重启后必须能打开之前的会话 ───────────────────────────────────
    // 用"重启前的实际条数"做基准，而不是写死数字：这条路径上新增任何一轮
    // 对话都不该让断言失效（写死 3 就踩过这个坑）。
    const int messagesBeforeRestart = conversation->messageCount();
    QVERIFY(messagesBeforeRestart >= 3);
    // 曾经的缺陷：onSessionSelected 在 loadSession **之后**才 clear()，而
    // loadSession 会为每条历史消息发 messageAdded，于是刚载入的历史被立刻抹掉。
    // 用户看到的现象就是"关掉软件再打开，之前的会话打不开（点开是空的）"。
    // 这里直接新建一个 MainWindow 模拟重启，走的是完全相同的启动路径。
    {
        MainWindow restarted;
        restarted.resize(1280, 820);
        restarted.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restarted));

        auto *restartedConversation =
            restarted.findChild<ConversationView *>(QStringLiteral("conversation"));
        QVERIFY(restartedConversation != nullptr);

        QVERIFY2(waitFor([&]() { return restartedConversation->messageCount() >= 3; }, 8000),
                 qPrintable(QStringLiteral("重启后应自动打开最近会话并显示历史消息，实际 %1 条")
                                .arg(restartedConversation->messageCount())));
        QCOMPARE(restartedConversation->messageCount(), messagesBeforeRestart);

        // 历史里的工具卡片也要重建出来，而不是只剩纯文本。
        QVERIFY2(!restartedConversation->findChildren<ToolCallWidget *>().isEmpty(),
                 "重启后重建的对话流里应当恢复工具调用卡片");

        const QString restartShot =
            screenshotDir() + QStringLiteral("/07-reopened-session.png");
        QVERIFY2(restarted.grab().save(restartShot), qPrintable(restartShot));
    }

    // ── 设置页的「模型能力」编辑入口 ───────────────────────────────────────
    // 只验证了后端与工具条还不够：用户能不能在设置里改"上下文窗口/思考档位"，
    // 必须走一遍真实的对话框才作数。
    AppSettings loadedSettings;
    QString configError;
    QVERIFY2(AppConfig::load(&loadedSettings, &configError), qPrintable(configError));

    SettingsDialog settingsDialog(loadedSettings, &window);
    settingsDialog.resize(940, 640);
    settingsDialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&settingsDialog));

    auto *tabs = settingsDialog.findChild<QTabWidget *>();
    QVERIFY(tabs != nullptr);
    // 切到 Provider 页（模型能力分组在那里）。
    tabs->setCurrentIndex(1);
    QTest::qWait(200);

    auto *capabilityGroup =
        settingsDialog.findChild<QGroupBox *>(QStringLiteral("modelCapabilityGroup"));
    QVERIFY2(capabilityGroup != nullptr, "设置页应当有「模型能力」分组");
    QVERIFY(capabilityGroup->isVisible());

    // 把设置页滚到底：「模型能力」分组在表单下方，不滚动的话截图里看不到它
    // （也就看不到用户实际要用的编辑入口）。
    if (auto *scrollArea = settingsDialog.findChild<QScrollArea *>()) {
        scrollArea->verticalScrollBar()->setValue(scrollArea->verticalScrollBar()->maximum());
        QTest::qWait(100);
    }

    auto *capabilityTable =
        settingsDialog.findChild<QTableWidget *>(QStringLiteral("modelCapabilityTable"));
    QVERIFY2(capabilityTable != nullptr, "模型能力分组里应当有编辑表格");
    // 测试配置里只有一个模型 smoke-model。
    QCOMPARE(capabilityTable->rowCount(), 1);
    QCOMPARE(capabilityTable->item(0, 0)->text(), QStringLiteral("smoke-model"));

    // 表头必须是中文列名而不是列号 1/2/3：QTableWidget::clear() 会把表头一起
    // 清空，只在构造里设一次的话 reload 之后就会退化成列号（实测踩到）。
    QCOMPARE(capabilityTable->horizontalHeaderItem(0)->text(), QStringLiteral("模型"));
    QCOMPARE(capabilityTable->horizontalHeaderItem(1)->text(), QStringLiteral("上下文窗口"));
    QCOMPARE(capabilityTable->horizontalHeaderItem(3)->text(), QStringLiteral("思考档位"));
    QCOMPARE(capabilityTable->columnCount(), 5);

    // 五列必须能塞进可视宽度，否则用户一打开就看不到最右边的"默认档位"。
    int tableWidth = 0;
    for (int column = 0; column < capabilityTable->columnCount(); ++column) {
        tableWidth += capabilityTable->columnWidth(column);
    }
    QVERIFY2(tableWidth <= capabilityTable->viewport()->width() + 4,
             qPrintable(QStringLiteral("表格总列宽 %1 超过可视宽度 %2，会出现横向滚动")
                            .arg(tableWidth)
                            .arg(capabilityTable->viewport()->width())));

    // 上下文窗口那一列必须回显出设置文件里的覆盖值（64000），
    // 而不是显示成"默认"——那意味着设置没被读进来。
    auto *contextSpin =
        capabilityTable->findChild<QSpinBox *>(QStringLiteral("modelContextSpin_0"));
    QVERIFY2(contextSpin != nullptr, "每行应当有上下文窗口输入框");
    QCOMPARE(contextSpin->value(), 64000);

    const QString settingsShot =
        screenshotDir() + QStringLiteral("/05-settings-model-capabilities.png");
    QVERIFY2(settingsDialog.grab().save(settingsShot), qPrintable(settingsShot));

    settingsDialog.close();

    // ── 回归：改完"上下文窗口"后工具条必须立刻更新 ─────────────────────────
    // 曾经的缺陷是改完设置没有任何东西触发重新测量，界面一直显示旧值
    // （128.0k），用户会认为设置没生效——只能等下一个 turn 结束才刷新。
    // 这里走的是真实路径：菜单 → 设置对话框 → 修改 → 确定。
    QAction *settingsAction = nullptr;
    for (QAction *menuAction : window.menuBar()->actions()) {
        QMenu *menu = menuAction->menu();
        if (menu == nullptr) {
            continue;
        }
        for (QAction *action : menu->actions()) {
            if (action->text().contains(QStringLiteral("设置"))) {
                settingsAction = action;
                break;
            }
        }
        if (settingsAction != nullptr) {
            break;
        }
    }
    QVERIFY2(settingsAction != nullptr, "菜单里应当有「设置…」项");

    // 设置对话框是模态的（exec() 会开嵌套事件循环），所以用 singleShot
    // 在它打开之后去操作它。这块代码跑在嵌套循环里，不是另一个线程。
    bool settingsEdited = false;
    QTimer::singleShot(300, [&]() {
        auto *modal = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
        if (modal == nullptr) {
            return;
        }
        if (auto *modalTabs = modal->findChild<QTabWidget *>()) {
            modalTabs->setCurrentIndex(1);
        }
        auto *modalTable =
            modal->findChild<QTableWidget *>(QStringLiteral("modelCapabilityTable"));
        if (modalTable == nullptr || modalTable->rowCount() == 0) {
            return;
        }
        auto *spin = modalTable->findChild<QSpinBox *>(QStringLiteral("modelContextSpin_0"));
        if (spin == nullptr) {
            return;
        }
        spin->setValue(1000000);
        settingsEdited = true;
        modal->accept();
    });

    settingsAction->trigger();  // 阻塞，直到上面的 lambda 把对话框关掉

    QVERIFY2(settingsEdited, "未能在设置对话框里修改上下文窗口");
    QVERIFY2(contextLabel->text().contains(QStringLiteral("1.0M")),
             qPrintable(QStringLiteral("改完设置后工具条应立即显示新分母（1.0M），实际：") +
                        contextLabel->text()));

    // 留一张"改完设置立刻生效"的截图作为证据。上下文用量现在在状态栏，
    // 所以截底部一条（输入区 + 状态栏）而不是顶部。
    const QString afterSettingsShot =
        screenshotDir() + QStringLiteral("/06-after-context-change.png");
    const QPixmap afterSettings = window.grab();
    QVERIFY2(afterSettings.copy(0, afterSettings.height() - 150, afterSettings.width(), 150)
                 .save(afterSettingsShot),
             qPrintable(afterSettingsShot));

    // 设置必须真的落盘，重启后还在。
    AppSettings reloaded;
    QVERIFY(AppConfig::load(&reloaded, &configError));
    QCOMPARE(reloaded.modelOverride(QStringLiteral("smoke"), QStringLiteral("smoke-model"))
                 .contextWindow,
             1000000);

    // ── 再验一次"正常关闭软件后重新打开" ───────────────────────────────────
    // 上一段是"两个窗口并存"，这一段走真实的 closeEvent（会 closeSession +
    // store.close()），然后再全新启动一次。这才是用户报的那个场景的完整路径。
    const int messagesBeforeClose = conversation->messageCount();

    window.close();
    {
        MainWindow reopened;
        reopened.resize(1280, 820);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));

        auto *reopenedConversation =
            reopened.findChild<ConversationView *>(QStringLiteral("conversation"));
        QVERIFY(reopenedConversation != nullptr);
        QVERIFY2(waitFor(
                     [&]() { return reopenedConversation->messageCount() >= messagesBeforeClose; },
                     8000),
                 qPrintable(QStringLiteral("正常关闭后重新打开应能看到历史，实际 %1 条")
                                .arg(reopenedConversation->messageCount())));
        QCOMPARE(reopenedConversation->messageCount(), messagesBeforeClose);
        QVERIFY(!reopenedConversation->findChildren<ToolCallWidget *>().isEmpty());
    }

    // 会话应当已落盘（标题取自首条用户输入）。
    QVERIFY(QFile::exists(dataDir_->path() + QStringLiteral("/qt/sessions.db")));
}

QTEST_MAIN(TestUiFlow)
#include "test_ui_flow.moc"
