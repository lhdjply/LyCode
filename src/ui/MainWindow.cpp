#include "ui/MainWindow.h"

#include "core/Ids.h"
#include "core/Json.h"
#include "core/Logging.h"
#include "mcp/McpManager.h"
#include "skills/SkillLibrary.h"
#include "tools/BackgroundTaskRegistry.h"
#include "tools/TodoStore.h"
#include "ui/ConversationView.h"
#include "ui/Markdown.h"
#include "model/ReasoningLevels.h"
#include "ui/PermissionDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/SidebarPanel.h"
#include "ui/Theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QMenuBar>
#include <QBuffer>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace lycode::ui
{
namespace
{
/// 第一次请求最多为 MCP 握手等多久。卡住的服务器不该把用户的输入一起卡住。
constexpr int kMcpWaitTimeoutMs = 8000;
/// 等待期间的重试间隔。
constexpr int kMcpPollIntervalMs = 150;
}  // namespace

namespace
{

Q_LOGGING_CATEGORY(log, "lycode.ui.main")

/// 上下文用量超过该比例时用警示色，提示用户即将触发压缩。
constexpr double kContextWarnPercent = 75.0;
constexpr double kContextCriticalPercent = 90.0;
/// 用量与运行时长刷新间隔。
constexpr int kTickIntervalMs = 1000;

QString runStateText(RunState state)
{
  switch(state) {
    case RunState::Idle:
      return QStringLiteral("就绪");
    case RunState::Streaming:
      return QStringLiteral("正在生成…");
    case RunState::ExecutingTools:
      return QStringLiteral("正在执行工具…");
    case RunState::WaitingPermission:
      return QStringLiteral("等待你确认…");
    case RunState::Cancelling:
      return QStringLiteral("正在中断…");
    case RunState::Failed:
      return QStringLiteral("出错");
  }
  return QStringLiteral("就绪");
}

/// 状态栏用的紧凑 token 数：用 k/M 缩写，原始数字在 tooltip 里给全。
/// 超过 100 万时用 M：1000000 写成 "1000.0k" 既长又难读，写成 "1.0M" 一眼就懂。
QString compactTokens(int value)
{
  if(value >= 1000000) {
    return QStringLiteral("%1M").arg(value / 1000000.0, 0, 'f', 1);
  }
  if(value >= 1000) {
    return QStringLiteral("%1k").arg(value / 1000.0, 0, 'f', 1);
  }
  return QString::number(value);
}

/// 状态栏的上下文文案：分母 + 自动压缩阈值。
///
/// 阈值必须显示出来：用户看到进度条涨到某个位置时会自动压缩，不该靠猜。
/// 分母未知（新会话、模型没配好）时退回只显示"上下文"。
QString contextUsageText(const QJsonObject & usage)
{
  const int used = json::integer(usage, QStringLiteral("usedTokens"));
  const int max = json::integer(usage, QStringLiteral("maxTokens"));
  if(max <= 0) {
    return {};
  }
  const int threshold = json::integer(usage, QStringLiteral("autoCompactThresholdTokens"));
  if(threshold > 0) {
    return QStringLiteral("上下文 %1 / %2 · 自动压缩于 %3")
           .arg(compactTokens(used), compactTokens(max), compactTokens(threshold));
  }
  return QStringLiteral("上下文 %1 / %2").arg(compactTokens(used), compactTokens(max));
}

/// 上下文进度的悬浮说明：把两个阈值与"当前做过压缩"讲清楚。
QString contextTooltipText(const QJsonObject & usage)
{
  const int used = json::integer(usage, QStringLiteral("usedTokens"));
  const int max = json::integer(usage, QStringLiteral("maxTokens"));
  if(max <= 0) {
    return {};
  }
  const int micro = json::integer(usage, QStringLiteral("autoCompactThresholdTokens"));
  const int full = json::integer(usage, QStringLiteral("fullCompactThresholdTokens"));

  QStringList lines;
  lines.append(QStringLiteral("已用 %1 / %2 tokens").arg(used).arg(max));
  if(micro > 0) {
    lines.append(QStringLiteral("· 达到 %1 起：下发时裁剪旧工具输出（不调模型）").arg(micro));
  }
  if(full > 0) {
    lines.append(QStringLiteral("· 达到 %1 起：调用模型生成摘要，替换较早的历史").arg(full));
  }
  if(json::boolean(usage, QStringLiteral("compacted"))) {
    lines.append(QStringLiteral("· 本会话已经压缩过上下文，原始消息保存在归档里"));
  }
  lines.append(QStringLiteral("输入 /compact 可立即压缩。"));
  return lines.join(QLatin1Char('\n'));
}

/// 用量明细文案。字段口径见 Usage 的注释：
///   未缓存 = 真正要模型从头处理的输入；缓存读 = 命中缓存的部分；
///   输出 = 生成的 token；命中率分母不含"写入缓存"（那部分本来没机会命中）。
QString usageBreakdownText(const Usage & usage)
{
  return QStringLiteral("缓存 %1% · 未缓存 %2 · 缓存读 %3 · 输出 %4")
         .arg(qRound(usage.cacheHitRate() * 100.0))
         .arg(compactTokens(usage.inputTokens), compactTokens(usage.cacheReadTokens),
              compactTokens(usage.outputTokens));
}

/// 用量明细的悬浮说明：给出精确数字与口径，避免缩写带来的歧义。
QString usageTooltipText(const Usage & cumulative, const Usage & lastTurn)
{
  const auto block = [](const QString & title, const Usage & usage) {
    return QStringLiteral("%1\n"
                          "  未缓存输入  %2\n"
                          "  缓存读取    %3\n"
                          "  缓存写入    %4\n"
                          "  输出        %5\n"
                          "  缓存命中率  %6%")
           .arg(title)
           .arg(usage.inputTokens)
           .arg(usage.cacheReadTokens)
           .arg(usage.cacheWriteTokens)
           .arg(usage.outputTokens)
           .arg(qRound(usage.cacheHitRate() * 100.0));
  };

  return QStringLiteral("缓存命中率 = 缓存读取 /（缓存读取 + 未缓存输入）。\n"
                        "分母不含缓存写入：首次写入的部分本来就没有机会命中。\n\n%1\n\n%2")
         .arg(block(QStringLiteral("本次会话累计"), cumulative),
              block(QStringLiteral("最近一轮"), lastTurn));
}

}  // namespace

MainWindow::MainWindow(QWidget * parent) : QMainWindow(parent)
{
  setWindowTitle(QStringLiteral("LyCode"));
  resize(1280, 820);
  setMinimumSize(900, 600);

  // ── 配置 ────────────────────────────────────────────────────────────────
  QString configError;
  if(!AppConfig::load(&settings_, &configError)) {
    // 配置损坏不是致命错误：用默认值继续，让用户能在设置里修回来。
    qCWarning(log) << "配置载入失败，使用默认值:" << configError;
    statusBar()->showMessage(QStringLiteral("配置载入失败，已使用默认设置：") + configError, 8000);
  }

  Theme::instance().setMode(settings_.themeMode);
  Theme::instance().setUiFontSize(settings_.uiFontSize);
  Theme::instance().setCodeFontSize(settings_.codeFontSize);

  // ── 存储与运行时 ────────────────────────────────────────────────────────
  if(!store_.open()) {
    qCCritical(log) << "会话数据库打开失败:" << store_.lastError();
    QMessageBox::warning(this, QStringLiteral("会话存储不可用"),
                         QStringLiteral("无法打开会话数据库：\n%1\n\n"
                                        "本次运行不会保存历史记录。")
                         .arg(store_.lastError()));
  }

  todoStore_ = std::make_unique<TodoStore>();

  // 必须显式注册内置工具：ToolRegistry 默认构造出来是**空**的。
  // 漏掉这一步的表现是模型收到"零个工具"、所有工具调用都报 tool_not_found，
  // 而且不会有任何编译期或启动期报错——只能在端到端跑一次才会暴露。
  tools_ = ToolRegistry::createWithBuiltins();

  providers_.replaceAll(settings_.providers);
  providers_.setModelOverrides(settings_.modelOverrides);

  runtime_.setProviderRegistry(&providers_);
  runtime_.setToolRegistry(&tools_);

  // MCP：按配置拉起服务器。**异步**——不阻塞界面启动；服务器就绪后再把
  // 它的工具注册进 tools_，所以模型第一次请求可能还不包含这些工具，
  // 下一次请求就会包含（工具声明是每次请求重新构建的）。
  // Skills：扫描用户级与项目级目录。换工作区时会重扫（见 applyWorkspace）。
  rescanSkills();

  mcp_ = std::make_unique<mcp::Manager>();
  connect(mcp_.get(), &mcp::Manager::serverReady, this, [this](const QString & serverId, int count) {
    const int added = mcp_->registerToolsInto(tools_);
    qCInfo(log) << "MCP 服务器就绪; id=" << serverId << "工具数=" << count
                << "本次新增=" << added;
    onMcpChanged();
  });
  connect(mcp_.get(), &mcp::Manager::serverFailed, this,
  [this](const QString & serverId, const QString & reason) {
    qCWarning(log) << "MCP 服务器失败; id=" << serverId << "reason=" << reason;
    setStatusMessage(QStringLiteral("MCP 服务器「%1」不可用：%2")
                     .arg(serverId, reason));
    onMcpChanged();
  });
  connect(mcp_.get(), &mcp::Manager::changed, this, &MainWindow::onMcpChanged);
  runtime_.setSessionStore(&store_);
  runtime_.setTodoStore(todoStore_.get());

  // ── 工作区 ──────────────────────────────────────────────────────────────
  workspace_.path = settings_.lastWorkspace;
  if(!workspace_.path.isEmpty() && !QDir(workspace_.path).exists()) {
    // 目录已被删掉/移走：不保留一个不存在的工作区。
    qCWarning(log) << "上次的工作区目录已不存在，回退到无工作区:" << workspace_.path;
    workspace_.path.clear();
  }
  // 没有任何工作区时**不**用当前目录兜底：那会让"移除最后一个工作区"
  // 在重启后复活。停在无工作区状态，由界面引导用户去打开一个目录
  //（首次运行也一样——提示语已经说清了下一步该做什么）。

  buildUi();
  buildMenus();
  wireRuntime();
  applyTheme();
  applySettingsToUi();

  // 启动时的工作区也要进列表：否则它只以"当前工作区"的身份出现在树上，
  // 一旦切到别的目录，它就从树上消失了——用户会以为工作区被删了。
  if(!workspace_.path.isEmpty() && !settings_.recentWorkspaces.contains(workspace_.path)) {
    settings_.recentWorkspaces.prepend(workspace_.path);
    saveSettings();
  }

  // 清掉历史遗留的孤儿记录：早先版本移除工作区时没有清理这些表，
  // 于是 settings.json 里可能还留着已经不存在的工作区。启动时统一对账一次。
  {
    const QStringList known = settings_.recentWorkspaces;
    int pruned = 0;
    for(auto it = settings_.workspaceLastModel.begin();
        it != settings_.workspaceLastModel.end();) {
      if(!known.contains(it.key())) {
        it = settings_.workspaceLastModel.erase(it);
        ++pruned;
      }
      else {
        ++it;
      }
    }
    const int before = settings_.expandedWorkspaces.size();
    for(const QString & path : std::as_const(settings_.expandedWorkspaces)) {
      if(!known.contains(path)) {
        settings_.expandedWorkspaces.removeAll(path);
      }
    }
    pruned += before - settings_.expandedWorkspaces.size();
    if(pruned > 0) {
      qCInfo(log) << "已清理不再存在的工作区配置; 条数=" << pruned;
      saveSettings();
    }
  }


  sidebar_->setWorkspace(workspace_);
  sidebar_->setRecentWorkspaces(settings_.recentWorkspaces);
  // 恢复上次退出时的展开状态。没有记录时（首次运行）只展开当前工作区——
  // 全部展开会在启动时把每个工作区的会话都查一遍，最近列表长的时候会拖慢启动。
  if(settings_.expandedWorkspaces.isEmpty()) {
    sidebar_->setExpandedWorkspaces({workspace_.path});
  }
  else {
    sidebar_->setExpandedWorkspaces(settings_.expandedWorkspaces);
  }
  if(store_.isOpen()) {
    loadWorkspaceSessions();
  }

  // MCP 服务器在界面搭好之后再拉起：失败时要能通过状态栏告知用户，
  // 太早启动的话那些提示还没有落点。
  if(!settings_.mcpServers.isEmpty()) {
    qCInfo(log) << "启动 MCP 服务器; 数量=" << settings_.mcpServers.size();
    mcp_->startAll(settings_.mcpServers);
  }

  // 运行时长与用量的低频刷新。放在定时器里而不是每次增量都算，
  // 避免高频更新状态栏造成不必要的重绘。
  usageTimer_ = new QTimer(this);
  usageTimer_->setInterval(kTickIntervalMs);
  connect(usageTimer_, &QTimer::timeout, this, [this]() {
    refreshContextUsage();
    refreshRunState();
  });
  usageTimer_->start();

  qCInfo(log) << "主窗口已就绪; workspace=" << workspace_.path
              << "providers=" << settings_.providers.size();

  QTimer::singleShot(0, this, &MainWindow::openSessionOrCreate);
}

MainWindow::~MainWindow() = default;

// ─────────────────────────────────────────────────────────────────────────────
// 界面构建
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::buildUi()
{
  auto * central = new QWidget;
  auto * rootLayout = new QVBoxLayout(central);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  // ── 主体：侧边栏 + 对话 ─────────────────────────────────────────────────
  splitter_ = new QSplitter(Qt::Horizontal);
  sidebar_ = new SidebarPanel;
  sidebar_->setObjectName(QStringLiteral("sidebar"));
  sidebar_->setMinimumWidth(200);
  sidebar_->setMaximumWidth(420);
  conversation_ = new ConversationView;
  conversation_->setObjectName(QStringLiteral("conversation"));

  splitter_->addWidget(sidebar_);
  splitter_->addWidget(conversation_);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  // 从 260 加宽到 300：树形结构有缩进，还要留出右侧的 "+" 列，
  // 260 的时候正文只剩 190px，两行的会话条目第二行几乎整行被省略成"…"。
  splitter_->setSizes({300, 980});
  rootLayout->addWidget(splitter_, 1);

  // ── 输入区 ──────────────────────────────────────────────────────────────
  auto * composerFrame = new QWidget;
  composerFrame->setObjectName(QStringLiteral("composerFrame"));
  auto * composerLayout = new QVBoxLayout(composerFrame);
  composerLayout->setContentsMargins(16, 8, 16, 12);
  composerLayout->setSpacing(6);

  // 附件条放在输入框**上方**：它描述的是"这次要发什么"，
  // 与输入框同属一个编辑单元，因此必须紧贴输入框而不是放到工具条上。
  attachmentStrip_ = new QWidget;
  attachmentStrip_->setObjectName(QStringLiteral("attachmentStrip"));
  attachmentLayout_ = new QHBoxLayout(attachmentStrip_);
  attachmentLayout_->setContentsMargins(0, 0, 0, 0);
  attachmentLayout_->setSpacing(6);
  attachmentStrip_->hide();  // 没有附件时不占位置
  composerLayout->addWidget(attachmentStrip_);

  // 选项按钮条：位置在附件条与输入框之间。放在这里而不是对话流里，
  // 是因为它描述的是"这次要输入什么"，与输入框同属一个操作单元。
  choiceBar_ = new QWidget;
  choiceBar_->setObjectName(QStringLiteral("choiceBar"));
  choiceLayout_ = new QHBoxLayout(choiceBar_);
  choiceLayout_->setContentsMargins(0, 0, 0, 0);
  choiceLayout_->setSpacing(6);
  choiceBar_->hide();
  composerLayout->addWidget(choiceBar_);

  composer_ = new QPlainTextEdit;
  composer_->setObjectName(QStringLiteral("composer"));
  composer_->setPlaceholderText(
    QStringLiteral("描述你想完成的任务…（Enter 发送，Shift+Enter 换行，可粘贴图片）\n"
                   "输入 /compact 压缩上下文，/help 查看命令"));
  composer_->setFixedHeight(96);
  composer_->installEventFilter(this);
  // 拖入图片文件即可附加。
  composer_->setAcceptDrops(true);
  composerLayout->addWidget(composer_);

  // ── 操作行：模式在左，模型/思考/停止/发送在右 ───────────────────────────
  // 这三个选择器都放进输入区而不是顶部工具条：它们影响的是"这一次发送"，
  // 贴着输入框才符合"先设参数再发送"的操作顺序，也省掉一整条顶部横栏。
  auto * actionRow = new QHBoxLayout;
  actionRow->setContentsMargins(0, 0, 0, 0);
  actionRow->setSpacing(8);

  modeCombo_ = new QComboBox;
  modeCombo_->setObjectName(QStringLiteral("modeCombo"));
  modeCombo_->setToolTip(
    QStringLiteral("会话模式：plan 只读规划 / build 默认 / edit 编辑 / yolo 免确认"));
  modeCombo_->addItem(QStringLiteral("plan 只读规划"), static_cast<int>(SessionMode::Plan));
  modeCombo_->addItem(QStringLiteral("build 默认"), static_cast<int>(SessionMode::Build));
  modeCombo_->addItem(QStringLiteral("edit 编辑"), static_cast<int>(SessionMode::Edit));
  modeCombo_->addItem(QStringLiteral("yolo 免确认"), static_cast<int>(SessionMode::Yolo));
  connect(modeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onModeChanged);
  actionRow->addWidget(modeCombo_);

  attachButton_ = new QPushButton(QStringLiteral("图片"));
  attachButton_->setObjectName(QStringLiteral("attachButton"));
  attachButton_->setToolTip(QStringLiteral("附加图片（也可以直接 Ctrl+V 粘贴或拖入）"));
  connect(attachButton_, &QPushButton::clicked, this, &MainWindow::onAttachImagesRequested);
  actionRow->addWidget(attachButton_);

  actionRow->addStretch(1);

  modelCombo_ = new QComboBox;
  // objectName 供样式表与 UI 级测试定位控件；改名要同步更新测试。
  modelCombo_->setObjectName(QStringLiteral("modelCombo"));
  modelCombo_->setMinimumWidth(180);
  modelCombo_->setToolTip(QStringLiteral("选择本会话使用的模型"));
  connect(modelCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onModelChanged);
  actionRow->addWidget(modelCombo_);

  // 思考等级下拉：紧挨模型选择器。它的可见性由模型是否支持思考决定，
  // 所以不支持的模型不会看到一个永远禁用的空控件。
  reasoningCombo_ = new QComboBox;
  reasoningCombo_->setObjectName(QStringLiteral("reasoningCombo"));
  reasoningCombo_->setToolTip(
    QStringLiteral("思考等级：越高推理越充分，消耗的 token 也越多"));
  connect(reasoningCombo_, &QComboBox::currentIndexChanged, this,
          &MainWindow::onReasoningLevelChanged);
  actionRow->addWidget(reasoningCombo_);

  stopButton_ = new QPushButton(QStringLiteral("停止"));
  stopButton_->setObjectName(QStringLiteral("stopButton"));
  stopButton_->setEnabled(false);
  connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStopRequested);
  actionRow->addWidget(stopButton_);

  sendButton_ = new QPushButton(QStringLiteral("发送"));
  sendButton_->setObjectName(QStringLiteral("sendButton"));
  sendButton_->setProperty("accent", true);
  connect(sendButton_, &QPushButton::clicked, this, &MainWindow::onSendRequested);
  actionRow->addWidget(sendButton_);

  composerLayout->addLayout(actionRow);
  rootLayout->addWidget(composerFrame);

  setCentralWidget(central);

  // ── 状态栏 ──────────────────────────────────────────────────────────────
  // 上下文用量原先在顶部工具条右侧。三个选择器搬进输入区后顶部就空了，
  // 与其留一条只有一项的横栏，不如把它并入状态栏（这里本来就在展示用量）。
  // ⚠ 一律用 addPermanentWidget，不用 addWidget。
  // QStatusBar::showMessage() 会**隐藏所有普通控件**（addWidget 添加的），
  // 只保留 permanent 的。用它的话，"就绪/正在生成"与后台任务提示会在每次
  // 状态消息出现的几秒里消失——而那恰恰是用户最需要看到它们的时刻（实测踩到）。
  runStateLabel_ = new QLabel(runStateText(RunState::Idle));
  statusBar()->addPermanentWidget(runStateLabel_);

  // 后台任务计数。没有它用户根本不知道还有进程在跑——后台任务的全部意义
  // 就是"离开视线继续工作"，因此必须有个常驻的可见提示。
  // MCP 连接数。与后台任务一样用 permanent：状态消息不该把它藏起来。
  mcpLabel_ = new QLabel;
  mcpLabel_->setObjectName(QStringLiteral("mcpLabel"));
  mcpLabel_->setFont(Theme::instance().font(FontRole::UiXs));
  statusBar()->addPermanentWidget(mcpLabel_);

  backgroundLabel_ = new QLabel;
  backgroundLabel_->setObjectName(QStringLiteral("backgroundLabel"));
  backgroundLabel_->setFont(Theme::instance().font(FontRole::UiXs));
  statusBar()->addPermanentWidget(backgroundLabel_);

  // 主动拉一次初始计数：后台任务注册表在**构造时**就完成了账本对账
  // （认领上次退出时遗留的任务），那时界面还没接上信号。不拉这一下，
  // 重启后被认领的任务就完全不可见——而那正是"跨重启"唯一的可见结果。
  onBackgroundTasksChanged(runtime_.backgroundTasks()->runningCount());

  contextLabel_ = new QLabel;
  contextLabel_->setObjectName(QStringLiteral("contextLabel"));
  contextLabel_->setFont(Theme::instance().font(FontRole::UiXs));
  // 预留最宽可能的文案宽度：否则用户把窗口调到 200 万时最后一位会被裁掉
  // （QLabel 在布局里可以被压缩到 sizeHint 以下）。用字体度量而不是写死像素，
  // 这样界面字号变化也仍然够用。
  contextLabel_->setMinimumWidth(
    QFontMetrics(contextLabel_->font())
    .horizontalAdvance(QStringLiteral("上下文 000.0 / 2000.0M")) +
    8);
  statusBar()->addPermanentWidget(contextLabel_);

  contextBar_ = new QProgressBar;
  contextBar_->setRange(0, 100);
  contextBar_->setValue(0);
  contextBar_->setTextVisible(false);
  contextBar_->setFixedWidth(120);
  contextBar_->setFixedHeight(6);
  statusBar()->addPermanentWidget(contextBar_);

  usageLabel_ = new QLabel;
  usageLabel_->setObjectName(QStringLiteral("usageLabel"));
  usageLabel_->setFont(Theme::instance().font(FontRole::UiXs));
  // 预留最宽文案宽度，否则四个数字涨上去后末尾会被裁（QLabel 可被压缩到
  // sizeHint 以下）。用字体度量而不是写死像素，界面字号变化也仍然够用。
  usageLabel_->setMinimumWidth(
    QFontMetrics(usageLabel_->font())
    .horizontalAdvance(QStringLiteral("缓存 100% · 未缓存 000.0M · 缓存读 000.0M · 输出 000.0M")) +
    12);
  statusBar()->addPermanentWidget(usageLabel_);
}

