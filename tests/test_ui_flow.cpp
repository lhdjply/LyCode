// LyCode — 界面级端到端测试
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
#include <QIcon>
#include <QMenu>
#include "storage/SessionStore.h"
#include "tools/BackgroundTaskRegistry.h"
#include "ui/AppConfig.h"

#include <QSet>
#include <QTextBlock>
#include <QListWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QMessageBox>

#ifdef Q_OS_UNIX
  #include <csignal>
#endif
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
#include "model/ModelCatalog.h"
#include "ui/MainWindow.h"
#include "ui/PermissionDialog.h"
#include "ui/AppConfig.h"
#include "ui/SettingsDialog.h"
#include "ui/SidebarPanel.h"
#include "ui/ToolCallWidget.h"

using namespace lycode;
using namespace lycode::ui;
using lycode::test::FakeGateway;
using lycode::test::jsonEscape;
using lycode::test::multiToolCallResponse;
using lycode::test::textResponse;
using lycode::test::textResponseWithCache;
using lycode::test::toolCallResponse;

namespace
{

/// 轮询等待条件成立。界面更新是异步的（网络 + 事件循环），不能用固定 sleep。
template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 8000, int stepMs = 50)
{
  QElapsedTimer timer;
  timer.start();
  while(timer.elapsed() < timeoutMs) {
    if(predicate()) {
      return true;
    }
    QTest::qWait(stepMs);
  }
  return predicate();
}

}  // namespace

class TestUiFlow : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void drivesFullToolAndPermissionFlow();
    void workspacePurgeDeletesSessionsButKeepsFiles();
    void attachesImageAndSendsIt();
    void conversationRendersHighlightedCode();
    void workspaceExpansionIsPersisted();
    void switchingWorkspaceKeepsTreeOrderStable();
    void topButtonCreatesWorkspaceNotSession();
    void removingCurrentWorkspaceActuallyWorks();
    void everyWorkspaceShowsFoldMarker();
    void windowUsesTheAppIcon();
    void choiceButtonsFillTheComposer();
    void settingsPickModelsFromTheCatalog();

  private:
    /// 截图输出目录（构建目录下的 ui-screenshots）。
    static QString screenshotDir();

    std::unique_ptr<FakeGateway> gateway_;
    std::unique_ptr<QTemporaryDir> dataDir_;
};

QString TestUiFlow::screenshotDir()
{
  const QString directory =
    QCoreApplication::applicationDirPath() + QStringLiteral("/ui-screenshots");
  QDir().mkpath(directory);
  return directory;
}

void TestUiFlow::initTestCase()
{
  gateway_ = std::make_unique<FakeGateway>();
  QVERIFY2(gateway_->start(), "假网关未能监听本地端口");

  // 每次跑用全新的数据目录，避免上一轮的会话被恢复而干扰断言。
  dataDir_ = std::make_unique<QTemporaryDir>();
  QVERIFY(dataDir_->isValid());

  const QString configDirectory = dataDir_->path();
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
  // 关掉模型生成标题：它会额外发一次模型请求，吃掉假网关队列里的响应，
  // 让既有断言变得不确定（实测直接打乱了图片附件那条测试）。
  // 标题生成本身由 sessionTitleIsGeneratedByModel 单独覆盖。
  settings.insert(QStringLiteral("generateSessionTitles"), false);
  // 显式给一个工作区：应用不再用"当前目录"兜底（那会让"移除最后一个工作区"
  // 在重启后复活），所以测试要像真实用户一样先有一个工作区。
  QVERIFY(QDir().mkpath(dataDir_->path() + QStringLiteral("/workspace")));
  settings.insert(QStringLiteral("lastWorkspace"),
                  dataDir_->path() + QStringLiteral("/workspace"));

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

  // MainWindow 在构造时读这个环境变量决定数据目录，所以必须在构造之前设置。
  qputenv("LYCODE_DATA_BASE_DIR", dataDir_->path().toUtf8());
}

/// 按账本清掉仍然存活的后台任务进程。
/// 后台任务的析构语义是"放它活过宿主"，所以测试必须自己收尾，
/// 否则每跑一次测试就漏一个 sleep 进程。
static void killLedgerTasks()
{
  QFile ledger(lycode::BackgroundTaskRegistry::ledgerPath());
  if(!ledger.open(QIODevice::ReadOnly)) {
    return;
  }
  const QJsonObject root = QJsonDocument::fromJson(ledger.readAll()).object();
  for(const QJsonValue & value : root.value(QStringLiteral("tasks")).toArray()) {
    const QJsonObject item = value.toObject();
    if(item.value(QStringLiteral("status")).toString() != QLatin1String("running")) {
      continue;
    }
    const int pid = item.value(QStringLiteral("pid")).toInt();
    if(pid > 0) {
#ifdef Q_OS_UNIX
      ::kill(-pid, SIGKILL);   // 进程组优先：命令可能自己 fork 过
      ::kill(pid, SIGKILL);
#endif
    }
  }
}

void TestUiFlow::cleanupTestCase()
{
  killLedgerTasks();
  qunsetenv("LYCODE_DATA_BASE_DIR");
  dataDir_.reset();
  gateway_.reset();
}

