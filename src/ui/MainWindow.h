// LyCode — 主窗口
//
// 主窗口同时是**组装点**：它持有会话存储、工具注册表、Provider 注册表、
// todo 存储与 AgentRuntime，并把 UI 与运行时用信号连起来。
//
// 不额外引入 Application 层的原因：这个应用只有一个窗口与一个运行时，
// 中间再加一层只会让"谁拥有什么"变模糊。若将来需要多窗口，再把组装逻辑
// 提到 Application 即可。
//
// ── 状态所有权（唯一写入路径）────────────────────────────────────────────
//   会话与消息      → AgentRuntime
//   应用设置        → MainWindow::settings_（唯一写盘入口 saveSettings()）
//   Provider 配置   → ProviderRegistry（由 settings_ 驱动 replaceAll）
//   权限弹窗队列    → MainWindow（UI 关注点，运行时只负责决策）
#pragma once

class QHBoxLayout;

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <memory>

#include "agent/AgentRuntime.h"
#include "agent/PermissionGate.h"
#include "core/Types.h"
#include "mcp/McpManager.h"
#include "skills/SkillLibrary.h"
#include "model/ProviderRegistry.h"
#include "storage/SessionStore.h"
#include "tools/Tool.h"
#include "ui/AppConfig.h"

class QActionGroup;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTimer;

