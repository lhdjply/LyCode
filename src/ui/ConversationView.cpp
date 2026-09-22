#include "ui/ConversationView.h"

#include "ui/Markdown.h"
#include "ui/Theme.h"
#include "ui/ToolCallWidget.h"

#include <QDateTime>
#include <QLabel>
#include <QLoggingCategory>
#include <QScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace zcode::ui {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.ui.conversation")

/// 触发"贴底跟随"的阈值（像素）。太大会导致用户刚往上滚一点就被拉回底部。
constexpr int kFollowBottomThreshold = 60;

QString roleDisplayName(MessageRole role) {
    switch (role) {
        case MessageRole::User:
            return QStringLiteral("你");
        case MessageRole::Assistant:
            return QStringLiteral("ZCode");
        case MessageRole::System:
            return QStringLiteral("系统");
    }
    return QStringLiteral("未知");
}

QString statusDisplayName(MessageStatus status) {
    switch (status) {
        case MessageStatus::Pending:
            return QStringLiteral("等待中");
        case MessageStatus::Streaming:
            return QStringLiteral("生成中");
        case MessageStatus::Complete:
            return {};
        case MessageStatus::Interrupted:
            return QStringLiteral("已中断");
        case MessageStatus::Failed:
            return QStringLiteral("失败");
    }
    return {};
}

/// 自适应高度的富文本视图。
///
/// 为什么必须专门做这件事：把 QTextBrowser 塞进 QScrollArea 时，内层不能再出现
/// 滚动条，所以控件高度必须等于文档高度。而"手动算一次高度再 setFixedHeight"
/// 是错的——构造期文档尚未按最终宽度排版，`document()->size()` 会返回 0，
/// 于是控件被永久锁成 ~4px，正文完全看不见（实测踩到）。
/// 正确做法是：让文档宽度随控件宽度变化，并在 documentSizeChanged 里同步高度。
class RichTextView : public QTextBrowser {
public:
    explicit RichTextView(QWidget *parent = nullptr) : QTextBrowser(parent) {
        setOpenExternalLinks(true);
        setFrameShape(QFrame::NoFrame);
        setReadOnly(true);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        document()->setDocumentMargin(0);

        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this](const QSizeF &size) {
                    const int target = static_cast<int>(size.height()) + 2;
                    // 只在真的变化时改：否则 setFixedHeight → resize → 重新排版
                    // → 再次发出信号，会形成回环。
                    if (target > 0 && height() != target) {
                        setFixedHeight(target);
                    }
                });
    }
};

QString formatTime(TimestampMs milliseconds) {
    if (milliseconds <= 0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("HH:mm"));
}

/// 从当前调色板构造 Markdown 渲染样式。集中在函数里，保证各处渲染一致。
MarkdownStyle markdownStyle() {
    const Theme &theme = Theme::instance();
    const Palette &palette = theme.palette();

    MarkdownStyle style;
    style.foreground = palette.foreground;
    style.foregroundSubtle = palette.foregroundSubtle;
    style.foregroundSubtlest = palette.foregroundSubtlest;
    style.surface = palette.surface;
    style.border = palette.border;
    style.codeBackground = palette.surface;
    style.link = palette.iconBlue;
    style.quoteBar = palette.border;
    style.tableHeader = palette.surface;
    style.baseFontPx = theme.fontPixelSize(FontRole::UiBase);
    style.codeFontPx = theme.fontPixelSize(FontRole::Mono);
    style.sansFamily = sansFamily();
    style.monoFamily = monospaceFamily();
    return style;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// MessageWidget
// ─────────────────────────────────────────────────────────────────────────────

MessageWidget::MessageWidget(const Message &message, QWidget *parent)
    : QWidget(parent), message_(message) {
    buildUi();
    refreshHeader();

    for (const Part &part : message_.parts) {
        ensurePartWidget(part);
    }

    connect(&Theme::instance(), &Theme::changed, this, &MessageWidget::refreshTheme);
}

MessageWidget::~MessageWidget() = default;

void MessageWidget::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    auto *headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);

    roleLabel_ = new QLabel;
    roleLabel_->setFont(Theme::instance().font(FontRole::UiBase));
    headerLayout->addWidget(roleLabel_);

    metaLabel_ = new QLabel;
    metaLabel_->setFont(Theme::instance().font(FontRole::UiXs));
    headerLayout->addWidget(metaLabel_);
    headerLayout->addStretch(1);

    root->addLayout(headerLayout);

    // 错误提示默认隐藏，只有真的失败时才占位。
    errorLabel_ = new QLabel;
    errorLabel_->setFont(Theme::instance().font(FontRole::UiSm));
    errorLabel_->setWordWrap(true);
    errorLabel_->setVisible(false);
    root->addWidget(errorLabel_);

    auto *content = new QWidget;
    contentLayout_ = new QVBoxLayout(content);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(6);
    root->addWidget(content);
}