void MainWindow::buildMenus()
{
  QMenu * fileMenu = menuBar()->addMenu(QStringLiteral("文件"));

  QAction * newSession = fileMenu->addAction(QStringLiteral("新建会话"));
  newSession->setShortcut(QKeySequence::New);
  connect(newSession, &QAction::triggered, this, &MainWindow::onNewSessionRequested);

  QAction * openWorkspace = fileMenu->addAction(QStringLiteral("打开工作区…"));
  openWorkspace->setShortcut(QKeySequence::Open);
  connect(openWorkspace, &QAction::triggered, this, &MainWindow::onWorkspaceChangeRequested);

  fileMenu->addSeparator();
  QAction * settings = fileMenu->addAction(QStringLiteral("设置…"));
  settings->setObjectName(QStringLiteral("settingsAction"));
  settings->setShortcut(QKeySequence::Preferences);
  connect(settings, &QAction::triggered, this, &MainWindow::onSettingsRequested);

  fileMenu->addSeparator();
  QAction * quit = fileMenu->addAction(QStringLiteral("退出"));
  quit->setShortcut(QKeySequence::Quit);
  connect(quit, &QAction::triggered, this, &QWidget::close);

  QMenu * viewMenu = menuBar()->addMenu(QStringLiteral("视图"));

  // 主题切换做成互斥项，当前项打勾，避免用户不知道现在是哪一档。
  auto * themeGroup = new QActionGroup(this);
  themeGroup->setExclusive(true);
  struct ThemeEntry {
    const char * label;
    ThemeMode mode;
  };
  for(const ThemeEntry & entry : {
        ThemeEntry{"跟随系统", ThemeMode::System},
        ThemeEntry{"浅色", ThemeMode::Light},
        ThemeEntry{"深色", ThemeMode::Dark}
      }) {
    QAction * action = viewMenu->addAction(QString::fromUtf8(entry.label));
    action->setCheckable(true);
    action->setChecked(settings_.themeMode == entry.mode);
    themeGroup->addAction(action);
    const ThemeMode mode = entry.mode;
    connect(action, &QAction::triggered, this, [this, mode]() {
      settings_.themeMode = mode;
      Theme::instance().setMode(mode);
      saveSettings();
    });
  }

  viewMenu->addSeparator();
  QAction * zoomIn = viewMenu->addAction(QStringLiteral("增大字号"));
  zoomIn->setShortcut(QKeySequence::ZoomIn);
  connect(zoomIn, &QAction::triggered, this, [this]() {
    settings_.uiFontSize = qMin(24, settings_.uiFontSize + 1);
    Theme::instance().setUiFontSize(settings_.uiFontSize);
    saveSettings();
  });

  QAction * zoomOut = viewMenu->addAction(QStringLiteral("减小字号"));
  zoomOut->setShortcut(QKeySequence::ZoomOut);
  connect(zoomOut, &QAction::triggered, this, [this]() {
    settings_.uiFontSize = qMax(10, settings_.uiFontSize - 1);
    Theme::instance().setUiFontSize(settings_.uiFontSize);
    saveSettings();
  });

  QMenu * helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
  QAction * about = helpMenu->addAction(QStringLiteral("关于 LyCode"));
  connect(about, &QAction::triggered, this, [this]() {
    QMessageBox::about(
      this, QStringLiteral("关于 LyCode"),
      QStringLiteral("<b>LyCode</b> %1<br/><br/>"
                     "AI 编程工作台 —— Qt6 / C++ 原生实现。<br/>"
                     "基于 LyCode 的设计规范与 Agent 语义重写。")
      .arg(QStringLiteral(LYCODE_QT_VERSION)));
  });
}