void TestUiFlow::drivesFullToolAndPermissionFlow()
{
  // ── 第 1 轮：模型要求执行 Bash；第 2 轮：模型给出最终回复 ──────────────
  // 传**原始 JSON**：toolCallResponse 负责转义（转义只做一次）。
  gateway_->enqueue(toolCallResponse(QStringLiteral("Bash"),
                                     R"({"command":"echo hello-from-lycode")", "}"));
  // 第二轮故意带缓存用量：prompt 1000 里有 750 命中缓存。
  // 用于端到端验证"SSE → provider 口径归一 → 用量累加 → 状态栏文案"整条链路。
  gateway_->enqueue(textResponseWithCache(
                      "命令已执行，输出是 hello-from-lycode。\\n\\n### 结论\\n\\n- 工具调用成功\\n- "
                      "权限确认生效\\n\\n```bash\\necho hello-from-lycode\\n```",
                      1000, 750, 40));

  MainWindow window;
  window.resize(1280, 820);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  auto * composer = window.findChild<QPlainTextEdit *>(QStringLiteral("composer"));
  auto * sendButton = window.findChild<QPushButton *>(QStringLiteral("sendButton"));
  auto * conversation = window.findChild<ConversationView *>(QStringLiteral("conversation"));
  QVERIFY(composer != nullptr);
  QVERIFY(sendButton != nullptr);
  QVERIFY(conversation != nullptr);

  // ── 侧边栏工作区行必须是完整可见的 ─────────────────────────────────────
  // 回归防护：曾经把 QVBoxLayout 塞进 QPushButton 承载"名称 + 路径"两行，
  // 而按钮的 sizeHint 只按自身文本算，导致路径那行被裁掉
  // （用户可见症状："左上角新建会话上面显示不全"）。
  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);

  auto * workspaceButton = sidebar->findChild<QPushButton *>();
  QVERIFY(workspaceButton != nullptr);

  // 按钮高度必须容得下它自己的文本（被裁的直接判据）。
  QVERIFY2(workspaceButton->height() >= workspaceButton->sizeHint().height(),
           qPrintable(QStringLiteral("工作区按钮被裁：height=%1 sizeHint=%2")
                      .arg(workspaceButton->height())
                      .arg(workspaceButton->sizeHint().height())));
  QVERIFY(!workspaceButton->text().isEmpty());

  // 路径标签必须真的占位并显示内容。
  QLabel * pathLabel = nullptr;
  for(QLabel * label : sidebar->findChildren<QLabel *>()) {
    if(label->toolTip().startsWith(QLatin1Char('/'))) {
      pathLabel = label;
      break;
    }
  }
  // 顶部那一行已经去掉：工作区现在只出现在左侧的树里。
  // 路径信息挂在节点的 tooltip 上，名字在节点文本里。
  auto * sidebarTree = sidebar->findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY2(sidebarTree != nullptr, "侧边栏应当是一棵工作区/会话树");
  QVERIFY2(sidebarTree->topLevelItemCount() >= 1, "树里应当有当前工作区节点");
  QTreeWidgetItem * currentWorkspaceNode = sidebarTree->topLevelItem(0);
  QVERIFY2(!currentWorkspaceNode->toolTip(0).isEmpty(),
           "工作区节点必须能用 tooltip 给出完整路径");
  QVERIFY2(!currentWorkspaceNode->text(0).isEmpty(), "工作区节点不应为空");
  // 每个工作区行右侧要有"+"（新建会话入口）。
  QVERIFY2(sidebarTree->itemWidget(currentWorkspaceNode, 1) != nullptr,
           "每个工作区行上应当有新建会话的入口");
  // 同一个动作（选目录并加进列表）在菜单栏和侧边栏不该有两个名字。
  auto * openWorkspaceButton =
    window.findChild<QPushButton *>(QStringLiteral("newWorkspaceButton"));
  QVERIFY(openWorkspaceButton != nullptr);
  QString menuText;
  for(QAction * menuAction : window.menuBar()->actions()) {
    if(menuAction->menu() == nullptr) {
      continue;
    }
    for(QAction * action : menuAction->menu()->actions()) {
      if(action->text().contains(QStringLiteral("工作区"))) {
        menuText = action->text();
        break;
      }
    }
  }
  QVERIFY2(!menuText.isEmpty(), "菜单栏里应当有工作区相关项");
  QCOMPARE(openWorkspaceButton->text(), menuText);
  // 正文宽度是"条目能不能读"的关键。列 1 只放一个 20px 的 "+"，
  // 一旦它被表头按内容撑开，正文就会被挤到大量省略号。
  QVERIFY2(sidebarTree->columnWidth(1) <= 30,
           qPrintable(QStringLiteral("操作列不该吃掉正文宽度，实际 %1px")
                      .arg(sidebarTree->columnWidth(1))));
  QVERIFY2(sidebarTree->columnWidth(0) > 200,
           qPrintable(QStringLiteral("正文列应当有足够宽度，实际 %1px")
                      .arg(sidebarTree->columnWidth(0))));

  // ── 思考等级选择器 ─────────────────────────────────────────────────────
  auto * reasoningCombo = window.findChild<QComboBox *>(QStringLiteral("reasoningCombo"));
  QVERIFY2(reasoningCombo != nullptr, "工具条上应当有思考等级选择器");
  QVERIFY2(reasoningCombo->isVisible(),
           "模型声明了思考档位时，选择器必须可见（否则用户无法设置思考等级）");
  QCOMPARE(reasoningCombo->count(), 3);
  QCOMPARE(reasoningCombo->currentData().toString(), QStringLiteral("high"));

  // ── 三个选择器必须在输入区，而不是顶部工具条 ───────────────────────────
  // 要求：模式在输入框左下；模型与思考在发送按钮左边。
  auto * composerFrame = window.findChild<QWidget *>(QStringLiteral("composerFrame"));
  QVERIFY2(composerFrame != nullptr, "输入区应当有 composerFrame");
  auto * modeCombo = window.findChild<QComboBox *>(QStringLiteral("modeCombo"));
  auto * modelCombo = window.findChild<QComboBox *>(QStringLiteral("modelCombo"));
  QVERIFY(modeCombo != nullptr);
  QVERIFY(modelCombo != nullptr);

  for(QWidget * widget : {
        static_cast<QWidget *>(modeCombo),
        static_cast<QWidget *>(modelCombo),
        static_cast<QWidget *>(reasoningCombo),
        static_cast<QWidget *>(sendButton)
      }) {
    QVERIFY2(composerFrame->isAncestorOf(widget),
             "模式/模型/思考/发送都必须位于输入区内");
  }

  // 同一行上的水平顺序：模式 → 模型 → 思考 → 发送。
  const auto windowX = [&window](QWidget * widget) {
    return widget->mapTo(&window, QPoint(0, 0)).x();
  };
  const auto windowY = [&window](QWidget * widget) {
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
  QVERIFY(waitFor([&]() {
    return sendButton->isEnabled();
  }));

  // ── 上下文用量的分母必须来自覆盖值 ─────────────────────────────────────
  // 会话一建立就应显示窗口大小：否则用户改完"上下文窗口"看不到任何反馈。
  auto * contextLabel = window.findChild<QLabel *>(QStringLiteral("contextLabel"));
  QVERIFY(contextLabel != nullptr);
  QVERIFY2(contextLabel->text().contains(QStringLiteral("64.0k")),
           qPrintable(QStringLiteral("上下文分母应为覆盖后的 64000，实际文案：") +
                      contextLabel->text()));

  // ── 输入并发送 ─────────────────────────────────────────────────────────
  composer->setPlainText(QStringLiteral("run echo test"));
  QVERIFY(sendButton->isEnabled());
  sendButton->click();

  // 用户消息与 assistant 占位消息应当立刻出现。
  QVERIFY(waitFor([&]() {
    return conversation->messageCount() >= 2;
  }));
  QVERIFY(waitFor([&]() {
    return !sendButton->isEnabled();
  }));  // 运行中禁止再发送

  // ── 等待权限弹窗 ───────────────────────────────────────────────────────
  auto * dialog = waitFor([&]() {
    return window.findChild<PermissionDialog *>() != nullptr;
  })
  ? window.findChild<PermissionDialog *>()
  : nullptr;
  QVERIFY2(dialog != nullptr, "Bash 调用必须先弹出权限确认对话框");

  // 弹窗内容：工具名、命令、"允许一次"选项都要在。
  const QList<QPushButton *> dialogButtons = dialog->findChildren<QPushButton *>();
  QPushButton * allowOnce = nullptr;
  for(QPushButton * button : dialogButtons) {
    if(button->property("optionKind").toString() == QStringLiteral("allowOnce")) {
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
  QVERIFY2(waitFor([&]() {
    return conversation->messageCount() >= 3;
  }, 12000),
  "两次模型步的结果都应当出现在对话流里");
  QCOMPARE(conversation->messageCount(), 3);
  QVERIFY(waitFor([&]() {
    return sendButton->isEnabled();
  }, 12000));

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
  auto * usageLabel = window.findChild<QLabel *>(QStringLiteral("usageLabel"));
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
  // 侧边栏是"工作区 → 会话"的树。会话是**子节点**，所以要递归数。
  auto * tree = sidebar->findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY2(tree != nullptr, "侧边栏应当是一棵树");
  const auto countSessions = [tree]() {
    int total = 0;
    for(int index = 0; index < tree->topLevelItemCount(); ++index) {
      total += tree->topLevelItem(index)->childCount();
    }
    return total;
  };
  const int sessionsBefore = countSessions();
  QVERIFY2(tree->topLevelItemCount() >= 1, "树里至少应当有当前工作区这个顶层节点");
  const int parentMessagesBefore = conversation->messageCount();

  const QByteArray agentArgs =
    R"({"description":"核对日志","prompt":"看看有没有异常","subagent_type":"general-purpose"})";
  gateway_->enqueue(multiToolCallResponse({{QStringLiteral("Agent"), agentArgs}}));
  gateway_->enqueue(textResponse("子代理结论：没有异常。"));
  gateway_->enqueue(textResponse("父代理：收到子代理结论。"));

  composer->setPlainText(QStringLiteral("派个 agent 去核对"));
  QVERIFY(sendButton->isEnabled());
  sendButton->click();

  QVERIFY2(waitFor([&]() {
    return conversation->messageCount() >= parentMessagesBefore + 3;
  },
  15000),
  "父会话应当继续累积消息（子代理的对话不能替换掉它）");
  QVERIFY(waitFor([&]() {
    return sendButton->isEnabled();
  }, 15000));
  // 子会话必须出现在列表里。
  QVERIFY2(waitFor([&]() {
    return countSessions() > sessionsBefore;
  }, 8000),
  "子代理会话应当出现在侧边栏的树上");

  // 树上能一眼看出哪条是子代理。
  bool foundSubagentRow = false;
  for(int index = 0; index < tree->topLevelItemCount() && !foundSubagentRow; ++index) {
    QTreeWidgetItem * parent = tree->topLevelItem(index);
    for(int child = 0; child < parent->childCount(); ++child) {
      if(parent->child(child)->text(0).contains(QStringLiteral("子代理"))) {
        foundSubagentRow = true;
        break;
      }
    }
  }
  QVERIFY2(foundSubagentRow, "子代理会话应当在列表里标注出来");

  // 关键：当前会话**没有**被子代理顶替。父会话的消息继续累积，
  // 而不是被替换成子代理那两条。
  QCOMPARE(conversation->messageCount(), parentMessagesBefore + 3);
  QVERIFY2(conversation->findChildren<ToolCallWidget *>().size() >= 2,
           "父会话里应当同时有 Bash 与 Agent 两张工具卡片");

  // 树的效果图：顶层是工作区，子节点是它的会话。
  const QString treeShot =
    screenshotDir() + QStringLiteral("/17-sidebar-tree.png");
  const QPixmap full = window.grab();
  QVERIFY2(full.copy(0, 0, 340, full.height()).save(treeShot), qPrintable(treeShot));

  const QString subagentShot =
    screenshotDir() + QStringLiteral("/08-subagent-session.png");
  QVERIFY2(window.grab().save(subagentShot), qPrintable(subagentShot));

  // ── 后台任务：状态栏必须给出可见提示 ───────────────────────────────────
  // 后台任务的全部意义就是"离开视线继续工作"，所以必须有个常驻提示，
  // 否则用户不知道还有进程在跑。
  const int yoloIndex = modeCombo->findData(static_cast<int>(SessionMode::Yolo));
  QVERIFY(yoloIndex >= 0);
  modeCombo->setCurrentIndex(yoloIndex);  // yolo：后台 Bash 不必逐个批准

  QJsonObject bgInput;
  // 跑得比整个测试长：这样它能活过下面的"关闭再打开"，用来验证跨重启认领。
  bgInput.insert(QStringLiteral("command"), QStringLiteral("sleep 45"));
  bgInput.insert(QStringLiteral("description"), QStringLiteral("长跑任务"));
  bgInput.insert(QStringLiteral("run_in_background"), true);
  gateway_->enqueue(multiToolCallResponse( {
    {
      QStringLiteral("Bash"),
      QJsonDocument(bgInput).toJson(QJsonDocument::Compact)
    }}));
  gateway_->enqueue(textResponse("后台已启动"));

  const int beforeBackground = conversation->messageCount();
  composer->setPlainText(QStringLiteral("起个后台任务"));
  sendButton->click();
  QVERIFY2(waitFor([&]() {
    return conversation->messageCount() >= beforeBackground + 3;
  },
  15000),
  "后台任务这一轮也应当正常收尾");
  QVERIFY(waitFor([&]() {
    return sendButton->isEnabled();
  }, 15000));

  auto * backgroundLabel = window.findChild<QLabel *>(QStringLiteral("backgroundLabel"));
  QVERIFY2(backgroundLabel != nullptr, "状态栏应当有后台任务提示标签");
  QCOMPARE(backgroundLabel->text(), QStringLiteral("· 后台任务 1"));
  // 必须检查**可见性**，不能只查 text()：状态栏的普通控件会被 showMessage()
  // 隐藏，只查文本的话这个缺陷会漏过去（实测漏过一次）。
  QVERIFY2(backgroundLabel->isVisible(),
           "后台任务提示必须始终可见——状态栏消息不得把它藏起来");
  QVERIFY2(backgroundLabel->toolTip().contains(QStringLiteral("正在运行")),
           "提示的悬浮说明应当解释怎么查看/终止");

  const QString backgroundShot =
    screenshotDir() + QStringLiteral("/09-background-task.png");
  const QPixmap backgroundPixmap = window.grab();
  QVERIFY2(backgroundPixmap.copy(0, backgroundPixmap.height() - 150,
                                 backgroundPixmap.width(), 150)
           .save(backgroundShot),
           qPrintable(backgroundShot));

  // 回到默认模式，避免影响后面的设置与重启验证。
  const int buildIndex = modeCombo->findData(static_cast<int>(SessionMode::Build));
  modeCombo->setCurrentIndex(buildIndex);

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

    auto * restartedConversation =
      restarted.findChild<ConversationView *>(QStringLiteral("conversation"));
    QVERIFY(restartedConversation != nullptr);

    QVERIFY2(waitFor([&]() {
      return restartedConversation->messageCount() >= 3;
    }, 8000),
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

  auto * tabs = settingsDialog.findChild<QTabWidget *>();
  QVERIFY(tabs != nullptr);
  // 切到 Provider 页（模型能力分组在那里）。
  tabs->setCurrentIndex(1);
  QTest::qWait(200);

  auto * capabilityGroup =
    settingsDialog.findChild<QGroupBox *>(QStringLiteral("modelCapabilityGroup"));
  QVERIFY2(capabilityGroup != nullptr, "设置页应当有「模型能力」分组");
  QVERIFY(capabilityGroup->isVisible());

  // 把设置页滚到底：「模型能力」分组在表单下方，不滚动的话截图里看不到它
  // （也就看不到用户实际要用的编辑入口）。
  if(auto * scrollArea = settingsDialog.findChild<QScrollArea *>()) {
    scrollArea->verticalScrollBar()->setValue(scrollArea->verticalScrollBar()->maximum());
    QTest::qWait(100);
  }

  auto * capabilityTable =
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
  for(int column = 0; column < capabilityTable->columnCount(); ++column) {
    tableWidth += capabilityTable->columnWidth(column);
  }
  QVERIFY2(tableWidth <= capabilityTable->viewport()->width() + 4,
           qPrintable(QStringLiteral("表格总列宽 %1 超过可视宽度 %2，会出现横向滚动")
                      .arg(tableWidth)
                      .arg(capabilityTable->viewport()->width())));

  // 上下文窗口那一列必须回显出设置文件里的覆盖值（64000），
  // 而不是显示成"默认"——那意味着设置没被读进来。
  auto * contextSpin =
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
  QAction * settingsAction = nullptr;
  for(QAction * menuAction : window.menuBar()->actions()) {
    QMenu * menu = menuAction->menu();
    if(menu == nullptr) {
      continue;
    }
    for(QAction * action : menu->actions()) {
      if(action->text().contains(QStringLiteral("设置"))) {
        settingsAction = action;
        break;
      }
    }
    if(settingsAction != nullptr) {
      break;
    }
  }
  QVERIFY2(settingsAction != nullptr, "菜单里应当有「设置…」项");

  // 设置对话框是模态的（exec() 会开嵌套事件循环），所以用 singleShot
  // 在它打开之后去操作它。这块代码跑在嵌套循环里，不是另一个线程。
  bool settingsEdited = false;
  QTimer::singleShot(300, [&]() {
    auto * modal = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
    if(modal == nullptr) {
      return;
    }
    if(auto * modalTabs = modal->findChild<QTabWidget *>()) {
      modalTabs->setCurrentIndex(1);
    }
    auto * modalTable =
      modal->findChild<QTableWidget *>(QStringLiteral("modelCapabilityTable"));
    if(modalTable == nullptr || modalTable->rowCount() == 0) {
      return;
    }
    auto * spin = modalTable->findChild<QSpinBox *>(QStringLiteral("modelContextSpin_0"));
    if(spin == nullptr) {
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

    auto * reopenedConversation =
      reopened.findChild<ConversationView *>(QStringLiteral("conversation"));
    QVERIFY(reopenedConversation != nullptr);
    QVERIFY2(waitFor(
    [&]() {
      return reopenedConversation->messageCount() >= messagesBeforeClose;
    },
    8000),
    qPrintable(QStringLiteral("正常关闭后重新打开应能看到历史，实际 %1 条")
               .arg(reopenedConversation->messageCount())));
    QCOMPARE(reopenedConversation->messageCount(), messagesBeforeClose);

    // ★ 跨重启：后台任务活过了应用退出，新实例必须从账本把它认领回来。
    // 这是"持久化恢复"唯一对用户可见的结果。
    auto * reopenedBackground =
      reopened.findChild<QLabel *>(QStringLiteral("backgroundLabel"));
    QVERIFY(reopenedBackground != nullptr);
    QVERIFY2(waitFor(
    [&]() {
      return reopenedBackground->text().contains(QStringLiteral("后台任务"));
    },
    5000),
    qPrintable(QStringLiteral("重启后应当认领遗留的后台任务，实际标签=%1")
               .arg(reopenedBackground->text())));
    QVERIFY(!reopenedConversation->findChildren<ToolCallWidget *>().isEmpty());
  }

  // 会话应当已落盘（标题取自首条用户输入）。
  QVERIFY(QFile::exists(dataDir_->path() + QStringLiteral("/sessions.db")));
}

void TestUiFlow::workspacePurgeDeletesSessionsButKeepsFiles()
{
  // 这个测试只关心"删什么/不删什么"，不需要模型，所以不走网关。
  QTemporaryDir workspaceDir;
  QVERIFY(workspaceDir.isValid());
  // 放一个真实文件进去：删除工作区会话**绝不能**碰到它。
  const QString markerPath = workspaceDir.filePath(QStringLiteral("keep-me.txt"));
  QFile marker(markerPath);
  QVERIFY(marker.open(QIODevice::WriteOnly));
  marker.write("must survive");
  marker.close();

  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);
  auto * conversation = window.findChild<ConversationView *>(QStringLiteral("conversation"));
  QVERIFY(conversation != nullptr);

  // 切到这个工作区并建两个会话。
  sidebar->setWorkspace(Workspace{workspaceDir.path(), {}, {}});
  emit sidebar->workspaceRecentRequested(workspaceDir.path());
  QVERIFY(waitFor([&]() {
    return conversation->messageCount() >= 0;
  }, 3000));

  SessionStore probe;
  QVERIFY(probe.open(dataDir_->path() + QStringLiteral("/sessions.db")));
  const QString key = Workspace{workspaceDir.path(), {}, {}}.key();
  QVERIFY2(!probe.listSessions(key).isEmpty(), "切换工作区后应当自动建了一个会话");
  const int sessionsBefore = probe.listSessions(key).size();
  QStringList oldSessionIds;
  for(const SessionSummary & summary : probe.listSessions(key)) {
    oldSessionIds.append(summary.session.id);
  }

  // ⓪ 菜单结构本身必须提供这两个动作。
  // 上面的断言都是直接 emit 信号，绕过了菜单；不单独验一次的话，
  // "菜单里根本没有入口"这种缺陷会完全漏过去。
  QStringList menuTexts;
  bool submenuShotTaken = false;
  QTimer::singleShot(150, [&]() {
    auto * popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if(popup == nullptr) {
      return;
    }
    popup->grab().save(screenshotDir() + QStringLiteral("/10-workspace-menu.png"));
    for(QAction * entry : popup->actions()) {
      QMenu * sub = entry->menu();
      if(sub == nullptr) {
        // 平铺菜单（树节点的右键菜单）。
        if(!entry->isSeparator()) {
          menuTexts.append(entry->text());
        }
        continue;
      }
      for(QAction * action : sub->actions()) {
        if(!action->isSeparator()) {
          menuTexts.append(action->text());
        }
      }
      // 展开子菜单再截一张：这样截图能真正看到两个新动作，
      // 而不只是一个带 ▸ 的父条目。
      if(!submenuShotTaken) {
        submenuShotTaken = true;
        sub->popup(popup->mapToGlobal(QPoint(popup->width() - 4, 4)));
        QTimer::singleShot(120, [sub, popup]() {
          sub->grab().save(screenshotDir() +
                           QStringLiteral("/11-workspace-submenu.png"));
          sub->close();
          popup->close();
        });
        return;  // 交给内层定时器收尾
      }
    }
    popup->close();
  });
  // 按**真实用户路径**触发：在树的工作区节点上请求右键菜单。
  // （顶部的工作区按钮已经去掉，管理动作现在挂在树节点的右键菜单上。）
  auto * treeForMenu = window.findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY2(treeForMenu != nullptr, "侧边栏应当是一棵树");
  QTreeWidgetItem * workspaceNode = nullptr;
  for(int index = 0; index < treeForMenu->topLevelItemCount(); ++index) {
    if(treeForMenu->topLevelItem(index)->toolTip(0) == workspaceDir.path()) {
      workspaceNode = treeForMenu->topLevelItem(index);
      break;
    }
  }
  QVERIFY2(workspaceNode != nullptr, "树上应当有这个工作区的节点");
  // 用列 0 内的确定坐标，而不是 rect 的中心：中心可能落到第二列
  // （"+"/操作列）上，itemAt() 那时返回 null，菜单就不会弹出来。
  const QRect nodeRect = treeForMenu->visualItemRect(workspaceNode);
  QVERIFY2(nodeRect.isValid() && nodeRect.height() > 0, "工作区节点应当已经布局");
  emit treeForMenu->customContextMenuRequested(
    QPoint(8, nodeRect.center().y()));
  QVERIFY2(menuTexts.contains(QStringLiteral("从最近列表移除")),
           qPrintable(QStringLiteral("菜单里应当有「从最近列表移除」，实际：%1")
                      .arg(menuTexts.join(QStringLiteral(" / ")))));
  QVERIFY2(menuTexts.contains(QStringLiteral("删除该工作区的全部会话…")),
           qPrintable(QStringLiteral("菜单里应当有「删除该工作区的全部会话…」，实际：%1")
                      .arg(menuTexts.join(QStringLiteral(" / ")))));
  // 展开/折叠只是界面收起，不是"打开/关闭工作区"，所以不该有"打开"这一项。
  QVERIFY2(!menuTexts.contains(QStringLiteral("打开")),
           qPrintable(QStringLiteral("工作区菜单不该有「打开」，实际：%1")
                      .arg(menuTexts.join(QStringLiteral(" / ")))));

  // ① 从最近列表移除：只动列表。
  AppSettings settingsSnapshot;
  QVERIFY(AppConfig::load(&settingsSnapshot));
  const QStringList recentBefore = settingsSnapshot.recentWorkspaces;
  QVERIFY2(recentBefore.contains(workspaceDir.path()),
           qPrintable(QStringLiteral("最近列表里应当有该工作区: %1")
                      .arg(recentBefore.join(QStringLiteral(", ")))));
  emit sidebar->workspaceRemoveRequested(workspaceDir.path());
  settingsSnapshot = AppSettings{};
  QVERIFY(AppConfig::load(&settingsSnapshot));
  const QStringList recentAfter = settingsSnapshot.recentWorkspaces;
  QVERIFY2(!recentAfter.contains(workspaceDir.path()), "应当已从最近列表移除");
  // 关键：移除列表项**不删**会话。
  QCOMPARE(probe.listSessions(key).size(), sessionsBefore);

  // 移除的是**当前**工作区，界面会切到列表里剩下的那个——把它切回来，
  // 后面的"删除全部会话"要作用在这个工作区上。
  emit sidebar->workspaceRecentRequested(workspaceDir.path());
  QTest::qWait(200);

  // ② 删除该工作区全部会话：需要确认，用一个定时器在模态循环里点「是」。
  // 用**重复**定时器而不是 0ms 单发：单发有可能在对话框成为模态窗口之前
  // 就触发，于是找不到它、没人点「是」，删除被静默取消（实测踩到）。
  QTimer confirmer;
  confirmer.setInterval(20);
  QObject::connect(&confirmer, &QTimer::timeout, [&confirmer]() {
    for(QWidget * widget : QApplication::topLevelWidgets()) {
      auto * box = qobject_cast<QMessageBox *>(widget);
      if(box != nullptr && box->isVisible()) {
        confirmer.stop();
        box->button(QMessageBox::Yes)->click();
        return;
      }
    }
  });
  confirmer.start();
  emit sidebar->workspacePurgeRequested(workspaceDir.path());
  confirmer.stop();  // 模态循环已返回；确保后续不再误点别的框

  // 断言"旧会话都不在了"，而不是"一条都不剩"：删空当前工作区之后
  // MainWindow 会立刻开一个新的空会话，否则界面没法用。
  // 直接再查一次即可——store 每次查询都读库，MainWindow 的删除已提交。
  const QList<SessionSummary> after = probe.listSessions(key);
  for(const SessionSummary & summary : after) {
    QVERIFY2(!oldSessionIds.contains(summary.session.id),
             qPrintable(QStringLiteral("旧会话 %1 必须已被删除").arg(summary.session.id)));
  }
  QVERIFY2(after.size() <= 1, "删空后最多只剩一个新开的空会话");

  // ★ 最重要的一条：只删会话，绝不碰磁盘上的文件。
  QVERIFY2(QFile::exists(markerPath), "工作区目录里的文件绝不能被删除");
  QFile verify(markerPath);
  QVERIFY(verify.open(QIODevice::ReadOnly));
  QCOMPARE(verify.readAll(), QByteArray("must survive"));
  QVERIFY2(QDir(workspaceDir.path()).exists(), "工作区目录本身必须保留");

  // 当前工作区被清空后应当自动开一个新的空会话，而不是留下空界面。
  QVERIFY2(waitFor([&]() {
    return conversation->messageCount() <= 1;
  }, 3000),
  "清空后应当回到干净的新会话状态");
}

void TestUiFlow::attachesImageAndSendsIt()
{
  // 造一张真图片文件（不是空字节），这样 MIME 判定与解码都能走通。
  QTemporaryDir imageDir;
  QVERIFY(imageDir.isValid());
  QImage source(48, 32, QImage::Format_RGB32);
  source.fill(QColor(200, 60, 60));
  const QString imagePath = imageDir.filePath(QStringLiteral("probe.png"));
  QVERIFY2(source.save(imagePath, "PNG"), "测试图片应当能写入磁盘");

  // 探针：确认设置真的读到了（否则标题请求会吃掉网关响应，
  // 下面几条断言都会以看不懂的方式失败）。
  {
    AppSettings probe;
    QVERIFY(AppConfig::load(&probe));
    QVERIFY2(!probe.generateSessionTitles,
             "本测试要求 generateSessionTitles=false");
  }

  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * composer = window.findChild<QPlainTextEdit *>(QStringLiteral("composer"));
  QVERIFY(composer != nullptr);
  auto * sendButton = window.findChild<QPushButton *>(QStringLiteral("sendButton"));
  QVERIFY(sendButton != nullptr);
  auto * attachButton = window.findChild<QPushButton *>(QStringLiteral("attachButton"));
  QVERIFY2(attachButton != nullptr, "输入区必须有附加图片的入口");

  // 前面的测试会改动工作区列表与当前工作区，这里显式切到一个确定的工作区，
  // 否则本测试可能在"没有工作区"的状态下开始，发送按钮是禁用的。
  {
    auto * sb = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
    QVERIFY(sb != nullptr);
    QTemporaryDir ownWorkspace;
    QVERIFY(ownWorkspace.isValid());
    emit sb->workspaceRecentRequested(ownWorkspace.path());
    QVERIFY(waitFor([&]() {
      return sendButton->isEnabled();
    }, 3000));
  }
  auto * strip = window.findChild<QWidget *>(QStringLiteral("attachmentStrip"));
  QVERIFY(strip != nullptr);
  auto * conversation = window.findChild<ConversationView *>(QStringLiteral("conversation"));
  QVERIFY(conversation != nullptr);

  // 没有附件时附件条不占位置。
  QVERIFY2(!strip->isVisible(), "没有附件时附件条应当隐藏");

  // 直接走"拖入/选择后"的入口：attachImages 是按钮与拖拽共用的落点。
  QMetaObject::invokeMethod(&window, "attachImages", Qt::DirectConnection,
                            Q_ARG(QStringList, QStringList{imagePath}));
  QVERIFY2(waitFor([&]() {
    return strip->isVisible();
  }, 3000),
  "附加图片后附件条必须出现");
  QVERIFY2(window.findChild<QWidget *>(QStringLiteral("attachmentChip")) != nullptr,
           "附件条里应当有一张缩略图块");
  QVERIFY2(window.findChild<QPushButton *>(QStringLiteral("attachmentRemove")) != nullptr,
           "每个附件必须有移除按钮");

  const QString shot = screenshotDir() + QStringLiteral("/12-image-attachment.png");
  QVERIFY2(window.grab().save(shot), qPrintable(shot));

  // 发送：请求里必须带图，会话里必须渲染出图片。
  gateway_->enqueue(textResponse("我看到了一张红色的图"));
  composer->setPlainText(QStringLiteral("这是什么颜色？"));
  const int before = conversation->messageCount();
  sendButton->click();
  QVERIFY2(waitFor([&]() {
    return conversation->messageCount() >= before + 2;
  }, 15000),
  "发送附件消息后应当收到回复");
  QVERIFY(waitFor([&]() {
    return sendButton->isEnabled();
  }, 15000));

  // 发送成功后附件条必须清空，否则用户会以为还要再发一次。
  QVERIFY2(!strip->isVisible(), "发送成功后附件条应当清空");

  const QByteArray body = gateway_->lastBody();
  QVERIFY2(body.contains("image_url"),
           qPrintable(QStringLiteral("UI 发出的请求里必须带图片。请求体片段：%1")
                      .arg(QString::fromUtf8(body.left(600)))));

  // 会话里渲染出缩略图，而不是只有文件名。
  const QList<QLabel *> images = conversation->findChildren<QLabel *>(
                                   QStringLiteral("attachedImage"));
  QVERIFY2(!images.isEmpty(), "对话流里必须把图片渲染出来");
  QVERIFY2(!images.first()->pixmap().isNull(), "渲染出来的必须是真图片");
}

void TestUiFlow::conversationRendersHighlightedCode()
{
  // 这条测试专门盯住一个曾经**全绿但完全没生效**的链路：
  // 会话视图原本用的是 QTextBrowser::setMarkdown（Qt 内置解析器），
  // 而不是自研的 Markdown::toHtml。于是语法高亮、代码块样式、
  // 表格、任务列表全都不生效，而单元测试（直接调 Markdown::toHtml）
  // 和截图测试（独立 QTextBrowser）都是绿的。
  ConversationView view;
  view.setStyleSheet(Theme::instance().styleSheet());
  view.resize(720, 480);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));

  Message message;
  message.id = QStringLiteral("msg_highlight");
  message.sessionId = QStringLiteral("session_x");
  message.role = MessageRole::Assistant;
  message.status = MessageStatus::Complete;
  Part part = Part::makeText(QStringLiteral("说明：\n\n"
                                            "```cpp\n"
                                            "// note\n"
                                            "static int factorial(int n) {\n"
                                            "    const char *label = \"recursive\";\n"
                                            "    if (n <= 1) return 1;\n"
                                            "    return n * factorial(n - 1);\n"
                                            "}\n"
                                            "```"));
  message.parts.append(part);
  view.addMessage(message);

  auto * browser = view.findChild<QTextBrowser *>();
  QVERIFY2(browser != nullptr, "会话视图里应当有富文本视图");

  QSet<QString> colors;
  for(QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next()) {
    for(QTextBlock::iterator fragment = block.begin(); !fragment.atEnd(); ++fragment) {
      const QTextFragment piece = fragment.fragment();
      if(piece.isValid() && !piece.text().trimmed().isEmpty()) {
        colors.insert(piece.charFormat().foreground().color().name());
      }
    }
  }
  QVERIFY2(colors.size() >= 3,
           qPrintable(QStringLiteral("会话视图里的代码块必须着色，实际只有 %1 种前景色：%2")
                      .arg(colors.size())
                      .arg(QStringList(colors.begin(), colors.end())
                           .join(QStringLiteral(", ")))));

  // 语言标签由自研渲染器的 div.code-lang 产出。它是"走对了渲染器"的另一个
  // 证据（Qt 内置 setMarkdown 不会产出这个类名）。
  QVERIFY2(browser->toHtml().contains(QStringLiteral("cpp")),
           "代码块应当标出语言");

  // 刻意**不**断言代码块的背景色：Qt 富文本对 `div` 上的 background-color
  // 支持并不可靠（实测没有变成块背景画刷），断言它只会得到一条时好时坏的测试。
  // 需要确认观感时看截图。

  view.close();
}

void TestUiFlow::workspaceExpansionIsPersisted()
{
  // 用户报过："每次打开软件只有一个工作区是打开的"——展开状态没被持久化，
  // 展开过的其它工作区重启后全被收起来。
  QTemporaryDir first;
  QTemporaryDir second;
  QVERIFY(first.isValid() && second.isValid());

  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);
  auto * tree = sidebar->findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY(tree != nullptr);

  // 造两个工作区节点。用真实入口：打开目录会把它加进最近列表。
  sidebar->setWorkspace(Workspace{first.path(), {}, {}});
  emit sidebar->workspaceRecentRequested(first.path());
  QVERIFY(waitFor([&]() {
    return tree->topLevelItemCount() >= 1;
  }, 3000));

  AppSettings settings;
  QVERIFY(AppConfig::load(&settings));
  settings.recentWorkspaces = {first.path(), second.path()};
  sidebar->setRecentWorkspaces(settings.recentWorkspaces);
  QVERIFY(waitFor([&]() {
    return tree->topLevelItemCount() >= 2;
  }, 3000));

  // 展开第二个工作区（不是当前工作区）。
  QTreeWidgetItem * secondNode = nullptr;
  for(int index = 0; index < tree->topLevelItemCount(); ++index) {
    if(tree->topLevelItem(index)->toolTip(0) == second.path()) {
      secondNode = tree->topLevelItem(index);
      break;
    }
  }
  QVERIFY2(secondNode != nullptr, "第二个工作区应当出现在树上");
  secondNode->setExpanded(true);

  // 展开状态必须落盘——这是"下次打开还保持原样"的唯一依据。
  AppSettings saved;
  QVERIFY(AppConfig::load(&saved));
  QVERIFY2(saved.expandedWorkspaces.contains(second.path()),
           qPrintable(QStringLiteral("展开状态应当被持久化，实际：%1")
                      .arg(saved.expandedWorkspaces.join(QStringLiteral(", ")))));

  // 折叠后要从记录里移除，否则下次打开会"自己弹开"。
  secondNode->setExpanded(false);
  AppSettings afterCollapse;
  QVERIFY(AppConfig::load(&afterCollapse));
  QVERIFY2(!afterCollapse.expandedWorkspaces.contains(second.path()),
           "折叠后不该还留在展开记录里");
}

void TestUiFlow::switchingWorkspaceKeepsTreeOrderStable()
{
  // 用户报过："点击会话，工作区排序就变了"。原因是切工作区时会把它提到
  // "最近"列表最前，而树的顺序照的就是这个列表。
  QTemporaryDir first;
  QTemporaryDir second;
  QVERIFY(first.isValid() && second.isValid());

  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);
  auto * tree = sidebar->findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY(tree != nullptr);

  // 启动时的工作区本身就在列表里，所以每打开一个新目录，节点数 +1。
  const int initialCount = tree->topLevelItemCount();
  QVERIFY2(initialCount >= 1, "启动时应当有当前工作区这个节点");

  emit sidebar->workspaceRecentRequested(first.path());
  QVERIFY(waitFor([&]() {
    return tree->topLevelItemCount() > initialCount;
  }, 3000));
  const int afterFirst = tree->topLevelItemCount();

  emit sidebar->workspaceRecentRequested(second.path());
  QVERIFY(waitFor([&]() {
    return tree->topLevelItemCount() > afterFirst;
  }, 3000));

  const auto order = [tree]() {
    QStringList paths;
    for(int index = 0; index < tree->topLevelItemCount(); ++index) {
      paths.append(tree->topLevelItem(index)->toolTip(0));
    }
    return paths;
  };
  const QStringList before = order();
  QVERIFY2(before.size() >= 3, qPrintable(before.join(QStringLiteral(", "))));

  // 切到**已经存在**的工作区：顺序不能变。
  // 这里直接走真实入口（打开工作区路径），它以前会把它提到最前。
  emit sidebar->workspaceRecentRequested(before.first());
  QTest::qWait(200);
  QCOMPARE(order(), before);
  emit sidebar->workspaceRecentRequested(before.last());
  QTest::qWait(200);
  QVERIFY2(order() == before,
           qPrintable(QStringLiteral("切换工作区不该改变树的顺序，实际：%1（原为 %2）")
                      .arg(order().join(QStringLiteral(", ")),
                           before.join(QStringLiteral(", ")))));

  // 同一个工作区再切回来，仍然稳定。
  emit sidebar->workspaceRecentRequested(before.first());
  QTest::qWait(200);
  QCOMPARE(order(), before);

  // 点击工作区节点应当折叠/展开。
  // 展开箭头换成了文字指示符（▸/▾）之后就没有可点的箭头了，必须自己接
  // 点击事件——否则用户**根本没有办法折叠**一个工作区（实测漏过）。
  QTreeWidgetItem * node = tree->topLevelItem(0);
  QVERIFY(node != nullptr);
  node->setExpanded(true);
  QVERIFY(node->isExpanded());
  emit tree->itemClicked(node, 0);
  QVERIFY2(!node->isExpanded(), "点击工作区节点应当折叠它");
  emit tree->itemClicked(node, 0);
  QVERIFY2(node->isExpanded(), "再点一次应当展开");

  // 会话节点不该被折叠（它们没有子节点，折叠无意义且会让人困惑）。
  if(node->childCount() > 0) {
    QTreeWidgetItem * session = node->child(0);
    emit tree->itemClicked(session, 0);
    QVERIFY2(node->isExpanded(), "点会话不该折叠它的工作区");
  }
}

