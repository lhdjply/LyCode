#include "ui/SidebarPanel.h"

#include "ui/Markdown.h"
#include "ui/Theme.h"

#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QHeaderView>
#include <QToolButton>
#include <QTreeWidget>
#include <QLoggingCategory>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace lycode::ui {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.ui.sidebar")

/// 会话列表摘要的最大字符数。列表项只有一行摘要，太长会挤掉标题的视觉权重。

/// 会话列表项上挂的自定义数据角色。
constexpr int kSessionIdRole = Qt::UserRole + 1;
/// 工作区顶层节点上存工作区路径。会话子节点不带这个角色，
/// 靠 workspacePathOf() 向上找到祖先。
constexpr int kWorkspacePathRole = Qt::UserRole + 3;
/// 工作区节点的显示名（不含展开指示符，指示符是拼上去的）。
constexpr int kWorkspaceNameRole = Qt::UserRole + 4;
/// 放 "+" 按钮的列。
constexpr int kActionColumn = 1;
/// "+" 那一列的宽度。够放一个 20px 的按钮即可，正文要尽量宽。
constexpr int kActionColumnWidth = 22;
/// 会话条目的行高（单行）。
constexpr int kSessionRowHeight = 26;
constexpr int kNeedsAttentionRole = Qt::UserRole + 2;

/// 会话状态的中文短标签。空字符串表示"无需展示徽章"。
QString statusBadge(const Session &session) {
    if (session.pendingPermissionCount > 0) {
        return QStringLiteral("%1 待确认").arg(session.pendingPermissionCount);
    }
    // 子代理会话与主会话混在同一个列表里，必须能一眼分出来。
    if (session.isSubagent()) {
        switch (session.status) {
            case SessionStatus::Running:
            case SessionStatus::Prewarming:
                return QStringLiteral("子代理 · 运行中");
            case SessionStatus::Error:
                return QStringLiteral("子代理 · 出错");
            default:
                return QStringLiteral("子代理");
        }
    }
    switch (session.status) {
        case SessionStatus::Running:
        case SessionStatus::Prewarming:
            return QStringLiteral("运行中");
        case SessionStatus::Error:
            return QStringLiteral("出错");
        case SessionStatus::Draft:
        case SessionStatus::CompletedSuccess:
        case SessionStatus::CompletedInterrupted:
            return {};
    }
    return {};
}

}  // namespace

SidebarPanel::SidebarPanel(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sidebarPanel"));
    buildUi();
    connect(&Theme::instance(), &Theme::changed, this, &SidebarPanel::applyTheme);
    applyTheme();
}

SidebarPanel::~SidebarPanel() = default;

