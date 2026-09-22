#include "ui/PermissionDialog.h"

#include "ui/Theme.h"

#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace lycode::ui {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.ui.dialogs")

/// 入参展示预算。超长入参（MCP 工具可能塞进整篇文档）既拖慢布局又让用户抓不到重点，
/// 因此截断后明确告知，而不是静默丢弃。
constexpr int kMaxInputChars = 4000;
/// 命令块最多展示 4 行：再长交给纵向滚动，横向不换行以保持命令原貌。
constexpr int kCommandBlockMaxLines = 4;
/// JSON 预览最少 3 行、最多 10 行（≈200px，UiBase=14 时行高约 19px），超出滚动。
constexpr int kJsonBlockMinLines = 3;
constexpr int kJsonBlockMaxLines = 10;

// ── 令牌化排版 ──────────────────────────────────────────────────────────────
// 全局样式表里有 `QWidget { font-family / font-size }` 通配规则，会盖掉 setFont()。
// 因此需要精确令牌排版的地方，都在控件自身写一条 ID 选择器的本地规则（优先级更高），
// 并把"用了哪个令牌"记进动态属性；Theme::changed 时按属性整棵树重刷，
// 这样实时切换主题/字号不会残留旧令牌的颜色与字号。

/// 标签取色令牌。存"令牌名"而不是颜色值，重刷时才能解析到新主题。
enum class ColorToken {
    Foreground,
    ForegroundSubtle,
    ForegroundSubtlest,
    Destructive,
};

QColor resolveColor(ColorToken token) {
    const Palette &palette = Theme::instance().palette();
    switch (token) {
        case ColorToken::Foreground:
            return palette.foreground;
        case ColorToken::ForegroundSubtle:
            return palette.foregroundSubtle;
        case ColorToken::ForegroundSubtlest:
            return palette.foregroundSubtlest;
        case ColorToken::Destructive:
            return palette.destructive;
    }
    return palette.foreground;
}

bool isMonoRole(FontRole role) {
    return role == FontRole::Mono || role == FontRole::MonoSm;
}

/// 风险档位文案。四档词表固定，不允许 UI 自行改写。
QString riskText(RiskLevel level) {
    switch (level) {
        case RiskLevel::Low:
            return QStringLiteral("低");
        case RiskLevel::Medium:
            return QStringLiteral("中");
        case RiskLevel::High:
            return QStringLiteral("高");
        case RiskLevel::Critical:
            return QStringLiteral("严重");
    }
    return QStringLiteral("中");
}

/// 风险 → 语义令牌色。Low/Medium/High 依次 success/warning/destructive；
/// Critical 与 High 同色（都是破坏性），靠字重再加一档层级。
QColor riskColor(RiskLevel level) {
    const Palette &palette = Theme::instance().palette();
    switch (level) {
        case RiskLevel::Low:
            return palette.success;
        case RiskLevel::Medium:
            return palette.warning;
        case RiskLevel::High:
        case RiskLevel::Critical:
            return palette.destructive;
    }
    return palette.warning;
}

QString kindText(PermissionKind kind) {
    switch (kind) {
        case PermissionKind::Read:
            return QStringLiteral("读取");
        case PermissionKind::Write:
            return QStringLiteral("写入");
        case PermissionKind::Execute:
            return QStringLiteral("执行");
        case PermissionKind::Network:
            return QStringLiteral("网络");
        case PermissionKind::Other:
            return QStringLiteral("其它");
    }
    return QStringLiteral("其它");
}

/// 选项文案兜底：正常走工具给出的 option.label；为空时用词表文案，
/// 否则会渲染出一个没有文字的按钮。注意这是**文案**兜底，不是新增选项。
QString optionFallbackLabel(PermissionOptionKind kind) {
    switch (kind) {
        case PermissionOptionKind::AllowOnce:
            return QStringLiteral("允许一次");
        case PermissionOptionKind::AllowAlways:
            return QStringLiteral("始终允许");
        case PermissionOptionKind::Deny:
            return QStringLiteral("拒绝");
        case PermissionOptionKind::Custom:
            return QStringLiteral("自定义");
    }
    return QStringLiteral("继续");
}