void MainWindow::wireRuntime()
{
  connect(&runtime_, &AgentRuntime::messageAdded, this, &MainWindow::onMessageAdded);
  connect(&runtime_, &AgentRuntime::messageFinished, this, &MainWindow::onMessageFinished);
  connect(&runtime_, &AgentRuntime::partAppended, this, &MainWindow::onPartAppended);
  connect(&runtime_, &AgentRuntime::partUpdated, this, &MainWindow::onPartUpdated);
  connect(&runtime_, &AgentRuntime::deltaAppended, this, &MainWindow::onDeltaAppended);
  connect(&runtime_, &AgentRuntime::sessionChanged, this, &MainWindow::onSessionChanged);
  connect(&runtime_, &AgentRuntime::runStateChanged, this, &MainWindow::onRunStateChanged);
  connect(&runtime_, &AgentRuntime::permissionRequested, this,
          &MainWindow::onPermissionRequested);
  connect(&runtime_, &AgentRuntime::permissionResolved, this,
          &MainWindow::onPermissionResolved);
  connect(&runtime_, &AgentRuntime::turnFinished, this, &MainWindow::onTurnFinished);
  connect(&runtime_, &AgentRuntime::failed, this, &MainWindow::onFailed);
  // 压缩提示刻意不复用 onFailed：压缩失败不属于 turn 失败，
  // 显示成错误会让用户以为这轮对话出问题了。
  connect(&runtime_, &AgentRuntime::compactionNotice, this, &MainWindow::onCompactionNotice);
  connect(&runtime_, &AgentRuntime::conversationReplaced, this,
          &MainWindow::onConversationReplaced);
  connect(&runtime_, &AgentRuntime::subagentSessionChanged, this,
          &MainWindow::onSubagentSessionChanged);
  connect(&runtime_, &AgentRuntime::subagentFinished, this,
          &MainWindow::onSubagentFinished);
  connect(&runtime_, &AgentRuntime::backgroundTasksChanged, this,
          &MainWindow::onBackgroundTasksChanged);

  connect(sidebar_, &SidebarPanel::newSessionRequested, this,
          &MainWindow::onNewSessionRequested);
  // 工作区行上的 "+"：在那个工作区里新建会话。不在当前工作区就先切过去，
  // 否则新会话会建到别处——用户点的是那一行，期望落点也在那一行。
  // 展开状态变化就落盘：这是"下次打开还保持原样"的唯一依据。
  // 只在内存里记的话，用户每次重启都要重新展开一遍。
  connect(sidebar_, &SidebarPanel::workspaceExpansionChanged, this,
  [this](const QString & path, bool expanded) {
    if(path.isEmpty()) {
      return;
    }
    settings_.expandedWorkspaces.removeAll(path);
    if(expanded) {
      settings_.expandedWorkspaces.append(path);
    }
    saveSettings();
  });
  connect(sidebar_, &SidebarPanel::newSessionRequestedInWorkspace, this,
  [this](const QString & path) {
    if(!path.isEmpty() && QDir(path) != QDir(workspace_.path)) {
      onOpenWorkspacePath(path);
      // onOpenWorkspacePath 内部会在没有可用会话时新建一个，
      // 所以这里不用再建一次。
      return;
    }
    onNewSessionRequested();
  });
  connect(sidebar_, &SidebarPanel::sessionSelected, this, &MainWindow::onSessionSelected);
  // 展开一个还没加载过的工作区时，才去库里取它的会话——启动时不查，
  // 否则最近工作区多的时候会拖慢启动。
  connect(sidebar_, &SidebarPanel::workspaceExpandRequested, this,
  [this](const QString & path) {
    if(!store_.isOpen()) {
      return;
    }
    Workspace workspace;
    workspace.path = path;
    sidebar_->setWorkspaceSessions(path, store_.listSessions(workspace.key()));
  });
  connect(sidebar_, &SidebarPanel::sessionDeleteRequested, this,
          &MainWindow::onSessionDeleteRequested);
  connect(sidebar_, &SidebarPanel::workspaceChangeRequested, this,
          &MainWindow::onWorkspaceChangeRequested);
  connect(sidebar_, &SidebarPanel::workspaceRemoveRequested, this,
          &MainWindow::onWorkspaceRemoveRequested);
  connect(sidebar_, &SidebarPanel::workspacePurgeRequested, this,
          &MainWindow::onWorkspacePurgeRequested);
  connect(sidebar_, &SidebarPanel::workspaceRecentRequested, this,
          &MainWindow::onOpenWorkspacePath);
  connect(sidebar_, &SidebarPanel::settingsRequested, this, &MainWindow::onSettingsRequested);

  connect(&providers_, &ProviderRegistry::changed, this, &MainWindow::refreshModelCombo);
  connect(&Theme::instance(), &Theme::changed, this, &MainWindow::applyTheme);
}

