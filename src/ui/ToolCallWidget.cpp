#include "ui/ToolCallWidget.h"

#include "core/Json.h"
#include "ui/DiffView.h"
#include "ui/FileViewerDialog.h"
#include "ui/Theme.h"

#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace zcode::ui {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.ui.toolcall")

/// 卡片内文本域的展示上限。超出部分靠滚动查看，不截断数据本身
/// （数据仍在 ToolPart 里，只是不一次性塞进控件，避免卡顿）。
constexpr int kInlineOutputLimit = 200 * 1024;
/// 代码/输出区最大高度，防止一个长输出把整个对话流挤没。
constexpr int kDiffMaxHeight = 260;
constexpr int kOutputMaxHeight = 260;

/// 把入参渲染成可读文本：有 command 就优先展示命令本身。
QString formatInput(const QJsonObject &input) {
    if (input.isEmpty()) {
        return {};
    }
    if (input.contains(QStringLiteral("command"))) {
        const QString command = input.value(QStringLiteral("command")).toString();
        if (!command.isEmpty()) {
            // 其余参数附在命令下方，避免用户以为只执行了 command。
            QJsonObject rest = input;
            rest.remove(QStringLiteral("command"));
            if (rest.isEmpty()) {
                return command;
            }
            return command + QStringLiteral("\n\n") +
                   QString::fromUtf8(QJsonDocument(rest).toJson(QJsonDocument::Indented));
        }
    }
    return QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented));
}

/// 生成一个只读的等宽文本域。
QPlainTextEdit *makeMonoView() {
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setFrameShape(QFrame::NoFrame);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    // 只读文本域默认仍会显示文本光标，视觉上像可编辑，这里去掉。
    view->setCursorWidth(0);
    return view;
}

}  // namespace

ToolCallWidget::ToolCallWidget(const Part &part, QWidget *parent) : QFrame(parent), part_(part) {
    callId_ = part.kind == PartKind::Tool ? part.tool.callId : QString();

    setObjectName(QStringLiteral("toolCallCard"));
    // 通过属性而不是 objectName 让样式表可以按角色命中，避免每个实例一个选择器。
    setProperty("role", QStringLiteral("card"));
    setFrameShape(QFrame::NoFrame);

    buildUi();
    refreshHeader();
    refreshBodies();
}

ToolCallWidget::~ToolCallWidget() = default;