/// 构造一个拒绝裁决。逐字段赋值：既可读，也避开聚合初始化漏字段的
/// -Wmissing-field-initializers（PermissionResponse 有 4 个字段）。
PermissionResponse denyResponse(const QString &reason) {
    PermissionResponse response;
    response.decision = PermissionDecision::Deny;
    response.reason = reason;
    return response;
}

void applyLabelStyle(QLabel *label);

/// 创建一个令牌化标签。objectName 必须唯一：本地规则用 ID 选择器。
QLabel *makeLabel(const QString &objectName, const QString &text, FontRole role, ColorToken color,
                  bool bold, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setProperty("tokenFontRole", static_cast<int>(role));
    label->setProperty("tokenColorToken", static_cast<int>(color));
    label->setProperty("tokenBold", bold);
    // 权限对话框的核心诉求是"看清要批准什么"，所以文字一律可选中复制。
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applyLabelStyle(label);
    return label;
}

void applyLabelStyle(QLabel *label) {
    if (!label->property("tokenFontRole").isValid()) {
        return;
    }
    Theme &theme = Theme::instance();
    const auto role = static_cast<FontRole>(label->property("tokenFontRole").toInt());
    const auto colorToken = static_cast<ColorToken>(label->property("tokenColorToken").toInt());
    const bool bold = label->property("tokenBold").toBool();

    QFont font = theme.font(role);
    font.setBold(bold);
    label->setFont(font);
    label->setStyleSheet(
        QStringLiteral("QLabel#%1 { font-family: \"%2\"; font-size: %3px; font-weight: %4;"
                       " color: %5; background: transparent; }")
            .arg(label->objectName(), isMonoRole(role) ? monospaceFamily() : sansFamily(),
                 QString::number(theme.fontPixelSize(role)),
                 bold ? QStringLiteral("600") : QStringLiteral("400"),
                 Theme::css(resolveColor(colorToken))));
}

void applyBadgeStyle(QLabel *badge);

/// 徽章：风险用语义色填充，类别用弱化描边。都是圆角小标签，不做整行填充。
QLabel *makeBadge(const QString &objectName, const QString &text, const QString &badgeKind,
                  RiskLevel risk, QWidget *parent) {
    auto *badge = new QLabel(text, parent);
    badge->setObjectName(objectName);
    badge->setProperty("tokenBadge", badgeKind);
    badge->setProperty("tokenRisk", static_cast<int>(risk));
    applyBadgeStyle(badge);
    return badge;
}

void applyBadgeStyle(QLabel *badge) {
    if (!badge->property("tokenBadge").isValid()) {
        return;
    }
    Theme &theme = Theme::instance();
    const Palette &palette = theme.palette();
    const QString badgeKind = badge->property("tokenBadge").toString();
    const auto risk = static_cast<RiskLevel>(badge->property("tokenRisk").toInt());
    const int pixelSize = theme.fontPixelSize(FontRole::UiXs);

    QFont font = theme.font(FontRole::UiXs);
    font.setBold(badgeKind == QStringLiteral("risk") && risk == RiskLevel::Critical);
    badge->setFont(font);

    if (badgeKind == QStringLiteral("outline")) {
        badge->setStyleSheet(
            QStringLiteral("QLabel#%1 { font-family: \"%2\"; font-size: %3px; font-weight: 400;"
                           " color: %4; background: transparent; border: 1px solid %5;"
                           " border-radius: 6px; padding: 0px 6px; }")
                .arg(badge->objectName(), sansFamily(), QString::number(pixelSize),
                     Theme::css(palette.foregroundSubtle), Theme::css(palette.border)));
        return;
    }

    const QColor color = riskColor(risk);
    badge->setStyleSheet(
        QStringLiteral("QLabel#%1 { font-family: \"%2\"; font-size: %3px; font-weight: %4;"
                       " color: %5; background: %6; border-radius: 6px; padding: 1px 6px; }")
            .arg(badge->objectName(), sansFamily(), QString::number(pixelSize),
                 font.bold() ? QStringLiteral("600") : QStringLiteral("400"), Theme::css(color),
                 Theme::css(color, 0.14)));
}

