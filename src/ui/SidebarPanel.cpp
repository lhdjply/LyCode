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
#include <QLoggingCategory>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace zcode::ui {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.ui.sidebar")

/// 会话列表摘要的最大字符数。列表项只有一行摘要，太长会挤掉标题的视觉权重。
constexpr int kPreviewChars = 60;

/// 会话列表项上挂的自定义数据角色。
constexpr int kSessionIdRole = Qt::UserRole + 1;
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

    // ── 工作区切换行 ────────────────────────────────────────────────────────
    // 结构性注意：**不要**把布局塞进 QPushButton。QPushButton 的 sizeHint 只按
    // 自身文本计算，塞进去的子控件会被裁掉——这正是"新建会话上方显示不全"的成因。
    // 正确做法：按钮只承担名称与键盘可达性，路径另用一个标签承载。
    auto *workspaceRow = new QWidget;
    auto *workspaceLayout = new QVBoxLayout(workspaceRow);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(2);

    workspaceButton_ = new QPushButton;
    workspaceButton_->setFlat(true);
    workspaceButton_->setCursor(Qt::PointingHandCursor);
    workspaceButton_->setToolTip(QStringLiteral("切换工作区目录"));
    // 工作区行是"扁平切换器"：不要全局 QSS 的按钮底色，只在 hover 时给弱填充。
    // 注意要显式写 background/border，否则会继承全局 QPushButton 的卡片样式。
    workspaceButton_->setFlat(true);
    workspaceLayout->addWidget(workspaceButton_);

    workspacePathLabel_ = new QLabel;
    workspacePathLabel_->setFont(Theme::instance().font(FontRole::UiXs));
    workspacePathLabel_->setWordWrap(false);
    workspacePathLabel_->setContentsMargins(8, 0, 8, 0);
    workspacePathLabel_->setCursor(Qt::PointingHandCursor);
    workspacePathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 水平 Ignored：长路径不允许把侧边栏撑宽，而是靠省略号收敛。
    workspacePathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    workspacePathLabel_->installEventFilter(this);
    workspaceLayout->addWidget(workspacePathLabel_);

    connect(workspaceButton_, &QPushButton::clicked, this, &SidebarPanel::showWorkspaceMenu);
    root->addWidget(workspaceRow);

    // ── 新建会话 ────────────────────────────────────────────────────────────
    newSessionButton_ = new QPushButton(QStringLiteral("新建会话"));
    newSessionButton_->setProperty("accent", true);
    newSessionButton_->setCursor(Qt::PointingHandCursor);
    connect(newSessionButton_, &QPushButton::clicked, this, &SidebarPanel::newSessionRequested);
    root->addWidget(newSessionButton_);

    // ── 会话列表 ────────────────────────────────────────────────────────────
    sessionList_ = new QListWidget;
    sessionList_->setFrameShape(QFrame::NoFrame);
    sessionList_->setSelectionMode(QAbstractItemView::SingleSelection);
    sessionList_->setContextMenuPolicy(Qt::CustomContextMenu);
    sessionList_->setWordWrap(false);
    connect(sessionList_, &QListWidget::itemSelectionChanged, this, [this]() {
        if (updatingSelection_) {
            return;
        }
        const Id id = activeSessionId();
        if (!id.isEmpty()) {
            emit sessionSelected(id);
        }
    });
    connect(sessionList_, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                QListWidgetItem *item = sessionList_->itemAt(position);
                if (item == nullptr) {
                    return;
                }
                const Id id = itemSessionId(item);
                QMenu menu(this);
                QAction *remove = menu.addAction(QStringLiteral("删除会话"));
                if (menu.exec(sessionList_->mapToGlobal(position)) == remove) {
                    emit sessionDeleteRequested(id);
                }
            });
    root->addWidget(sessionList_, 1);

    // 空状态提示与列表互斥显示。
    emptyHint_ = new QLabel(QStringLiteral("还没有会话。"));
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setFont(Theme::instance().font(FontRole::UiSm));
    emptyHint_->setVisible(false);
    root->addWidget(emptyHint_);

    // ── 设置入口 ────────────────────────────────────────────────────────────
    settingsButton_ = new QPushButton(QStringLiteral("设置"));
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

    // 左内边距固定为 8px，路径标签也用同样的 8px，两者文字才能对齐。
    workspaceButton_->setStyleSheet(
        QStringLiteral("QPushButton { text-align: left; padding: 5px 8px; border: none; "
                       "background: transparent; color: %1; font-weight: 500; "
                       "border-radius: 6px; }"
                       "QPushButton:hover { background-color: %2; }"
                       "QPushButton:focus { background-color: %2; }")
            .arg(Theme::css(palette.foreground), Theme::css(palette.surfaceHover)));
    workspacePathLabel_->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;")
            .arg(Theme::css(palette.foregroundSubtlest)));
    emptyHint_->setStyleSheet(
        QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));

    sessionList_->setStyleSheet(
        QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"
                       "QListWidget::item { padding: 6px 8px; border-radius: 6px; }"
                       "QListWidget::item:selected { background-color: %1; }")
            .arg(Theme::css(palette.selected)));
}