namespace lycode::ui
{

class ChoiceCard;
class ConversationView;
class SidebarPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget * parent = nullptr);
    ~MainWindow() override;

  protected:
    void closeEvent(QCloseEvent * event) override;
    /// 拦截输入框的回车：Enter 发送、Shift+Enter 换行，
    /// 但输入法组合期间必须放行，否则中文选词的回车会被吃掉。
    bool eventFilter(QObject * watched, QEvent * event) override;

  private slots:
    // ── 会话 ────────────────────────────────────────────────────────────────
    void onNewSessionRequested();
    void onSessionSelected(const Id & sessionId);
    void onSessionDeleteRequested(const Id & sessionId);
    void onWorkspaceChangeRequested();
    /// 把某个工作区从最近列表移除（不碰数据）。
    void onWorkspaceRemoveRequested(const QString & path);
    /// 删除某个工作区下的全部会话（二次确认后执行）。
    void onWorkspacePurgeRequested(const QString & path);
    void onOpenWorkspacePath(const QString & path);
    /// 切换工作区的真正实现。`remember` 决定要不要把它写回工作区列表——
    /// 从树里点开一个会话时**不能**写回，否则用户从列表移除过的工作区
    /// 会因为他点了一下它的会话就复活。
    void openWorkspacePath(const QString & path, bool remember);

    // ── 运行时信号 ──────────────────────────────────────────────────────────
    void onMessageAdded(const Message & message);
    void onMessageFinished(const Message & message);
    void onPartAppended(const Id & messageId, const Part & part);
    void onPartUpdated(const Id & messageId, const Part & part);
    void onDeltaAppended(const Id & messageId, const Id & partId, const QString & delta,
                         bool reasoning);
    void onSessionChanged(const Session & session);
    void onRunStateChanged(RunState state);
    void onPermissionRequested(const PermissionRequest & request);
    void onPermissionResolved(const Id & requestId);
    void onTurnFinished(TurnResult result);
    void onFailed(const QString & message);
    /// 上下文压缩的进展/结果提示（不是错误）。
    void onCompactionNotice(const QString & message);
    /// 消息列表被整体替换（压缩）：清空并重建对话流。
    void onConversationReplaced();
    /// 子代理的会话状态变化：只更新列表，**不切换**当前会话。
    void onSubagentSessionChanged(const lycode::Session & session);
    void onSubagentFinished(const lycode::Id & childSessionId, bool ok);
    /// 后台任务数量变化：状态栏给出可见提示，否则用户不知道有进程还在跑。
    void onBackgroundTasksChanged(int runningCount);
    /// MCP 服务器就绪/失败时刷新状态提示。
    void onMcpChanged();
    /// 清掉某个工作区的按工作区索引的配置（模型记忆、展开状态）。
    void forgetWorkspaceRecords(const QString & path);
    /// 重新扫描技能目录并注入运行时。
    void rescanSkills();
    /// 语言改变后询问是否立即重启（本窗口不做逐控件重译，见 .cpp 说明）。
    void offerRestartForLanguageChange();
    /// 处理以 `/` 开头的斜杠命令（不发给模型）。
    void handleSlashCommand(const QString & rawText);

    // ── 交互 ────────────────────────────────────────────────────────────────
    void onSendRequested();
    /// 选择图片附件（文件对话框）。
    void onAttachImagesRequested();
    /// 粘贴/拖入的图片直接进待发附件。
    void attachImages(const QStringList & paths);
    void attachImageData(const QByteArray & bytes, const QString & mimeType,
                         const QString & suggestedName);
    void onStopRequested();
    void onModelChanged(int index);
    void onModeChanged(int index);
    void onReasoningLevelChanged(int index);
    void onSettingsRequested();

  private:
    void buildUi();
    void buildMenus();
    void wireRuntime();
    void applyTheme();
    /// 把 settings_.themeMode 同步到「视图 → 主题」那三个互斥勾选项上。
    void syncThemeMenuChecks();
    void applySettingsToUi();
    void loadWorkspaceSessions();
    void refreshModelCombo();
    /// 按当前模型重建思考档位下拉。不支持思考的模型会隐藏该控件。
    void refreshReasoningCombo();
    void refreshContextUsage();
    void refreshRunState();
    void showNextPermissionDialog();
    void saveSettings();
    void openSessionOrCreate();
    /// 当前工作区下最近使用的模型；无记录时回退到全局最近模型或第一个可用模型。
    ModelSelection effectiveModelSelection() const;
    /// 状态栏文案。
    void setStatusMessage(const QString & message);

    // ── 依赖与状态 ──────────────────────────────────────────────────────────
    AppSettings settings_;
    SessionStore store_;
    ToolRegistry tools_;
    ProviderRegistry providers_;
    AgentRuntime runtime_;
    /// TodoStore 由 runtime_ 借用，本窗口持有其生命周期。
    std::unique_ptr<TodoStore> todoStore_;

    Workspace workspace_;
    ModelSelection currentModel_;
    Id activeSessionId_;
    bool suppressModelSignal_ = false;
    bool suppressModeSignal_ = false;

    /// 等待展示的权限请求队列。UI 一次只展示一个，避免弹窗风暴。
    QList<PermissionRequest> permissionQueue_;
    QHash<Id, PermissionRequest> permissionRequests_;

    // ── 菜单 ────────────────────────────────────────────────────────────────
    /// 「视图 → 主题」的互斥组。必须留成员：设置对话框里改了主题后要同步勾选，
    /// 否则菜单仍显示旧主题选中，与实际配色矛盾（用户报的就是这个）。
    QActionGroup * themeGroup_ = nullptr;

    // ── 控件 ────────────────────────────────────────────────────────────────
    QSplitter * splitter_ = nullptr;
    SidebarPanel * sidebar_ = nullptr;
    ConversationView * conversation_ = nullptr;

    QComboBox * modelCombo_ = nullptr;
    QComboBox * reasoningCombo_ = nullptr;
    QComboBox * modeCombo_ = nullptr;
    QLabel * contextLabel_ = nullptr;
    QProgressBar * contextBar_ = nullptr;

    QPlainTextEdit * composer_ = nullptr;
    /// 附件条：待发图片的缩略图与移除按钮。空时隐藏。
    QPushButton * attachButton_ = nullptr;
    QWidget * attachmentStrip_ = nullptr;
    QHBoxLayout * attachmentLayout_ = nullptr;
    /// 待发附件。发送成功后清空；切换会话时清空（它们属于"这次编辑"）。
    QList<FilePart> pendingAttachments_;
    /// 重建附件条。
    void refreshAttachmentStrip();

    /// 问询卡片：模型按统一的 `choices` 格式给出选项时，在输入框上方渲染成卡片。
    /// 直接开在输入框上方——它是"这一次输入"的辅助，不是对话内容的一部分。
    void refreshChoices();
    /// 用户选完并提交：把拼好的回答写进输入框并**直接发送**。
    void onChoicesAnswered(const QString & answer);
    ChoiceCard * choiceCard_ = nullptr;
    /// 已经放弃过问询卡片的那条消息。刷新时据此不再把同一张卡片摆回来。
    Id choicesDismissedFor_;
    QPushButton * sendButton_ = nullptr;
    QPushButton * stopButton_ = nullptr;

    QLabel * runStateLabel_ = nullptr;
    QLabel * backgroundLabel_ = nullptr;
    QLabel * mcpLabel_ = nullptr;
    /// MCP 服务器集合。启动时按配置拉起，工具直接注册进 tools_。
    std::unique_ptr<lycode::mcp::Manager> mcp_;
    /// Skills 库。扫描一次，注入运行时；换工作区时重扫。
    lycode::skills::Library skills_;
    /// 为 MCP 握手等待用户输入的计时与状态。
    QElapsedTimer mcpWaitElapsed_;
    bool mcpWaitPending_ = false;
    QLabel * usageLabel_ = nullptr;
    QTimer * usageTimer_ = nullptr;
    /// 本轮运行已耗时。用于状态栏的"运行中（12s）"。
    QElapsedTimer turnTimer_;
};

}  // namespace lycode::ui