QTextBrowser *MessageWidget::createRichTextView(bool reasoning) {
    // 高度自适应由 RichTextView 自己维护，这里只负责配色。
    auto *view = new RichTextView;

    if (reasoning) {
        const Palette &palette = Theme::instance().palette();
        view->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));
    }
    return view;
}

void MessageWidget::buildPartWidget(const Part &part) {
    switch (part.kind) {
        case PartKind::Text:
        case PartKind::Reasoning: {
            const bool reasoning = part.kind == PartKind::Reasoning;
            if (reasoning) {
                // 思考内容折在一条弱化的说明行下，避免抢占正文注意力。
                auto *caption = new QLabel(QStringLiteral("思考过程"));
                caption->setFont(Theme::instance().font(FontRole::UiXs));
                caption->setStyleSheet(QStringLiteral("color: %1;")
                                           .arg(Theme::css(Theme::instance().palette()
                                                               .foregroundSubtlest)));
                caption->setProperty("partCaptionFor", part.id);
                contentLayout_->addWidget(caption);
            }
            QTextBrowser *view = createRichTextView(reasoning);
            richViews_.insert(part.id, view);
            richSources_.insert(part.id, reasoning ? part.reasoning.text : part.text.text);
            contentLayout_->addWidget(view);
            renderRichText(part.id);
            break;
        }

        case PartKind::Tool: {
            auto *card = new ToolCallWidget(part, this);
            toolViews_.insert(part.tool.callId, card);
            contentLayout_->addWidget(card);
            break;
        }

        case PartKind::Timeline: {
            // 时间线标记（压缩、换模型、fork）渲染成分隔行，不做成卡片：
            // 它是流程注解，不是内容。
            auto *separator = new QLabel(part.timeline.summary.isEmpty()
                                             ? part.timeline.rawType
                                             : part.timeline.summary);
            separator->setFont(Theme::instance().font(FontRole::UiXs));
            separator->setAlignment(Qt::AlignCenter);
            separator->setWordWrap(true);
            separator->setProperty("role", QStringLiteral("timelineMarker"));
            separator->setStyleSheet(QStringLiteral("color: %1;")
                                         .arg(Theme::css(Theme::instance().palette()
                                                             .foregroundSubtlest)));
            contentLayout_->addWidget(separator);
            break;
        }

        case PartKind::File: {
            auto *label = new QLabel(part.file.fileName.isEmpty() ? part.file.path
                                                                 : part.file.fileName);
            label->setFont(Theme::instance().font(FontRole::UiSm));
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentLayout_->addWidget(label);
            break;
        }

        case PartKind::Artifact: {
            auto *label = new QLabel(QStringLiteral("产物：") + part.artifact.displayName);
            label->setFont(Theme::instance().font(FontRole::UiSm));
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentLayout_->addWidget(label);
            break;
        }

        case PartKind::Subagent: {
            auto *label = new QLabel(QStringLiteral("子代理：") + part.subagent.title);
            label->setFont(Theme::instance().font(FontRole::UiSm));
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentLayout_->addWidget(label);
            break;
        }

        case PartKind::Step: {
            // 步骤分隔暂不单独渲染：它与工具卡片的信息重复，单独显示会显得吵。
            return;
        }
    }

    renderedParts_.insert(part.id, true);
}

void MessageWidget::ensurePartWidget(const Part &part) {
    if (part.isEmpty() && part.kind != PartKind::Tool) {
        return;
    }
    if (renderedParts_.contains(part.id)) {
        return;
    }

    if (part.kind == PartKind::Tool) {
        // 工具卡片按 callId 索引，因为流式期间 part.id 与 callId 可能不同步出现。
        if (toolViews_.contains(part.tool.callId)) {
            renderedParts_.insert(part.id, true);
            return;
        }
    }
    buildPartWidget(part);
}

void MessageWidget::renderRichText(const Id &partId) {
    QTextBrowser *view = richViews_.value(partId, nullptr);
    if (view == nullptr) {
        return;
    }
    const QString source = richSources_.value(partId);

    const MarkdownStyle style = markdownStyle();
    // 先设样式表再设内容：顺序反了的话首次排版用的还是旧样式。
    // 高度由 RichTextView 在 documentSizeChanged 里同步，这里不要手工设置。
    view->document()->setDefaultStyleSheet(Markdown::styleSheet(style));
    view->setMarkdown(source);
}