void applyCodeBlockStyle(QPlainTextEdit *block);

/// 只读代码块。属性里记录排版令牌，供主题变化后重刷。
QPlainTextEdit *makeCodeBlock(const QString &objectName, FontRole role, bool wrap, int minLines,
                              int maxLines, const QString &blockKind, QWidget *parent) {
    auto *block = new QPlainTextEdit(parent);
    block->setObjectName(objectName);
    block->setProperty("tokenFontRole", static_cast<int>(role));
    block->setProperty("tokenWrap", wrap);
    block->setProperty("tokenMinLines", minLines);
    block->setProperty("tokenMaxLines", maxLines);
    block->setProperty("tokenBlockKind", blockKind);
    block->setReadOnly(true);
    block->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    block->setFocusPolicy(Qt::ClickFocus);
    block->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    block->setFrameShape(QFrame::NoFrame);
    block->setTabChangesFocus(true);  // Tab 留在对话框内切换焦点，不插入制表符
    applyCodeBlockStyle(block);
    return block;
}

void applyCodeBlockStyle(QPlainTextEdit *block) {
    if (!block->property("tokenFontRole").isValid()) {
        return;
    }
    Theme &theme = Theme::instance();
    const Palette &palette = theme.palette();
    const auto role = static_cast<FontRole>(block->property("tokenFontRole").toInt());
    const bool wrap = block->property("tokenWrap").toBool();
    // 命令块需要"底 + 边框"把它和正文分开；纯 JSON 预览只需要 weak 底色。
    const bool bordered = block->property("tokenBlockKind").toString() == QStringLiteral("command");

    QFont font = theme.font(role);
    block->setFont(font);
    block->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    block->setHorizontalScrollBarPolicy(wrap ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    block->setStyleSheet(
        QStringLiteral("QPlainTextEdit#%1 { font-family: \"%2\"; font-size: %3px; color: %4;"
                       " background: %5; border: %6; border-radius: 8px; padding: 6px 8px;"
                       " selection-background-color: %7; selection-color: %4; }")
            .arg(block->objectName(), monospaceFamily(), QString::number(theme.fontPixelSize(role)),
                 Theme::css(palette.foreground), Theme::css(palette.surface),
                 bordered ? QStringLiteral("1px solid ") + Theme::css(palette.border)
                          : QStringLiteral("none"),
                 Theme::css(palette.selected)));

    // 行高随字号变，高度要重算：minLines 保底可视，maxLines 封顶后交给滚动条。
    const QFontMetrics metrics(font);
    const int lineHeight = std::max(metrics.lineSpacing(), 1);
    const int lines = std::clamp(block->document()->blockCount(),
                                 block->property("tokenMinLines").toInt(),
                                 block->property("tokenMaxLines").toInt());
    block->setFixedHeight(lines * lineHeight + 14);
}

/// 超长文本截断并显式告知，避免用户以为"就这么多"。
QString truncateForDisplay(const QString &text) {
    if (text.size() <= kMaxInputChars) {
        return text;
    }
    return text.left(kMaxInputChars) +
           QStringLiteral("\n… 内容过长，已截断（共 %1 字符）").arg(text.size());
}

/// JSON 入参块：只读、等宽、surface 底、无边框、限高可滚动。
QPlainTextEdit *makeJsonBlock(const QString &objectName, const QJsonObject &object, QWidget *parent) {
    auto *block = makeCodeBlock(objectName, FontRole::MonoSm, /*wrap=*/true, kJsonBlockMinLines,
                               kJsonBlockMaxLines, QStringLiteral("json"), parent);
    QString text = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
    while (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);  // toJson 自带尾换行，去掉以免多出一行空白
    }
    block->setPlainText(truncateForDisplay(text));
    applyCodeBlockStyle(block);  // 有文本后才能按行数定高
    return block;
}

}  // namespace