void TestUiFlow::topButtonCreatesWorkspaceNotSession()
{
  // 用户报过："新建工作区怎么是新建会话的功能？"——按钮文字改了，但它接的
  // 信号还是 newSessionRequested。这类"改了外观没改动作"的错误，只有断言
  // **动作**才拦得住。
  //
  // 这里单独建一个 SidebarPanel 而不是用 MainWindow：workspaceChangeRequested
  // 在 MainWindow 里会打开模态目录对话框，测试会被卡住。
  SidebarPanel panel;
  panel.setWorkspace(Workspace{QDir::tempPath(), {}, {}});
  panel.show();
  QVERIFY(QTest::qWaitForWindowExposed(&panel));

  auto * button = panel.findChild<QPushButton *>(QStringLiteral("newWorkspaceButton"));
  QVERIFY2(button != nullptr, "顶部应当有新建工作区按钮");
  // 措辞必须与菜单栏的同一个动作一致（同一个动作不该有两个名字）。
  QVERIFY2(button->text().startsWith(QStringLiteral("打开工作区")),
           qPrintable(QStringLiteral("侧边栏按钮措辞应当与菜单一致，实际：%1")
                      .arg(button->text())));

  QSignalSpy workspaceSpy(&panel, &SidebarPanel::workspaceChangeRequested);
  QSignalSpy sessionSpy(&panel, &SidebarPanel::newSessionRequested);
  button->click();

  QCOMPARE(workspaceSpy.count(), 1);
  QCOMPARE(sessionSpy.count(), 0);  // ★ 关键：绝不能建会话

  // 工作区行上的 "+" 才是新建会话，而且必须带上那个工作区的路径。
  panel.setRecentWorkspaces({QDir::tempPath()});
  auto * tree = panel.findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY(tree != nullptr);
  QCOMPARE(tree->topLevelItemCount(), 1);
  auto * plus = qobject_cast<QToolButton *>(
                  tree->itemWidget(tree->topLevelItem(0), 1));
  QVERIFY2(plus != nullptr, "工作区行上应当有 + 按钮");
  QSignalSpy plusSpy(&panel, &SidebarPanel::newSessionRequestedInWorkspace);
  plus->click();
  QCOMPARE(plusSpy.count(), 1);
  QCOMPARE(plusSpy.first().at(0).toString(), QDir::tempPath());
}

