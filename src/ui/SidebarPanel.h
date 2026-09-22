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
class QTreeWidgetItem;
class QListWidgetItem;
class QPushButton;
class QResizeEvent;

#include <QSet>

#include <QTreeWidget>

namespace zcode::ui {

class SidebarPanel : public QWidget {
    Q_OBJECT

public:
    explicit SidebarPanel(QWidget *parent = nullptr);
    ~SidebarPanel() override;

    void setWorkspace(const Workspace &workspace);
    /// 重建顶层的工作区节点。`paths` 里的顺序就是树的顺序。
    void setWorkspaces(const QStringList &paths);
    /// 设置**当前工作区**的会话（树的子节点）。
    void setSessions(const QList<SessionSummary> &sessions);
    /// 设置任意工作区的会话。展开一个非当前工作区时由 MainWindow 调用。
    void setWorkspaceSessions(const QString &workspacePath,
                              const QList<SessionSummary> &sessions);
    /// 展开工作区节点；子节点还没加载时发出 workspaceExpandRequested。
    void onWorkspaceExpanded(QTreeWidgetItem *item);
    /// 恢复这批工作区的展开状态（启动时用）。
    /// 展开会触发 workspaceExpandRequested，从而按需加载它们的会话。
    void setExpandedWorkspaces(const QStringList &paths);
    void setActiveSession(const Id &sessionId);
    /// 就地更新一条会话的摘要（运行中标题/状态会变），不存在则插入到顶部。
    void upsertSession(const SessionSummary &summary);
    /// 选中当前项对应的会话 id；无选中返回空。
    Id activeSessionId() const;
    /// 某个会话属于哪个工作区。MainWindow 用它决定"要不要先切工作区"。
    QString workspacePathForSession(const Id &sessionId) const;
    /// 当前展开的工作区集合（重建树时用来恢复展开状态）。
    QSet<QString> expandedWorkspaces() const;
    /// 某个工作区的会话节点是否已经加载过。
    bool workspaceLoaded(const QString &path) const { return loadedWorkspaces_.contains(path); }

    /// 设置最近工作区列表（用于切换菜单中的快捷项）。
    void setRecentWorkspaces(const QStringList &paths);

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
    /// 某个工作区的展开/折叠状态变了。调用方据此持久化。
    void workspaceExpansionChanged(const QString &path, bool expanded);
    /// 在某个工作区里新建会话（工作区行上的 "+"）。
    /// 带上路径而不是用"当前工作区"：入口在树上的每一行，用户点的是**那一行**。
    void newSessionRequestedInWorkspace(const QString &path);
    /// 工作区节点被展开且子节点尚未加载。
    void workspaceExpandRequested(const QString &path);
    void sessionDeleteRequested(const zcode::Id &sessionId);
    void settingsRequested();

private:
    void buildUi();
    void applyTheme();
    void refreshEmptyState();
    /// 建一个会话子节点。
    QTreeWidgetItem *makeSessionItem(const SessionSummary &summary) const;
    /// 刷新工作区节点的文本（含展开指示符）。
    void updateWorkspaceItemText(QTreeWidgetItem *item);
    /// 建/取一个工作区顶层节点。
    QTreeWidgetItem *ensureWorkspaceItem(const QString &path);
    /// 从 item 上取会话 id（非会话节点返回空）。
    static Id itemSessionId(const QTreeWidgetItem *item);
    /// 该项或其祖先的工作区路径。
    QString workspacePathOf(const QTreeWidgetItem *item) const;

    Workspace workspace_;
    QList<SessionSummary> sessions_;

    /// 顶部按钮：选目录并加进工作区列表（**不是**新建会话）。
    QPushButton *newWorkspaceButton_ = nullptr;
    QTreeWidget *sessionTree_ = nullptr;
    QLabel *emptyHint_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QHash<Id, QTreeWidgetItem *> itemsById_;
    /// 工作区路径 → 顶层节点。
    QHash<QString, QTreeWidgetItem *> workspaceItems_;
    /// 已加载过子节点的工作区。
    QSet<QString> loadedWorkspaces_;
    bool updatingSelection_ = false;
    /// 最近工作区（由 MainWindow 从设置同步过来），用于切换菜单。
    QStringList recentWorkspaces_;
};

}  // namespace zcode::ui
