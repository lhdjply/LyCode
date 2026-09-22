#include "ui/SettingsDialog.h"

#include "core/Ids.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace zcode::ui {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.ui.dialogs")

/// Provider 列表固定宽度：右栏表单需要剩余全部横向空间，左栏只做选择。
constexpr int kProviderListWidth = 224;
/// 列表项文本省略预算（列表宽度 - 内边距 - 滚动条），避免长 URL 撑出横向滚动。
constexpr int kProviderItemTextWidth = 196;
/// 模型列表输入框高度（约 4-5 行）。
constexpr int kModelsEditHeight = 96;

// ── 令牌化排版 ──────────────────────────────────────────────────────────────
// 全局样式表里有 `QWidget { font-family / font-size }` 通配规则，会盖掉 setFont()。
// 需要精确令牌排版的地方就在控件自身写一条 ID 选择器本地规则（优先级更高），
// 并把"用了哪个令牌"记进动态属性；Theme::changed 时按属性整树重刷。
// 这样设置页自己实时预览主题/字号时，对话框不会残留旧令牌。

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

void applyLabelStyle(QLabel *label);

/// 创建令牌化标签。objectName 可以复用（同类文本共用同一套令牌）。
QLabel *makeLabel(const QString &objectName, const QString &text, FontRole role, ColorToken color,
                  QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setProperty("tokenFontRole", static_cast<int>(role));
    label->setProperty("tokenColorToken", static_cast<int>(color));
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
    QFont font = theme.font(role);
    label->setFont(font);
    label->setStyleSheet(
        QStringLiteral("QLabel#%1 { font-family: \"%2\"; font-size: %3px; font-weight: 400;"
                       " color: %4; background: transparent; }")
            .arg(label->objectName(), isMonoRole(role) ? monospaceFamily() : sansFamily(),
                 QString::number(theme.fontPixelSize(role)),
                 Theme::css(resolveColor(colorToken))));
}

/// 填充 Provider 列表项：主行 name，副行「协议 · baseUrl（· 已停用）」。
/// 按像素预算省略，长 URL 不会把固定宽度的左栏撑开。
void fillProviderItemWidget(QWidget *widget, const ProviderConfig &config);

QString providerKindDisplay(ProviderKind kind) {
    return kind == ProviderKind::Anthropic ? QStringLiteral("Anthropic Messages")
                                          : QStringLiteral("OpenAI 兼容");
}

QString availabilityDisplay(AccountAvailability availability) {
    switch (availability) {
        case AccountAvailability::Available:
            return QStringLiteral("可用");
        case AccountAvailability::Pending:
            return QStringLiteral("等待中");
        case AccountAvailability::Unavailable:
            return QStringLiteral("不可用");
        case AccountAvailability::Unknown:
            return QStringLiteral("未知");
    }
    return QStringLiteral("未知");
}

QString unavailableReasonDisplay(AccountUnavailableReason reason) {
    switch (reason) {
        case AccountUnavailableReason::None:
            return {};
        case AccountUnavailableReason::NotAuthenticated:
            return QStringLiteral("未登录");
        case AccountUnavailableReason::NotConnected:
            return QStringLiteral("未连接");
        case AccountUnavailableReason::CredentialFailed:
            return QStringLiteral("凭据失效");
        case AccountUnavailableReason::NotEntitled:
            return QStringLiteral("无权限");
    }
    return {};
}

/// 会话模式展示串。SessionMode::Auto 是运行时内部态，不在下拉框里暴露，
/// 因此这里只覆盖 UI 允许的四个值。
QString sessionModeDisplay(SessionMode mode) {
    switch (mode) {
        case SessionMode::Plan:
            return QStringLiteral("plan · 只读规划");
        case SessionMode::Build:
            return QStringLiteral("build · 默认");
        case SessionMode::Edit:
            return QStringLiteral("edit · 编辑");
        case SessionMode::Yolo:
            return QStringLiteral("yolo · 免确认");
        case SessionMode::Auto:
            return QStringLiteral("auto · 内部态");
    }
    return QStringLiteral("build · 默认");
}

/// 模型列表：每行一个 id。去重但保持顺序（重复配置没有意义）。
QStringList parseModelIds(const QString &text) {
    QStringList models;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !models.contains(trimmed)) {
            models.append(trimmed);
        }
    }
    return models;
}

/// 表单字段：字段名（弱化 UiSm）+ 控件 + 校验提示（destructive，默认隐藏）。
/// 返回错误提示标签，调用方保存指针以便实时更新文案。
QLabel *addFormField(QVBoxLayout *layout, const QString &title, QWidget *field, QWidget *parent) {
    layout->addWidget(makeLabel(QStringLiteral("formFieldCaption"), title, FontRole::UiSm,
                                ColorToken::ForegroundSubtle, parent));
    layout->addWidget(field);
    auto *error = makeLabel(QStringLiteral("formError"), QString(), FontRole::UiSm,
                            ColorToken::Destructive, parent);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    return error;
}