void ToolCallWidget::buildUi() {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 8);
    root->setSpacing(6);
    root->setSizeConstraint(QLayout::SetMinimumSize);

    // ── 头部 ────────────────────────────────────────────────────────────────
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    toggleButton_ = new QToolButton;
    toggleButton_->setArrowType(Qt::RightArrow);
    toggleButton_->setAutoRaise(true);
    toggleButton_->setCheckable(true);
    toggleButton_->setToolTip(QStringLiteral("展开或收起详情"));
    connect(toggleButton_, &QToolButton::toggled, this, [this](bool expanded) {
        toggleButton_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        if (body_ != nullptr) {
            body_->setVisible(expanded);
        }
    });
    header->addWidget(toggleButton_);

    // 状态灯：用一个圆点字符而不是自绘，避免引入额外绘制代码。
    stateDot_ = new QLabel(QStringLiteral("\u25CF"));
    stateDot_->setToolTip(QStringLiteral("工具状态"));
    header->addWidget(stateDot_);

    nameLabel_ = new QLabel;
    nameLabel_->setFont(Theme::instance().font(FontRole::Mono));
    header->addWidget(nameLabel_);

    titleLabel_ = new QLabel;
    titleLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(titleLabel_, 1);

    metaLabel_ = new QLabel;
    metaLabel_->setFont(Theme::instance().font(FontRole::UiXs));
    header->addWidget(metaLabel_);

    // 头部整行可点，扩大点击目标（箭头本身太小）。
    setCursor(Qt::PointingHandCursor);

    root->addLayout(header);

    // ── 主体 ────────────────────────────────────────────────────────────────
    body_ = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body_);
    bodyLayout->setContentsMargins(0, 4, 0, 0);
    bodyLayout->setSpacing(4);

    inputCaption_ = new QLabel(QStringLiteral("入参"));
    inputCaption_->setFont(Theme::instance().font(FontRole::UiXs));
    inputView_ = makeMonoView();
    inputView_->setMaximumHeight(140);
    bodyLayout->addWidget(inputCaption_);
    bodyLayout->addWidget(inputView_);

    outputCaption_ = new QLabel(QStringLiteral("输出"));
    outputCaption_->setFont(Theme::instance().font(FontRole::UiXs));
    outputView_ = makeMonoView();
    outputView_->setMaximumHeight(kOutputMaxHeight);
    bodyLayout->addWidget(outputCaption_);
    bodyLayout->addWidget(outputView_);

    // 改动补丁放在输出之前：对 Write/Edit 来说"改了什么"才是用户要看的，
    // 那句"Wrote 3 lines"没什么信息量。
    diffCaption_ = new QLabel(QStringLiteral("改动"));
    diffCaption_->setFont(Theme::instance().font(FontRole::UiXs));
    diffView_ = new DiffView;
    diffView_->setMaximumHeight(kDiffMaxHeight);
    diffCaption_->hide();
    diffView_->hide();
    bodyLayout->addWidget(diffCaption_);
    bodyLayout->addWidget(diffView_);

    // 文件入口：卡片里的输出是给模型看的（被预算截断、被限高），
    // 用户想核对完整内容需要一个出口。做成看起来像链接的按钮。
    fileCaption_ = new QPushButton;
    fileCaption_->setObjectName(QStringLiteral("toolFileLink"));
    fileCaption_->setFlat(true);
    fileCaption_->setCursor(Qt::PointingHandCursor);
    fileCaption_->setFont(Theme::instance().font(FontRole::UiXs));
    fileCaption_->hide();
    connect(fileCaption_, &QPushButton::clicked, this, [this]() {
        const QString path = json::str(part_.tool.metadata, QStringLiteral("filePath"));
        if (!path.isEmpty()) {
            FileViewerDialog::showContent(window(), path, {});
        }
    });
    bodyLayout->addWidget(fileCaption_, 0, Qt::AlignLeft);

    // 图片单独一块：读图时"看到的那张图"比一句话摘要有用得多，
    // 用户也需要能确认模型看的确实是这张。
    imageStrip_ = new QWidget;
    imageStrip_->setObjectName(QStringLiteral("toolImageStrip"));
    imageLayout_ = new QHBoxLayout(imageStrip_);
    imageLayout_->setContentsMargins(0, 0, 0, 0);
    imageLayout_->setSpacing(6);
    imageStrip_->hide();
    bodyLayout->addWidget(imageStrip_);

    emptyHint_ = new QLabel;
    emptyHint_->setFont(Theme::instance().font(FontRole::UiSm));
    emptyHint_->setWordWrap(true);
    bodyLayout->addWidget(emptyHint_);

    root->addWidget(body_);

    // 默认收起：对话流里工具卡片很多，全展开会淹没正文。
    body_->setVisible(false);

    // 主题变化时重新取色。控件自己订阅而不是等父级遍历重建，
    // 这样主题切换不会丢失用户的展开/收起选择。
    connect(&Theme::instance(), &Theme::changed, this, [this]() {
        refreshHeader();
        refreshBodies();
    });
}

bool ToolCallWidget::isExpanded() const {
    return toggleButton_ != nullptr && toggleButton_->isChecked();
}

void ToolCallWidget::setExpanded(bool expanded) {
    if (toggleButton_ != nullptr) {
        toggleButton_->setChecked(expanded);
    }
}

QColor ToolCallWidget::stateColor() const {
    const Palette &palette = Theme::instance().palette();
    switch (part_.tool.state) {
        case ToolState::InputStreaming:
        case ToolState::PendingApproval:
        case ToolState::Running:
            return palette.warning;
        case ToolState::Success:
            return palette.success;
        case ToolState::Error:
            return palette.destructive;
        case ToolState::Cancelled:
            return palette.foregroundSubtlest;
    }
    return palette.foregroundSubtle;
}

QString ToolCallWidget::stateText() const {
    switch (part_.tool.state) {
        case ToolState::InputStreaming:
            return QStringLiteral("接收参数中");
        case ToolState::PendingApproval:
            return QStringLiteral("等待确认");
        case ToolState::Running:
            return QStringLiteral("执行中");
        case ToolState::Success:
            return QStringLiteral("成功");
        case ToolState::Error:
            return QStringLiteral("失败");
        case ToolState::Cancelled:
            return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

void ToolCallWidget::refreshHeader() {
    const Palette &palette = Theme::instance().palette();

    stateDot_->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::css(stateColor())));
    stateDot_->setToolTip(stateText());

    nameLabel_->setText(part_.tool.name.isEmpty() ? QStringLiteral("(未命名工具)")
                                                 : part_.tool.name);
    nameLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::css(palette.foreground)));

    if (!part_.tool.title.isEmpty() && part_.tool.title != part_.tool.name) {
        titleLabel_->setText(part_.tool.title);
        titleLabel_->setStyleSheet(
            QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));
    } else {
        titleLabel_->clear();
    }

    // 元信息：耗时 + 状态文案。等待中的卡片不显示 0ms，那没有信息量。
    QStringList meta;
    const qint64 duration = part_.tool.durationMs();
    if (duration > 0) {
        meta << (duration < 1000 ? QStringLiteral("%1 ms").arg(duration)
                                 : QStringLiteral("%1 s").arg(duration / 1000.0, 0, 'f', 1));
    }
    meta << stateText();
    metaLabel_->setText(meta.join(QStringLiteral(" · ")));
    metaLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));

    // 标题栏的提示同时给出摘要，让收起的卡片也能说明自己做了什么。
    setToolTip(part_.tool.error.isEmpty() ? part_.tool.title : part_.tool.error);
}