void SidebarPanel::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    // ── 新建工作区 ──────────────────────────────────────────────────────────
    // 顶部只留一个按钮：新建工作区。会话的新建入口移到了**每个工作区行上**——
    // 那才是"在哪建"这个信息真正所在的位置。
    // 措辞与菜单栏的「打开工作区…」保持一致——同一个动作在两个地方叫两个名字，
    // 用户会以为是两件事。也不该叫"新建"：目录本来就在磁盘上，是**打开**它。
    newWorkspaceButton_ = new QPushButton(QStringLiteral("打开工作区…"));
    newWorkspaceButton_->setObjectName(QStringLiteral("newWorkspaceButton"));
    newWorkspaceButton_->setToolTip(QStringLiteral("选择已有目录，把它加进工作区列表"));
    newWorkspaceButton_->setProperty("accent", true);
    newWorkspaceButton_->setCursor(Qt::PointingHandCursor);
    // ⚠ 这个按钮的动作是"选目录并加进工作区列表"，**不是**新建会话。
    // 会话的新建入口在每一行工作区右侧的 "+" 上。（曾经改了按钮文字却忘了改
    // 这里接的信号，于是"新建工作区"点下去实际建了一个会话。）
    connect(newWorkspaceButton_, &QPushButton::clicked, this,
            &SidebarPanel::workspaceChangeRequested);
    root->addWidget(newWorkspaceButton_);

    // ── 工作区 / 会话树 ─────────────────────────────────────────────────────
    // 用树而不是"工作区按钮 + 当前工作区的会话列表"：用户要同时看到**多个**
    // 工作区各自有哪些会话，平铺列表只能显示当前那一个。
    sessionTree_ = new QTreeWidget;
    sessionTree_->setObjectName(QStringLiteral("sessionTree"));
    sessionTree_->setFrameShape(QFrame::NoFrame);
    sessionTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    sessionTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    sessionTree_->setWordWrap(false);
    // 去掉树枝与展开箭头之外的装饰，让层级靠缩进表达。
    sessionTree_->setColumnCount(2);
    // 第一列是名字（拉伸），第二列固定宽度放 "+"。
    sessionTree_->header()->setStretchLastSection(false);
    sessionTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    sessionTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    // 用 resizeSection 而不是 setColumnWidth：后者是"建议宽度"，
    // 表头会按内容再算一遍——实测要 26px 却给了 46px，白白吃掉 20px 正文宽度。
    sessionTree_->header()->resizeSection(1, kActionColumnWidth);
    sessionTree_->header()->setMinimumSectionSize(1);
    sessionTree_->setIndentation(14);
    // 关掉 Qt 自带的树枝装饰：展开指示符已经由我们自己画（`▸`/`▾`），
    // 而它的装饰在深色主题下会画出一小块底色——就是选中行左侧那个色块。
    sessionTree_->setRootIsDecorated(false);
    sessionTree_->setHeaderHidden(true);
    sessionTree_->setExpandsOnDoubleClick(false);
    connect(sessionTree_, &QTreeWidget::itemSelectionChanged, this, [this]() {
        if (updatingSelection_) {
            return;
        }
        const Id id = activeSessionId();
        if (!id.isEmpty()) {
            emit sessionSelected(id);
        }
    });
    connect(sessionTree_, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) { onWorkspaceExpanded(item); });
    // 点击工作区节点 = 展开/折叠。
    //
    // 必须自己做：展开箭头换成了文字指示符（`▸`/`▾`）之后就没有可点的箭头了，
    // 而只靠双击展开又被 setExpandsOnDoubleClick(false) 关掉——结果是用户**根本
    // 没有办法折叠**一个工作区。会话节点不受影响（它们没有子节点）。
    connect(sessionTree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (item == nullptr || !itemSessionId(item).isEmpty()) {
                    return;
                }
                item->setExpanded(!item->isExpanded());
            });
    connect(sessionTree_, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem *item) {
                updateWorkspaceItemText(item);
                emit workspaceExpansionChanged(item->data(0, kWorkspacePathRole).toString(),
                                               false);
            });
    connect(sessionTree_, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                QTreeWidgetItem *item = sessionTree_->itemAt(position);
                if (item == nullptr) {
                    return;
                }
                QMenu menu(this);
                const Id id = itemSessionId(item);
                if (id.isEmpty()) {
                    // 工作区节点**没有**"打开"这一项：展开/折叠只是界面收起，
                    // 不是"打开/关闭工作区"。要切到某个工作区，点它下面的会话即可
                    //（MainWindow 会先切工作区再打开），或点它右侧的 "+" 新建一个。
                    const QString path = workspacePathOf(item);
                    QAction *remove = menu.addAction(QStringLiteral("从最近列表移除"));
                    QAction *purge = menu.addAction(QStringLiteral("删除该工作区的全部会话…"));
                    QAction *chosen = menu.exec(sessionTree_->mapToGlobal(position));
                    if (chosen == remove) {
                        emit workspaceRemoveRequested(path);
                    } else if (chosen == purge) {
                        emit workspacePurgeRequested(path);
                    }
                    return;
                }
                QAction *remove = menu.addAction(QStringLiteral("删除会话"));
                if (menu.exec(sessionTree_->mapToGlobal(position)) == remove) {
                    emit sessionDeleteRequested(id);
                }
            });
    root->addWidget(sessionTree_, 1);

    // 空状态提示与树互斥显示。
    emptyHint_ = new QLabel(QStringLiteral("还没有会话。"));
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setFont(Theme::instance().font(FontRole::UiSm));
    emptyHint_->setVisible(false);
    root->addWidget(emptyHint_);

    // ── 设置入口 ────────────────────────────────────────────────────────────
    settingsButton_ = new QPushButton(QStringLiteral("设置"));
    settingsButton_->setObjectName(QStringLiteral("settingsButton"));
    settingsButton_->setProperty("variant", QStringLiteral("ghost"));
    settingsButton_->setCursor(Qt::PointingHandCursor);
    connect(settingsButton_, &QPushButton::clicked, this, &SidebarPanel::settingsRequested);
    root->addWidget(settingsButton_);
}