/// 只覆盖等宽字体族与字号的技术值输入框：底色/边框仍由全局令牌样式负责。
/// selector 写成 `QLineEdit#providerBaseUrl` 这种 ID 形式，优先级高于全局规则。
void enableMonoTypography(QWidget *widget, const QString &selector, FontRole role) {
    widget->setProperty("tokenMonoRole", static_cast<int>(role));
    widget->setProperty("tokenMonoSelector", selector);
    Theme &theme = Theme::instance();
    const auto storedRole = static_cast<FontRole>(widget->property("tokenMonoRole").toInt());
    widget->setFont(theme.font(storedRole));
    widget->setStyleSheet(QStringLiteral("%1 { font-family: \"%2\"; font-size: %3px; }")
                              .arg(selector, monospaceFamily(),
                                   QString::number(theme.fontPixelSize(storedRole))));
}

/// 列表项内的标签：鼠标事件必须穿透到 QListWidget 的 item 上，否则点不中。
QLabel *makeItemLabel(const QString &objectName, FontRole role, ColorToken color, QWidget *parent) {
    QLabel *label = makeLabel(objectName, QString(), role, color, parent);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

QWidget *makeProviderItemWidget(const ProviderConfig &config, QListWidget *parent) {
    auto *widget = new QWidget(parent);
    // 整棵子树对鼠标透明：点击要落到 QListWidget 的 item 上才能正常选中/换行。
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(1);
    layout->addWidget(makeItemLabel(QStringLiteral("providerItemName"), FontRole::UiBase,
                                    ColorToken::Foreground, widget));
    layout->addWidget(makeItemLabel(QStringLiteral("providerItemSubtitle"), FontRole::UiXs,
                                    ColorToken::ForegroundSubtlest, widget));
    fillProviderItemWidget(widget, config);
    return widget;
}

void fillProviderItemWidget(QWidget *widget, const ProviderConfig &config) {
    auto *name = widget->findChild<QLabel *>(QStringLiteral("providerItemName"));
    auto *subtitle = widget->findChild<QLabel *>(QStringLiteral("providerItemSubtitle"));
    if (name != nullptr) {
        const QString text = config.name.isEmpty() ? QStringLiteral("（未命名）") : config.name;
        name->setText(QFontMetrics(name->font()).elidedText(text, Qt::ElideRight,
                                                            kProviderItemTextWidth));
    }
    if (subtitle != nullptr) {
        QString text = providerKindDisplay(config.kind) + QStringLiteral(" · ") +
                       (config.baseUrl.isEmpty() ? QStringLiteral("未设置 Base URL") : config.baseUrl);
        if (!config.enabled) {
            text += QStringLiteral(" · 已停用");
        }
        subtitle->setText(
            QFontMetrics(subtitle->font()).elidedText(text, Qt::ElideMiddle, kProviderItemTextWidth));
    }
}

}  // namespace