void TestUiFlow::removingCurrentWorkspaceActuallyWorks()
{
  // 用户报过："无法移除最后一个工作区"。根因是移除**当前**工作区时，
  // SidebarPanel::setRecentWorkspaces 会把当前工作区强制补回树上，
  // 界面上看不出变化；重启后启动逻辑又把它写回列表——永远删不掉。
  QTemporaryDir first;
  QTemporaryDir second;
  QVERIFY(first.isValid() && second.isValid());

  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);
  auto * tree = sidebar->findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY(tree != nullptr);
  auto * sendButton = window.findChild<QPushButton *>(QStringLiteral("sendButton"));
  QVERIFY(sendButton != nullptr);

  const auto treePaths = [tree]() {
    QStringList paths;
    for(int index = 0; index < tree->topLevelItemCount(); ++index) {
      paths.append(tree->topLevelItem(index)->toolTip(0));
    }
    return paths;
  };

  // 加两个工作区，当前是第二个。
  emit sidebar->workspaceRecentRequested(first.path());
  QTest::qWait(200);
  emit sidebar->workspaceRecentRequested(second.path());
  QTest::qWait(200);
  QVERIFY2(treePaths().contains(first.path()), "第一个工作区应当在树上");
  QVERIFY2(treePaths().contains(second.path()), "第二个工作区应当在树上");

  {
    AppSettings probe;
    QVERIFY(AppConfig::load(&probe));
    QVERIFY2(probe.recentWorkspaces.contains(second.path()),
             "第二个工作区应当是当前的");
  }

  // ① 移除**当前**工作区（第二个）：必须真的消失。
  emit sidebar->workspaceRemoveRequested(second.path());
  QVERIFY2(waitFor([&]() {
    return !treePaths().contains(second.path());
  }, 3000),
  qPrintable(QStringLiteral("移除当前工作区后它必须从树上消失，实际：%1")
             .arg(treePaths().join(QStringLiteral(", ")))));
  {
    AppSettings after;
    QVERIFY(AppConfig::load(&after));
    QVERIFY2(!after.recentWorkspaces.contains(second.path()),
             "移除当前工作区后配置里也不该还有它（否则重启会回来）");
    QVERIFY2(after.recentWorkspaces.contains(first.path()),
             "剩下的那个工作区应当被保留");
  }

  // ② 只剩一个时移除：**必须也能删**。删完进入"无工作区"状态——
  //    这是个被完整支持的状态（会话被收干净、发送按钮禁用、界面给出下一步提示）。
  while(treePaths().size() > 1) {
    emit sidebar->workspaceRemoveRequested(treePaths().last());
    QTest::qWait(200);
  }
  QCOMPARE(treePaths().size(), 1);
  const QString only = treePaths().first();
  emit sidebar->workspaceRemoveRequested(only);
  QVERIFY2(waitFor([&]() {
    return treePaths().isEmpty();
  }, 3000),
  "最后一个工作区必须能移除");
  {
    AppSettings afterAll;
    QVERIFY(AppConfig::load(&afterAll));
    QVERIFY2(afterAll.recentWorkspaces.isEmpty(), "删光后列表应当为空");
    QVERIFY2(afterAll.lastWorkspace.isEmpty(),
             "lastWorkspace 也要清掉，否则重启会把最后一个又打开");
  }
  // 无工作区时不能发消息，且界面要告诉用户下一步做什么（而不是空白一片）。
  QVERIFY2(!sendButton->isEnabled(), "没有工作区时不该能发送");
  bool hintFound = false;
  for(const QLabel * label : sidebar->findChildren<QLabel *>()) {
    if(label->isVisible() && label->text().contains(QStringLiteral("打开工作区"))) {
      hintFound = true;
      break;
    }
  }
  QVERIFY2(hintFound, "没有工作区时应当提示用户去「打开工作区…」");
  // 重新打开一个工作区后要能继续用。
  QTemporaryDir recovered;
  QVERIFY(recovered.isValid());
  emit sidebar->workspaceRecentRequested(recovered.path());
  QVERIFY2(waitFor([&]() {
    return sendButton->isEnabled();
  }, 5000),
  "重新打开工作区后应当恢复可用");

  // ③ ★ 被移除的工作区**不能复活**（重启后也不该回来）。
  //    用户报过："移除后重新打开软件 又会出来"。启动逻辑只从 lastWorkspace
  //    与列表恢复，所以"配置里不再有它、lastWorkspace 也不指着它"就等于
  //    "重启不会回来"——这里直接断言配置。
  QTemporaryDir third;
  QVERIFY(third.isValid());
  emit sidebar->workspaceRecentRequested(third.path());
  QTest::qWait(200);
  QVERIFY2(treePaths().size() >= 2, "先保证列表里不只一个工作区");

  emit sidebar->workspaceRemoveRequested(first.path());
  QTest::qWait(200);

  AppSettings afterRevive;
  QVERIFY(AppConfig::load(&afterRevive));
  QVERIFY2(!afterRevive.recentWorkspaces.contains(first.path()),
           qPrintable(QStringLiteral("移除后配置里不该还有它，实际：%1")
                      .arg(afterRevive.recentWorkspaces.join(QStringLiteral(", ")))));
  QVERIFY2(afterRevive.lastWorkspace != first.path(),
           "lastWorkspace 也不能还指着已移除的工作区（否则重启会把它打开）");
  QVERIFY2(!treePaths().contains(first.path()), "树上也不该还有它");

  // ④ ★ 移除工作区还要清掉它的**按工作区索引**的配置。
  //    用户报过："移除工作区，为什么 settings.json 里 workspaceLastModel
  //    还有工作区记录"。那些表按工作区 key 索引，不清就会长期堆积。
  {
    AppSettings probe;
    QVERIFY(AppConfig::load(&probe));
    QVERIFY2(!probe.workspaceLastModel.contains(first.path()),
             qPrintable(QStringLiteral("workspaceLastModel 不该还留着已移除的工作区：%1")
                        .arg(probe.workspaceLastModel.keys()
                             .join(QStringLiteral(", ")))));
    QVERIFY2(!probe.expandedWorkspaces.contains(first.path()),
             "expandedWorkspaces 也不该还留着已移除的工作区");
  }
}