void MessageWidget::appendDelta(const Id &partId, const QString &delta, bool reasoning) {
    Q_UNUSED(reasoning)

    if (!richSources_.contains(partId)) {
        // 增量先于控件到达（例如 partAppended 还没处理）：补建控件。
        Part placeholder;
        placeholder.id = partId;
        placeholder.kind = reasoning ? PartKind::Reasoning : PartKind::Text;
        buildPartWidget(placeholder);
    }

    richSources_[partId] += delta;

    QTextBrowser *view = richViews_.value(partId, nullptr);
    if (view == nullptr) {
        return;
    }

    // 流式阶段直接追加纯文本：避免每个 token 都重新解析整篇 Markdown。
    QTextCursor cursor = view->textCursor();
    cursor.movePosition(QTextCursor::End);
    view->setTextCursor(cursor);
    view->insertPlainText(delta);
}

void MessageWidget::applyPart(const Part &part) {
    if (part.kind == PartKind::Tool) {
        if (ToolCallWidget *card = toolViews_.value(part.tool.callId, nullptr)) {
            card->applyPart(part);
            renderedParts_.insert(part.id, true);
            return;
        }
        ensurePartWidget(part);
        if (ToolCallWidget *card = toolViews_.value(part.tool.callId, nullptr)) {
            card->applyPart(part);
        }
        return;
    }

    if (part.kind == PartKind::Text || part.kind == PartKind::Reasoning) {
        richSources_.insert(part.id,
                            part.kind == PartKind::Reasoning ? part.reasoning.text : part.text.text);
        renderRichText(part.id);
        return;
    }

    ensurePartWidget(part);
}

void MessageWidget::applyMessage(const Message &message) {
    if (message.id != message_.id) {
        qCWarning(log) << "拒绝用不同 id 的消息刷新视图; expected=" << message_.id
                       << "got=" << message.id;
        return;
    }
    message_ = message;

    for (const Part &part : message_.parts) {
        if ((part.kind == PartKind::Text || part.kind == PartKind::Reasoning) &&
            !richSources_.contains(part.id)) {
            ensurePartWidget(part);
        }
    }

    refreshHeader();

    // 终态：用累积的 Markdown 源重新排版一次，得到正确的标题/列表/代码块。
    for (auto it = richSources_.constBegin(); it != richSources_.constEnd(); ++it) {
        const Part *source = message_.findPart(it.key());
        if (source != nullptr) {
            richSources_[it.key()] =
                source->kind == PartKind::Reasoning ? source->reasoning.text : source->text.text;
        }
        renderRichText(it.key());
    }

    for (const Part &part : message_.parts) {
        if (part.kind == PartKind::Tool) {
            if (ToolCallWidget *card = toolViews_.value(part.tool.callId, nullptr)) {
                card->applyPart(part);
            } else {
                ensurePartWidget(part);
            }
        }
    }
}

void MessageWidget::refreshHeader() {
    const Palette &palette = Theme::instance().palette();

    roleLabel_->setText(roleDisplayName(message_.role));
    const bool isUser = message_.role == MessageRole::User;
    roleLabel_->setStyleSheet(QStringLiteral("color: %1; font-weight: %2;")
                                  .arg(Theme::css(isUser ? palette.foreground : palette.brand))
                                  .arg(isUser ? QStringLiteral("500") : QStringLiteral("600")));

    QStringList meta;
    const QString time = formatTime(message_.createdAtMs);
    if (!time.isEmpty()) {
        meta << time;
    }
    if (!message_.modelId.isEmpty() && !isUser) {
        meta << message_.modelId;
    }
    const QString status = statusDisplayName(message_.status);
    if (!status.isEmpty()) {
        meta << status;
    }
    if (message_.usage.effectiveTotal() > 0 && !isUser) {
        meta << QStringLiteral("%1 tokens").arg(message_.usage.effectiveTotal());
    }
    metaLabel_->setText(meta.join(QStringLiteral(" · ")));
    metaLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtlest)));

    if (message_.errorMessage.isEmpty()) {
        errorLabel_->setVisible(false);
    } else {
        errorLabel_->setVisible(true);
        errorLabel_->setText(message_.errorMessage);
        errorLabel_->setStyleSheet(
            QStringLiteral("color: %1;").arg(Theme::css(palette.destructive)));
    }
}

void MessageWidget::refreshTheme() {
    for (auto it = richSources_.constBegin(); it != richSources_.constEnd(); ++it) {
        renderRichText(it.key());
    }
    refreshHeader();
}