SettingsDialog::SettingsDialog(const AppSettings &settings, QWidget *parent)
    : QDialog(parent), settings_(settings) {
    setObjectName(QStringLiteral("SettingsDialog"));
    setWindowTitle(QStringLiteral("设置"));
    resize(720, 560);
    setMinimumSize(640, 480);

    // 实时预览必须能回滚：记录"屏幕上当时的样子"，而不是 settings 里的值。
    const Theme &theme = Theme::instance();
    originalMode_ = theme.mode();
    originalUiFontSize_ = theme.uiFontSize();
    originalCodeFontSize_ = theme.codeFontSize();

    if (settings_.language.isEmpty()) {
        settings_.language = QStringLiteral("zh-CN");
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("settingsTabs"));
    tabs_->addTab(buildAppearancePage(), QStringLiteral("外观"));
    tabs_->addTab(buildProviderPage(), QStringLiteral("Provider"));
    tabs_->addTab(buildSessionPage(), QStringLiteral("会话"));
    root->addWidget(tabs_, 1);

    buildButtonBox(root);
    refreshTokenStyles();
    updateProviderState();

    // 设置页自己会实时预览主题，所以也必须跟着重刷本地令牌样式。
    connect(&Theme::instance(), &Theme::changed, this, [this]() { refreshTokenStyles(); });

    qCInfo(log) << "设置对话框已打开: providers=" << settings_.providers.size()
                << "themeMode=" << toToken(settings_.themeMode)
                << "uiFontSize=" << settings_.uiFontSize
                << "codeFontSize=" << settings_.codeFontSize
                << "defaultSessionMode=" << toToken(settings_.defaultSessionMode);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::accept() {
    if (!validateAllProviders()) {
        // 校验不通过：不关闭，并把用户带到出问题的那个 provider。
        tabs_->setCurrentIndex(1);
        return;
    }
    logAcceptedSettings();
    QDialog::accept();
}

void SettingsDialog::reject() {
    Theme &theme = Theme::instance();
    const bool previewed = theme.mode() != originalMode_ ||
                           theme.uiFontSize() != originalUiFontSize_ ||
                           theme.codeFontSize() != originalCodeFontSize_;
    theme.setMode(originalMode_);
    theme.setUiFontSize(originalUiFontSize_);
    theme.setCodeFontSize(originalCodeFontSize_);
    if (previewed) {
        // 回滚后把"恢复态"再广播一次，调用方才能把预览状态一起撤回。
        AppSettings restored = settings_;
        restored.themeMode = originalMode_;
        restored.uiFontSize = originalUiFontSize_;
        restored.codeFontSize = originalCodeFontSize_;
        qCDebug(log) << "取消设置：主题与字号已回滚";
        emit settingsPreviewChanged(restored);
    }
    QDialog::reject();
}

// ─────────────────────────────────────────────────────────────────────────────
// 外观
// ─────────────────────────────────────────────────────────────────────────────

QWidget *SettingsDialog::buildAppearancePage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    themeCombo_ = new QComboBox(page);
    themeCombo_->setObjectName(QStringLiteral("themeModeCombo"));
    themeCombo_->addItem(QStringLiteral("跟随系统"), static_cast<int>(ThemeMode::System));
    themeCombo_->addItem(QStringLiteral("浅色"), static_cast<int>(ThemeMode::Light));
    themeCombo_->addItem(QStringLiteral("深色"), static_cast<int>(ThemeMode::Dark));
    themeCombo_->setCurrentIndex(
        std::max(themeCombo_->findData(static_cast<int>(settings_.themeMode)), 0));

    uiFontSpin_ = new QSpinBox(page);
    uiFontSpin_->setObjectName(QStringLiteral("uiFontSizeSpin"));
    uiFontSpin_->setRange(10, 24);  // 与 Theme::setUiFontSize 的钳位范围一致
    uiFontSpin_->setSuffix(QStringLiteral(" px"));
    uiFontSpin_->setValue(std::clamp(settings_.uiFontSize, 10, 24));

    codeFontSpin_ = new QSpinBox(page);
    codeFontSpin_->setObjectName(QStringLiteral("codeFontSizeSpin"));
    codeFontSpin_->setRange(8, 32);  // 与 Theme::setCodeFontSize 的钳位范围一致
    codeFontSpin_->setSuffix(QStringLiteral(" px"));
    codeFontSpin_->setValue(std::clamp(settings_.codeFontSize, 8, 32));

    languageCombo_ = new QComboBox(page);
    languageCombo_->setObjectName(QStringLiteral("languageCombo"));
    languageCombo_->addItem(QStringLiteral("简体中文 (zh-CN)"), QStringLiteral("zh-CN"));
    languageCombo_->addItem(QStringLiteral("English (en-US)"), QStringLiteral("en-US"));
    const int languageIndex = languageCombo_->findData(settings_.language);
    languageCombo_->setCurrentIndex(std::max(languageIndex, 0));
    // 未知语言值在打开时就被归一化，避免"界面显示中文、保存里却留着 fr-FR"。
    settings_.language = languageCombo_->currentData().toString();

    addFormField(layout, QStringLiteral("主题模式"), themeCombo_, page);
    addFormField(layout, QStringLiteral("界面字号"), uiFontSpin_, page);
    addFormField(layout, QStringLiteral("代码字号"), codeFontSpin_, page);
    addFormField(layout, QStringLiteral("语言"), languageCombo_, page);
    layout->addWidget(makeLabel(QStringLiteral("formHint"), QStringLiteral("语言偏好只保存取值，界面翻译尚未接入。"),
                                FontRole::UiXs, ColorToken::ForegroundSubtlest, page));
    layout->addStretch(1);

    // 先赋值、后连接：构造期不会触发一次多余的主题预览。
    connect(themeCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { applyAppearancePreview(); });
    connect(uiFontSpin_, &QSpinBox::valueChanged, this, [this](int) { applyAppearancePreview(); });
    connect(codeFontSpin_, &QSpinBox::valueChanged, this, [this](int) { applyAppearancePreview(); });
    connect(languageCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        // 语言只保存值，不影响主题，因此不发预览信号。
        settings_.language = languageCombo_->currentData().toString();
        qCDebug(log) << "语言偏好改为:" << settings_.language;
    });
    return page;
}