void SidebarPanel::applyTheme() {
    const Theme &theme = Theme::instance();
    const Palette &palette = theme.palette();

    setStyleSheet(QStringLiteral("QWidget#sidebarPanel { background-color: %1; }")
                      .arg(Theme::css(palette.sidebar)));

    emptyHint_->setStyleSheet(
        QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));

    // ⚠ 这几点是"标题左右各有一块脏色"的成因，别随手改：
    //   * `padding` 会**把背景一起内缩**。Qt 的 `::item` 是**按单元格**生效的，
    //     而会话行第二列是空的（那一列只给工作区放 "+"）——横向 padding 让两列
    //     的背景各自内缩，中间留出缝隙，看起来就是左边一块、右边一块。
    //     横向 padding 必须是 0，行高改用纵向 padding 控制。
    //   * 同理不要加 border-radius：那会让每个单元格各自画一个圆角块。
    //   * ::branch 要显式透明，否则缩进区域会露出默认底色（左边那条竖色块）。
    // show-decoration-selected 必须归零：全局样式表给所有 item view 设了
    // `show-decoration-selected: 1`，那会让选中背景**延伸进缩进/树枝区域**，
    // 而那块是独立绘制的，于是选中行左侧又多出一个分离的小色块。
    sessionTree_->setStyleSheet(
        QStringLiteral("QTreeWidget { background: transparent; border: none; outline: none; "
                       "show-decoration-selected: 0; }"
                       "QTreeView::branch { background: transparent; }"
                       // margin 必须显式归零：全局样式表里有
                       // `QTreeView::item { margin: 1px 4px; border-radius: 8px; }`，
                       // 它按**单元格**把背景内缩——会话行第二列是空的
                       // （那一列只给工作区放 "+"），于是同一行被画成左右两个
                       // 独立圆角块，看起来就是"标题左右有色块"。
                       "QTreeWidget::item { margin: 0px; padding: 5px 0px; "
                       "border-radius: 0px; }"
                       "QTreeWidget::item:selected { background-color: %1; }")
            .arg(Theme::css(palette.selected)));
}

QTreeWidgetItem *SidebarPanel::makeSessionItem(const SessionSummary &summary) const {
    const Session &session = summary.session;

    QString title = session.title.trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("未命名会话");
    }

    // **只显示标题，一行。**
    //
    // 曾经把"最后收到的消息"摘要作为第二行：一是两行让每条会话都很占高度，
    // 二是那段摘要要经过 markdown 去标记 + 截断才能勉强读，信息量低却抢走
    // 一半的可视面积。消息内容看右侧对话流即可。
    //
    // 状态徽章（待确认/运行中/子代理/出错）仍然保留——它是有行动价值的信息，
    // 但合并到同一行，不再占第二行。
    const QString badge = statusBadge(session);
    const QString line = badge.isEmpty() ? title : badge + QStringLiteral(" · ") + title;

    auto *item = new QTreeWidgetItem;
    item->setText(0, line);
    // 单行高度：不设的话条目仍按两行的旧高度留白。
    item->setSizeHint(0, QSize(0, kSessionRowHeight));
    item->setData(0, kSessionIdRole, session.id);
    item->setData(0, kNeedsAttentionRole, session.isWaitingOnUser());
    item->setToolTip(0, session.workspace.path);
    // 会话节点不可展开：它没有子节点，箭头会误导。
    item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
    return item;
}

Id SidebarPanel::itemSessionId(const QTreeWidgetItem *item) {
    if (item == nullptr) {
        return {};
    }
    return item->data(0, kSessionIdRole).toString();
}

void SidebarPanel::setWorkspace(const Workspace &workspace) {
    workspace_ = workspace;
    newWorkspaceButton_->setEnabled(true);  // 新建工作区任何时候都可用
    // 当前工作区必须出现在树上（切到一个不在最近列表里的目录时）。
    ensureWorkspaceItem(workspace_.path);
}

