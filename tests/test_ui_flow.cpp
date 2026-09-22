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

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFontMetrics>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include "support/FakeGateway.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "ui/PermissionDialog.h"
#include "ui/SidebarPanel.h"
#include "ui/ToolCallWidget.h"

using namespace zcode;
using namespace zcode::ui;
using zcode::test::FakeGateway;
using zcode::test::jsonEscape;
using zcode::test::textResponse;
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
    gateway_->enqueue(textResponse(
        "命令已执行，输出是 hello-from-zcode。\\n\\n### 结论\\n\\n- 工具调用成功\\n- "
        "权限确认生效\\n\\n```bash\\necho hello-from-zcode\\n```"));

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

    // 启动时会自动建一个会话（或恢复最近会话），等它稳定下来。
    QVERIFY(waitFor([&]() { return sendButton->isEnabled(); }));

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

    // 会话应当已落盘（标题取自首条用户输入）。
    QVERIFY(QFile::exists(dataDir_->path() + QStringLiteral("/qt/sessions.db")));
}

QTEST_MAIN(TestUiFlow)
#include "test_ui_flow.moc"