PermissionDialog::PermissionDialog(const PermissionRequest &request, QWidget *parent)
    : QDialog(parent), request_(request) {
    response_ = denyResponse(QStringLiteral("用户关闭了确认窗口"));

    setObjectName(QStringLiteral("PermissionDialog"));
    setWindowTitle(QStringLiteral("权限确认 — ") + resolvedTitle());
    // 长标题/长描述要能换行，但对话框本身不能被撑到屏幕外。
    setMinimumWidth(480);
    setMaximumWidth(720);
    setSizeGripEnabled(false);
    buildUi();
    refreshTokenStyles();

    // 主题（明暗或字号）变化后重刷本地令牌样式。this 作为上下文对象，析构即自动断开。
    connect(&Theme::instance(), &Theme::changed, this, [this]() { refreshTokenStyles(); });

    qCDebug(log) << "权限对话框已构建: tool=" << request_.toolName
                 << "kind=" << toToken(request_.kind) << "risk=" << toToken(request_.riskLevel)
                 << "options=" << request_.options.size();
    // 注意：不打印 input —— 命令/入参可能含凭据，日志只留元数据。
}

PermissionDialog::~PermissionDialog() = default;

void PermissionDialog::present(QWidget *parent, const PermissionRequest &request,
                               std::function<void(const PermissionResponse &)> onResolved) {
    if (request.options.isEmpty()) {
        // 没有可选项时绝不弹空壳对话框：UI 不得自造选项，唯一诚实且安全的收尾是按拒绝回调。
        qCWarning(log) << "权限请求没有可选项，按拒绝直接收尾:" << request.toolName;
        if (onResolved) {
            onResolved(denyResponse(QStringLiteral("权限请求没有可选项")));
        }
        return;
    }

    auto *dialog = new PermissionDialog(request, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (onResolved) {
        // finished 在关闭时同步发出（早于 deleteLater），此刻读 response() 是安全的。
        QObject::connect(dialog, &QDialog::finished, dialog,
                         [dialog, callback = std::move(onResolved)](int) {
                             callback(dialog->response());
                         });
    }
    qCDebug(log) << "权限对话框以非阻塞方式弹出: request=" << request.id;
    dialog->open();  // 非阻塞 + 窗口模态
}

void PermissionDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        if (const PermissionOption *deny = findOption(PermissionOptionKind::Deny)) {
            qCDebug(log) << "Esc 命中拒绝选项";
            chooseOption(*deny);
            event->accept();
            return;
        }
        // 没有 Deny 选项时交回基类走 reject()：response_ 保持"用户关闭了确认窗口"。
    }
    QDialog::keyPressEvent(event);
}

void PermissionDialog::buildUi() {
    auto *root = new QVBoxLayout(this);
    // 对话框内部留白 20px（对话框 20-24px），控件间距 12px = 列表项间距。
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    addHeader(root);
    addInputSection(root);
    // 弹性空间放在按钮行之前：窗口被拉高时按钮贴底，而不是飘在内容中间。
    root->addStretch(1);
    addOptionButtons(root);
}

void PermissionDialog::addHeader(QVBoxLayout *root) {
    auto *title = makeLabel(QStringLiteral("permissionTitle"), resolvedTitle(), FontRole::UiLg,
                            ColorToken::Foreground, /*bold=*/true, this);
    title->setWordWrap(true);
    root->addWidget(title);

    if (!request_.description.isEmpty()) {
        auto *description =
            makeLabel(QStringLiteral("permissionDescription"), request_.description,
                      FontRole::UiSm, ColorToken::ForegroundSubtle, /*bold=*/false, this);
        description->setWordWrap(true);
        root->addWidget(description);
    }

    // 徽章行：风险（语义色）+ 类别（弱化描边），工具名等宽弱化靠右。
    auto *badges = new QHBoxLayout;
    badges->setContentsMargins(0, 0, 0, 0);
    badges->setSpacing(6);  // 图标/文字间距 4-6px

    auto *riskBadge = makeBadge(QStringLiteral("riskBadge"), riskText(request_.riskLevel),
                                QStringLiteral("risk"), request_.riskLevel, this);
    riskBadge->setToolTip(QStringLiteral("风险等级：") + riskText(request_.riskLevel));
    badges->addWidget(riskBadge);

    auto *kindBadge = makeBadge(QStringLiteral("categoryBadge"), kindText(request_.kind),
                                QStringLiteral("outline"), request_.riskLevel, this);
    kindBadge->setToolTip(QStringLiteral("权限类别：") + kindText(request_.kind));
    badges->addWidget(kindBadge);

    badges->addStretch(1);

    if (!request_.toolName.isEmpty()) {
        auto *toolLabel = makeLabel(QStringLiteral("permissionToolName"), request_.toolName,
                                    FontRole::MonoSm, ColorToken::ForegroundSubtlest,
                                    /*bold=*/false, this);
        toolLabel->setToolTip(QStringLiteral("发起本次请求的工具"));
        badges->addWidget(toolLabel);
    }

    root->addLayout(badges);
}