void SidebarPanel::setExpandedWorkspaces(const QStringList &paths) {
    for (const QString &path : paths) {
        if (QTreeWidgetItem *item = workspaceItems_.value(path, nullptr)) {
            // setExpanded 会触发 itemExpanded → onWorkspaceExpanded →
            // workspaceExpandRequested，于是会话按需加载，不需要在这里再查一次库。
            item->setExpanded(true);
        }
    }
}

void SidebarPanel::setRecentWorkspaces(const QStringList &paths) {
    recentWorkspaces_ = paths;
    // 最近工作区就是树的顶层节点。放在这里而不是让 MainWindow 再调一次
    // setWorkspaces：两者必须是同一份列表，分开调用迟早会不一致。
    QStringList topLevel = paths;
    if (!workspace_.path.isEmpty() && !topLevel.contains(workspace_.path)) {
        topLevel.prepend(workspace_.path);
    }
    setWorkspaces(topLevel);
}

void SidebarPanel::setWorkspaces(const QStringList &paths) {
    // 保留已有节点的展开状态与加载标记：重建时不该把用户的展开收起来。
    const QSet<QString> expandedBefore = expandedWorkspaces();

    updatingSelection_ = true;

    // 逐个复用/新建，而不是 clear() 重来：clear 会销毁 item，
    // 顺带丢掉展开状态与滚动位置，用户每切一次会话就要重新展开一遍。
    QStringList wanted;
    for (const QString &path : paths) {
        if (!path.isEmpty() && !wanted.contains(path)) {
            wanted.append(path);
        }
    }
    if (!workspace_.path.isEmpty() && !wanted.contains(workspace_.path)) {
        wanted.prepend(workspace_.path);
    }

    // 删掉不再需要的节点。
    for (auto it = workspaceItems_.begin(); it != workspaceItems_.end();) {
        if (!wanted.contains(it.key())) {
            const int index = sessionTree_->indexOfTopLevelItem(it.value());
            delete sessionTree_->takeTopLevelItem(index);
            loadedWorkspaces_.remove(it.key());
            it = workspaceItems_.erase(it);
        } else {
            ++it;
        }
    }

    for (int index = 0; index < wanted.size(); ++index) {
        QTreeWidgetItem *item = ensureWorkspaceItem(wanted.at(index));
        const int current = sessionTree_->indexOfTopLevelItem(item);
        if (current != index) {
            sessionTree_->takeTopLevelItem(current);
            sessionTree_->insertTopLevelItem(index, item);
        }
    }
    updatingSelection_ = false;

    refreshEmptyState();

    // ⚠ 必须**在条目建好之后**再设一次列宽。表头会在插入带 itemWidget 的条目时
    // 按内容重算，把之前设的 22px 撑成 40px，白吃掉 18px 正文宽度
    // （实测：要 22 给了 40）。这是最后一次生效的机会。
    sessionTree_->header()->resizeSection(kActionColumn, kActionColumnWidth);

    // 恢复展开状态。
    for (const QString &path : expandedBefore) {
        if (QTreeWidgetItem *item = workspaceItems_.value(path, nullptr)) {
            item->setExpanded(true);
        }
    }
    qCDebug(log) << "工作区树已刷新，共" << wanted.size() << "个工作区";
}

QSet<QString> SidebarPanel::expandedWorkspaces() const {
    QSet<QString> result;
    for (auto it = workspaceItems_.constBegin(); it != workspaceItems_.constEnd(); ++it) {
        if (it.value()->isExpanded()) {
            result.insert(it.key());
        }
    }
    return result;
}