void SettingsDialog::applyAppearancePreview() {
    Theme &theme = Theme::instance();
    settings_.themeMode = static_cast<ThemeMode>(themeCombo_->currentData().toInt());
    settings_.uiFontSize = uiFontSpin_->value();
    settings_.codeFontSize = codeFontSpin_->value();
    theme.setMode(settings_.themeMode);
    theme.setUiFontSize(settings_.uiFontSize);
    theme.setCodeFontSize(settings_.codeFontSize);
    qCDebug(log) << "外观实时预览: mode=" << toToken(settings_.themeMode)
                 << "uiFontSize=" << settings_.uiFontSize
                 << "codeFontSize=" << settings_.codeFontSize;
    emit settingsPreviewChanged(settings_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider
// ─────────────────────────────────────────────────────────────────────────────

QWidget *SettingsDialog::buildProviderPage() {
    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(16);

    // ── 左栏：已配置列表 + 底部增删/排序（放在列表下方，不与右侧表单抢横向空间）──
    auto *left = new QVBoxLayout;
    left->setSpacing(8);
    left->addWidget(makeLabel(QStringLiteral("formFieldCaption"), QStringLiteral("已配置 Provider"),
                              FontRole::UiSm, ColorToken::ForegroundSubtle, page));

    auto *listFrame = new QFrame(page);
    listFrame->setProperty("role", "surface");  // 嵌套容器：圆角降级到 10px
    auto *listLayout = new QVBoxLayout(listFrame);
    listLayout->setContentsMargins(6, 6, 6, 6);
    listLayout->setSpacing(0);

    providerList_ = new QListWidget(listFrame);
    providerList_->setObjectName(QStringLiteral("providerList"));
    providerList_->setFixedWidth(kProviderListWidth);
    providerList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    providerList_->setSelectionMode(QAbstractItemView::SingleSelection);
    providerList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    listLayout->addWidget(providerList_);
    left->addWidget(listFrame, 1);

    auto *addButton = new QPushButton(QStringLiteral("新增"), page);
    addButton->setToolTip(QStringLiteral("新增一个 Provider 配置"));
    removeProviderButton_ = new QPushButton(QStringLiteral("删除"), page);
    // 删除会连本地保存的 key 一起丢掉，用 destructive 变体提示后果。
    removeProviderButton_->setProperty("variant", "destructive");
    removeProviderButton_->setToolTip(QStringLiteral("删除选中的 Provider（点「确定」后生效）"));
    moveUpButton_ = new QPushButton(QStringLiteral("上移"), page);
    moveDownButton_ = new QPushButton(QStringLiteral("下移"), page);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    buttonRow->addWidget(addButton);
    buttonRow->addWidget(removeProviderButton_);
    buttonRow->addStretch(1);
    buttonRow->addWidget(moveUpButton_);
    buttonRow->addWidget(moveDownButton_);
    left->addLayout(buttonRow);
    root->addLayout(left);

    // ── 右栏：编辑表单（放进滚动区，小窗口下不会被压扁）──
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    providerForm_ = new QWidget;
    auto *form = new QVBoxLayout(providerForm_);
    form->setContentsMargins(0, 0, 8, 0);  // 右侧留出滚动条位置
    form->setSpacing(8);

    providerEmptyHintLabel_ =
        makeLabel(QStringLiteral("formHint"), QStringLiteral("尚未配置 Provider，点击左下角「新增」开始。"),
                  FontRole::UiSm, ColorToken::ForegroundSubtle, providerForm_);
    providerEmptyHintLabel_->setWordWrap(true);
    form->addWidget(providerEmptyHintLabel_);

    providerNameEdit_ = new QLineEdit(providerForm_);
    providerNameEdit_->setObjectName(QStringLiteral("providerName"));
    providerNameEdit_->setPlaceholderText(QStringLiteral("例如：公司网关"));
    providerNameErrorLabel_ = addFormField(form, QStringLiteral("名称"), providerNameEdit_, providerForm_);

    providerKindCombo_ = new QComboBox(providerForm_);
    providerKindCombo_->setObjectName(QStringLiteral("providerKind"));
    providerKindCombo_->addItem(QStringLiteral("Anthropic Messages"),
                                static_cast<int>(ProviderKind::Anthropic));
    providerKindCombo_->addItem(QStringLiteral("OpenAI 兼容"),
                                static_cast<int>(ProviderKind::OpenAICompatible));
    addFormField(form, QStringLiteral("协议"), providerKindCombo_, providerForm_);

    providerBaseUrlEdit_ = new QLineEdit(providerForm_);
    providerBaseUrlEdit_->setObjectName(QStringLiteral("providerBaseUrl"));
    // URL 是技术值 → 等宽字体（只覆盖字体，输入框底色/边框仍走全局令牌）。
    enableMonoTypography(providerBaseUrlEdit_, QStringLiteral("QLineEdit#providerBaseUrl"),
                         FontRole::UiBase);
    providerBaseUrlErrorLabel_ =
        addFormField(form, QStringLiteral("Base URL"), providerBaseUrlEdit_, providerForm_);

    auto *keyRow = new QWidget(providerForm_);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(8);
    providerApiKeyEdit_ = new QLineEdit(keyRow);
    providerApiKeyEdit_->setObjectName(QStringLiteral("providerApiKey"));
    providerApiKeyEdit_->setEchoMode(QLineEdit::Password);  // 默认遮蔽
    enableMonoTypography(providerApiKeyEdit_, QStringLiteral("QLineEdit#providerApiKey"),
                         FontRole::UiBase);
    providerShowKeyCheck_ = new QCheckBox(QStringLiteral("显示"), keyRow);
    providerShowKeyCheck_->setToolTip(QStringLiteral("临时显示本次输入的密钥；已保存的密钥不会回填"));
    keyLayout->addWidget(providerApiKeyEdit_, 1);
    keyLayout->addWidget(providerShowKeyCheck_);
    addFormField(form, QStringLiteral("API Key"), keyRow, providerForm_);

    providerModelsEdit_ = new QPlainTextEdit(providerForm_);
    providerModelsEdit_->setObjectName(QStringLiteral("providerModels"));
    providerModelsEdit_->setProperty("role", "code");  // 嵌套容器：card 底 + cardBorder
    providerModelsEdit_->setFixedHeight(kModelsEditHeight);
    providerModelsEdit_->setTabChangesFocus(true);
    providerModelsEdit_->setPlaceholderText(QStringLiteral("claude-sonnet-4-5"));
    enableMonoTypography(providerModelsEdit_, QStringLiteral("QPlainTextEdit#providerModels"),
                         FontRole::MonoSm);
    addFormField(form, QStringLiteral("模型列表"), providerModelsEdit_, providerForm_);
    providerModelsHintLabel_ =
        makeLabel(QStringLiteral("formHint"), QStringLiteral("每行一个模型 id，空行会被忽略。"),
                  FontRole::UiXs, ColorToken::ForegroundSubtlest, providerForm_);
    form->addWidget(providerModelsHintLabel_);

    providerEnabledCheck_ = new QCheckBox(QStringLiteral("启用"), providerForm_);
    providerEnabledCheck_->setObjectName(QStringLiteral("providerEnabled"));
    providerEnabledCheck_->setToolTip(QStringLiteral("停用后该 Provider 不会出现在模型选择器中"));
    form->addWidget(providerEnabledCheck_);

    auto *separator = new QFrame(providerForm_);
    separator->setProperty("role", "separator");
    separator->setFrameShape(QFrame::HLine);
    form->addWidget(separator);

    providerAccountCaptionLabel_ = makeLabel(
        QStringLiteral("formFieldCaption"),
        QStringLiteral("账号状态（运行时字段，本对话框只读展示）"), FontRole::UiSm,
        ColorToken::ForegroundSubtle, providerForm_);
    form->addWidget(providerAccountCaptionLabel_);

    auto *accountRow = new QHBoxLayout;
    accountRow->setSpacing(12);
    providerAvailabilityLabel_ = makeLabel(QStringLiteral("formHint"), QString(), FontRole::UiSm,
                                           ColorToken::ForegroundSubtle, providerForm_);
    providerReasonLabel_ = makeLabel(QStringLiteral("formHintWeak"), QString(), FontRole::UiSm,
                                     ColorToken::ForegroundSubtlest, providerForm_);
    accountRow->addWidget(providerAvailabilityLabel_);
    accountRow->addWidget(providerReasonLabel_);
    accountRow->addStretch(1);
    form->addLayout(accountRow);
    form->addStretch(1);

    scroll->setWidget(providerForm_);
    root->addWidget(scroll, 1);

    // 信号连接（全部在初值设置之后，避免构造期回写）。
    connect(providerList_, &QListWidget::currentRowChanged, this,
            [this](int row) { loadProviderForm(row); });
    connect(addButton, &QPushButton::clicked, this, [this]() { addProvider(); });
    connect(removeProviderButton_, &QPushButton::clicked, this, [this]() { removeProvider(); });
    connect(moveUpButton_, &QPushButton::clicked, this, [this]() { moveProvider(-1); });
    connect(moveDownButton_, &QPushButton::clicked, this, [this]() { moveProvider(1); });
    connect(providerNameEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { applyProviderForm(); });
    connect(providerBaseUrlEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { applyProviderForm(); });
    connect(providerApiKeyEdit_, &QLineEdit::textChanged, this,
            [this](const QString &) { applyProviderForm(); });
    connect(providerKindCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        syncBaseUrlPlaceholder();
        applyProviderForm();
    });
    connect(providerShowKeyCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        providerApiKeyEdit_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        // 只记"是否显示"，绝不记内容。
        qCDebug(log) << "API Key 可见性切换:" << visible;
    });
    connect(providerModelsEdit_, &QPlainTextEdit::textChanged, this,
            [this]() { applyProviderForm(); });
    connect(providerEnabledCheck_, &QCheckBox::toggled, this,
            [this](bool) { applyProviderForm(); });

    reloadProviderList(settings_.providers.isEmpty() ? -1 : 0);
    return page;
}