QListWidgetItem *SidebarPanel::makeItem(const SessionSummary &summary) const {
    const Session &session = summary.session;

    QString title = session.title.trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("未命名会话");
    }

    // 第一行标题，第二行摘要/元信息。用换行把两行压在一个 item 里，
    // 避免为每个会话建自定义控件（列表会有几百项）。
    QStringList lines;
    lines << title;

    // 摘要必须是**单行纯文本**：消息正文本身是 Markdown 源（含 `###`、列表符号、
    // 换行），直接塞进列表项会把一项撑成十几行高。统一走 toPlainPreview 去掉标记
    // 并折叠空白，再按长度截断。
    QString preview = summary.lastMessagePreview;
    if (preview.isEmpty()) {
        preview = session.title;
    }
    preview = Markdown::toPlainPreview(preview, kPreviewChars);

    const QString badge = statusBadge(session);
    if (!badge.isEmpty()) {
        lines << (preview.isEmpty() ? badge : badge + QStringLiteral(" · ") + preview);
    } else if (!preview.isEmpty()) {
        lines << preview;
    }

    auto *item = new QListWidgetItem(lines.join(QLatin1Char('\n')));
    item->setData(kSessionIdRole, session.id);
    item->setData(kNeedsAttentionRole, session.isWaitingOnUser());
    item->setToolTip(session.workspace.path);
    return item;
}

Id SidebarPanel::itemSessionId(const QListWidgetItem *item) {
    if (item == nullptr) {
        return {};
    }
    return item->data(kSessionIdRole).toString();
}

void SidebarPanel::setWorkspace(const Workspace &workspace) {
    workspace_ = workspace;
    const QString name = workspace.displayName();
    workspaceButton_->setText(name.isEmpty() ? QStringLiteral("未选择工作区") : name);
    workspaceButton_->setToolTip(workspace.path.isEmpty() ? QStringLiteral("切换工作区目录")
                                                          : workspace.path);
    updateWorkspacePathLabel();
    newSessionButton_->setEnabled(workspace.isValid());
}

void SidebarPanel::showWorkspaceMenu() {
    // 最近工作区菜单 + "选择其它目录"，避免每次都要走文件对话框。
    QMenu menu(this);

    if (!workspace_.path.isEmpty()) {
        QAction *current = menu.addAction(QStringLiteral("当前：") + workspace_.path);
        current->setEnabled(false);
        menu.addSeparator();
    }

    for (const QString &recent : recentWorkspaces_) {
        if (recent == workspace_.path) {
            continue;
        }
        QAction *entry = menu.addAction(QDir(recent).dirName().isEmpty()
                                            ? recent
                                            : QDir(recent).dirName() + QStringLiteral("  —  ") +
                                                  recent);
        connect(entry, &QAction::triggered, this,
                [this, recent]() { emit workspaceRecentRequested(recent); });
    }
    if (!recentWorkspaces_.isEmpty()) {
        menu.addSeparator();
    }

    QAction *browse = menu.addAction(QStringLiteral("选择其它目录…"));
    connect(browse, &QAction::triggered, this, &SidebarPanel::workspaceChangeRequested);
    menu.exec(workspaceButton_->mapToGlobal(workspaceButton_->rect().bottomLeft()));
}