void ToolCallWidget::refreshBodies() {
    const Palette &palette = Theme::instance().palette();

    const QString inputText = formatInput(part_.tool.input);
    // 入参流式接收中时，input 还没解析出来，退而展示原始片段。
    const QString effectiveInput =
        inputText.isEmpty() ? part_.tool.inputText : inputText;

    if (effectiveInput.isEmpty()) {
        inputCaption_->setVisible(false);
        inputView_->setVisible(false);
    } else {
        inputCaption_->setVisible(true);
        inputView_->setVisible(true);
        inputCaption_->setStyleSheet(
            QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));
        inputView_->setPlainText(json::truncate(effectiveInput, kInlineOutputLimit));
        inputView_->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                      .arg(Theme::css(palette.surface), Theme::css(palette.foreground)));
    }

    // 输出优先给最终结果；执行中则给进度预览。
    QString outputText = part_.tool.output;
    if (outputText.isEmpty() && !part_.tool.progress.isEmpty()) {
        outputText = part_.tool.progress.value(QStringLiteral("preview")).toString();
    }
    if (!part_.tool.error.isEmpty()) {
        outputText = part_.tool.error;
    }

    if (outputText.isEmpty()) {
        outputCaption_->setVisible(false);
        outputView_->setVisible(false);
    } else {
        outputCaption_->setVisible(true);
        outputView_->setVisible(true);
        outputCaption_->setText(part_.tool.state == ToolState::Error ? QStringLiteral("错误")
                                                                    : QStringLiteral("输出"));
        outputView_->setPlainText(json::truncate(outputText, kInlineOutputLimit));
        const QColor textColor = part_.tool.state == ToolState::Error ? palette.destructive
                                                                     : palette.foreground;
        outputView_->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                       .arg(Theme::css(palette.surface), Theme::css(textColor)));
    }

    // 运行中但还没有任何输出时，给一句解释，避免卡片看起来是空的。
    const bool hasAnyContent = !effectiveInput.isEmpty() || !outputText.isEmpty();
    if (!hasAnyContent) {
        emptyHint_->setVisible(true);
        emptyHint_->setStyleSheet(
            QStringLiteral("color: %1;").arg(Theme::css(palette.foregroundSubtle)));
        switch (part_.tool.state) {
            case ToolState::InputStreaming:
                emptyHint_->setText(QStringLiteral("正在接收调用参数…"));
                break;
            case ToolState::PendingApproval:
                emptyHint_->setText(QStringLiteral("等待你确认后才会执行。"));
                break;
            case ToolState::Running:
                emptyHint_->setText(QStringLiteral("正在执行…"));
                break;
            case ToolState::Cancelled:
                emptyHint_->setText(QStringLiteral("该调用已取消，未产生输出。"));
                break;
            case ToolState::Success:
                emptyHint_->setText(QStringLiteral("执行成功，无输出。"));
                break;
            case ToolState::Error:
                emptyHint_->setText(QStringLiteral("执行失败，无错误详情。"));
                break;
        }
    } else {
        emptyHint_->setVisible(false);
    }

    syncDiff();
    syncImages();
    syncFileLink();
}