void SettingsDialog::reloadProviderList(int selectRow) {
    const bool wasSyncing = syncing_;
    syncing_ = true;

    providerList_->clear();
    for (const ProviderConfig &config : std::as_const(settings_.providers)) {
        auto *item = new QListWidgetItem(providerList_);
        QWidget *widget = makeProviderItemWidget(config, providerList_);
        item->setSizeHint(widget->sizeHint());
        providerList_->setItemWidget(item, widget);
    }
    if (selectRow >= 0 && selectRow < providerList_->count()) {
        providerList_->setCurrentRow(selectRow);
    } else {
        providerList_->setCurrentRow(-1);
    }

    syncing_ = wasSyncing;
    loadProviderForm(providerList_->currentRow());
}

void SettingsDialog::loadProviderForm(int row) {
    const bool wasSyncing = syncing_;
    syncing_ = true;

    currentProvider_ = (row >= 0 && row < settings_.providers.size()) ? row : -1;
    const bool hasProvider = currentProvider_ >= 0;
    providerForm_->setEnabled(hasProvider);
    providerEmptyHintLabel_->setVisible(!hasProvider);

    if (hasProvider) {
        const ProviderConfig &config = settings_.providers.at(currentProvider_);
        providerNameEdit_->setText(config.name);
        const int kindIndex = providerKindCombo_->findData(static_cast<int>(config.kind));
        providerKindCombo_->setCurrentIndex(kindIndex >= 0 ? kindIndex : 0);
        providerBaseUrlEdit_->setText(config.baseUrl);
        // 安全：绝不把已保存的 key 回填到输入框（避免肩窥/误复制）。
        // 已配置时用占位文案表达"留空 = 不修改"。
        providerApiKeyEdit_->clear();
        providerApiKeyEdit_->setPlaceholderText(config.apiKey.isEmpty()
                                                    ? QStringLiteral("未配置")
                                                    : QStringLiteral("已配置，留空表示不修改"));
        providerShowKeyCheck_->setChecked(false);
        providerApiKeyEdit_->setEchoMode(QLineEdit::Password);
        providerModelsEdit_->setPlainText(config.models.join(QLatin1Char('\n')));
        providerEnabledCheck_->setChecked(config.enabled);
        providerAvailabilityLabel_->setText(QStringLiteral("可用性：") +
                                            availabilityDisplay(config.availability));
        const QString reason = unavailableReasonDisplay(config.unavailableReason);
        providerReasonLabel_->setText(reason.isEmpty() ? QString()
                                                      : QStringLiteral("原因：") + reason);
        providerReasonLabel_->setVisible(!reason.isEmpty());
    } else {
        providerNameEdit_->clear();
        providerBaseUrlEdit_->clear();
        providerApiKeyEdit_->clear();
        providerApiKeyEdit_->setPlaceholderText(QStringLiteral("未配置"));
        providerModelsEdit_->clear();
        providerEnabledCheck_->setChecked(false);
        providerAvailabilityLabel_->clear();
        providerReasonLabel_->clear();
        providerReasonLabel_->hide();
    }

    syncBaseUrlPlaceholder();
    syncing_ = wasSyncing;

    updateProviderState();
}