QTreeWidgetItem *SidebarPanel::ensureWorkspaceItem(const QString &path) {
    if (path.isEmpty()) {
        // 没有工作区时不该凭空建一个空路径节点（会显示成一个没有名字的顶层项）。
        return nullptr;
    }
    if (QTreeWidgetItem *existing = workspaceItems_.value(path, nullptr)) {
        // 当前工作区的名字要跟着 workspace_ 走（路径可能被规范化过）。
        if (path == workspace_.path) {
            existing->setData(0, kWorkspaceNameRole,
                              workspace_.displayName().isEmpty() ? path
                                                                 : workspace_.displayName());
            updateWorkspaceItemText(existing);
        }
        return existing;
    }

    auto *item = new QTreeWidgetItem;
    item->setData(0, kWorkspaceNameRole,
                  path == workspace_.path && !workspace_.displayName().isEmpty()
                      ? workspace_.displayName()
                      : (QDir(path).dirName().isEmpty() ? path : QDir(path).dirName()));
    updateWorkspaceItemText(item);
    item->setToolTip(0, path);
    item->setData(0, kWorkspacePathRole, path);
    // 已加载过就直接给箭头；没加载过先不给——展开时才去库里取，
    // 避免启动时为每个工作区都查一次库。
    // ShowIndicator 表示"即使现在没有子节点也显示展开器"。工作区是懒加载的，
    // 未加载时 childCount() 为 0，用 DontShowIndicator 会让它看起来不可展开。
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    sessionTree_->addTopLevelItem(item);

    // 每个工作区行右侧一个 "+"：新建会话的入口放在**它所属的工作区**上，
    // 而不是顶部——"在哪建"这个信息本来就属于这一行。
    auto *addSession = new QToolButton;
    addSession->setObjectName(QStringLiteral("addSessionButton"));
    addSession->setText(QStringLiteral("+"));
    addSession->setCursor(Qt::PointingHandCursor);
    addSession->setAutoRaise(true);
    addSession->setFixedSize(20, 20);
    addSession->setToolTip(QStringLiteral("在「%1」里新建会话")
                               .arg(item->data(0, kWorkspaceNameRole).toString()));
    item->setToolTip(0, path);
    connect(addSession, &QToolButton::clicked, this,
            [this, path]() { emit newSessionRequestedInWorkspace(path); });
    sessionTree_->setItemWidget(item, kActionColumn, addSession);
    workspaceItems_.insert(path, item);
    return item;
}

void SidebarPanel::updateWorkspaceItemText(QTreeWidgetItem *item) {
    if (item == nullptr) {
        return;
    }
    const QString name = item->data(0, kWorkspaceNameRole).toString();
    // ⚠ 展开指示符必须**自己画**：一旦给 QTreeView 设了样式表，Qt 就改用
    // `QTreeView::branch` 来画箭头，而它默认没有图像资源——结果就是箭头整个
    // 消失，用户看不出这个节点能不能折叠。用文字指示符最省事且不依赖图片。
    //
    // ⚠ 而且**必须无条件显示**，不能按 childCount() 判断：会话是懒加载的，
    // 一个没被展开过的工作区在界面上 childCount() 就是 0——按它判断会让这些
    // 工作区既没有 `▸` 也没有 `▾`，看起来根本不能折叠（实测踩到）。
    // 空工作区多一个小小的 `▸` 无所谓，而"看不出能不能点"是真问题。
    const QString marker = item->isExpanded() ? QStringLiteral("▾ ") : QStringLiteral("▸ ");
    item->setText(0, marker + name);
}

void SidebarPanel::onWorkspaceExpanded(QTreeWidgetItem *item) {
    if (item == nullptr || item->parent() != nullptr) {
        return;
    }
    updateWorkspaceItemText(item);
    const QString path = item->data(0, kWorkspacePathRole).toString();
    emit workspaceExpansionChanged(path, true);
    if (path.isEmpty() || loadedWorkspaces_.contains(path)) {
        return;
    }
    emit workspaceExpandRequested(path);
}

void SidebarPanel::setSessions(const QList<SessionSummary> &sessions) {
    setWorkspaceSessions(workspace_.path, sessions);
}

void SidebarPanel::setWorkspaceSessions(const QString &workspacePath,
                                        const QList<SessionSummary> &sessions) {
    if (workspacePath.isEmpty()) {
        return;
    }
    QTreeWidgetItem *parent = ensureWorkspaceItem(workspacePath);
    if (parent == nullptr) {
        return;
    }
    loadedWorkspaces_.insert(workspacePath);

    const Id previouslyActive = activeSessionId();
    updatingSelection_ = true;

    // 重建这个工作区的子节点。先摘掉旧节点并清索引，避免 itemsById_ 里
    // 留下指向已销毁 item 的悬垂指针。
    for (QTreeWidgetItem *child : parent->takeChildren()) {
        itemsById_.remove(itemSessionId(child));
        delete child;
    }
    for (const SessionSummary &summary : sessions) {
        QTreeWidgetItem *item = makeSessionItem(summary);
        parent->addChild(item);
        itemsById_.insert(summary.session.id, item);
    }
    // 始终保留展开器：折叠状态本身是可见的（`▸`/`▾`），
    // 藏起来反而让人以为这个工作区不能点。
    parent->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);

    updatingSelection_ = false;
    updateWorkspaceItemText(parent);

    if (workspacePath == workspace_.path) {
        sessions_ = sessions;
    }
    refreshEmptyState();

    if (!previouslyActive.isEmpty() && itemsById_.contains(previouslyActive)) {
        setActiveSession(previouslyActive);
    }
    qCDebug(log) << "工作区会话已刷新;" << workspacePath << sessions.size() << "项";
}