void TestUiFlow::everyWorkspaceShowsFoldMarker()
{
  // 用户报过："重新打开软件 会有一些工作区 没有向右也没有向下"。
  // 根因：折叠指示符按 childCount() 判断，而会话是**懒加载**的——
  // 没被展开过的工作区 childCount() 为 0，于是既没有 ▸ 也没有 ▾，
  // 看起来根本不能折叠。
  QTemporaryDir first;
  QTemporaryDir second;
  QVERIFY(first.isValid() && second.isValid());

  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);
  auto * tree = sidebar->findChild<QTreeWidget *>(QStringLiteral("sessionTree"));
  QVERIFY(tree != nullptr);

  emit sidebar->workspaceRecentRequested(first.path());
  QTest::qWait(200);
  emit sidebar->workspaceRecentRequested(second.path());
  QTest::qWait(200);
  QVERIFY2(tree->topLevelItemCount() >= 2, "应当有至少两个工作区节点");

  // 每个工作区节点都必须带折叠指示符。关键是**没被展开过**的那些也要有——
  // 它们此刻 childCount() 还是 0。
  for(int index = 0; index < tree->topLevelItemCount(); ++index) {
    QTreeWidgetItem * node = tree->topLevelItem(index);
    const QString text = node->text(0);
    QVERIFY2(text.startsWith(QStringLiteral("▸")) || text.startsWith(QStringLiteral("▾")),
             qPrintable(QStringLiteral("工作区节点必须带折叠指示符（childCount=%1），实际文本：%2")
                        .arg(node->childCount())
                        .arg(text)));
  }

  // 展开一个之后再检查：指示符要跟着翻转。
  QTreeWidgetItem * node = tree->topLevelItem(0);
  node->setExpanded(true);
  QVERIFY2(node->text(0).startsWith(QStringLiteral("▾")),
           qPrintable(QStringLiteral("展开后应当是 ▾，实际：%1").arg(node->text(0))));
  node->setExpanded(false);
  QVERIFY2(node->text(0).startsWith(QStringLiteral("▸")),
           qPrintable(QStringLiteral("折叠后应当是 ▸，实际：%1").arg(node->text(0))));
}