void SettingsDialog::applyProviderForm() {
    if (syncing_ || currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
        return;
    }
    ProviderConfig &config = settings_.providers[currentProvider_];
    config.name = providerNameEdit_->text().trimmed();
    config.kind = static_cast<ProviderKind>(providerKindCombo_->currentData().toInt());
    config.baseUrl = providerBaseUrlEdit_->text().trimmed();
    // 安全：输入框为空表示"不修改"，保留原 key。只有用户真的输入了才覆盖。
    // 这里也绝不打印任何密钥内容。
    if (!providerApiKeyEdit_->text().isEmpty()) {
        config.apiKey = providerApiKeyEdit_->text();
    }
    config.models = parseModelIds(providerModelsEdit_->toPlainText());
    config.enabled = providerEnabledCheck_->isChecked();
    // availability / unavailableReason / extraHeaders / id 是运行时或身份字段，
    // UI 不编辑，原样保留。

    refreshProviderRow(currentProvider_);
    updateProviderState();
}

void SettingsDialog::refreshProviderRow(int row) {
    QListWidgetItem *item = providerList_->item(row);
    if (item == nullptr) {
        return;
    }
    QWidget *widget = providerList_->itemWidget(item);
    if (widget == nullptr) {
        return;
    }
    fillProviderItemWidget(widget, settings_.providers.at(row));
}

void SettingsDialog::updateProviderState() {
    const bool hasProvider = currentProvider_ >= 0 && currentProvider_ < settings_.providers.size();

    QString nameError;
    QString baseUrlError;
    if (hasProvider) {
        if (providerNameEdit_->text().trimmed().isEmpty()) {
            nameError = QStringLiteral("名称不能为空");
        }
        if (providerBaseUrlEdit_->text().trimmed().isEmpty()) {
            baseUrlError = QStringLiteral("Base URL 不能为空");
        }
    }
    providerNameErrorLabel_->setText(nameError);
    providerNameErrorLabel_->setVisible(!nameError.isEmpty());
    providerBaseUrlErrorLabel_->setText(baseUrlError);
    providerBaseUrlErrorLabel_->setVisible(!baseUrlError.isEmpty());

    if (removeProviderButton_ != nullptr) {
        removeProviderButton_->setEnabled(hasProvider);
    }
    if (moveUpButton_ != nullptr) {
        moveUpButton_->setEnabled(hasProvider && currentProvider_ > 0);
    }
    if (moveDownButton_ != nullptr) {
        moveDownButton_->setEnabled(hasProvider &&
                                    currentProvider_ + 1 < settings_.providers.size());
    }
    // 没有选中项时不算"非法"（列表为空也是合法状态），只有选中且缺必填项才拦确定。
    if (okButton_ != nullptr) {
        okButton_->setEnabled(!hasProvider || (nameError.isEmpty() && baseUrlError.isEmpty()));
    }
    if (hasProvider && (!nameError.isEmpty() || !baseUrlError.isEmpty())) {
        qCDebug(log) << "Provider 表单校验未通过:" << nameError << baseUrlError;
    }
}

void SettingsDialog::syncBaseUrlPlaceholder() {
    const auto kind = static_cast<ProviderKind>(providerKindCombo_->currentData().toInt());
    // 占位符按协议给默认值：用户不必去翻文档。
    providerBaseUrlEdit_->setPlaceholderText(kind == ProviderKind::Anthropic
                                                 ? QStringLiteral("https://api.anthropic.com")
                                                 : QStringLiteral("https://api.openai.com/v1"));
}

void SettingsDialog::addProvider() {
    ProviderConfig config;
    config.id = newId(QStringLiteral("provider"));
    config.name = QStringLiteral("新 Provider");
    config.kind = ProviderKind::OpenAICompatible;
    // baseUrl 留空：由占位符按协议提示默认值，避免"选了 Anthropic 却留着 OpenAI 的 URL"。
    config.enabled = true;
    settings_.providers.append(config);
    qCInfo(log) << "新增 Provider:" << config.id << toToken(config.kind);
    reloadProviderList(settings_.providers.size() - 1);
}