void SidebarPanel::upsertSession(const SessionSummary &summary) {
    const Id id = summary.session.id;
    const QString path = summary.session.workspace.path;
    if (path.isEmpty()) {
        // 没有工作区归属的会话不该出现在树上（它属于"无工作区"这个过渡状态）。
        return;
    }
    QTreeWidgetItem *parent = ensureWorkspaceItem(path);
    if (parent == nullptr) {
        return;
    }

    if (QTreeWidgetItem *existing = itemsById_.value(id, nullptr)) {
        // 就地更新文本，保留选中态与滚动位置。
        QTreeWidgetItem *fresh = makeSessionItem(summary);
        existing->setText(0, fresh->text(0));
        existing->setData(0, kNeedsAttentionRole, fresh->data(0, kNeedsAttentionRole));
        existing->setToolTip(0, fresh->toolTip(0));
        delete fresh;
        return;
    }

    QTreeWidgetItem *item = makeSessionItem(summary);
    parent->insertChild(0, item);
    itemsById_.insert(id, item);
    parent->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    loadedWorkspaces_.insert(path);
    updateWorkspaceItemText(parent);
    if (path == workspace_.path) {
        sessions_.prepend(summary);
    }
    refreshEmptyState();
}

void SidebarPanel::setActiveSession(const Id &sessionId) {
    QTreeWidgetItem *item = itemsById_.value(sessionId, nullptr);
    if (item == nullptr) {
        updatingSelection_ = true;
        sessionTree_->clearSelection();
        updatingSelection_ = false;
        return;
    }

    // 选中前先把它的父工作区展开：折叠状态下选中一个看不到的节点，
    // 用户会以为点击没反应。
    if (QTreeWidgetItem *parent = item->parent()) {
        parent->setExpanded(true);
    }

    // 程序化选中不应反过来触发 sessionSelected，否则会形成回环。
    updatingSelection_ = true;
    sessionTree_->setCurrentItem(item);
    sessionTree_->scrollToItem(item, QAbstractItemView::EnsureVisible);
    updatingSelection_ = false;
}

Id SidebarPanel::activeSessionId() const {
    return itemSessionId(sessionTree_->currentItem());
}

QString SidebarPanel::workspacePathForSession(const Id &sessionId) const {
    QTreeWidgetItem *item = itemsById_.value(sessionId, nullptr);
    return item != nullptr ? workspacePathOf(item) : QString();
}

QString SidebarPanel::workspacePathOf(const QTreeWidgetItem *item) const {
    const QTreeWidgetItem *current = item;
    while (current != nullptr) {
        const QString path = current->data(0, kWorkspacePathRole).toString();
        if (!path.isEmpty()) {
            return path;
        }
        current = current->parent();
    }
    return {};
}

void SidebarPanel::refreshEmptyState() {
    const bool isEmpty = sessionTree_->topLevelItemCount() == 0;
    if (isEmpty) {
        // 两种"空"的下一步动作完全不同：没有工作区要去加一个；
        // 有工作区但没会话就去新建会话。提示语混用会让用户点错地方。
        emptyHint_->setText(workspace_.isValid()
                                ? QStringLiteral("这个工作区还没有会话。\n点工作区行右侧的 + 新建一个。")
                                : QStringLiteral("还没有工作区。\n点上面的「打开工作区…」选择一个目录。"));
    }
    emptyHint_->setVisible(isEmpty);
    sessionTree_->setVisible(!isEmpty);
}

}  // namespace lycode::ui