void MainWindow::applyTheme()
{
  const Theme & theme = Theme::instance();
  const Palette & palette = theme.palette();

  setStyleSheet(theme.styleSheet() +
                QStringLiteral("QWidget#composerFrame { background-color: %1; "
                               "border-top: 1px solid %2; }")
                .arg(Theme::css(palette.backgroundAlt), Theme::css(palette.border)));

  composer_->setFont(theme.font(FontRole::UiBase));
  runStateLabel_->setFont(theme.font(FontRole::UiSm));
  contextLabel_->setStyleSheet(
    QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));
  usageLabel_->setStyleSheet(
    QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtlest)));

  // 进度条不能显示百分比文字（空间不够），用色块表达压力。
  contextBar_->setStyleSheet(
    QStringLiteral("QProgressBar { background-color: %1; border: none; border-radius: 3px; }"
                   "QProgressBar::chunk { background-color: %2; border-radius: 3px; }")
    .arg(Theme::css(palette.surface), Theme::css(palette.brand)));
}

void MainWindow::applySettingsToUi()
{
  // 顺序很重要：currentModel_ 必须先定下来。refreshModelCombo() 内部会调用
  // refreshReasoningCombo()，而后者要靠 currentModel_ 去查模型能力；
  // 反过来的话思考档位下拉会因为"没有当前模型"而被判为不可见（实测踩到）。
  currentModel_ = effectiveModelSelection();
  refreshModelCombo();

  suppressModeSignal_ = true;
  const int modeIndex = modeCombo_->findData(static_cast<int>(settings_.defaultSessionMode));
  if(modeIndex >= 0) {
    modeCombo_->setCurrentIndex(modeIndex);
  }
  suppressModeSignal_ = false;

  if(currentModel_.isValid()) {
    // modelCombo_ 的 itemData 不含思考档位，所以这里只同步选中项；
    // 档位由 refreshReasoningCombo() 负责恢复。
    const int index = modelCombo_->findData(currentModel_.displayValue());
    if(index >= 0) {
      suppressModelSignal_ = true;
      modelCombo_->setCurrentIndex(index);
      suppressModelSignal_ = false;
    }
  }
}

void MainWindow::refreshModelCombo()
{
  suppressModelSignal_ = true;
  modelCombo_->clear();

  const QList<ModelInfo> models = providers_.allModels();
  for(const ModelInfo & model : models) {
    ModelSelection selection;
    selection.providerId = model.providerId;
    selection.modelId = model.modelId;
    QString label = model.displayName.isEmpty() ? model.modelId : model.displayName;
    if(!model.supportsTools) {
      // 不支持工具调用的模型对本产品基本不可用，明确标注而不是隐藏。
      label += QStringLiteral("（不支持工具）");
    }
    modelCombo_->addItem(label, selection.displayValue());
  }

  if(models.isEmpty()) {
    modelCombo_->addItem(QStringLiteral("（未配置模型，请在设置中添加）"), QString());
    modelCombo_->setEnabled(false);
  }
  else {
    modelCombo_->setEnabled(true);
  }
  suppressModelSignal_ = false;

  refreshReasoningCombo();
}

void MainWindow::refreshReasoningCombo()
{
  if(reasoningCombo_ == nullptr) {
    return;
  }

  suppressModelSignal_ = true;
  reasoningCombo_->clear();

  if(!currentModel_.isValid()) {
    reasoningCombo_->setVisible(false);
    suppressModelSignal_ = false;
    return;
  }

  // providers_ 是值成员，直接调用；ModelInfo 不含默认档位，默认档位属于
  // 用户配置（ModelOptionOverride），所以从设置里取。
  const ModelInfo info =
    providers_.effectiveModelInfo(currentModel_.providerId, currentModel_.modelId);
  const ModelOptionOverride override =
    settings_.modelOverride(currentModel_.providerId, currentModel_.modelId);

  if(!modelSupportsReasoning(info)) {
    reasoningCombo_->setVisible(false);
    suppressModelSignal_ = false;
    return;
  }

  // 模型自报档位为空但声明支持思考时，用保守的标准集合兜底，
  // 否则用户会看到一个支持思考却无法选择强度的模型。
  const QStringList levelIds =
    info.reasoningLevels.isEmpty() ? defaultReasoningLevelIds() : info.reasoningLevels;

  // 决定的顺序：用户对该模型的上次选择 → 模型默认档位 → 规整后的第一个可用档位。
  QString desired = settings_.reasoningLevelFor(currentModel_.providerId,
                                                currentModel_.modelId);
  if(desired.isEmpty()) {
    desired = override.defaultReasoningLevel;
  }
  desired = normalizeReasoningLevel(desired, levelIds);
  currentModel_.reasoningLevel = desired;

  for(const QString & id : levelIds) {
    reasoningCombo_->addItem(QStringLiteral("思考 ") + reasoningLevelLabel(id), id);
  }
  const int index = reasoningCombo_->findData(desired);
  if(index >= 0) {
    reasoningCombo_->setCurrentIndex(index);
  }
  reasoningCombo_->setVisible(true);
  suppressModelSignal_ = false;
}

void MainWindow::onReasoningLevelChanged(int index)
{
  if(suppressModelSignal_ || index < 0 || !currentModel_.isValid()) {
    return;
  }
  const QString levelId = reasoningCombo_->itemData(index).toString();
  if(levelId == currentModel_.reasoningLevel) {
    return;
  }

  currentModel_.reasoningLevel = levelId;
  settings_.rememberReasoningLevel(currentModel_.providerId, currentModel_.modelId, levelId);
  settings_.rememberModelForWorkspace(workspace_.key(), currentModel_);
  saveSettings();

  if(runtime_.hasSession()) {
    runtime_.setModel(currentModel_);
  }
  // 上下文预算不受档位影响，但状态栏的运行提示需要跟着更新一次。
  refreshRunState();

  qCInfo(log) << "思考等级变更为:" << levelId;
}

void MainWindow::loadWorkspaceSessions()
{
  if(!workspace_.isValid()) {
    // 同上：空 key 会返回全部会话，绝不能拿它当"当前工作区没有会话"。
    sidebar_->setSessions({});
    return;
  }
  const QList<SessionSummary> sessions = store_.listSessions(workspace_.key());
  sidebar_->setSessions(sessions);
}

void MainWindow::refreshContextUsage()
{
  QJsonObject usage = runtime_.session().contextUsage;

  // 没有会话时 session 里没有用量快照，但用户仍然需要看到"改完设置到底生效没有"。
  // 用有效模型元信息（含覆盖）兜底展示分母，已用记为 0。
  if(usage.isEmpty() && currentModel_.isValid()) {
    const ModelInfo info =
      providers_.effectiveModelInfo(currentModel_.providerId, currentModel_.modelId);
    if(info.contextWindow > 0) {
      usage.insert(QStringLiteral("usedTokens"), 0);
      usage.insert(QStringLiteral("maxTokens"), info.contextWindow);
      usage.insert(QStringLiteral("percent"), 0.0);
    }
  }

  const int percent = static_cast<int>(json::number(usage, QStringLiteral("percent")));

  contextBar_->setValue(qBound(0, percent, 100));
  contextLabel_->setText(contextUsageText(usage));
  contextLabel_->setToolTip(contextTooltipText(usage));
  contextBar_->setToolTip(contextLabel_->toolTip());

  const Palette & palette = Theme::instance().palette();
  QColor chunk = palette.brand;
  if(percent >= kContextCriticalPercent) {
    chunk = palette.destructive;
  }
  else if(percent >= kContextWarnPercent) {
    chunk = palette.warning;
  }
  contextBar_->setStyleSheet(
    QStringLiteral("QProgressBar { background-color: %1; border: none; border-radius: 3px; }"
                   "QProgressBar::chunk { background-color: %2; border-radius: 3px; }")
    .arg(Theme::css(palette.surface), Theme::css(chunk)));

  // 用量明细：始终显示四个字段（哪怕是 0），这样用户能确认这些指标存在、
  // 也能一眼看出某次请求是否命中了缓存。
  const Usage cumulative = runtime_.session().cumulativeUsage;
  const Usage lastTurn = runtime_.lastTurnUsage();
  usageLabel_->setText(usageBreakdownText(cumulative));
  usageLabel_->setToolTip(usageTooltipText(cumulative, lastTurn));
}

void MainWindow::refreshRunState()
{
  const RunState state = runtime_.runState();
  QString text = runStateText(state);

  if(state != RunState::Idle && state != RunState::Failed) {
    const qint64 elapsed = turnTimer_.isValid() ? turnTimer_.elapsed() / 1000 : 0;
    if(elapsed > 0) {
      text += QStringLiteral("（%1s）").arg(elapsed);
    }
  }
  runStateLabel_->setText(text);

  const bool running = runtime_.isRunning();
  sendButton_->setEnabled(!running && workspace_.isValid());
  stopButton_->setEnabled(running);
}