void PermissionDialog::addInputSection(QVBoxLayout *root) {
    if (request_.input.isEmpty()) {
        root->addWidget(makeLabel(QStringLiteral("emptyInputHint"), QStringLiteral("（无入参）"),
                                  FontRole::UiSm, ColorToken::ForegroundSubtle, /*bold=*/false,
                                  this));
        return;
    }

    const QJsonValue command = request_.input.value(QStringLiteral("command"));
    if (command.isString() && !command.toString().isEmpty()) {
        // 命令块：QPlainTextEdit 而不是 QLabel —— 只有它能同时做到等宽、可选中、
        // 横向滚动且不自动换行（换行会歪曲命令原貌）。
        root->addWidget(makeLabel(QStringLiteral("commandCaption"), QStringLiteral("命令"),
                                  FontRole::UiSm, ColorToken::ForegroundSubtle, /*bold=*/false,
                                  this));

        auto *commandBlock = makeCodeBlock(QStringLiteral("commandBlock"), FontRole::Mono,
                                           /*wrap=*/false, /*minLines=*/1, kCommandBlockMaxLines,
                                           QStringLiteral("command"), this);
        commandBlock->setToolTip(QStringLiteral("即将执行的命令（可选中复制）"));
        commandBlock->setPlainText(truncateForDisplay(command.toString()));
        applyCodeBlockStyle(commandBlock);  // 有文本后才能按行数定高
        root->addWidget(commandBlock);

        // command 之外的字段（cwd、超时、参数…）同样影响"要不要批准"，因此不隐藏，
        // 只是排在命令之后并用小标题区分主次。
        QJsonObject rest = request_.input;
        rest.remove(QStringLiteral("command"));
        if (!rest.isEmpty()) {
            root->addWidget(makeLabel(QStringLiteral("extraCaption"), QStringLiteral("其它入参"),
                                      FontRole::UiSm, ColorToken::ForegroundSubtle,
                                      /*bold=*/false, this));
            root->addWidget(makeJsonBlock(QStringLiteral("extraJson"), rest, this));
        }
        return;
    }

    // 没有字符串 command（或它根本不是字符串）时退回完整 JSON 展示。
    root->addWidget(makeLabel(QStringLiteral("inputCaption"), QStringLiteral("入参"),
                              FontRole::UiSm, ColorToken::ForegroundSubtle, /*bold=*/false, this));
    root->addWidget(makeJsonBlock(QStringLiteral("inputJson"), request_.input, this));
}

