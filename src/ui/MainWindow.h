// ZCode Qt — 主窗口
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

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <memory>

#include "agent/AgentRuntime.h"
#include "agent/PermissionGate.h"
#include "core/Types.h"
#include "model/ProviderRegistry.h"
#include "storage/SessionStore.h"
#include "tools/Tool.h"
#include "ui/AppConfig.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTimer;

namespace zcode::ui {

class ConversationView;
class SidebarPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    /// 拦截输入框的回车：Enter 发送、Shift+Enter 换行，
    /// 但输入法组合期间必须放行，否则中文选词的回车会被吃掉。
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // ── 会话 ────────────────────────────────────────────────────────────────
    void onNewSessionRequested();
    void onSessionSelected(const Id &sessionId);
    void onSessionDeleteRequested(const Id &sessionId);
    void onWorkspaceChangeRequested();
    void onOpenWorkspacePath(const QString &path);

    // ── 运行时信号 ──────────────────────────────────────────────────────────
    void onMessageAdded(const Message &message);
    void onMessageFinished(const Message &message);
    void onPartAppended(const Id &messageId, const Part &part);
    void onPartUpdated(const Id &messageId, const Part &part);
    void onDeltaAppended(const Id &messageId, const Id &partId, const QString &delta,
                         bool reasoning);
    void onSessionChanged(const Session &session);
    void onRunStateChanged(RunState state);
    void onPermissionRequested(const PermissionRequest &request);
    void onPermissionResolved(const Id &requestId);
    void onTurnFinished(TurnResult result);
    void onFailed(const QString &message);
    /// 子代理的会话状态变化：只更新列表，**不切换**当前会话。
    void onSubagentSessionChanged(const zcode::Session &session);
    void onSubagentFinished(const zcode::Id &childSessionId, bool ok);
    /// 后台任务数量变化：状态栏给出可见提示，否则用户不知道有进程还在跑。
    void onBackgroundTasksChanged(int runningCount);

    // ── 交互 ────────────────────────────────────────────────────────────────
    void onSendRequested();
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
    void setStatusMessage(const QString &message);

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

    // ── 控件 ────────────────────────────────────────────────────────────────
    QSplitter *splitter_ = nullptr;
    SidebarPanel *sidebar_ = nullptr;
    ConversationView *conversation_ = nullptr;

    QComboBox *modelCombo_ = nullptr;
    QComboBox *reasoningCombo_ = nullptr;
    QComboBox *modeCombo_ = nullptr;
    QLabel *contextLabel_ = nullptr;
    QProgressBar *contextBar_ = nullptr;

    QPlainTextEdit *composer_ = nullptr;
    QPushButton *sendButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;

    QLabel *runStateLabel_ = nullptr;
    QLabel *backgroundLabel_ = nullptr;
    QLabel *usageLabel_ = nullptr;
    QTimer *usageTimer_ = nullptr;
    /// 本轮运行已耗时。用于状态栏的"运行中（12s）"。
    QElapsedTimer turnTimer_;
};

}  // namespace zcode::ui