// ─────────────────────────────────────────────────────────────────────────────
// ConversationView
// ─────────────────────────────────────────────────────────────────────────────

ConversationView::ConversationView(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    container_ = new QWidget;
    containerLayout_ = new QVBoxLayout(container_);
    // 左右留出较宽的内边距：长文本的可读性强烈依赖行宽上限。
    containerLayout_->setContentsMargins(24, 16, 24, 16);
    containerLayout_->setSpacing(18);
    containerLayout_->addStretch(1);

    emptyState_ = new QLabel;
    emptyState_->setAlignment(Qt::AlignCenter);
    emptyState_->setWordWrap(true);
    emptyState_->setText(
        QStringLiteral("开始一个新会话\n\n在工作区里描述你想完成的任务，"
                       "ZCode 会读取代码、执行命令并给出改动。"));
    containerLayout_->insertWidget(0, emptyState_, 1);

    scroll_->setWidget(container_);
    root->addWidget(scroll_);

    connect(&Theme::instance(), &Theme::changed, this, &ConversationView::applyTheme);
    applyTheme();
}

ConversationView::~ConversationView() = default;

void ConversationView::applyTheme() {
    const Palette &palette = Theme::instance().palette();
    // 对话框/面板背景与滚动容器背景必须一致，否则滚动到底部会露出色差。
    scroll_->setStyleSheet(QStringLiteral("QScrollArea { background-color: %1; border: none; }")
                               .arg(Theme::css(palette.background)));
    container_->setStyleSheet(
        QStringLiteral("background-color: %1;").arg(Theme::css(palette.background)));
    emptyState_->setFont(Theme::instance().font(FontRole::UiBase));
    emptyState_->setStyleSheet(
        QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));
}

void ConversationView::clear() {
    for (MessageWidget *widget : std::as_const(messages_)) {
        containerLayout_->removeWidget(widget);
        widget->deleteLater();
    }
    messages_.clear();
    messageOrder_.clear();
    refreshEmptyState();
}

void ConversationView::addMessage(const Message &message) {
    // model-only 消息（合成的工具结果轮）是给模型看的上下文，不属于用户可见的
    // 对话记录。工具输出已经渲染在对应工具卡片里，重复展示会多出一个假的"你"轮次。
    if (message.modelOnly) {
        return;
    }
    if (messages_.contains(message.id)) {
        // 幂等：重复添加同一条消息只做刷新，不插入第二个控件。
        applyMessage(message);
        return;
    }

    const bool follow = shouldFollowBottom();

    auto *widget = new MessageWidget(message, container_);
    messages_.insert(message.id, widget);
    messageOrder_.append(message.id);
    // 插在末尾 stretch 之前，保证消息向上增长而不是被 stretch 推到底部。
    containerLayout_->insertWidget(containerLayout_->count() - 1, widget);

    refreshEmptyState();
    if (follow) {
        scrollToBottom();
    }
}

void ConversationView::applyMessage(const Message &message) {
    if (message.modelOnly) {
        return;
    }
    if (MessageWidget *widget = messages_.value(message.id, nullptr)) {
        widget->applyMessage(message);
    }
}

void ConversationView::appendDelta(const Id &messageId, const Id &partId, const QString &delta,
                                   bool reasoning) {
    MessageWidget *widget = messages_.value(messageId, nullptr);
    if (widget == nullptr) {
        return;
    }
    const bool follow = shouldFollowBottom();
    widget->appendDelta(partId, delta, reasoning);
    if (follow) {
        scrollToBottom();
    }
}

void ConversationView::applyPart(const Id &messageId, const Part &part) {
    MessageWidget *widget = messages_.value(messageId, nullptr);
    if (widget == nullptr) {
        return;
    }
    const bool follow = shouldFollowBottom();
    widget->applyPart(part);
    if (follow) {
        scrollToBottom();
    }
}

bool ConversationView::shouldFollowBottom() const {
    QScrollBar *bar = scroll_->verticalScrollBar();
    return bar->value() >= bar->maximum() - kFollowBottomThreshold;
}

void ConversationView::scrollToBottom() {
    // 延后一跳：此刻布局还没算完，直接设最大值会停在旧的底部。
    QTimer::singleShot(0, this, [this]() {
        QScrollBar *bar = scroll_->verticalScrollBar();
        bar->setValue(bar->maximum());
    });
}

void ConversationView::refreshEmptyState() {
    emptyState_->setVisible(messages_.isEmpty());
}

}  // namespace zcode::ui