bool ToolCallWidget::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *label = qobject_cast<QLabel *>(watched);
        if (label != nullptr && label->objectName() == QLatin1String("toolImage")) {
            openImageViewer(label);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ToolCallWidget::openImageViewer(QWidget *source) {
    // 从缩略图反查它是第几张图：标签顺序与 part_.tool.images 一一对应。
    const QList<QLabel *> labels =
        imageStrip_->findChildren<QLabel *>(QStringLiteral("toolImage"));
    const int index = labels.indexOf(qobject_cast<QLabel *>(source));
    if (index < 0 || index >= part_.tool.images.size()) {
        return;
    }
    const FilePart &image = part_.tool.images.at(index);
    // 直接传 base64 解码后的字节：粘贴来的图片没有磁盘路径，
    // 只有 bytes 才能保证查看器一定能显示。
    FileViewerDialog::showContent(window(), image.path,
                                  QByteArray::fromBase64(image.base64.toLatin1()));
}

void ToolCallWidget::syncDiff() {
    if (diffView_ == nullptr || diffCaption_ == nullptr) {
        return;
    }
    const QJsonArray hunks = json::array(part_.tool.metadata, QStringLiteral("structuredPatch"));
    if (hunks.isEmpty()) {
        diffView_->hide();
        diffCaption_->hide();
        return;
    }

    diffView_->setHunks(hunks);
    // 头部直接给出增删行数：不展开卡片也能看出改动规模。
    diffCaption_->setText(QStringLiteral("改动  +%1  −%2")
                              .arg(diffView_->additions())
                              .arg(diffView_->deletions()));
    diffCaption_->show();
    diffView_->show();
}

void ToolCallWidget::syncFileLink() {
    if (fileCaption_ == nullptr) {
        return;
    }
    // 只在工具确实读/写了某个文件时给入口。有图片时不重复给：
    // 图片本身已经可点，多一个按钮只是噪音。
    const QString path = json::str(part_.tool.metadata, QStringLiteral("filePath"));
    const bool show = !path.isEmpty() && part_.tool.images.isEmpty();
    if (!show) {
        fileCaption_->hide();
        return;
    }
    fileCaption_->setText(QStringLiteral("查看文件  %1  ▸").arg(QFileInfo(path).fileName()));
    fileCaption_->setToolTip(QStringLiteral("点击查看完整内容：%1").arg(path));
    fileCaption_->show();
}

void ToolCallWidget::syncImages() {
    if (imageStrip_ == nullptr || imageLayout_ == nullptr) {
        return;
    }
    while (QLayoutItem *item = imageLayout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    if (part_.tool.images.isEmpty()) {
        imageStrip_->hide();
        return;
    }

    for (const FilePart &image : part_.tool.images) {
        QPixmap pixmap;
        if (!image.base64.isEmpty() &&
            pixmap.loadFromData(QByteArray::fromBase64(image.base64.toLatin1()))) {
            auto *label = new QLabel;
            label->setObjectName(QStringLiteral("toolImage"));
            // 缩略图只有 240px，看不清细节。整块可点，点开看原尺寸。
            label->setCursor(Qt::PointingHandCursor);
            label->setToolTip(QStringLiteral("点击查看原图（%1）")
                                  .arg(image.fileName.isEmpty() ? QStringLiteral("图片")
                                                                : image.fileName));
            label->installEventFilter(const_cast<ToolCallWidget *>(this));
            constexpr int kMaxEdge = 240;
            label->setPixmap(pixmap.width() > kMaxEdge || pixmap.height() > kMaxEdge
                                 ? pixmap.scaled(kMaxEdge, kMaxEdge, Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation)
                                 : pixmap);
            label->setToolTip(QStringLiteral("%1（%2 · %3×%4）")
                                  .arg(image.fileName.isEmpty() ? QStringLiteral("图片")
                                                                : image.fileName,
                                       image.mimeType)
                                  .arg(pixmap.width())
                                  .arg(pixmap.height()));
            imageLayout_->addWidget(label);
            continue;
        }
        // 解不出图时不要静默：至少说明"这里有张图但没能解码"。
        auto *label = new QLabel(QStringLiteral("图片无法解码：%1").arg(image.fileName));
        label->setFont(Theme::instance().font(FontRole::UiXs));
        imageLayout_->addWidget(label);
    }
    imageLayout_->addStretch(1);
    imageStrip_->show();
}

void ToolCallWidget::applyPart(const Part &part) {
    if (part.kind != PartKind::Tool) {
        return;
    }
    if (!callId_.isEmpty() && part.tool.callId != callId_) {
        qCWarning(log) << "拒绝用不同 callId 的 part 刷新卡片; expected=" << callId_
                       << "got=" << part.tool.callId;
        return;
    }

    const ToolState previousState = part_.tool.state;
    part_ = part;
    if (callId_.isEmpty()) {
        callId_ = part.tool.callId;
    }

    refreshHeader();
    refreshBodies();

    // 状态从"等待确认"进入"执行中"时自动展开一次：
    // 这是用户最需要看到细节的时刻。之后不再自动改变用户的展开选择。
    if (previousState == ToolState::PendingApproval && part_.tool.state == ToolState::Running &&
        !isExpanded()) {
        setExpanded(true);
    }
}

}  // namespace zcode::ui