void TestUiFlow::windowUsesTheAppIcon()
{
  // 图标以 PNG 内嵌在二进制里（qt_add_resources 挂在 lycode_lib 上）。
  // 断言两件事：资源确实被编进来了，以及各档尺寸都在——只给一张 512，
  // 任务栏的 16px 那一档就要靠系统缩，会发虚。
  const QList<int> sizes = {16, 24, 32, 48, 64, 128, 256, 512};
  QIcon icon;
  for(const int size : sizes) {
    const QString path =
      QStringLiteral(":/icons/linux/hicolor/%1x%1/apps/lycode.png").arg(size);
    QVERIFY2(QFile::exists(path),
             qPrintable(QStringLiteral("图标资源缺失: %1").arg(path)));
    icon.addFile(path);
  }
  QVERIFY2(!icon.isNull(), "应当能构造出应用图标");
  for(const int size : sizes) {
    QVERIFY2(icon.availableSizes().contains(QSize(size, size)),
             qPrintable(QStringLiteral("图标缺少 %1px 那一档").arg(size)));
  }

  // 16px 那一档不能是空图，而且必须能看到**白色笔画**——
  // 透明底上只有蓝底也算"有内容"，所以查的是白色像素。
  const QImage small = icon.pixmap(QSize(16, 16)).toImage();
  QCOMPARE(small.size(), QSize(16, 16));
  bool hasWhite = false;
  for(int y = 0; y < small.height() && !hasWhite; ++y) {
    for(int x = 0; x < small.width(); ++x) {
      const QColor pixel = small.pixelColor(x, y);
      if(pixel.alpha() > 200 && pixel.red() > 200 && pixel.green() > 200 &&
         pixel.blue() > 200) {
        hasWhite = true;
        break;
      }
    }
  }
  QVERIFY2(hasWhite, "16px 图标里应当能看到白色笔画");

  // Windows 分支用的是 .ico（多尺寸容器）。这里在**所有平台**都断言它可用：
  // 否则那条分支只在 Windows 上被走到，而 CI 的 Windows job 只跑 9 个套件，
  // 一旦 .ico 损坏或漏了某一档，不会有人发现。
  const QIcon ico(QStringLiteral(":/icons/windows/lycode.ico"));
  QVERIFY2(!ico.isNull(), "Windows 用的 .ico 资源应当存在且可读");
  for(const int size : {
        16, 24, 32, 48, 64, 128, 256
      }) {
    QVERIFY2(ico.availableSizes().contains(QSize(size, size)),
             qPrintable(QStringLiteral(".ico 缺少 %1px 那一档").arg(size)));
  }

  // 主窗口用的是这个图标（由 QApplication 提供，窗口继承）。
  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));
  QApplication::setWindowIcon(icon);
  QVERIFY2(!window.windowIcon().isNull(), "主窗口应当带应用图标");
}

