// ZCode Qt — 侧边栏（工作区 + 会话列表）
//
// 结构（自上而下）：
//   1. 工作区切换行：显示当前工作区名与路径，点击可换目录
//   2. 新建会话按钮
//   3. 会话列表：按更新时间倒序，显示标题、一行摘要、等待确认徽章
//   4. 底部：设置入口
//
// 会话列表只展示 SessionSummary（不携带消息体），避免为了画列表把所有历史
// 加载进内存——这在有几百个会话时会非常明显。
#pragma once

#include <QHash>
#include <QList>
#include <QWidget>

#include "core/Types.h"
#include "storage/SessionStore.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QResizeEvent;

namespace zcode::ui {

class SidebarPanel : public QWidget {
    Q_OBJECT

public:
    explicit SidebarPanel(QWidget *parent = nullptr);
    ~SidebarPanel() override;

    void setWorkspace(const Workspace &workspace);
    void setSessions(const QList<SessionSummary> &sessions);
    void setActiveSession(const Id &sessionId);
    /// 就地更新一条会话的摘要（运行中标题/状态会变），不存在则插入到顶部。
    void upsertSession(const SessionSummary &summary);
    /// 选中当前项对应的会话 id；无选中返回空。
    Id activeSessionId() const;

    /// 设置最近工作区列表（用于切换菜单中的快捷项）。
    void setRecentWorkspaces(const QStringList &paths);

protected:
    /// 侧边栏宽度变化时重新省略工作区路径。
    void resizeEvent(QResizeEvent *event) override;
    /// 让路径标签与工作区按钮一样可点。
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void newSessionRequested();
    void sessionSelected(const zcode::Id &sessionId);
    void workspaceChangeRequested();
    /// 从最近工作区列表移除（**不碰任何数据**）。
    /// 与下一个信号分开，是因为"不想在菜单里看到它"和"删掉它的会话"
    /// 是两件性质完全不同的事，不能合成一个动作。
    void workspaceRemoveRequested(const QString &path);
    /// 删除该工作区下的全部会话（调用方负责二次确认）。
    void workspacePurgeRequested(const QString &path);

    void workspaceRecentRequested(const QString &path);
    void sessionDeleteRequested(const zcode::Id &sessionId);
    void settingsRequested();

private:
    void buildUi();
    void applyTheme();
    void refreshEmptyState();
    /// 弹出工作区切换菜单（按钮点击与路径标签点击共用）。
    void showWorkspaceMenu();
    /// 按当前标签宽度重新计算路径的省略显示。
    void updateWorkspacePathLabel();
    QListWidgetItem *makeItem(const SessionSummary &summary) const;
    /// 从 item 上取会话 id。
    static Id itemSessionId(const QListWidgetItem *item);

    Workspace workspace_;
    QList<SessionSummary> sessions_;

    QLabel *workspacePathLabel_ = nullptr;
    QPushButton *workspaceButton_ = nullptr;
    QPushButton *newSessionButton_ = nullptr;
    QListWidget *sessionList_ = nullptr;
    QLabel *emptyHint_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QHash<Id, QListWidgetItem *> itemsById_;
    bool updatingSelection_ = false;
    /// 最近工作区（由 MainWindow 从设置同步过来），用于切换菜单。
    QStringList recentWorkspaces_;
};

}  // namespace zcode::ui