void MainWindow::setStatusMessage(const QString & message)
{
  statusBar()->showMessage(message, 6000);
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话
// ─────────────────────────────────────────────────────────────────────────────

ModelSelection MainWindow::effectiveModelSelection() const
{
  ModelSelection selection = settings_.modelForWorkspace(workspace_.key());
  if(selection.isValid()) {
    return selection;
  }
  // 没有任何记录时取第一个可用模型，避免用户一进来就卡在"未选模型"。
  const QList<ModelInfo> models = providers_.allModels();
  if(!models.isEmpty()) {
    ModelSelection fallback;
    fallback.providerId = models.first().providerId;
    fallback.modelId = models.first().modelId;
    return fallback;
  }
  return {};
}

void MainWindow::openSessionOrCreate()
{
  if(!workspace_.isValid()) {
    // 没有工作区就没有会话可言。**不要**调用 listSessions("")——
    // 空 key 的语义是"不过滤"，会返回**所有**工作区的会话。
    sidebar_->setSessions({});
    return;
  }
  // 优先恢复最近一次会话；没有就新建。
  const QList<SessionSummary> sessions = store_.listSessions(workspace_.key(), 1);
  if(!sessions.isEmpty()) {
    onSessionSelected(sessions.first().session.id);
    return;
  }
  onNewSessionRequested();
}

void MainWindow::onNewSessionRequested()
{
  // 待发附件属于"这次编辑"，换会话就作废——否则用户会以为图片跟着
  // 新会话走了，实际上悄悄发到了另一个会话里。
  if(!pendingAttachments_.isEmpty()) {
    pendingAttachments_.clear();
    refreshAttachmentStrip();
  }

  if(runtime_.isRunning()) {
    const auto answer = QMessageBox::question(
                          this, QStringLiteral("会话正在运行"),
                          QStringLiteral("当前会话仍在运行。新建会话会中断它，是否继续？"),
                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if(answer != QMessageBox::Yes) {
      return;
    }
    runtime_.abort();
  }

  if(!workspace_.isValid()) {
    // ⚠ 用状态栏提示而不是模态框：没有工作区是一个**合法状态**，
    // 而模态框会让任何走到这里的路径都停下来等人点确定（实测把测试卡死）。
    setStatusMessage(QStringLiteral("还没有工作区：先用「打开工作区…」选择一个目录。"));
    return;
  }

  ModelSelection selection = currentModel_.isValid() ? currentModel_ : effectiveModelSelection();
  if(!selection.isValid()) {
    QMessageBox::information(
      this, QStringLiteral("需要配置模型"),
      QStringLiteral("还没有可用的模型。请在「设置 → Provider」中添加一个 Provider "
                     "并填写模型列表。"));
    onSettingsRequested();
    return;
  }

  const SessionMode mode =
    static_cast<SessionMode>(modeCombo_->currentData().toInt());

  QString error;
  if(!runtime_.startSession(workspace_, mode, selection, &error)) {
    QMessageBox::warning(this, QStringLiteral("无法新建会话"), error);
    return;
  }

  // 与 loadSession 保持一致：先清空视图，再让 runtime 驱动填充。
  // startSession 不发 messageAdded（新会话没有历史），所以这里先后都安全，
  // 但统一顺序后"清空 → 由 runtime 发消息填充"就是唯一的不变式。
  conversation_->clear();
  activeSessionId_ = runtime_.session().id;
  sidebar_->setActiveSession(activeSessionId_);
  currentModel_ = selection;
  settings_.rememberModelForWorkspace(workspace_.key(), selection);
  saveSettings();

  refreshContextUsage();
  refreshRunState();
  composer_->setFocus();
}

void MainWindow::onSessionSelected(const Id & sessionId)
{
  if(sessionId.isEmpty() || sessionId == activeSessionId_) {
    return;
  }
  if(runtime_.isRunning()) {
    runtime_.abort();
  }

  // 树上的会话可能属于**另一个**工作区。先切过去再打开，否则会话会被
  // 载入到不匹配的工作区上下文里（工具的工作目录、会话列表都是错的）。
  const QString owner = sidebar_->workspacePathForSession(sessionId);
  if(!owner.isEmpty() && QDir(owner) != QDir(workspace_.path)) {
    qCInfo(log) << "从树里打开其它工作区的会话，先切换工作区;" << owner;
    openWorkspacePath(owner, false);  // 只是切过去看，不写回列表
  }

  // ⚠ 必须先清空视图，再让 runtime 载入。
  // loadSession() 会为每条历史消息发 messageAdded 并由本窗口塞进对话流，
  // 如果 clear() 放在它**之后**，刚载入的历史会被立刻抹掉——表现就是
  // "关掉软件再打开，之前的会话打不开（点开是空的）"（实测踩到）。
  conversation_->clear();

  QString error;
  if(!runtime_.loadSession(sessionId, &error)) {
    setStatusMessage(error);
    return;
  }

  activeSessionId_ = sessionId;

  const ModelSelection selection = runtime_.session().providerId.isEmpty()
                                   ? effectiveModelSelection()
                                   : ModelSelection{runtime_.session().providerId,
                                                    runtime_.session().modelId, {}};
  currentModel_ = selection;
  refreshReasoningCombo();

  suppressModeSignal_ = true;
  const int modeIndex = modeCombo_->findData(static_cast<int>(runtime_.session().mode));
  if(modeIndex >= 0) {
    modeCombo_->setCurrentIndex(modeIndex);
  }
  suppressModeSignal_ = false;

  refreshContextUsage();
  refreshRunState();
  composer_->setFocus();
}

void MainWindow::onSessionDeleteRequested(const Id & sessionId)
{
  const auto answer = QMessageBox::question(
                        this, QStringLiteral("删除会话"),
                        QStringLiteral("确定要删除这个会话及其全部消息吗？此操作不可撤销。"),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if(answer != QMessageBox::Yes) {
    return;
  }

  if(sessionId == activeSessionId_) {
    if(runtime_.isRunning()) {
      runtime_.abort();
    }
    runtime_.closeSession();
    conversation_->clear();
    activeSessionId_.clear();
  }

  if(!store_.deleteSession(sessionId)) {
    setStatusMessage(QStringLiteral("删除失败：") + store_.lastError());
    return;
  }

  loadWorkspaceSessions();
  setStatusMessage(QStringLiteral("会话已删除。"));

  if(activeSessionId_.isEmpty()) {
    openSessionOrCreate();
  }
}

void MainWindow::forgetWorkspaceRecords(const QString & path)
{
  // 工作区被移除后，它的**按工作区索引**的配置要一起清掉，否则 settings.json
  // 会长期堆积已经不存在的工作区记录（用户会看到 workspaceLastModel 里还有
  // 一个已经删掉的工作区）。本地工作区的身份 key 就是路径，与
  // rememberModelForWorkspace / listSessions 的口径一致。
  Workspace target;
  target.path = QDir(path).absolutePath();
  const QString key = target.key();

  settings_.workspaceLastModel.remove(key);
  settings_.expandedWorkspaces.removeAll(key);
  settings_.expandedWorkspaces.removeAll(path);
  qCDebug(log) << "已清理工作区记录:" << key;
}


void MainWindow::onWorkspaceRemoveRequested(const QString & path)
{
  // 只动列表，不动数据。工作区目录与它的会话都原样保留，
  // 下次打开该目录时会重新出现在列表里。
  const bool isCurrent = QDir(path) == QDir(workspace_.path);
  const int removed = settings_.recentWorkspaces.removeAll(path);

  if(!isCurrent) {
    if(removed == 0) {
      setStatusMessage(QStringLiteral("该工作区不在列表里。"));
      return;
    }
    forgetWorkspaceRecords(path);
    saveSettings();
    sidebar_->setRecentWorkspaces(settings_.recentWorkspaces);
    qCInfo(log) << "已从最近工作区移除:" << path;
    setStatusMessage(QStringLiteral("已从列表移除：%1（会话与文件都还在）").arg(path));
    return;
  }

  // ── 移除的是**当前**工作区 ──────────────────────────────────────────────
  // 这里以前是静默无效的：列表里删掉了，但 SidebarPanel::setRecentWorkspaces
  // 会把"当前工作区"强制补回树上，界面上看不出任何变化；重启后启动逻辑又把
  // 它写回列表——于是当前工作区永远删不掉。
  if(settings_.recentWorkspaces.isEmpty()) {
    // 最后一个工作区也允许移除：应用进入**无工作区**状态。
    //
    // 这是一个被完整支持的状态，不是"半坏"：会话与对话流被收干净、
    // workspace_ 清空、lastWorkspace 清空，发送按钮由 refreshRunState()
    // 自动禁用（它要求 workspace_.isValid()），界面提示用户去打开一个目录。
    // 没有工作区归属的会话不会被写进会话表，也不会在树上建出空节点。
    qCInfo(log) << "移除最后一个工作区，进入无工作区状态:" << path;
    if(runtime_.isRunning()) {
      runtime_.abort();
    }
    runtime_.closeSession();
    conversation_->clear();
    activeSessionId_.clear();
    workspace_ = Workspace{};
    forgetWorkspaceRecords(path);
    settings_.lastWorkspace.clear();
    saveSettings();

    sidebar_->setWorkspace(workspace_);
    sidebar_->setRecentWorkspaces(settings_.recentWorkspaces);
    applySettingsToUi();
    refreshRunState();
    setStatusMessage(
      QStringLiteral("已移除最后一个工作区。用「打开工作区…」选择目录即可继续。"));
    return;
  }

  // 还有别的工作区：切到第一个，再把它从列表里去掉。这样"移除"是真的生效。
  const QString next = settings_.recentWorkspaces.first();
  qCInfo(log) << "移除当前工作区，先切到:" << next << "再移除" << path;
  forgetWorkspaceRecords(path);
  saveSettings();
  onOpenWorkspacePath(next);
  // onOpenWorkspacePath 会按新工作区重写列表；被移除的那个不该被加回来。
  if(settings_.recentWorkspaces.removeAll(path) > 0) {
    saveSettings();
    sidebar_->setRecentWorkspaces(settings_.recentWorkspaces);
  }
  setStatusMessage(QStringLiteral("已从列表移除：%1（工作区文件未被改动）").arg(path));
}

void MainWindow::onWorkspacePurgeRequested(const QString & path)
{
  // 本地工作区的身份 key 就是路径（identity 只可能来自外部写入的会话），
  // 与 listSessions / 建会话时的口径一致。
  Workspace target;
  target.path = QDir(path).absolutePath();
  const QString key = target.key();

  const QList<SessionSummary> sessions =
    store_.isOpen() ? store_.listSessions(key) : QList<SessionSummary> {};
  if(sessions.isEmpty()) {
    setStatusMessage(QStringLiteral("该工作区没有可删除的会话。"));
    return;
  }

  // 二次确认必须说清"删什么"与"不删什么"：用户对"删除工作区"的直觉
  // 往往是删磁盘目录，而我们只删数据库里的会话记录。
  const auto answer = QMessageBox::warning(
                        this, QStringLiteral("删除工作区的会话"),
                        QStringLiteral("将删除「%1」下的全部 %2 个会话及其消息。\n\n"
                                       "此操作不可撤销。\n"
                                       "工作区目录本身和其中的文件**不会**被删除。")
                        .arg(target.path)
                        .arg(sessions.size()),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if(answer != QMessageBox::Yes) {
    return;
  }

  // 当前会话也在被删之列时，先把它干净地收掉，否则运行时还持有一个
  // 即将从库里消失的会话，后续落盘会一路报外键失败。
  const bool activeAffected = target.path == workspace_.path;
  if(activeAffected && !activeSessionId_.isEmpty()) {
    if(runtime_.isRunning()) {
      runtime_.abort();
    }
    runtime_.closeSession();
    conversation_->clear();
    activeSessionId_.clear();
  }

  if(!store_.deleteSessionsForWorkspace(key)) {
    setStatusMessage(QStringLiteral("删除失败：") + store_.lastError());
    return;
  }

  loadWorkspaceSessions();
  qCInfo(log) << "已删除工作区全部会话:" << target.path << "数量=" << sessions.size();

  if(activeSessionId_.isEmpty() && target.path == workspace_.path) {
    openSessionOrCreate();  // 当前工作区被清空 → 直接开一个新的空会话
  }
  setStatusMessage(QStringLiteral("已删除「%1」的 %2 个会话（工作区文件未受影响）。")
                   .arg(QDir(target.path).dirName())
                   .arg(sessions.size()));
}

void MainWindow::onWorkspaceChangeRequested()
{
  const QString directory = QFileDialog::getExistingDirectory(
                              this, QStringLiteral("选择工作区目录"), workspace_.path);
  if(!directory.isEmpty()) {
    onOpenWorkspacePath(directory);
  }
}

void MainWindow::onOpenWorkspacePath(const QString & path)
{
  openWorkspacePath(path, true);
}

void MainWindow::openWorkspacePath(const QString & path, bool remember)
{
  if(path.isEmpty() || !QDir(path).exists()) {
    setStatusMessage(QStringLiteral("目录不存在：") + path);
    return;
  }
  if(runtime_.isRunning()) {
    runtime_.abort();
  }

  workspace_.path = QDir(path).absolutePath();
  workspace_.identity.clear();
  settings_.lastWorkspace = workspace_.path;
  // ⚠ **只有新加入的工作区**才放到最前；已经在列表里的保持原位。
  //
  // 这里原来是"无条件 removeAll + prepend"（最近使用优先）。那在还顶部有
  // 切换菜单时是合理的，但树上显示的**就是**这个列表的顺序——于是每次点会话
  // 切到另一个工作区，那个工作区就被提到最前，用户正在点的条目会在眼皮底下
  // 跳走。切换菜单删掉之后，"最近优先"没有任何消费者，只剩这个副作用。
  // ⚠ 只有"用户显式打开"才写回列表。从树里点开一个会话时传 remember=false：
  // 那个工作区可能正是用户刚从列表里移除的，写回等于让它复活。
  if(remember && !settings_.recentWorkspaces.contains(workspace_.path)) {
    settings_.recentWorkspaces.prepend(workspace_.path);
    // 列表要有上限，否则工作区会无限增长。
    while(settings_.recentWorkspaces.size() > 12) {
      settings_.recentWorkspaces.removeLast();
    }
  }
  saveSettings();

  sidebar_->setWorkspace(workspace_);
  sidebar_->setRecentWorkspaces(settings_.recentWorkspaces);
  if(store_.isOpen()) {
    loadWorkspaceSessions();
  }

  currentModel_ = settings_.modelForWorkspace(workspace_.key());
  applySettingsToUi();

  rescanSkills();  // 项目级技能目录跟着工作区走
  qCInfo(log) << "切换工作区:" << workspace_.path;
  setStatusMessage(QStringLiteral("已切换工作区：") + workspace_.path);

  activeSessionId_.clear();
  openSessionOrCreate();
}

// ─────────────────────────────────────────────────────────────────────────────
// 运行时信号
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onMessageAdded(const Message & message)
{
  conversation_->addMessage(message);
}

void MainWindow::onMessageFinished(const Message & message)
{
  conversation_->applyMessage(message);
}

void MainWindow::onPartAppended(const Id & messageId, const Part & part)
{
  Q_UNUSED(part)
  // 控件本身由 MessageWidget 在收到增量或 part 更新时补建；
  // 这里只需要保证终态渲染会把新 part 纳入。
  Q_UNUSED(messageId)
}

void MainWindow::onPartUpdated(const Id & messageId, const Part & part)
{
  conversation_->applyPart(messageId, part);
}

void MainWindow::onDeltaAppended(const Id & messageId, const Id & partId, const QString & delta,
                                 bool reasoning)
{
  conversation_->appendDelta(messageId, partId, delta, reasoning);
}

void MainWindow::onSessionChanged(const Session & session)
{
  activeSessionId_ = session.id;

  SessionSummary summary;
  summary.session = session;
  if(!session.contextUsage.isEmpty()) {
    // 列表不展示上下文信息，但保持 summary 与会话一致。
    summary.session.contextUsage = session.contextUsage;
  }
  sidebar_->upsertSession(summary);
  sidebar_->setActiveSession(session.id);
  refreshContextUsage();
}

void MainWindow::onRunStateChanged(RunState state)
{
  if(state != RunState::Idle && !turnTimer_.isValid()) {
    turnTimer_.start();
  }
  if(state == RunState::Idle || state == RunState::Failed) {
    turnTimer_.invalidate();
  }
  refreshRunState();
}

void MainWindow::onTurnFinished(TurnResult result)
{
  turnTimer_.invalidate();
  refreshRunState();
  refreshContextUsage();

  switch(result) {
    case TurnResult::Success:
      // 正常结束不打扰用户，状态栏一句话足够。
      setStatusMessage(QStringLiteral("完成。"));
      // 会话有内容之后才值得生成标题（空会话没有素材）。
      // 幂等由运行时的 titleGenerated 保证，这里只管"要不要发起"。
      if(settings_.generateSessionTitles) {
        runtime_.requestTitleFromModel();
      }
      // 模型刚给出这一轮的结论，看看有没有要用户选的选项。
      refreshChoices();
      break;
    case TurnResult::Interrupted:
      setStatusMessage(QStringLiteral("已中断。"));
      break;
    case TurnResult::Failed:
      break;  // 失败由 onFailed 给出具体原因，避免双重提示
    case TurnResult::Skipped:
      setStatusMessage(QStringLiteral("本次回合未执行。"));
      break;
    case TurnResult::None:
      break;
  }

  if(store_.isOpen()) {
    loadWorkspaceSessions();
    sidebar_->setActiveSession(activeSessionId_);
  }
  composer_->setFocus();
}

void MainWindow::onSubagentSessionChanged(const Session & session)
{
  // 子会话只进列表，**绝不切换**当前会话：用户正在看的对话不能被
  // 后台派生的子代理顶掉。这正是它与 onSessionChanged 分成两个信号的原因。
  SessionSummary summary;
  summary.session = session;
  sidebar_->upsertSession(summary);
  qCDebug(log) << "子代理会话已更新; id=" << session.id
               << "parent=" << session.parentSessionId;
}

void MainWindow::onSubagentFinished(const Id & childSessionId, bool ok)
{
  qCInfo(log) << "子代理结束; child=" << childSessionId << "ok=" << ok;
  setStatusMessage(ok ? QStringLiteral("子代理已完成，可在左侧列表查看它的完整过程。")
                   : QStringLiteral("子代理执行失败，详见左侧列表中的该会话。"));

  // 重新拉一次列表让标题/状态与库一致；同时把选中项还原回当前会话，
  // 避免 loadWorkspaceSessions() 的刷新动作影响用户的当前视图。
  if(store_.isOpen()) {
    loadWorkspaceSessions();
    sidebar_->setActiveSession(activeSessionId_);
  }
}

void MainWindow::onBackgroundTasksChanged(int runningCount)
{
  const Palette & palette = Theme::instance().palette();
  if(runningCount <= 0) {
    backgroundLabel_->clear();
    backgroundLabel_->setToolTip(QString());
    return;
  }
  backgroundLabel_->setText(QStringLiteral("· 后台任务 %1").arg(runningCount));
  backgroundLabel_->setStyleSheet(
    QStringLiteral("color: %1;").arg(Theme::css(palette.warning)));
  backgroundLabel_->setToolTip(
    QStringLiteral("有 %1 个后台任务正在运行。用 TaskOutput 查看它们的输出，"
                   "或在工具卡片里用 TaskStop 终止。")
    .arg(runningCount));
}

void MainWindow::rescanSkills()
{
  // 非空即覆盖默认目录。空列表与"没配过"区分不开，所以用"非空即覆盖"的语义：
  // 用户在设置里删掉默认目录后，我们不该又把它加回来。
  const QStringList directories = settings_.skillDirectories.isEmpty()
                                  ? skills::Library::defaultDirectories(workspace_.path)
                                  : settings_.skillDirectories;
  skills_.rescan(directories);
  runtime_.setSkillLibrary(&skills_);
  if(!skills_.isEmpty()) {
    qCInfo(log) << "已加载技能; 数量=" << skills_.skills().size();
  }
}

void MainWindow::onMcpChanged()
{
  if(mcpLabel_ == nullptr) {
    return;
  }
  const int ready = mcp_ != nullptr ? mcp_->readyCount() : 0;
  if(ready <= 0) {
    mcpLabel_->clear();
    mcpLabel_->setToolTip(QString());
    return;
  }
  mcpLabel_->setText(QStringLiteral("· MCP %1").arg(ready));
  mcpLabel_->setStyleSheet(
    QStringLiteral("color: %1;").arg(Theme::css(Theme::instance().palette().success)));
  mcpLabel_->setToolTip(QStringLiteral("已连接 %1 个 MCP 服务器；它们提供的工具"
                                       "已注册为普通工具，模型可以直接调用。")
                        .arg(ready));
}

void MainWindow::onFailed(const QString & message)
{
  setStatusMessage(message);
  qCWarning(log) << "运行时失败:" << message;
}

void MainWindow::onCompactionNotice(const QString & message)
{
  setStatusMessage(message);
  qCInfo(log) << "上下文压缩:" << message;
}

void MainWindow::onConversationReplaced()
{
  // 压缩把一段历史换成了"摘要 + 分隔行"，这种删一段插一段的变化没法用
  // 增量信号表达（partAppended 只能追加）。整条对话流重建是最省事也最不会
  // 出错的做法：消息数量级是几十条，重建代价可以忽略。
  conversation_->clear();
  for(const Message & message : runtime_.messages()) {
    conversation_->addMessage(message);
  }
  qCDebug(log) << "对话流已按压缩后的消息列表重建; 消息数=" << runtime_.messages().size();
}

// ─────────────────────────────────────────────────────────────────────────────
// 权限
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onPermissionRequested(const PermissionRequest & request)
{
  permissionRequests_.insert(request.id, request);
  permissionQueue_.append(request);
  if(permissionQueue_.size() == 1) {
    showNextPermissionDialog();
  }
  else {
    qCDebug(log) << "权限请求已排队，当前队列长度:" << permissionQueue_.size();
  }
}

void MainWindow::showNextPermissionDialog()
{
  if(permissionQueue_.isEmpty()) {
    return;
  }
  const PermissionRequest request = permissionQueue_.first();

  PermissionDialog::present(this, request, [this](const PermissionResponse & response) {
    if(permissionQueue_.isEmpty()) {
      return;
    }
    const PermissionRequest current = permissionQueue_.takeFirst();
    permissionRequests_.remove(current.id);

    // 通过运行时提交裁决：它是唯一的决策入口，UI 不直接改权限规则。
    if(!runtime_.resolvePermission(current.id, response)) {
      // 请求已过期（例如用户中断了运行），记录即可，不必打扰用户。
      qCDebug(log) << "裁决提交时请求已不存在:" << current.id;
    }

    // 串行展示下一个，避免多个对话框同时弹出。
    if(!permissionQueue_.isEmpty()) {
      QTimer::singleShot(0, this, &MainWindow::showNextPermissionDialog);
    }
  });
}

void MainWindow::onPermissionResolved(const Id & requestId)
{
  permissionRequests_.remove(requestId);
}

// ─────────────────────────────────────────────────────────────────────────────
// 输入与配置
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onAttachImagesRequested()
{
  const QStringList paths = QFileDialog::getOpenFileNames(
                              this, QStringLiteral("附加图片"), QString(),
                              QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;所有文件 (*)"));
  if(paths.isEmpty()) {
    return;
  }
  attachImages(paths);
}

void MainWindow::attachImages(const QStringList & paths)
{
  for(const QString & path : paths) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {
      setStatusMessage(QStringLiteral("无法读取：%1").arg(path));
      continue;
    }
    const QByteArray bytes = file.readAll();
    // MIME 交给 QMimeDatabase 判定，而不是按扩展名硬编码：
    // 用户可能把 png 存成 .jpg，按扩展名会给出错误的 media_type，
    // 而 Anthropic/OpenAI 都会拒绝 media_type 与实际内容不符的图片。
    const QString mime =
      QMimeDatabase().mimeTypeForFileNameAndData(path, bytes).name();
    attachImageData(bytes, mime, QFileInfo(path).fileName());
    // 记住磁盘路径只是为了显示与排查；provider 用的是 base64，
    // 所以粘贴来的图片没有路径也完全正常。
  }
}

void MainWindow::attachImageData(const QByteArray & bytes, const QString & mimeType,
                                 const QString & suggestedName)
{
  if(bytes.isEmpty()) {
    return;
  }
  if(!mimeType.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive)) {
    setStatusMessage(QStringLiteral("只支持图片附件，收到的是 %1。").arg(mimeType));
    return;
  }

  // 单张上限：base64 之后还要进 SQLite 与 HTTP body，过大既慢又容易失败。
  constexpr qint64 kMaxImageBytes = 8 * 1024 * 1024;
  if(bytes.size() > kMaxImageBytes) {
    setStatusMessage(QStringLiteral("图片过大（%1 MB），上限 8 MB。")
                     .arg(QString::number(bytes.size() / 1024.0 / 1024.0, 'f', 1)));
    return;
  }

  FilePart part;
  part.mimeType = mimeType;
  part.fileName = suggestedName.isEmpty() ? QStringLiteral("粘贴的图片") : suggestedName;
  part.sizeBytes = bytes.size();
  part.base64 = QString::fromLatin1(bytes.toBase64());
  // 粘贴来的图片没有磁盘路径，留空即可——provider 只用 base64。
  pendingAttachments_.append(part);

  qCInfo(log) << "已附加图片;" << part.fileName << mimeType << part.sizeBytes << "字节";
  refreshAttachmentStrip();
}

void MainWindow::refreshAttachmentStrip()
{
  if(attachmentStrip_ == nullptr || attachmentLayout_ == nullptr) {
    return;
  }

  // 整体重建而不是增量更新：附件数量是个位数，重建更不容易出现
  // "删了一个但界面还留着"这类状态不同步。
  while(QLayoutItem * item = attachmentLayout_->takeAt(0)) {
    if(QWidget * widget = item->widget()) {
      widget->deleteLater();
    }
    delete item;
  }

  if(pendingAttachments_.isEmpty()) {
    attachmentStrip_->hide();
    return;
  }

  for(int index = 0; index < pendingAttachments_.size(); ++index) {
    const FilePart & part = pendingAttachments_.at(index);

    auto * chip = new QWidget;
    chip->setObjectName(QStringLiteral("attachmentChip"));
    auto * chipLayout = new QHBoxLayout(chip);
    chipLayout->setContentsMargins(6, 4, 6, 4);
    chipLayout->setSpacing(6);

    // 缩略图直接由 base64 解出：不依赖原文件还在不在（粘贴的图没有路径）。
    auto * thumb = new QLabel;
    QPixmap pixmap;
    if(pixmap.loadFromData(QByteArray::fromBase64(part.base64.toLatin1()))) {
      thumb->setPixmap(pixmap.scaled(36, 36, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
    }
    thumb->setFixedSize(36, 36);
    thumb->setAlignment(Qt::AlignCenter);
    chipLayout->addWidget(thumb);

    // 小于 1KB 时显示字节数：固定用 KB 会把小图显示成"0 KB"，
    // 看起来像数据丢了。
    const QString sizeText =
      part.sizeBytes < 1024
      ? QStringLiteral("%1 B").arg(part.sizeBytes)
      : QStringLiteral("%1 KB").arg(QString::number(part.sizeBytes / 1024.0, 'f', 0));
    auto * label = new QLabel(QStringLiteral("%1  ·  %2").arg(part.fileName, sizeText));
    label->setFont(Theme::instance().font(FontRole::UiXs));
    label->setToolTip(QStringLiteral("%1（%2）").arg(part.fileName, part.mimeType));
    chipLayout->addWidget(label);

    auto * remove = new QPushButton(QStringLiteral("×"));
    remove->setObjectName(QStringLiteral("attachmentRemove"));
    remove->setFixedSize(20, 20);
    remove->setToolTip(QStringLiteral("移除这张图片"));
    remove->setCursor(Qt::PointingHandCursor);
    connect(remove, &QPushButton::clicked, this, [this, index]() {
      if(index >= 0 && index < pendingAttachments_.size()) {
        qCDebug(log) << "移除附件" << pendingAttachments_.at(index).fileName;
        pendingAttachments_.removeAt(index);
        refreshAttachmentStrip();
      }
    });
    chipLayout->addWidget(remove);

    attachmentLayout_->addWidget(chip);
  }
  attachmentLayout_->addStretch(1);
  attachmentStrip_->show();
}

void MainWindow::refreshChoices()
{
  if(choiceBar_ == nullptr || choiceLayout_ == nullptr) {
    return;
  }
  while(QLayoutItem * item = choiceLayout_->takeAt(0)) {
    if(QWidget * widget = item->widget()) {
      widget->deleteLater();
    }
    delete item;
  }

  // 只看**最后一条有正文的 assistant 消息**：选项是针对"当前该选什么"的，
  // 翻看历史时不应该把早先那轮的选项又摆出来。
  QString latest;
  for(auto it = runtime_.messages().crbegin(); it != runtime_.messages().crend(); ++it) {
    if(it->role != MessageRole::Assistant || it->modelOnly) {
      continue;
    }
    const QString text = it->plainText().trimmed();
    if(!text.isEmpty()) {
      latest = text;
      break;
    }
  }

  const QList<Markdown::Choice> choices = Markdown::detectChoices(latest);
  if(choices.isEmpty()) {
    choiceBar_->hide();
    return;
  }

  for(const Markdown::Choice & choice : choices) {
    auto * button = new QPushButton(choice.label);
    button->setObjectName(QStringLiteral("choiceButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(QStringLiteral("点击填入输入框：%1").arg(choice.text));
    connect(button, &QPushButton::clicked, this, [this, choice]() {
      // 填进输入框而不是直接发送：用户可以改一改再发，也避免误点直接发出。
      composer_->setPlainText(choice.text);
      composer_->setFocus();
      choiceBar_->hide();
    });
    choiceLayout_->addWidget(button);
  }
  choiceLayout_->addStretch(1);
  choiceBar_->show();
  qCDebug(log) << "已显示选项按钮; 数量=" << choices.size();
}

void MainWindow::handleSlashCommand(const QString & rawText)
{
  const QString firstLine = rawText.section(QLatin1Char('\n'), 0, 0).trimmed();
  const QString verb = firstLine.section(QLatin1Char(' '), 0, 0).toLower();

  if(verb == QStringLiteral("/compact")) {
    if(!runtime_.hasSession()) {
      setStatusMessage(QStringLiteral("还没有会话，无法压缩上下文。"));
      return;
    }
    // 用户敲下命令就意味着"立刻做"：不看阈值，也不等下一个模型步。
    QString error;
    if(!runtime_.compactContextNow(&error)) {
      setStatusMessage(error);
      return;
    }
    // 命令本身不该留在输入框里：压缩成功后输入框该是干净的，可以直接继续提问。
    composer_->clear();
    pendingAttachments_.clear();
    refreshAttachmentStrip();
    setStatusMessage(QStringLiteral("正在压缩上下文…"));
    return;
  }

  if(verb == QStringLiteral("/help")) {
    composer_->clear();
    setStatusMessage(QStringLiteral("命令：/compact 压缩上下文，/help 查看这条说明。"));
    return;
  }

  // 未知命令：**不发给模型**。把它当普通文本发出去只会浪费一次调用，
  // 而且用户会以为自己敲的命令生效了。
  setStatusMessage(QStringLiteral("未知命令：%1（可用 /compact、/help）").arg(verb));
}

void MainWindow::onSendRequested()
{
  // 斜杠命令先处理：它们不该被当作提示词发给模型，也不该等 MCP 握手。
  {
    const QString command = composer_->toPlainText().trimmed();
    if(command.startsWith(QLatin1Char('/'))) {
      handleSlashCommand(command);
      return;
    }
  }

  // MCP 服务器还在握手时先等它就绪再发第一次请求。
  //
  // 为什么值得等：工具声明是**每次请求重新构建**的，所以第一次请求如果赶在
  // 握手完成之前发出，模型这一次就看不到 MCP 工具——它会以为自己没有这些能力，
  // 于是改用别的方式（甚至直接告诉用户"我没有这个工具"），而用户明明配了。
  // 等的上限是有界的：卡住的服务器不该把用户的输入也一起卡住。
  if(mcp_ != nullptr && mcp_->isSettling() && !mcpWaitElapsed_.isValid()) {
    mcpWaitElapsed_.start();
  }
  if(mcp_ != nullptr && mcp_->isSettling() &&
     mcpWaitElapsed_.elapsed() < kMcpWaitTimeoutMs) {
    if(!mcpWaitPending_) {
      mcpWaitPending_ = true;
      setStatusMessage(QStringLiteral("正在连接 MCP 服务器…"));
      // 一次性订阅：就绪或超时后自动重试这一次发送。
      auto * timer = new QTimer(this);
      timer->setSingleShot(true);
      connect(timer, &QTimer::timeout, this, [this]() {
        mcpWaitPending_ = false;
        onSendRequested();
      });
      timer->start(kMcpPollIntervalMs);
    }
    return;
  }

  const QString text = composer_->toPlainText().trimmed();
  // **先快照附件**：没有会话时下面会先建会话，而 onNewSessionRequested()
  // 会清空待发附件（它们属于"上一次编辑"）。不快照的话，用户在空会话里
  // 第一次发送时图片会在提交前被自己清掉——实测被测试抓到过。
  const QList<FilePart> attachments = pendingAttachments_;

  // 只发图片不写字是合法用法，所以两者都空才算空。
  if(text.isEmpty() && attachments.isEmpty()) {
    return;
  }
  if(!runtime_.hasSession()) {
    // 用户在没有会话时直接输入：先建会话再发送，减少一次点击。
    onNewSessionRequested();
    if(!runtime_.hasSession()) {
      return;
    }
  }

  QString error;
  if(!runtime_.submitMessage(text, attachments, &error)) {
    setStatusMessage(error);
    return;  // 失败时保留输入与附件，避免用户白选一遍
  }

  // 提交成功才清空：失败时保留内容，避免用户白打字/白选图。
  composer_->clear();
  pendingAttachments_.clear();
  refreshAttachmentStrip();
  refreshRunState();
}

void MainWindow::onStopRequested()
{
  runtime_.abort();
  setStatusMessage(QStringLiteral("正在中断…"));
}

void MainWindow::onModelChanged(int index)
{
  if(suppressModelSignal_ || index < 0) {
    return;
  }
  const QString value = modelCombo_->itemData(index).toString();
  const ModelSelection selection = ModelSelection::parseDisplayValue(value);
  if(!selection.isValid()) {
    return;
  }

  currentModel_ = selection;
  // 切到新模型时先恢复"这个模型上次用的档位"，再刷新下拉；
  // 否则档位会在换模型时被静默重置成默认值。
  refreshReasoningCombo();

  settings_.rememberModelForWorkspace(workspace_.key(), currentModel_);
  saveSettings();

  if(runtime_.hasSession()) {
    runtime_.setModel(currentModel_);
  }
}

void MainWindow::onModeChanged(int index)
{
  if(suppressModeSignal_ || index < 0) {
    return;
  }
  const SessionMode mode = static_cast<SessionMode>(modeCombo_->itemData(index).toInt());
  settings_.defaultSessionMode = mode;
  saveSettings();

  if(runtime_.hasSession()) {
    runtime_.setMode(mode);
  }
}

void MainWindow::onSettingsRequested()
{
  SettingsDialog dialog(settings_, this);

  // 实时预览：主题与字号改了立刻看到效果。取消时由对话框负责恢复。
  connect(&dialog, &SettingsDialog::settingsPreviewChanged, this,
  [this](const AppSettings & preview) {
    settings_.themeMode = preview.themeMode;
    settings_.uiFontSize = preview.uiFontSize;
    settings_.codeFontSize = preview.codeFontSize;
    applyTheme();
  });

  if(dialog.exec() != QDialog::Accepted) {
    // 取消：把设置恢复成对话框打开前的状态并重新应用。
    QString reloadError;
    AppSettings original;
    if(AppConfig::load(&original, &reloadError)) {
      settings_ = original;
    }
    Theme::instance().setMode(settings_.themeMode);
    Theme::instance().setUiFontSize(settings_.uiFontSize);
    Theme::instance().setCodeFontSize(settings_.codeFontSize);
    applyTheme();
    return;
  }

  settings_ = dialog.settings();
  Theme::instance().setMode(settings_.themeMode);
  Theme::instance().setUiFontSize(settings_.uiFontSize);
  Theme::instance().setCodeFontSize(settings_.codeFontSize);
  applyTheme();

  // Provider 配置变化要整体替换，避免残留被删除的 provider。
  providers_.replaceAll(settings_.providers);
  // 模型能力覆盖（上下文窗口、最大输出、思考档位）交给注册表统一应用，
  // 这样 resolve/allModels 的每个调用点都自动拿到有效值。
  providers_.setModelOverrides(settings_.modelOverrides);
  // 覆盖改了 → 上下文窗口可能变了 → 必须重新测量并通知 UI。
  // 少了这一步，用户改完"上下文窗口"后界面仍显示旧值（只能等下一个 turn
  // 结束才刷新），看起来就像设置没生效。
  // 覆盖改了 → 上下文窗口可能变了 → 必须重新测量并通知 UI。
  // 少了这一步，用户改完"上下文窗口"后界面仍显示旧值（只能等下一个 turn
  // 结束才刷新），看起来就像设置没生效。
  runtime_.remeasureContext();
  applySettingsToUi();
  saveSettings();

  // MCP 与 Skills 的配置也在这一页里，改完要立刻生效：
  // 重连服务器（否则用户会以为配置没保存），并重扫技能目录。
  rescanSkills();
  if(mcp_ != nullptr) {
    mcp_->startAll(settings_.mcpServers);
    onMcpChanged();
    if(mcp_->isSettling()) {
      // 就绪后把工具补进注册表——否则要等下一次改设置才生效。
      setStatusMessage(QStringLiteral("正在连接 MCP 服务器…"));
    }
  }

  if(!providers_.hasUsableProvider()) {
    setStatusMessage(QStringLiteral("当前没有可用的 Provider，请检查 Base URL 与 API Key。"));
  }
  else {
    setStatusMessage(QStringLiteral("设置已保存。"));
  }
}

void MainWindow::saveSettings()
{
  QString error;
  if(!AppConfig::save(settings_, &error)) {
    setStatusMessage(QStringLiteral("设置保存失败：") + error);
  }
}

void MainWindow::closeEvent(QCloseEvent * event)
{
  // MCP 服务器是子进程，必须显式终止；否则退出后会留下孤儿进程。
  if(mcp_ != nullptr) {
    mcp_->stopAll();
  }
  // 退出前收尾：中断运行、落盘设置。不这样做会留下孤儿工具进程与未保存配置。
  if(runtime_.isRunning()) {
    runtime_.abort();
  }
  runtime_.closeSession();
  saveSettings();
  if(store_.isOpen()) {
    store_.close();
  }
  qCInfo(log) << "主窗口关闭，已保存设置并关闭会话存储";
  event->accept();
}

bool MainWindow::eventFilter(QObject * watched, QEvent * event)
{
  if(watched == composer_ && event->type() == QEvent::KeyPress) {
    auto * keyEvent = static_cast<QKeyEvent *>(event);
    // Enter 发送，Shift+Enter 换行。这是同类工具的既定肌肉记忆，
    // 但必须在输入法组合期间放行，否则中文输入选词的回车会被吃掉。
    if((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
       !(keyEvent->modifiers() & Qt::ShiftModifier) && !keyEvent->isAutoRepeat()) {
      if(QApplication::inputMethod() != nullptr &&
         QApplication::inputMethod()->isVisible()) {
        return QMainWindow::eventFilter(watched, event);
      }
      onSendRequested();
      return true;
    }

    // Ctrl+V 粘贴图片：截图后直接粘贴是最常用的附加方式，
    // 比"另存为文件再选文件"少两步。
    if(keyEvent->matches(QKeySequence::Paste) && QApplication::clipboard() != nullptr) {
      const QMimeData * mime = QApplication::clipboard()->mimeData();
      if(mime != nullptr && mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if(!image.isNull()) {
          QByteArray bytes;
          QBuffer buffer(&bytes);
          buffer.open(QIODevice::WriteOnly);
          // 统一转 PNG：剪贴板里的 QImage 没有"原始格式"，
          // 直接声明 image/png 并转码，避免 media_type 与实际字节不符。
          image.save(&buffer, "PNG");
          attachImageData(bytes, QStringLiteral("image/png"),
                          QStringLiteral("粘贴的图片.png"));
          return true;  // 吃掉这次粘贴，不要把二进制塞进输入框
        }
      }
    }
  }

  // 拖入图片文件。
  if(watched == composer_ && event->type() == QEvent::Drop) {
    auto * dropEvent = static_cast<QDropEvent *>(event);
    QStringList paths;
    if(dropEvent->mimeData() != nullptr && dropEvent->mimeData()->hasUrls()) {
      for(const QUrl & url : dropEvent->mimeData()->urls()) {
        if(url.isLocalFile()) {
          paths.append(url.toLocalFile());
        }
      }
    }
    if(!paths.isEmpty()) {
      attachImages(paths);
      dropEvent->acceptProposedAction();
      return true;
    }
  }
  if(watched == composer_ && event->type() == QEvent::DragEnter) {
    auto * dragEvent = static_cast<QDragEnterEvent *>(event);
    if(dragEvent->mimeData() != nullptr && dragEvent->mimeData()->hasUrls()) {
      dragEvent->acceptProposedAction();
      return true;
    }
  }

  return QMainWindow::eventFilter(watched, event);
}

}  // namespace lycode::ui