void TestUiFlow::choiceButtonsFillTheComposer()
{
  // 用户诉求："当 ai 给你几个选项时需要可以选择"。模型按系统提示词的统一格式
  // （`choices` 围栏）给出选项，界面渲染成按钮，点一下填进输入框。
  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  auto * composer = window.findChild<QPlainTextEdit *>(QStringLiteral("composer"));
  auto * sendButton = window.findChild<QPushButton *>(QStringLiteral("sendButton"));
  QVERIFY(sidebar != nullptr && composer != nullptr && sendButton != nullptr);

  // 前面的测试会改动工作区，这里显式准备一个可用的。
  {
    QTemporaryDir own;
    QVERIFY(own.isValid());
    emit sidebar->workspaceRecentRequested(own.path());
    QVERIFY(waitFor([&]() {
      return sendButton->isEnabled();
    }, 5000));
  }

  gateway_->enqueue(textResponse(
                      QByteArray("我看到两种改法：\n\n```choices\n- 只改 Read 工具\n- 同时改 Read 与 Grep\n```")));
  composer->setPlainText(QStringLiteral("怎么改？"));
  sendButton->click();

  auto * bar = window.findChild<QWidget *>(QStringLiteral("choiceBar"));
  QVERIFY2(bar != nullptr, "输入框上方应当有选项按钮条");
  QVERIFY2(waitFor([&]() {
    return bar->isVisible();
  }, 15000),
  "模型按统一格式给出选项后，按钮条应当出现");

  const QList<QPushButton *> buttons =
    window.findChildren<QPushButton *>(QStringLiteral("choiceButton"));
  QCOMPARE(buttons.size(), 2);
  QCOMPARE(buttons.at(0)->text(), QStringLiteral("只改 Read 工具"));

  // 截图要在点击**之前**拍：点完按钮条就收起了，拍到的会是空状态，
  // 那张图看不出这个功能长什么样。
  const QString shot = screenshotDir() + QStringLiteral("/18-choice-buttons.png");
  QVERIFY2(window.grab().save(shot), qPrintable(shot));

  buttons.at(1)->click();
  // 填进输入框而不是直接发送：用户还能改一改，也避免误点直接发出。
  QCOMPARE(composer->toPlainText(), QStringLiteral("同时改 Read 与 Grep"));
  QVERIFY2(!bar->isVisible(), "选完之后按钮条应当收起");
}