void SettingsDialog::removeProvider() {
    if (currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
        return;
    }
    const ProviderConfig &config = settings_.providers.at(currentProvider_);
    // 只记 id 与名称，绝不记 apiKey。
    qCInfo(log) << "删除 Provider:" << config.id << config.name;
    const int removed = currentProvider_;
    settings_.providers.removeAt(removed);
    // QList::size() 是 qsizetype，先落到 int 再比大小，避免 std::min 类型不一致。
    const int remaining = static_cast<int>(settings_.providers.size());
    reloadProviderList(std::min(removed, remaining - 1));
}

void SettingsDialog::moveProvider(int delta) {
    if (currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
        return;
    }
    const int target = currentProvider_ + delta;
    if (target < 0 || target >= settings_.providers.size()) {
        return;
    }
    settings_.providers.swapItemsAt(currentProvider_, target);
    qCDebug(log) << "调整 Provider 顺序:" << currentProvider_ << "→" << target;
    reloadProviderList(target);
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话
// ─────────────────────────────────────────────────────────────────────────────

QWidget *SettingsDialog::buildSessionPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    sessionModeCombo_ = new QComboBox(page);
    sessionModeCombo_->setObjectName(QStringLiteral("sessionModeCombo"));
    // SessionMode::Auto 是运行时内部态，不在 UI 里暴露（Ticket 明确要求）。
    sessionModeCombo_->addItem(sessionModeDisplay(SessionMode::Plan),
                               static_cast<int>(SessionMode::Plan));
    sessionModeCombo_->addItem(sessionModeDisplay(SessionMode::Build),
                               static_cast<int>(SessionMode::Build));
    sessionModeCombo_->addItem(sessionModeDisplay(SessionMode::Edit),
                               static_cast<int>(SessionMode::Edit));
    sessionModeCombo_->addItem(sessionModeDisplay(SessionMode::Yolo),
                               static_cast<int>(SessionMode::Yolo));
    int modeIndex = sessionModeCombo_->findData(static_cast<int>(settings_.defaultSessionMode));
    if (modeIndex < 0) {
        // 内部态（auto）或未知值：不写进下拉框，回退 build 并留下告警。
        qCWarning(log) << "默认会话模式是内部态或未知值，回退 build:"
                       << toToken(settings_.defaultSessionMode);
        settings_.defaultSessionMode = SessionMode::Build;
        modeIndex = sessionModeCombo_->findData(static_cast<int>(SessionMode::Build));
    }
    sessionModeCombo_->setCurrentIndex(std::max(modeIndex, 0));

    persistSessionsCheck_ = new QCheckBox(QStringLiteral("持久化会话"), page);
    persistSessionsCheck_->setObjectName(QStringLiteral("persistSessionsCheck"));
    persistSessionsCheck_->setChecked(settings_.persistSessions);
    persistSessionsCheck_->setToolTip(
        QStringLiteral("关闭后新建会话仅存在于内存中（当前版本仍会落盘，保留开关以便后续演进）"));

    addFormField(layout, QStringLiteral("默认会话模式"), sessionModeCombo_, page);
    layout->addWidget(persistSessionsCheck_);

    auto *separator = new QFrame(page);
    separator->setProperty("role", "separator");
    separator->setFrameShape(QFrame::HLine);
    layout->addWidget(separator);

    auto *captionRow = new QHBoxLayout;
    captionRow->setSpacing(8);
    captionRow->addWidget(makeLabel(QStringLiteral("formFieldCaption"),
                                    QStringLiteral("最近工作区"), FontRole::UiSm,
                                    ColorToken::ForegroundSubtle, page));
    captionRow->addStretch(1);
    clearRecentButton_ = new QPushButton(QStringLiteral("清空记录"), page);
    clearRecentButton_->setToolTip(QStringLiteral("清空最近工作区列表（点「确定」后生效）"));
    captionRow->addWidget(clearRecentButton_);
    layout->addLayout(captionRow);

    recentList_ = new QListWidget(page);
    recentList_->setObjectName(QStringLiteral("recentWorkspaceList"));
    recentList_->setSelectionMode(QAbstractItemView::NoSelection);  // 只读列表
    recentList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recentList_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(recentList_, 1);

    recentEmptyLabel_ =
        makeLabel(QStringLiteral("formHint"), QStringLiteral("暂无最近工作区记录。"),
                  FontRole::UiXs, ColorToken::ForegroundSubtlest, page);
    layout->addWidget(recentEmptyLabel_);

    // 先赋值、后连接。
    connect(sessionModeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        settings_.defaultSessionMode =
            static_cast<SessionMode>(sessionModeCombo_->currentData().toInt());
        qCDebug(log) << "默认会话模式改为:" << toToken(settings_.defaultSessionMode);
    });
    connect(persistSessionsCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        settings_.persistSessions = checked;
        qCDebug(log) << "持久化会话:" << checked;
    });
    connect(clearRecentButton_, &QPushButton::clicked, this,
            [this]() { clearRecentWorkspaces(); });

    reloadRecentWorkspaces();
    return page;
}