void SidebarPanel::updateWorkspacePathLabel() {
    const QString path = workspace_.path;
    workspacePathLabel_->setToolTip(path);

    if (path.isEmpty()) {
        workspacePathLabel_->setText(QStringLiteral("未选择目录"));
        return;
    }

    // 按标签实际可用宽度做中间省略，而不是按固定字符数：
    // 界面字号变化与侧边栏拖宽都能正确适配。
    const QFontMetrics metrics(workspacePathLabel_->font());
    const int available = qMax(24, workspacePathLabel_->width() -
                                       workspacePathLabel_->contentsMargins().left() -
                                       workspacePathLabel_->contentsMargins().right());
    workspacePathLabel_->setText(metrics.elidedText(path, Qt::ElideMiddle, available));
}

void SidebarPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateWorkspacePathLabel();
}

bool SidebarPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == workspacePathLabel_ && event->type() == QEvent::MouseButtonRelease) {
        showWorkspaceMenu();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void SidebarPanel::setRecentWorkspaces(const QStringList &paths) {
    recentWorkspaces_ = paths;
}

void SidebarPanel::setSessions(const QList<SessionSummary> &sessions) {
    sessions_ = sessions;

    const Id previouslyActive = activeSessionId();
    updatingSelection_ = true;
    sessionList_->clear();
    itemsById_.clear();

    for (const SessionSummary &summary : sessions) {
        QListWidgetItem *item = makeItem(summary);
        sessionList_->addItem(item);
        itemsById_.insert(summary.session.id, item);
    }
    updatingSelection_ = false;

    refreshEmptyState();

    if (!previouslyActive.isEmpty() && itemsById_.contains(previouslyActive)) {
        setActiveSession(previouslyActive);
    }
    qCDebug(log) << "会话列表已刷新，共" << sessions.size() << "项";
}

void SidebarPanel::upsertSession(const SessionSummary &summary) {
    const Id id = summary.session.id;
    QListWidgetItem *existing = itemsById_.value(id, nullptr);

    if (existing == nullptr) {
        QListWidgetItem *item = makeItem(summary);
        sessionList_->insertItem(0, item);
        itemsById_.insert(id, item);
        sessions_.prepend(summary);
        refreshEmptyState();
        return;
    }

    // 就地更新文本，保留选中态与滚动位置。
    const QListWidgetItem *fresh = makeItem(summary);
    existing->setText(fresh->text());
    existing->setData(kNeedsAttentionRole, fresh->data(kNeedsAttentionRole));
    existing->setToolTip(fresh->toolTip());
    delete fresh;

    for (SessionSummary &candidate : sessions_) {
        if (candidate.session.id == id) {
            candidate = summary;
            break;
        }
    }
}

void SidebarPanel::setActiveSession(const Id &sessionId) {
    QListWidgetItem *item = itemsById_.value(sessionId, nullptr);
    if (item == nullptr) {
        updatingSelection_ = true;
        sessionList_->clearSelection();
        updatingSelection_ = false;
        return;
    }

    // 程序化选中不应反过来触发 sessionSelected，否则会形成回环。
    updatingSelection_ = true;
    sessionList_->setCurrentItem(item);
    sessionList_->scrollToItem(item, QAbstractItemView::EnsureVisible);
    updatingSelection_ = false;
}

Id SidebarPanel::activeSessionId() const {
    return itemSessionId(sessionList_->currentItem());
}

void SidebarPanel::refreshEmptyState() {
    const bool isEmpty = sessionList_->count() == 0;
    emptyHint_->setVisible(isEmpty);
    sessionList_->setVisible(!isEmpty);
}

}  // namespace zcode::ui