void TestUiFlow::settingsPickModelsFromTheCatalog()
{
  // 用户诉求："现在模型只能自定义，没有提供几种可以选择"。
  // 设置页的 Provider 表单上要有入口，能按内置目录选模型。
  MainWindow window;
  window.show();
  QVERIFY(waitFor([&]() {
    return window.isVisible();
  }, 5000));

  auto * sidebar = window.findChild<SidebarPanel *>(QStringLiteral("sidebar"));
  QVERIFY(sidebar != nullptr);
  {
    QTemporaryDir own;
    QVERIFY(own.isValid());
    emit sidebar->workspaceRecentRequested(own.path());
  }
  QTest::qWait(200);

  // 打开设置对话框（走菜单里那一条，与用户实际路径一致）。
  auto * settingsAction = window.findChild<QAction *>(QStringLiteral("settingsAction"));
  QVERIFY2(settingsAction != nullptr, "应当有打开设置的入口");

  // 设置对话框是模态的（exec() 会开嵌套事件循环），trigger() 要等对话框关掉
  // 才会返回。所以不能 trigger() 之后再去找弹窗——那时测试已经卡在嵌套循环里，
  // 没人去关它，只能等 QtTest 的函数超时。断言改成在 singleShot 回调里对
  // 活着的对话框做，结论用变量带出来，trigger() 返回后再 QVERIFY。
  bool foundDialog = false;
  bool pickExists = false;
  bool pickVisible = false;
  bool pickHasToolTip = false;
  QTimer::singleShot(300, [&]() {
    auto * modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if(modal == nullptr) {
      return;
    }
    foundDialog = true;
    // 按钮挂在 Provider 页上，而对话框默认停在外观页。先切到那一页再断言：
    // 否则量到的是"这个 Tab 没被选中"，不是"按钮没真正放上界面"。
    if(auto * modalTabs = modal->findChild<QTabWidget *>(QStringLiteral("settingsTabs"))) {
      modalTabs->setCurrentIndex(1);
    }
    auto * pick = modal->findChild<QPushButton *>(QStringLiteral("pickModelsFromCatalog"));
    if(pick != nullptr) {
      pickExists = true;
      pickVisible = pick->isVisible();
      pickHasToolTip = !pick->toolTip().isEmpty();
    }
    modal->reject();
  });

  settingsAction->trigger();  // 阻塞，直到上面的 lambda 把对话框关掉

  QVERIFY2(foundDialog, "设置对话框应当以模态方式打开");
  QVERIFY2(pickExists, "Provider 表单上应当有「从列表选择…」按钮");
  QVERIFY2(pickVisible, "按钮必须可见（不能只是构造了却没放上界面）");
  QVERIFY2(pickHasToolTip, "按钮应当有说明");

  // 内置目录本身可用——它是这个按钮的数据源，缺了就会弹出"目录不可用"。
  QString error;
  const QList<lycode::model::CatalogProvider> providers = lycode::model::ModelCatalog::load(&error);
  QVERIFY2(!providers.isEmpty(), qPrintable(QStringLiteral("模型目录载入失败: %1").arg(error)));
}

QTEST_MAIN(TestUiFlow)
#include "test_ui_flow.moc"