void PermissionDialog::addOptionButtons(QVBoxLayout *root) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);  // 紧凑控件间距

    QList<QPushButton *> denyButtons;
    QList<QPushButton *> otherButtons;

    // defaultOption() 返回第一个允许项（没有则第一项）。这里用下标比对而不是保存指针：
    // range-for 会触发 QList 的隐式 detach，早先取到的指针可能失效。
    const PermissionOption *defaultOption = request_.defaultOption();
    int defaultIndex = -1;
    for (int i = 0; i < request_.options.size(); ++i) {
        if (&request_.options.at(i) == defaultOption) {
            defaultIndex = i;
            break;
        }
    }

    for (int i = 0; i < request_.options.size(); ++i) {
        const PermissionOption &option = request_.options.at(i);
        if (option.label.isEmpty()) {
            qCWarning(log) << "权限选项缺少文案，使用词表兜底:" << toToken(option.kind);
        }
        auto *button =
            new QPushButton(option.label.isEmpty() ? optionFallbackLabel(option.kind) : option.label,
                            this);
        button->setObjectName(QStringLiteral("permissionOptionButton"));
        button->setProperty("optionKind", toToken(option.kind));
        button->setCursor(Qt::PointingHandCursor);
        // 只有默认项吃回车；其余按钮一律不能"顺手回车放行"。
        button->setAutoDefault(false);
        button->setDefault(false);

        // 样式分级：AllowOnce 主按钮 / AllowAlways 次按钮 / Deny 与 Custom 普通按钮。
        // 不给每个操作都上主按钮色，避免"哪个才是推荐动作"失焦。
        switch (option.kind) {
            case PermissionOptionKind::AllowOnce:
                button->setProperty("accent", true);
                break;
            case PermissionOptionKind::AllowAlways:
                button->setProperty("variant", "secondary");
                break;
            case PermissionOptionKind::Deny:
            case PermissionOptionKind::Custom:
                break;
        }

        connect(button, &QPushButton::clicked, this, [this, option]() { chooseOption(option); });

        if (option.kind == PermissionOptionKind::Deny) {
            // 拒绝类放最左：危险操作不落在右下角的回车默认位。
            denyButtons.append(button);
        } else {
            otherButtons.append(button);
        }

        if (i == defaultIndex) {
            button->setDefault(true);
            defaultButton_ = button;
        }
    }

    for (QPushButton *button : denyButtons) {
        row->addWidget(button);
    }
    row->addStretch(1);
    for (QPushButton *button : otherButtons) {
        row->addWidget(button);
    }
    root->addLayout(row);

    if (defaultButton_ != nullptr) {
        defaultButton_->setFocus();
    }

    qCDebug(log) << "权限选项已渲染: 拒绝类=" << denyButtons.size()
                 << "放行类=" << otherButtons.size();
}

void PermissionDialog::applyShellStyle() {
    const Palette &palette = Theme::instance().palette();
    // 对话框外壳 16px 圆角（一级容器），嵌套层级由各自的 10/8/6 负责。
    setStyleSheet(
        QStringLiteral("QDialog#%1 { background: %2; border: 1px solid %3; border-radius: 16px; }")
            .arg(objectName(), Theme::css(palette.background), Theme::css(palette.popoverBorder)));
}

void PermissionDialog::refreshTokenStyles() {
    applyShellStyle();
    // 分类重刷：徽章有自己的属性，其余标签走通用令牌排版。
    const QList<QLabel *> labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (label->property("tokenBadge").isValid()) {
            applyBadgeStyle(label);
        } else {
            applyLabelStyle(label);
        }
    }
    const QList<QPlainTextEdit *> blocks = findChildren<QPlainTextEdit *>();
    for (QPlainTextEdit *block : blocks) {
        applyCodeBlockStyle(block);
    }
}

QString PermissionDialog::resolvedTitle() const {
    if (!request_.title.trimmed().isEmpty()) {
        return request_.title.trimmed();
    }
    if (!request_.toolName.trimmed().isEmpty()) {
        return request_.toolName.trimmed();
    }
    return QStringLiteral("需要确认的操作");
}

const PermissionOption *PermissionDialog::findOption(PermissionOptionKind kind) const {
    for (int i = 0; i < request_.options.size(); ++i) {
        if (request_.options.at(i).kind == kind) {
            return &request_.options.at(i);
        }
    }
    return nullptr;
}

void PermissionDialog::chooseOption(const PermissionOption &option) {
    // 原样回传工具给出的载荷（含 reason / modifiedInput / permissionUpdates）：
    // UI 只负责选择，不构造策略内容。
    response_ = option.response;
    qCInfo(log) << "权限裁决:" << toToken(option.kind) << "→" << toToken(response_.decision)
                << "tool=" << request_.toolName << "option=" << option.optionId;
    accept();
}

}  // namespace lycode::ui