void SettingsDialog::reloadRecentWorkspaces() {
    recentList_->clear();
    for (const QString &path : std::as_const(settings_.recentWorkspaces)) {
        auto *item = new QListWidgetItem(path, recentList_);
        // 路径是技术值 → 等宽字体。
        item->setFont(Theme::instance().font(FontRole::MonoSm));
        item->setToolTip(path);
    }
    const bool empty = recentList_->count() == 0;
    recentEmptyLabel_->setVisible(empty);
    if (clearRecentButton_ != nullptr) {
        clearRecentButton_->setEnabled(!empty);
    }
}

void SettingsDialog::clearRecentWorkspaces() {
    const int removed = settings_.recentWorkspaces.size();
    if (removed == 0) {
        return;
    }
    // 只记条数：工作区路径属于用户数据，不进日志。
    qCInfo(log) << "清空最近工作区记录，条数=" << removed;
    settings_.recentWorkspaces.clear();
    reloadRecentWorkspaces();
}

// ─────────────────────────────────────────────────────────────────────────────
// 按钮盒 / 校验 / 日志 / 样式
// ─────────────────────────────────────────────────────────────────────────────

void SettingsDialog::buildButtonBox(QVBoxLayout *root) {
    auto *box = new QDialogButtonBox(this);
    okButton_ = box->addButton(QStringLiteral("确定"), QDialogButtonBox::AcceptRole);
    okButton_->setObjectName(QStringLiteral("settingsOkButton"));
    okButton_->setProperty("accent", true);  // 整个对话框只有这一个主按钮
    okButton_->setDefault(true);
    auto *cancelButton = box->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    cancelButton->setAutoDefault(false);
    connect(box, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    root->addWidget(box);
}

bool SettingsDialog::validateAllProviders() {
    bool valid = true;
    for (int i = 0; i < settings_.providers.size(); ++i) {
        ProviderConfig &config = settings_.providers[i];
        config.name = config.name.trimmed();
        config.baseUrl = config.baseUrl.trimmed();
        const bool nameEmpty = config.name.isEmpty();
        const bool baseUrlEmpty = config.baseUrl.isEmpty();
        if (!nameEmpty && !baseUrlEmpty) {
            continue;
        }
        qCWarning(log) << "Provider 校验失败:" << config.id << "名称空=" << nameEmpty
                       << "BaseURL 空=" << baseUrlEmpty;
        if (valid) {
            // 选中第一个出问题的项，用户能直接看到字段下方的 destructive 提示。
            providerList_->setCurrentRow(i);
            updateProviderState();
        }
        valid = false;
    }
    return valid;
}

void SettingsDialog::logAcceptedSettings() {
    // 只记录可审计的配置项。**绝不记录 apiKey**，只记"是否已配置"。
    qCInfo(log) << "设置已确认: themeMode=" << toToken(settings_.themeMode)
                << "uiFontSize=" << settings_.uiFontSize
                << "codeFontSize=" << settings_.codeFontSize
                << "language=" << settings_.language
                << "defaultSessionMode=" << toToken(settings_.defaultSessionMode)
                << "persistSessions=" << settings_.persistSessions
                << "providers=" << settings_.providers.size()
                << "recentWorkspaces=" << settings_.recentWorkspaces.size();
    for (const ProviderConfig &config : std::as_const(settings_.providers)) {
        qCInfo(log) << "  provider:" << config.id << config.name << toToken(config.kind)
                    << config.baseUrl << "models=" << config.models.size()
                    << "enabled=" << config.enabled
                    << "apiKeyConfigured=" << !config.apiKey.isEmpty()
                    << "availability=" << toToken(config.availability);
    }
}

void SettingsDialog::applyShellStyle() {
    const Palette &palette = Theme::instance().palette();
    // 对话框外壳 16px 圆角（一级容器），嵌套容器由 10/8px 负责。
    setStyleSheet(
        QStringLiteral("QDialog#%1 { background: %2; border: 1px solid %3; border-radius: 16px; }")
            .arg(objectName(), Theme::css(palette.background), Theme::css(palette.popoverBorder)));
}

void SettingsDialog::refreshTokenStyles() {
    applyShellStyle();

    // 1) 所有令牌化标签（含 provider 列表项内的主/副标题）。
    const QList<QLabel *> labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        applyLabelStyle(label);
    }

    // 2) 等宽技术值输入框。
    const QList<QWidget *> widgets = findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (!widget->property("tokenMonoRole").isValid()) {
            continue;
        }
        const auto role = static_cast<FontRole>(widget->property("tokenMonoRole").toInt());
        const QString selector = widget->property("tokenMonoSelector").toString();
        if (!selector.isEmpty()) {
            enableMonoTypography(widget, selector, role);
        }
    }

    // 3) 列表项文本：字号变了要按新行高重新省略。
    for (int i = 0; i < providerList_->count() && i < settings_.providers.size(); ++i) {
        QWidget *widget = providerList_->itemWidget(providerList_->item(i));
        if (widget != nullptr) {
            fillProviderItemWidget(widget, settings_.providers.at(i));
        }
    }
    for (int i = 0; i < recentList_->count(); ++i) {
        recentList_->item(i)->setFont(Theme::instance().font(FontRole::MonoSm));
    }
}

}  // namespace zcode::ui
