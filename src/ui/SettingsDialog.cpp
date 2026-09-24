#include "ui/SettingsDialog.h"

#include "model/ModelCatalog.h"

#include "skills/SkillLibrary.h"

#include "core/Ids.h"
#include "model/ReasoningLevels.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QListWidget>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QMessageBox>
#include <QTableWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <utility>
#include <QCoreApplication>

namespace lycode::ui
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.ui.dialogs")

/// Provider 列表固定宽度：右栏表单需要剩余全部横向空间，左栏只做选择。
constexpr int kProviderListWidth = 196;
/// 列表项文本省略预算（列表宽度 - 内边距 - 滚动条），避免长 URL 撑出横向滚动。
constexpr int kProviderItemTextWidth = 196;
/// 模型列表输入框高度（约 4-5 行）。
constexpr int kModelsEditHeight = 96;

// ── 模型能力覆盖 ────────────────────────────────────────────────────────────
/// 上下文窗口上限：足够覆盖当前最大的公开模型（Claude 200k、Gemini 1M-2M）。
constexpr int kMaxContextWindow = 2000000;
/// 单次最大输出上限。
constexpr int kMaxOutputTokens = 200000;
/// 表格整体高度上限：超过这个高度就由表格自身滚动，不再把页面撑长。
constexpr int kCapabilityTableMaxHeight = 320;
/// 表格整体高度下限：空/单行时也留出表头 + 一行的空间。
constexpr int kCapabilityTableMinHeight = 132;

/// 模型能力表格的列序。
enum CapabilityColumn {
  CapabilityModelColumn = 0,
  CapabilityContextColumn = 1,
  CapabilityOutputColumn = 2,
  CapabilityReasoningColumn = 3,
  CapabilityDefaultColumn = 4,
};

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

QColor resolveColor(ColorToken token)
{
  const Palette & palette = Theme::instance().palette();
  switch(token) {
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

bool isMonoRole(FontRole role)
{
  return role == FontRole::Mono || role == FontRole::MonoSm;
}

void applyLabelStyle(QLabel * label);

/// 创建令牌化标签。objectName 可以复用（同类文本共用同一套令牌）。
QLabel * makeLabel(const QString & objectName, const QString & text, FontRole role, ColorToken color,
                   QWidget * parent)
{
  auto * label = new QLabel(text, parent);
  label->setObjectName(objectName);
  label->setProperty("tokenFontRole", static_cast<int>(role));
  label->setProperty("tokenColorToken", static_cast<int>(color));
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  applyLabelStyle(label);
  return label;
}

void applyLabelStyle(QLabel * label)
{
  if(!label->property("tokenFontRole").isValid()) {
    return;
  }
  Theme & theme = Theme::instance();
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
void fillProviderItemWidget(QWidget * widget, const ProviderConfig & config);

QString providerKindDisplay(ProviderKind kind)
{
  return kind == ProviderKind::Anthropic ? QStringLiteral("Anthropic Messages")
         : QCoreApplication::translate("ui::SettingsDialog", "OpenAI-compatible");
}

QString availabilityDisplay(AccountAvailability availability)
{
  switch(availability) {
    case AccountAvailability::Available:
      return QCoreApplication::translate("ui::SettingsDialog", "Available");
    case AccountAvailability::Pending:
      return QCoreApplication::translate("ui::SettingsDialog", "Checking");
    case AccountAvailability::Unavailable:
      return QCoreApplication::translate("ui::SettingsDialog", "Unavailable");
    case AccountAvailability::Unknown:
      return QCoreApplication::translate("ui::SettingsDialog", "Unknown");
  }
  return QCoreApplication::translate("ui::SettingsDialog", "Unknown");
}

QString unavailableReasonDisplay(AccountUnavailableReason reason)
{
  switch(reason) {
    case AccountUnavailableReason::None:
      return {};
    case AccountUnavailableReason::NotAuthenticated:
      return QCoreApplication::translate("ui::SettingsDialog", "Not signed in");
    case AccountUnavailableReason::NotConnected:
      return QCoreApplication::translate("ui::SettingsDialog", "Not connected");
    case AccountUnavailableReason::CredentialFailed:
      return QCoreApplication::translate("ui::SettingsDialog", "Credentials expired");
    case AccountUnavailableReason::NotEntitled:
      return QCoreApplication::translate("ui::SettingsDialog", "No permission");
  }
  return {};
}

/// 会话模式展示串。SessionMode::Auto 是运行时内部态，不在下拉框里暴露，
/// 因此这里只覆盖 UI 允许的四个值。
QString sessionModeDisplay(SessionMode mode)
{
  switch(mode) {
    case SessionMode::Plan:
      return QCoreApplication::translate("ui::SettingsDialog", "plan · read-only");
    case SessionMode::Build:
      return QCoreApplication::translate("ui::SettingsDialog", "build · default");
    case SessionMode::Edit:
      return QCoreApplication::translate("ui::SettingsDialog", "edit · edit");
    case SessionMode::Yolo:
      return QCoreApplication::translate("ui::SettingsDialog", "yolo · no approvals");
    case SessionMode::Auto:
      return QCoreApplication::translate("ui::SettingsDialog", "auto · internal");
  }
  return QCoreApplication::translate("ui::SettingsDialog", "build · default");
}

/// 模型列表：每行一个 id。去重但保持顺序（重复配置没有意义）。
QStringList parseModelIds(const QString & text)
{
  QStringList models;
  const QStringList lines = text.split(QLatin1Char('\n'));
  for(const QString & line : lines) {
    const QString trimmed = line.trimmed();
    if(!trimmed.isEmpty() && !models.contains(trimmed)) {
      models.append(trimmed);
    }
  }
  return models;
}

/// 「思考档位」文本的解析结果：合法 id（去重保序）+ 非法 id（去重保序）。
struct ParsedReasoningIds {
  QStringList valid;
  QStringList unknown;
};

/// 分隔符同时接受半角/全角逗号、顿号与任意空白：用户从文档里复制时经常混用，
/// 只认半角逗号会让"看起来对"的输入被判非法，体验很差。
ParsedReasoningIds parseReasoningIds(const QString & text)
{
  ParsedReasoningIds result;
  QString normalized = text;
  normalized.replace(QChar(0xFF0C), QLatin1Char(','));  // 全角逗号
  normalized.replace(QChar(0x3001), QLatin1Char(','));  // 顿号
  const QStringList tokens =
    normalized.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
  for(const QString & token : tokens) {
    const QString id = token.trimmed();
    if(id.isEmpty()) {
      continue;
    }
    if(findReasoningLevel(id) == nullptr) {
      if(!result.unknown.contains(id)) {
        result.unknown.append(id);
      }
    }
    else if(!result.valid.contains(id)) {
      result.valid.append(id);
    }
  }
  return result;
}

/// 合法档位清单（供 tooltip 展示）。文案形如「off=关闭, low=低, ...」。
QString reasoningLevelsTooltip()
{
  QStringList parts;
  const QList<ReasoningLevel> & levels = standardReasoningLevels();
  parts.reserve(levels.size());
  for(const ReasoningLevel & level : levels) {
    parts.append(level.id + QLatin1Char('=') + level.label);
  }
  return QCoreApplication::translate("ui::SettingsDialog",
                                     "Comma-separated level ids, for example %1.\nValid values: %2\nLeave empty to use the default (whatever the model reports).")
         .arg(defaultReasoningLevelIds().join(QLatin1Char(',')), parts.join(QStringLiteral(", ")));
}

/// 「思考档位」输入框：固定等宽字体（技术值），非法时加一条 destructive 边框。
/// 只写字体/边框，底色与其它状态仍由全局令牌样式负责。
void applyReasoningEditStyle(QLineEdit * edit)
{
  if(edit == nullptr) {
    return;
  }
  const Theme & theme = Theme::instance();
  QString style =
    QStringLiteral("QLineEdit#%1 { font-family: \"%2\"; font-size: %3px;")
    .arg(edit->objectName(), monospaceFamily(),
         QString::number(theme.fontPixelSize(FontRole::MonoSm)));
  if(edit->property("capabilityInvalid").toBool()) {
    style += QStringLiteral(" border: 1px solid %1;")
             .arg(Theme::css(theme.palette().destructive));
  }
  style += QLatin1Char('}');
  edit->setStyleSheet(style);
}

/// 表格行高：必须容得下内嵌的 QSpinBox / QLineEdit，随界面字号令牌变化。
int capabilityRowHeight()
{
  return std::max(36, Theme::instance().fontPixelSize(FontRole::UiBase) + 22);
}

/// 表单字段：字段名（弱化 UiSm）+ 控件 + 校验提示（destructive，默认隐藏）。
/// 返回错误提示标签，调用方保存指针以便实时更新文案。
QLabel * addFormField(QVBoxLayout * layout, const QString & title, QWidget * field, QWidget * parent)
{
  layout->addWidget(makeLabel(QStringLiteral("formFieldCaption"), title, FontRole::UiSm,
                              ColorToken::ForegroundSubtle, parent));
  layout->addWidget(field);
  auto * error = makeLabel(QStringLiteral("formError"), QString(), FontRole::UiSm,
                           ColorToken::Destructive, parent);
  error->setWordWrap(true);
  error->hide();
  layout->addWidget(error);
  return error;
}

/// 只覆盖等宽字体族与字号的技术值输入框：底色/边框仍由全局令牌样式负责。
/// selector 写成 `QLineEdit#providerBaseUrl` 这种 ID 形式，优先级高于全局规则。
void enableMonoTypography(QWidget * widget, const QString & selector, FontRole role)
{
  widget->setProperty("tokenMonoRole", static_cast<int>(role));
  widget->setProperty("tokenMonoSelector", selector);
  Theme & theme = Theme::instance();
  const auto storedRole = static_cast<FontRole>(widget->property("tokenMonoRole").toInt());
  widget->setFont(theme.font(storedRole));
  widget->setStyleSheet(QStringLiteral("%1 { font-family: \"%2\"; font-size: %3px; }")
                        .arg(selector, monospaceFamily(),
                             QString::number(theme.fontPixelSize(storedRole))));
}

/// 列表项内的标签：鼠标事件必须穿透到 QListWidget 的 item 上，否则点不中。
QLabel * makeItemLabel(const QString & objectName, FontRole role, ColorToken color, QWidget * parent)
{
  QLabel * label = makeLabel(objectName, QString(), role, color, parent);
  label->setTextInteractionFlags(Qt::NoTextInteraction);
  label->setAttribute(Qt::WA_TransparentForMouseEvents);
  return label;
}

QWidget * makeProviderItemWidget(const ProviderConfig & config, QListWidget * parent)
{
  auto * widget = new QWidget(parent);
  // 整棵子树对鼠标透明：点击要落到 QListWidget 的 item 上才能正常选中/换行。
  widget->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto * layout = new QVBoxLayout(widget);
  layout->setContentsMargins(2, 2, 2, 2);
  layout->setSpacing(1);
  layout->addWidget(makeItemLabel(QStringLiteral("providerItemName"), FontRole::UiBase,
                                  ColorToken::Foreground, widget));
  layout->addWidget(makeItemLabel(QStringLiteral("providerItemSubtitle"), FontRole::UiXs,
                                  ColorToken::ForegroundSubtlest, widget));
  fillProviderItemWidget(widget, config);
  return widget;
}

void fillProviderItemWidget(QWidget * widget, const ProviderConfig & config)
{
  auto * name = widget->findChild<QLabel *>(QStringLiteral("providerItemName"));
  auto * subtitle = widget->findChild<QLabel *>(QStringLiteral("providerItemSubtitle"));
  if(name != nullptr) {
    const QString text = config.name.isEmpty() ? QCoreApplication::translate("ui::SettingsDialog",
                                                                             "(unnamed)") : config.name;
    name->setText(QFontMetrics(name->font()).elidedText(text, Qt::ElideRight,
                                                        kProviderItemTextWidth));
  }
  if(subtitle != nullptr) {
    QString text = providerKindDisplay(config.kind) + QStringLiteral(" · ") +
                   (config.baseUrl.isEmpty() ? QCoreApplication::translate("ui::SettingsDialog", "No Base URL set") : config.baseUrl);
    if(!config.enabled) {
      text += QCoreApplication::translate("ui::SettingsDialog", " · disabled");
    }
    subtitle->setText(
      QFontMetrics(subtitle->font()).elidedText(text, Qt::ElideMiddle, kProviderItemTextWidth));
  }
}

}  // namespace

SettingsDialog::SettingsDialog(const AppSettings & settings, QWidget * parent)
  : QDialog(parent), settings_(settings)
{
  setObjectName(QStringLiteral("SettingsDialog"));
  setWindowTitle(QCoreApplication::translate("ui::SettingsDialog", "Settings"));
  // 比原先宽得多：Provider 页新增了五列的「模型能力」表格。
  // 五列要在"不出现横向滚动条"的前提下放得下，右栏至少需要 ~620px，
  // 叠加左侧 Provider 列表与边距，对话框需要 ~940px。
  resize(940, 620);
  setMinimumSize(760, 520);

  // 实时预览必须能回滚：记录"屏幕上当时的样子"，而不是 settings 里的值。
  const Theme & theme = Theme::instance();
  originalMode_ = theme.mode();
  originalUiFontSize_ = theme.uiFontSize();
  originalCodeFontSize_ = theme.codeFontSize();

  if(settings_.language.isEmpty()) {
    settings_.language = QStringLiteral("zh-CN");
  }

  auto * root = new QVBoxLayout(this);
  root->setContentsMargins(20, 20, 20, 20);
  root->setSpacing(12);

  tabs_ = new QTabWidget(this);
  tabs_->setObjectName(QStringLiteral("settingsTabs"));
  tabs_->addTab(buildAppearancePage(), QCoreApplication::translate("ui::SettingsDialog", "Appearance"));
  tabs_->addTab(buildProviderPage(), QStringLiteral("Provider"));
  tabs_->addTab(buildSessionPage(), QCoreApplication::translate("ui::SettingsDialog", "Session"));
  tabs_->addTab(buildIntegrationsPage(), QStringLiteral("MCP / Skills"));
  root->addWidget(tabs_, 1);

  buildButtonBox(root);
  refreshTokenStyles();
  updateProviderState();

  // 设置页自己会实时预览主题，所以也必须跟着重刷本地令牌样式。
  connect(&Theme::instance(), &Theme::changed, this, [this]() {
    refreshTokenStyles();
  });

  qCInfo(log) << "设置对话框已打开: providers=" << settings_.providers.size()
              << "themeMode=" << toToken(settings_.themeMode)
              << "uiFontSize=" << settings_.uiFontSize
              << "codeFontSize=" << settings_.codeFontSize
              << "defaultSessionMode=" << toToken(settings_.defaultSessionMode);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::accept()
{
  const bool providersOk = validateAllProviders();
  // 非法的思考档位 id 不写进配置（只写合法子集），但必须在这里也拦一道：
  // 「确定」按钮虽然被禁用，键盘回车 / 程序化调用仍可能走到 accept()，
  // 放过去用户就会以为非法档位已经生效。
  const bool capabilitiesOk = !hasCapabilityErrors();
  if(!capabilitiesOk) {
    refreshCapabilityValidation();
    qCWarning(log) << "模型能力覆盖校验失败，保留对话框: 存在未知思考档位 id";
  }
  if(!providersOk || !capabilitiesOk) {
    // 校验不通过：不关闭，并把用户带到出问题的那个 provider。
    tabs_->setCurrentIndex(1);
    return;
  }
  // 编辑期间只保证"当前 Provider 的键"正确；收尾时统一删掉所有 Provider
  // 里已经从模型列表移除的键，避免配置文件长期堆积无用条目。
  pruneStaleModelOverrides();
  logAcceptedSettings();
  QDialog::accept();
}

void SettingsDialog::reject()
{
  Theme & theme = Theme::instance();
  const bool previewed = theme.mode() != originalMode_ ||
                         theme.uiFontSize() != originalUiFontSize_ ||
                         theme.codeFontSize() != originalCodeFontSize_;
  theme.setMode(originalMode_);
  theme.setUiFontSize(originalUiFontSize_);
  theme.setCodeFontSize(originalCodeFontSize_);
  if(previewed) {
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

QWidget * SettingsDialog::buildAppearancePage()
{
  auto * page = new QWidget;
  auto * layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(8);

  themeCombo_ = new QComboBox(page);
  themeCombo_->setObjectName(QStringLiteral("themeModeCombo"));
  themeCombo_->addItem(QCoreApplication::translate("ui::SettingsDialog", "Follow system"),
                       static_cast<int>(ThemeMode::System));
  themeCombo_->addItem(QCoreApplication::translate("ui::SettingsDialog", "Light"), static_cast<int>(ThemeMode::Light));
  themeCombo_->addItem(QCoreApplication::translate("ui::SettingsDialog", "Dark"), static_cast<int>(ThemeMode::Dark));
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
  languageCombo_->addItem(QCoreApplication::translate("ui::SettingsDialog", "简体中文 (zh-CN)"),
                          QStringLiteral("zh-CN"));
  languageCombo_->addItem(QStringLiteral("English (en-US)"), QStringLiteral("en-US"));
  const int languageIndex = languageCombo_->findData(settings_.language);
  languageCombo_->setCurrentIndex(std::max(languageIndex, 0));
  // 未知语言值在打开时就被归一化，避免"界面显示中文、保存里却留着 fr-FR"。
  settings_.language = languageCombo_->currentData().toString();

  addFormField(layout, QCoreApplication::translate("ui::SettingsDialog", "Theme"), themeCombo_, page);
  addFormField(layout, QCoreApplication::translate("ui::SettingsDialog", "UI font size"), uiFontSpin_, page);
  addFormField(layout, QCoreApplication::translate("ui::SettingsDialog", "Code font size"), codeFontSpin_, page);
  addFormField(layout, QCoreApplication::translate("ui::SettingsDialog", "Language"), languageCombo_, page);
  layout->addWidget(makeLabel(QStringLiteral("formHint"),
                              QCoreApplication::translate("ui::SettingsDialog",
                                                          "Switches the interface language. English is the source text; Chinese comes from the bundled translation file."),
                              FontRole::UiXs, ColorToken::ForegroundSubtlest, page));
  layout->addStretch(1);

  // 先赋值、后连接：构造期不会触发一次多余的主题预览。
  connect(themeCombo_, &QComboBox::currentIndexChanged, this,
  [this](int) {
    applyAppearancePreview();
  });
  connect(uiFontSpin_, &QSpinBox::valueChanged, this, [this](int) {
    applyAppearancePreview();
  });
  connect(codeFontSpin_, &QSpinBox::valueChanged, this, [this](int) {
    applyAppearancePreview();
  });
  connect(languageCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
    // 语言只保存值，不影响主题，因此不发预览信号。
    settings_.language = languageCombo_->currentData().toString();
    qCDebug(log) << "语言偏好改为:" << settings_.language;
  });
  return page;
}

void SettingsDialog::applyAppearancePreview()
{
  Theme & theme = Theme::instance();
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

QWidget * SettingsDialog::buildProviderPage()
{
  auto * page = new QWidget;
  auto * root = new QHBoxLayout(page);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(16);

  // ── 左栏：已配置列表 + 底部增删/排序（放在列表下方，不与右侧表单抢横向空间）──
  auto * left = new QVBoxLayout;
  left->setSpacing(8);
  left->addWidget(makeLabel(QStringLiteral("formFieldCaption"), QCoreApplication::translate("ui::SettingsDialog",
                                                                                            "Configured providers"),
                            FontRole::UiSm, ColorToken::ForegroundSubtle, page));

  auto * listFrame = new QFrame(page);
  listFrame->setProperty("role", "surface");  // 嵌套容器：圆角降级到 10px
  auto * listLayout = new QVBoxLayout(listFrame);
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

  auto * addButton = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Add"), page);
  addButton->setToolTip(QCoreApplication::translate("ui::SettingsDialog", "Add a provider"));
  removeProviderButton_ = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Delete"), page);
  // 删除会连本地保存的 key 一起丢掉，用 destructive 变体提示后果。
  removeProviderButton_->setProperty("variant", "destructive");
  removeProviderButton_->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                                "Delete the selected provider (applies when you click OK)"));
  moveUpButton_ = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Move up"), page);
  moveDownButton_ = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Move down"), page);

  auto * buttonRow = new QHBoxLayout;
  buttonRow->setSpacing(8);
  buttonRow->addWidget(addButton);
  buttonRow->addWidget(removeProviderButton_);
  buttonRow->addStretch(1);
  buttonRow->addWidget(moveUpButton_);
  buttonRow->addWidget(moveDownButton_);
  left->addLayout(buttonRow);
  root->addLayout(left);

  // ── 右栏：编辑表单（放进滚动区，小窗口下不会被压扁）──
  auto * scroll = new QScrollArea(page);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  providerForm_ = new QWidget;
  auto * form = new QVBoxLayout(providerForm_);
  form->setContentsMargins(0, 0, 8, 0);  // 右侧留出滚动条位置
  form->setSpacing(8);

  providerEmptyHintLabel_ =
    makeLabel(QStringLiteral("formHint"), QCoreApplication::translate("ui::SettingsDialog",
                                                                      "No providers configured yet. Click \"Add\" in the lower left to start."),
              FontRole::UiSm, ColorToken::ForegroundSubtle, providerForm_);
  providerEmptyHintLabel_->setWordWrap(true);
  form->addWidget(providerEmptyHintLabel_);

  providerNameEdit_ = new QLineEdit(providerForm_);
  providerNameEdit_->setObjectName(QStringLiteral("providerName"));
  providerNameEdit_->setPlaceholderText(QCoreApplication::translate("ui::SettingsDialog",
                                                                    "For example: Company gateway"));
  providerNameErrorLabel_ = addFormField(form, QCoreApplication::translate("ui::SettingsDialog", "Name"),
                                         providerNameEdit_, providerForm_);

  providerKindCombo_ = new QComboBox(providerForm_);
  providerKindCombo_->setObjectName(QStringLiteral("providerKind"));
  providerKindCombo_->addItem(QStringLiteral("Anthropic Messages"),
                              static_cast<int>(ProviderKind::Anthropic));
  providerKindCombo_->addItem(QCoreApplication::translate("ui::SettingsDialog", "OpenAI-compatible"),
                              static_cast<int>(ProviderKind::OpenAICompatible));
  addFormField(form, QCoreApplication::translate("ui::SettingsDialog", "Protocol"), providerKindCombo_, providerForm_);

  providerBaseUrlEdit_ = new QLineEdit(providerForm_);
  providerBaseUrlEdit_->setObjectName(QStringLiteral("providerBaseUrl"));
  // URL 是技术值 → 等宽字体（只覆盖字体，输入框底色/边框仍走全局令牌）。
  enableMonoTypography(providerBaseUrlEdit_, QStringLiteral("QLineEdit#providerBaseUrl"),
                       FontRole::UiBase);
  providerBaseUrlErrorLabel_ =
    addFormField(form, QStringLiteral("Base URL"), providerBaseUrlEdit_, providerForm_);

  auto * keyRow = new QWidget(providerForm_);
  auto * keyLayout = new QHBoxLayout(keyRow);
  keyLayout->setContentsMargins(0, 0, 0, 0);
  keyLayout->setSpacing(8);
  providerApiKeyEdit_ = new QLineEdit(keyRow);
  providerApiKeyEdit_->setObjectName(QStringLiteral("providerApiKey"));
  providerApiKeyEdit_->setEchoMode(QLineEdit::Password);  // 默认遮蔽
  enableMonoTypography(providerApiKeyEdit_, QStringLiteral("QLineEdit#providerApiKey"),
                       FontRole::UiBase);
  providerShowKeyCheck_ = new QCheckBox(QCoreApplication::translate("ui::SettingsDialog", "Show"), keyRow);
  providerShowKeyCheck_->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                                "Reveals the key you just typed; a saved key is never filled back in"));
  keyLayout->addWidget(providerApiKeyEdit_, 1);
  keyLayout->addWidget(providerShowKeyCheck_);
  addFormField(form, QStringLiteral("API Key"), keyRow, providerForm_);

  providerModelsEdit_ = new QPlainTextEdit(providerForm_);
  providerModelsEdit_->setObjectName(QStringLiteral("providerModels"));
  providerModelsEdit_->setProperty("role", "code");  // 嵌套容器：card 底 + cardBorder
  providerModelsEdit_->setFixedHeight(kModelsEditHeight);
  providerModelsEdit_->setTabChangesFocus(true);
  providerModelsEdit_->setPlaceholderText(QStringLiteral("claude-sonnet-4-5"));

  // 从内置目录选模型：模型 id、上下文窗口、思考档位都在目录里，
  // 让用户不必逐个手打（手打还容易把 id 拼错，报错要到调用时才发现）。
  enableMonoTypography(providerModelsEdit_, QStringLiteral("QPlainTextEdit#providerModels"),
                       FontRole::MonoSm);
  addFormField(form, QCoreApplication::translate("ui::SettingsDialog", "Model list"), providerModelsEdit_, providerForm_);
  providerModelsHintLabel_ =
    makeLabel(QStringLiteral("formHint"), QCoreApplication::translate("ui::SettingsDialog",
                                                                      "One model id per line; blank lines are ignored."),
              FontRole::UiXs, ColorToken::ForegroundSubtlest, providerForm_);
  form->addWidget(providerModelsHintLabel_);

  // 从内置目录选模型：模型 id、上下文窗口、思考档位都在目录里，
  // 让用户不必逐个手打（手打容易拼错 id，报错要等到调用时才发现）。
  {
    auto * pickRow = new QHBoxLayout;
    pickRow->setContentsMargins(0, 0, 0, 0);
    auto * pickButton = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Pick from list…"),
                                        providerForm_);
    pickButton->setObjectName(QStringLiteral("pickModelsFromCatalog"));
    pickButton->setToolTip(
      QCoreApplication::translate("ui::SettingsDialog",
                                  "Pick from the built-in model catalog, including context window and reasoning levels"));
    connect(pickButton, &QPushButton::clicked, this,
            &SettingsDialog::pickModelsFromCatalog);
    pickRow->addWidget(pickButton);
    pickRow->addStretch(1);
    form->addLayout(pickRow);
  }

  providerEnabledCheck_ = new QCheckBox(QCoreApplication::translate("ui::SettingsDialog", "Enabled"), providerForm_);
  providerEnabledCheck_->setObjectName(QStringLiteral("providerEnabled"));
  providerEnabledCheck_->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                                "Disabled providers do not appear in the model picker"));
  form->addWidget(providerEnabledCheck_);

  // ── 模型能力覆盖 ──────────────────────────────────────────────────────
  // 用 QGroupBox 而不是自绘容器：它的 sizeHint 由布局算出（本项目踩过
  // "把表格塞进 sizeHint 不含子布局的控件导致表格被压成 0 高"的坑）。
  // 表格本身放在 providerForm_ 的滚动区里，且自身高度有上限，两级都能滚。
  modelCapabilityGroup_ = new QGroupBox(QCoreApplication::translate("ui::SettingsDialog", "Model capabilities"),
                                        providerForm_);
  modelCapabilityGroup_->setObjectName(QStringLiteral("modelCapabilityGroup"));
  auto * capabilityLayout = new QVBoxLayout(modelCapabilityGroup_);
  capabilityLayout->setContentsMargins(12, 16, 12, 12);  // 顶部为 QGroupBox 标题留空
  capabilityLayout->setSpacing(8);
  capabilityLayout->addWidget(makeLabel(
                                QStringLiteral("modelCapabilityHint"),
                                QCoreApplication::translate("ui::SettingsDialog",
                                                            "Overrides the metadata the provider reports. Leave 0 or empty to keep the built-in default. Context window and output limit are in tokens."),
                                FontRole::UiXs, ColorToken::ForegroundSubtlest, modelCapabilityGroup_));

  modelCapabilityTable_ = new QTableWidget(0, 5, modelCapabilityGroup_);
  modelCapabilityTable_->setObjectName(QStringLiteral("modelCapabilityTable"));
  applyCapabilityHeaderLabels();
  modelCapabilityTable_->verticalHeader()->setVisible(false);
  // 「模型」列自适应剩余宽度，其余列固定宽度（数值/下拉宽度稳定才好扫读）。
  modelCapabilityTable_->horizontalHeader()->setSectionResizeMode(
    CapabilityModelColumn, QHeaderView::Stretch);
  for(int column = CapabilityContextColumn; column <= CapabilityDefaultColumn; ++column) {
    modelCapabilityTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
  }
  // 固定列宽之和要留在右栏可用宽度内：五列全固定会一打开就出现横向滚动条，
  // 用户看不到"默认档位"那一列。（原先 132+124+236+148 = 640 就超了。）
  modelCapabilityTable_->setColumnWidth(CapabilityContextColumn, 92);
  modelCapabilityTable_->setColumnWidth(CapabilityOutputColumn, 92);
  modelCapabilityTable_->setColumnWidth(CapabilityReasoningColumn, 132);
  modelCapabilityTable_->setColumnWidth(CapabilityDefaultColumn, 104);
  modelCapabilityTable_->horizontalHeader()->setStretchLastSection(false);
  modelCapabilityTable_->horizontalHeader()->setHighlightSections(false);
  modelCapabilityTable_->setWordWrap(false);
  // 只读表格：没有"当前单元格"这一说，避免表格抢焦点后键盘操作语义混乱。
  modelCapabilityTable_->setSelectionMode(QAbstractItemView::NoSelection);
  modelCapabilityTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  modelCapabilityTable_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  modelCapabilityTable_->setFrameShape(QFrame::NoFrame);
  modelCapabilityTable_->setShowGrid(true);
  capabilityLayout->addWidget(modelCapabilityTable_);

  modelCapabilityEmptyHint_ =
    makeLabel(QStringLiteral("modelCapabilityEmptyHint"),
              QCoreApplication::translate("ui::SettingsDialog", "Fill in the model list above first"), FontRole::UiXs,
              ColorToken::ForegroundSubtlest, modelCapabilityGroup_);
  capabilityLayout->addWidget(modelCapabilityEmptyHint_);

  modelCapabilityErrorLabel_ = makeLabel(QStringLiteral("modelCapabilityError"), QString(),
                                         FontRole::UiSm, ColorToken::Destructive,
                                         modelCapabilityGroup_);
  modelCapabilityErrorLabel_->setWordWrap(true);
  modelCapabilityErrorLabel_->hide();
  capabilityLayout->addWidget(modelCapabilityErrorLabel_);

  form->addWidget(modelCapabilityGroup_);

  auto * separator = new QFrame(providerForm_);
  separator->setProperty("role", "separator");
  separator->setFrameShape(QFrame::HLine);
  form->addWidget(separator);

  providerAccountCaptionLabel_ = makeLabel(
                                   QStringLiteral("formFieldCaption"),
                                   QCoreApplication::translate("ui::SettingsDialog", "Account status (a runtime field; read-only here)"), FontRole::UiSm,
                                   ColorToken::ForegroundSubtle, providerForm_);
  form->addWidget(providerAccountCaptionLabel_);

  auto * accountRow = new QHBoxLayout;
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
  [this](int row) {
    loadProviderForm(row);
  });
  connect(addButton, &QPushButton::clicked, this, [this]() {
    addProvider();
  });
  connect(removeProviderButton_, &QPushButton::clicked, this, [this]() {
    removeProvider();
  });
  connect(moveUpButton_, &QPushButton::clicked, this, [this]() {
    moveProvider(-1);
  });
  connect(moveDownButton_, &QPushButton::clicked, this, [this]() {
    moveProvider(1);
  });
  connect(providerNameEdit_, &QLineEdit::textChanged, this,
  [this](const QString &) {
    applyProviderForm();
  });
  connect(providerBaseUrlEdit_, &QLineEdit::textChanged, this,
  [this](const QString &) {
    applyProviderForm();
  });
  connect(providerApiKeyEdit_, &QLineEdit::textChanged, this,
  [this](const QString &) {
    applyProviderForm();
  });
  connect(providerKindCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
    syncBaseUrlPlaceholder();
    applyProviderForm();
  });
  connect(providerShowKeyCheck_, &QCheckBox::toggled, this, [this](bool visible) {
    providerApiKeyEdit_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    // 只记"是否显示"，绝不记内容。
    qCDebug(log) << "API Key 可见性切换:" << visible;
  });
  connect(providerModelsEdit_, &QPlainTextEdit::textChanged, this, [this]() {
    applyProviderForm();
    // 模型列表变了 → 表格行跟着增删。已有行的编辑值从 settings_ 回填，不会丢。
    reloadModelCapabilityTable();
  });
  connect(providerEnabledCheck_, &QCheckBox::toggled, this,
  [this](bool) {
    applyProviderForm();
  });

  reloadProviderList(settings_.providers.isEmpty() ? -1 : 0);
  return page;
}

void SettingsDialog::reloadProviderList(int selectRow)
{
  const bool wasSyncing = syncing_;
  syncing_ = true;

  providerList_->clear();
  for(const ProviderConfig & config : std::as_const(settings_.providers)) {
    auto * item = new QListWidgetItem(providerList_);
    QWidget * widget = makeProviderItemWidget(config, providerList_);
    item->setSizeHint(widget->sizeHint());
    providerList_->setItemWidget(item, widget);
  }
  if(selectRow >= 0 && selectRow < providerList_->count()) {
    providerList_->setCurrentRow(selectRow);
  }
  else {
    providerList_->setCurrentRow(-1);
  }

  syncing_ = wasSyncing;
  loadProviderForm(providerList_->currentRow());
}

void SettingsDialog::loadProviderForm(int row)
{
  const bool wasSyncing = syncing_;
  syncing_ = true;

  currentProvider_ = (row >= 0 && row < settings_.providers.size()) ? row : -1;
  const bool hasProvider = currentProvider_ >= 0;
  providerForm_->setEnabled(hasProvider);
  providerEmptyHintLabel_->setVisible(!hasProvider);

  if(hasProvider) {
    const ProviderConfig & config = settings_.providers.at(currentProvider_);
    providerNameEdit_->setText(config.name);
    const int kindIndex = providerKindCombo_->findData(static_cast<int>(config.kind));
    providerKindCombo_->setCurrentIndex(kindIndex >= 0 ? kindIndex : 0);
    providerBaseUrlEdit_->setText(config.baseUrl);
    // 安全：绝不把已保存的 key 回填到输入框（避免肩窥/误复制）。
    // 已配置时用占位文案表达"留空 = 不修改"。
    providerApiKeyEdit_->clear();
    providerApiKeyEdit_->setPlaceholderText(config.apiKey.isEmpty()
                                            ? QCoreApplication::translate("ui::SettingsDialog", "Not configured")
                                            : QCoreApplication::translate("ui::SettingsDialog", "Configured; leave empty to keep it unchanged"));
    providerShowKeyCheck_->setChecked(false);
    providerApiKeyEdit_->setEchoMode(QLineEdit::Password);
    providerModelsEdit_->setPlainText(config.models.join(QLatin1Char('\n')));
    providerEnabledCheck_->setChecked(config.enabled);
    providerAvailabilityLabel_->setText(QCoreApplication::translate("ui::SettingsDialog", "Availability: ") +
                                        availabilityDisplay(config.availability));
    const QString reason = unavailableReasonDisplay(config.unavailableReason);
    providerReasonLabel_->setText(reason.isEmpty() ? QString()
                                  : QCoreApplication::translate("ui::SettingsDialog", "Reason: ") + reason);
    providerReasonLabel_->setVisible(!reason.isEmpty());
  }
  else {
    providerNameEdit_->clear();
    providerBaseUrlEdit_->clear();
    providerApiKeyEdit_->clear();
    providerApiKeyEdit_->setPlaceholderText(QCoreApplication::translate("ui::SettingsDialog", "Not configured"));
    providerModelsEdit_->clear();
    providerEnabledCheck_->setChecked(false);
    providerAvailabilityLabel_->clear();
    providerReasonLabel_->clear();
    providerReasonLabel_->hide();
  }

  syncBaseUrlPlaceholder();
  syncing_ = wasSyncing;

  reloadModelCapabilityTable();
  updateProviderState();
}

void SettingsDialog::applyProviderForm()
{
  if(syncing_ || currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
    return;
  }
  ProviderConfig & config = settings_.providers[currentProvider_];
  config.name = providerNameEdit_->text().trimmed();
  config.kind = static_cast<ProviderKind>(providerKindCombo_->currentData().toInt());
  config.baseUrl = providerBaseUrlEdit_->text().trimmed();
  // 安全：输入框为空表示"不修改"，保留原 key。只有用户真的输入了才覆盖。
  // 这里也绝不打印任何密钥内容。
  if(!providerApiKeyEdit_->text().isEmpty()) {
    config.apiKey = providerApiKeyEdit_->text();
  }
  config.models = parseModelIds(providerModelsEdit_->toPlainText());
  config.enabled = providerEnabledCheck_->isChecked();
  // availability / unavailableReason / extraHeaders / id 是运行时或身份字段，
  // UI 不编辑，原样保留。

  refreshProviderRow(currentProvider_);
  updateProviderState();
}

void SettingsDialog::refreshProviderRow(int row)
{
  QListWidgetItem * item = providerList_->item(row);
  if(item == nullptr) {
    return;
  }
  QWidget * widget = providerList_->itemWidget(item);
  if(widget == nullptr) {
    return;
  }
  fillProviderItemWidget(widget, settings_.providers.at(row));
}

void SettingsDialog::updateProviderState()
{
  const bool hasProvider = currentProvider_ >= 0 && currentProvider_ < settings_.providers.size();

  QString nameError;
  QString baseUrlError;
  if(hasProvider) {
    if(providerNameEdit_->text().trimmed().isEmpty()) {
      nameError = QCoreApplication::translate("ui::SettingsDialog", "Name cannot be empty");
    }
    if(providerBaseUrlEdit_->text().trimmed().isEmpty()) {
      baseUrlError = QCoreApplication::translate("ui::SettingsDialog", "Base URL cannot be empty");
    }
  }
  providerNameErrorLabel_->setText(nameError);
  providerNameErrorLabel_->setVisible(!nameError.isEmpty());
  providerBaseUrlErrorLabel_->setText(baseUrlError);
  providerBaseUrlErrorLabel_->setVisible(!baseUrlError.isEmpty());

  if(removeProviderButton_ != nullptr) {
    removeProviderButton_->setEnabled(hasProvider);
  }
  if(moveUpButton_ != nullptr) {
    moveUpButton_->setEnabled(hasProvider && currentProvider_ > 0);
  }
  if(moveDownButton_ != nullptr) {
    moveDownButton_->setEnabled(hasProvider &&
                                currentProvider_ + 1 < settings_.providers.size());
  }
  // 没有选中项时不算"非法"（列表为空也是合法状态），只有选中且缺必填项才拦确定。
  // 模型能力里的非法档位 id 同样拦确定：那种值写进配置后在请求阶段只会被静默忽略，
  // 用户会以为设置生效了。
  const bool capabilityOk = !hasCapabilityErrors();
  if(okButton_ != nullptr) {
    okButton_->setEnabled((!hasProvider ||
                           (nameError.isEmpty() && baseUrlError.isEmpty())) &&
                          capabilityOk);
  }
  if(hasProvider && (!nameError.isEmpty() || !baseUrlError.isEmpty())) {
    qCDebug(log) << "Provider 表单校验未通过:" << nameError << baseUrlError;
  }
  if(!capabilityOk) {
    qCDebug(log) << "模型能力覆盖校验未通过: 存在未知思考档位 id";
  }
}

void SettingsDialog::syncBaseUrlPlaceholder()
{
  const auto kind = static_cast<ProviderKind>(providerKindCombo_->currentData().toInt());
  // 占位符按协议给默认值：用户不必去翻文档。
  providerBaseUrlEdit_->setPlaceholderText(kind == ProviderKind::Anthropic
                                           ? QStringLiteral("https://api.anthropic.com")
                                           : QStringLiteral("https://api.openai.com/v1"));
}

void SettingsDialog::addProvider()
{
  ProviderConfig config;
  config.id = newId(QStringLiteral("provider"));
  config.name = QCoreApplication::translate("ui::SettingsDialog", "New provider");
  config.kind = ProviderKind::OpenAICompatible;
  // baseUrl 留空：由占位符按协议提示默认值，避免"选了 Anthropic 却留着 OpenAI 的 URL"。
  config.enabled = true;
  settings_.providers.append(config);
  qCInfo(log) << "新增 Provider:" << config.id << toToken(config.kind);
  reloadProviderList(settings_.providers.size() - 1);
}

void SettingsDialog::removeProvider()
{
  if(currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
    return;
  }
  const ProviderConfig & config = settings_.providers.at(currentProvider_);
  // 只记 id 与名称，绝不记 apiKey。
  qCInfo(log) << "删除 Provider:" << config.id << config.name;
  const int removed = currentProvider_;
  settings_.providers.removeAt(removed);
  // QList::size() 是 qsizetype，先落到 int 再比大小，避免 std::min 类型不一致。
  const int remaining = static_cast<int>(settings_.providers.size());
  reloadProviderList(std::min(removed, remaining - 1));
}

void SettingsDialog::moveProvider(int delta)
{
  if(currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
    return;
  }
  const int target = currentProvider_ + delta;
  if(target < 0 || target >= settings_.providers.size()) {
    return;
  }
  settings_.providers.swapItemsAt(currentProvider_, target);
  qCDebug(log) << "调整 Provider 顺序:" << currentProvider_ << "→" << target;
  reloadProviderList(target);
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider · 模型能力覆盖
// ─────────────────────────────────────────────────────────────────────────────

void SettingsDialog::applyCapabilityHeaderLabels()
{
  // 集中在一处：构造与 reload 都必须调用它，否则表头会被 clear() 抹掉。
  modelCapabilityTable_->setHorizontalHeaderLabels({
    QCoreApplication::translate("ui::SettingsDialog", "Models"), QCoreApplication::translate("ui::SettingsDialog", "Context window"), QCoreApplication::translate("ui::SettingsDialog", "Max output"),
    QCoreApplication::translate("ui::SettingsDialog", "Reasoning levels"), QCoreApplication::translate("ui::SettingsDialog", "Default level")});
}

void SettingsDialog::reloadModelCapabilityTable()
{
  if(modelCapabilityTable_ == nullptr) {
    return;
  }
  const QStringList models = parseModelIds(providerModelsEdit_->toPlainText());
  const QString providerId =
    (currentProvider_ >= 0 && currentProvider_ < settings_.providers.size())
    ? settings_.providers.at(currentProvider_).id
    : QString();
  // 行集合与所属 Provider 都没变就不重建：重建会销毁用户正在编辑的单元格控件
  // （光标位置、下拉展开状态都会丢）。值本身存在 settings_ 里，不需要靠控件保活。
  if(models == capabilityModelIds_ && providerId == capabilityProviderId_) {
    return;
  }
  capabilityModelIds_ = models;
  capabilityProviderId_ = providerId;

  const bool wasSyncing = syncing_;
  syncing_ = true;

  // clear() 会连单元格控件一起销毁；只 setRowCount(0) 的清理语义依赖实现细节。
  //
  // ⚠ QTableWidget::clear() **同时清空表头文字**，所以表头必须在这里重写一遍。
  // 只在构造里设一次的话，第一次 reload 之后表头就退化成列号 1/2/3（实测踩到）。
  modelCapabilityTable_->clear();
  applyCapabilityHeaderLabels();
  modelCapabilityTable_->setRowCount(static_cast<int>(models.size()));
  for(int row = 0; row < models.size(); ++row) {
    const QString & modelId = models.at(row);
    addCapabilityRow(row, modelId, settings_.modelOverride(providerId, modelId));
  }

  syncing_ = wasSyncing;

  const bool empty = models.isEmpty();
  modelCapabilityTable_->setVisible(!empty);
  modelCapabilityEmptyHint_->setVisible(empty);
  if(empty) {
    modelCapabilityErrorLabel_->clear();
    modelCapabilityErrorLabel_->hide();
  }
  else {
    refreshCapabilityValidation();
  }
  applyCapabilityTableHeight();
}

void SettingsDialog::addCapabilityRow(int row, const QString & modelId,
                                      const ModelOptionOverride & override)
{
  auto * modelItem = new QTableWidgetItem(modelId);
  // 只读：模型 id 由上方「模型列表」维护，这里改会出现两处真相。
  modelItem->setFlags(Qt::ItemIsEnabled);
  // 模型 id 是技术值 → 等宽字体（FontRole::Mono）。
  modelItem->setFont(Theme::instance().font(FontRole::Mono));
  modelItem->setToolTip(modelId);
  modelItem->setData(Qt::UserRole, modelId);
  modelCapabilityTable_->setItem(row, CapabilityModelColumn, modelItem);

  auto * contextSpin = new QSpinBox(modelCapabilityTable_);
  contextSpin->setObjectName(QStringLiteral("modelContextSpin_%1").arg(row));
  contextSpin->setRange(0, kMaxContextWindow);
  contextSpin->setSingleStep(1000);
  // 不加 " tokens" 后缀：五列要挤在右栏里，后缀会把「模型」列压到看不出模型名。
  // 单位写在分组提示里，单元格里只留数字。
  // 0 是"不覆盖"的哨兵值：显示成「默认」而不是一个会让人以为窗口只有 0 的 0。
  contextSpin->setSpecialValueText(QCoreApplication::translate("ui::SettingsDialog", "Default"));
  contextSpin->setToolTip(
    QCoreApplication::translate("ui::SettingsDialog",
                                "0 keeps the built-in default (usually 128000; 200000 for Claude models)"));
  contextSpin->setValue(std::clamp(override.contextWindow, 0, kMaxContextWindow));
  modelCapabilityTable_->setCellWidget(row, CapabilityContextColumn, contextSpin);

  auto * outputSpin = new QSpinBox(modelCapabilityTable_);
  outputSpin->setObjectName(QStringLiteral("modelOutputSpin_%1").arg(row));
  outputSpin->setRange(0, kMaxOutputTokens);
  outputSpin->setSingleStep(256);
  // 不加 " tokens" 后缀：五列要挤在右栏里，后缀会把「模型」列压到看不出模型名。
  // 单位写在分组提示里，单元格里只留数字。
  outputSpin->setSpecialValueText(QCoreApplication::translate("ui::SettingsDialog", "Default"));
  outputSpin->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                     "0 keeps the built-in default (usually 8192)"));
  outputSpin->setValue(std::clamp(override.maxOutputTokens, 0, kMaxOutputTokens));
  modelCapabilityTable_->setCellWidget(row, CapabilityOutputColumn, outputSpin);

  auto * reasoningEdit = new QLineEdit(modelCapabilityTable_);
  reasoningEdit->setObjectName(QStringLiteral("modelReasoningEdit_%1").arg(row));
  // 占位符给一份保守默认示例；真正生效的默认由模型自报的档位决定。
  reasoningEdit->setPlaceholderText(defaultReasoningLevelIds().join(QLatin1Char(',')));
  reasoningEdit->setToolTip(reasoningLevelsTooltip());
  reasoningEdit->setProperty("capabilityInvalid", false);
  reasoningEdit->setText(override.reasoningLevels.join(QStringLiteral(", ")));
  applyReasoningEditStyle(reasoningEdit);
  modelCapabilityTable_->setCellWidget(row, CapabilityReasoningColumn, reasoningEdit);

  auto * defaultCombo = new QComboBox(modelCapabilityTable_);
  defaultCombo->setObjectName(QStringLiteral("modelDefaultReasoningCombo_%1").arg(row));
  defaultCombo->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                       "Reasoning level selected by default in new sessions"));
  modelCapabilityTable_->setCellWidget(row, CapabilityDefaultColumn, defaultCombo);
  // 下拉的选项必须由同一行的「思考档位」推导，所以先建输入框再建下拉。
  rebuildDefaultReasoningCombo(row, override.defaultReasoningLevel);

  // 连接放在初值之后：构造期不会把"加载"当成"用户编辑"（syncing_ 双保险）。
  connect(contextSpin, &QSpinBox::valueChanged, this, [this, row](int) {
    applyCapabilityRow(row);
  });
  connect(outputSpin, &QSpinBox::valueChanged, this, [this, row](int) {
    applyCapabilityRow(row);
  });
  connect(reasoningEdit, &QLineEdit::textChanged, this,
  [this, row](const QString &) {
    onCapabilityReasoningTextChanged(row);
  });
  connect(defaultCombo, &QComboBox::currentIndexChanged, this,
  [this, row](int) {
    applyCapabilityRow(row);
  });
}

void SettingsDialog::onCapabilityReasoningTextChanged(int row)
{
  if(syncing_) {
    return;
  }
  const auto * combo = qobject_cast<QComboBox *>(
                         modelCapabilityTable_->cellWidget(row, CapabilityDefaultColumn));
  const QString previous = combo != nullptr ? combo->currentData().toString() : QString();
  // 先重建下拉（保留仍合法的旧选择），再整体写回。
  rebuildDefaultReasoningCombo(row, previous);
  applyCapabilityRow(row);
}

void SettingsDialog::rebuildDefaultReasoningCombo(int row, const QString & preferredDefault)
{
  auto * combo =
    qobject_cast<QComboBox *>(modelCapabilityTable_->cellWidget(row, CapabilityDefaultColumn));
  if(combo == nullptr) {
    return;
  }
  const bool wasSyncing = syncing_;
  syncing_ = true;

  combo->clear();
  combo->addItem(QCoreApplication::translate("ui::SettingsDialog", "(unspecified)"), QString());
  const QStringList valid = validReasoningIdsForRow(row, nullptr);
  for(const QString & id : valid) {
    combo->addItem(QStringLiteral("%1 %2").arg(id, reasoningLevelLabel(id)), id);
  }
  // 档位列表为空时"默认档位"没有可选值，禁用而不是给一个空下拉。
  combo->setEnabled(!valid.isEmpty());
  const int preferredIndex = preferredDefault.isEmpty() ? 0 : combo->findData(preferredDefault);
  combo->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);

  syncing_ = wasSyncing;
}

QStringList SettingsDialog::validReasoningIdsForRow(int row, QStringList * unknownOut) const
{
  const auto * edit =
    qobject_cast<QLineEdit *>(modelCapabilityTable_->cellWidget(row, CapabilityReasoningColumn));
  const ParsedReasoningIds parsed =
    parseReasoningIds(edit != nullptr ? edit->text() : QString());
  if(unknownOut != nullptr) {
    *unknownOut = parsed.unknown;
  }
  return parsed.valid;
}

void SettingsDialog::applyCapabilityRow(int row)
{
  if(syncing_ || currentProvider_ < 0 || currentProvider_ >= settings_.providers.size()) {
    return;
  }
  const QString modelId = capabilityModelIdAt(row);
  if(modelId.isEmpty()) {
    return;
  }

  const auto * contextSpin =
    qobject_cast<QSpinBox *>(modelCapabilityTable_->cellWidget(row, CapabilityContextColumn));
  const auto * outputSpin =
    qobject_cast<QSpinBox *>(modelCapabilityTable_->cellWidget(row, CapabilityOutputColumn));
  auto * reasoningEdit =
    qobject_cast<QLineEdit *>(modelCapabilityTable_->cellWidget(row, CapabilityReasoningColumn));
  const auto * defaultCombo =
    qobject_cast<QComboBox *>(modelCapabilityTable_->cellWidget(row, CapabilityDefaultColumn));

  ModelOptionOverride override;
  override.contextWindow = contextSpin != nullptr ? contextSpin->value() : 0;
  override.maxOutputTokens = outputSpin != nullptr ? outputSpin->value() : 0;
  QStringList unknown;
  override.reasoningLevels = validReasoningIdsForRow(row, &unknown);
  const QString chosen = defaultCombo != nullptr ? defaultCombo->currentData().toString()
                         : QString();
  // 默认档位必须属于本行的档位列表：否则写出去的是自相矛盾的覆盖
  //（默认档位不在可用列表里，请求阶段会被静默丢弃）。
  if(!chosen.isEmpty() && override.reasoningLevels.contains(chosen)) {
    override.defaultReasoningLevel = chosen;
  }
  // 只有"当前 Provider + 当前模型"这一个键会被写/删：
  // 其它 Provider 的覆盖项绝不触碰（各 Provider 的模型 id 可能重名，键前缀不同）。
  // 三个字段全空时 AppSettings::setModelOverride 会删除该键——配置文件里不留空对象。
  settings_.setModelOverride(settings_.providers.at(currentProvider_).id, modelId, override);

  if(reasoningEdit != nullptr) {
    reasoningEdit->setProperty("capabilityInvalid", !unknown.isEmpty());
    applyReasoningEditStyle(reasoningEdit);
  }
  refreshCapabilityValidation();
  updateProviderState();
  qCDebug(log) << "模型能力覆盖已更新: provider=" << settings_.providers.at(currentProvider_).id
               << "model=" << modelId << "contextWindow=" << override.contextWindow
               << "maxOutputTokens=" << override.maxOutputTokens
               << "reasoningLevels=" << override.reasoningLevels.join(QLatin1Char(','))
               << "defaultReasoningLevel=" << override.defaultReasoningLevel;
}

void SettingsDialog::refreshCapabilityValidation()
{
  if(modelCapabilityTable_ == nullptr || modelCapabilityErrorLabel_ == nullptr) {
    return;
  }
  QStringList messages;
  for(int row = 0; row < modelCapabilityTable_->rowCount(); ++row) {
    QStringList unknown;
    validReasoningIdsForRow(row, &unknown);
    if(unknown.isEmpty()) {
      continue;
    }
    messages.append(QCoreApplication::translate("ui::SettingsDialog", "Unknown reasoning level id: %1 (model %2)")
                    .arg(unknown.join(QStringLiteral("、")), capabilityModelIdAt(row)));
  }
  const QString text = messages.join(QStringLiteral("；"));
  modelCapabilityErrorLabel_->setText(text);
  modelCapabilityErrorLabel_->setVisible(!text.isEmpty());
}

bool SettingsDialog::hasCapabilityErrors() const
{
  if(modelCapabilityTable_ == nullptr) {
    return false;
  }
  for(int row = 0; row < modelCapabilityTable_->rowCount(); ++row) {
    QStringList unknown;
    validReasoningIdsForRow(row, &unknown);
    if(!unknown.isEmpty()) {
      return true;
    }
  }
  return false;
}

QString SettingsDialog::capabilityModelIdAt(int row) const
{
  if(modelCapabilityTable_ == nullptr) {
    return {};
  }
  const QTableWidgetItem * item = modelCapabilityTable_->item(row, CapabilityModelColumn);
  return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
}

void SettingsDialog::applyCapabilityTableHeight()
{
  if(modelCapabilityTable_ == nullptr) {
    return;
  }
  const int rowHeight = capabilityRowHeight();
  modelCapabilityTable_->verticalHeader()->setDefaultSectionSize(rowHeight);
  const int rows = std::max(1, modelCapabilityTable_->rowCount());
  const int headerHeight = modelCapabilityTable_->horizontalHeader()->height();
  const int desired =
    std::clamp(headerHeight + rowHeight * rows + 6, kCapabilityTableMinHeight,
               kCapabilityTableMaxHeight);
  // 固定高度：行少时表格不占多余空间，行多时由表格自身滚动（外层还有滚动区兜底）。
  modelCapabilityTable_->setMinimumHeight(desired);
  modelCapabilityTable_->setMaximumHeight(desired);
}

void SettingsDialog::pruneStaleModelOverrides()
{
  int removed = 0;
  for(const ProviderConfig & config : std::as_const(settings_.providers)) {
    // 模型列表为空的 Provider 直接跳过：无法区分"用户删光了模型"与"还没填"，
    // 误删会让用户在别处配好的覆盖无声消失（另一个 Provider 的键绝不能被动到）。
    if(config.id.isEmpty() || config.models.isEmpty()) {
      continue;
    }
    QSet<QString> validKeys;
    for(const QString & modelId : config.models) {
      validKeys.insert(modelOptionKey(config.id, modelId));
    }
    // 键的格式是 providerId + '/' + modelId；modelId 里可以含 '/'，所以前缀匹配
    // 必须带上分隔符，否则 provider1 会误伤 provider10。
    const QString prefix = config.id + QLatin1Char('/');
    for(auto it = settings_.modelOverrides.begin(); it != settings_.modelOverrides.end();) {
      if(it.key().startsWith(prefix) && !validKeys.contains(it.key())) {
        it = settings_.modelOverrides.erase(it);
        ++removed;
      }
      else {
        ++it;
      }
    }
  }
  if(removed > 0) {
    qCInfo(log) << "清理已从模型列表移除的覆盖项，条数=" << removed;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// 会话
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// MCP / Skills
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

/// MCP 服务器的编辑对话框。返回 false 表示用户取消。
///
/// 做成一个独立的小对话框而不是就地编辑：服务器有 5 个字段（含多行的
/// args/env），塞进表格单元格里编辑会非常难受。
bool editServerDialog(QWidget * parent, lycode::mcp::ServerConfig * server, bool isNew)
{
  QDialog dialog(parent);
  dialog.setWindowTitle(isNew ? QCoreApplication::translate("ui::SettingsDialog", "Add MCP server")
                        : QCoreApplication::translate("ui::SettingsDialog", "Edit MCP server"));
  dialog.setStyleSheet(Theme::instance().styleSheet());
  dialog.resize(560, 420);

  auto * layout = new QVBoxLayout(&dialog);
  auto * form = new QFormLayout;

  auto * idEdit = new QLineEdit(server->id);
  idEdit->setPlaceholderText(QCoreApplication::translate("ui::SettingsDialog",
                                                         "For example github (it becomes the tool name prefix)"));
  form->addRow(QCoreApplication::translate("ui::SettingsDialog", "Name"), idEdit);

  auto * commandEdit = new QLineEdit(server->command);
  commandEdit->setPlaceholderText(QCoreApplication::translate("ui::SettingsDialog",
                                                              "For example npx / python3 / /abs/path/server"));
  form->addRow(QCoreApplication::translate("ui::SettingsDialog", "Command"), commandEdit);

  auto * argsEdit = new QPlainTextEdit(server->args.join(QLatin1Char('\n')));
  argsEdit->setPlaceholderText(QCoreApplication::translate("ui::SettingsDialog",
                                                           "One argument per line, for example\n-y\n@modelcontextprotocol/server-github"));
  argsEdit->setFixedHeight(90);
  form->addRow(QCoreApplication::translate("ui::SettingsDialog", "Arguments"), argsEdit);

  auto * envEdit = new QPlainTextEdit(server->env.join(QLatin1Char('\n')));
  envEdit->setPlaceholderText(QCoreApplication::translate("ui::SettingsDialog",
                                                          "One KEY=VALUE per line; layered on top of the inherited environment"));
  envEdit->setFixedHeight(70);
  form->addRow(QCoreApplication::translate("ui::SettingsDialog", "Environment"), envEdit);

  auto * enabledCheck = new QCheckBox(QCoreApplication::translate("ui::SettingsDialog", "Enabled"));
  enabledCheck->setChecked(server->enabled);
  form->addRow(QString(), enabledCheck);

  layout->addLayout(form);

  auto * hint = new QLabel(QCoreApplication::translate("ui::SettingsDialog",
                                                       "Tool permissions are conservative by default: a tool the server does not mark read-only asks for your approval on every call.\nA server's \"read-only\" claim is only trusted in the permissive direction — it is the server's word, not a verified fact."));
  hint->setWordWrap(true);
  hint->setFont(Theme::instance().font(FontRole::UiXs));
  layout->addWidget(hint);
  layout->addStretch(1);

  auto * buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  // 名称与命令是必填：缺任何一个服务器都起不来，与其存下去再报错不如拦在这里。
  const auto validate = [&]() {
    buttons->button(QDialogButtonBox::Ok)
    ->setEnabled(!idEdit->text().trimmed().isEmpty() &&
                 !commandEdit->text().trimmed().isEmpty());
  };
  QObject::connect(idEdit, &QLineEdit::textChanged, &dialog, validate);
  QObject::connect(commandEdit, &QLineEdit::textChanged, &dialog, validate);
  validate();

  layout->addWidget(buttons);

  if(dialog.exec() != QDialog::Accepted) {
    return false;
  }

  server->id = idEdit->text().trimmed();
  server->command = commandEdit->text().trimmed();
  server->args.clear();
  for(const QString & line : argsEdit->toPlainText().split(QLatin1Char('\n'))) {
    if(!line.trimmed().isEmpty()) {
      server->args.append(line.trimmed());
    }
  }
  server->env.clear();
  for(const QString & line : envEdit->toPlainText().split(QLatin1Char('\n'))) {
    if(!line.trimmed().isEmpty()) {
      // 顺手校验格式：写错的行会被运行时忽略，用户会以为已经生效。
      if(!line.contains(QLatin1Char('='))) {
        QMessageBox::warning(&dialog, QCoreApplication::translate("ui::SettingsDialog", "Malformed environment variable"),
                             QCoreApplication::translate("ui::SettingsDialog", "This line is missing '=': %1\nIt should be written as KEY=VALUE.")
                             .arg(line.trimmed()));
        return false;
      }
      server->env.append(line.trimmed());
    }
  }
  server->enabled = enabledCheck->isChecked();
  return true;
}

}  // namespace

void SettingsDialog::pickModelsFromCatalog()
{
  QString error;
  const QList<model::CatalogProvider> providers = model::ModelCatalog::load(&error);
  if(providers.isEmpty()) {
    QMessageBox::warning(this, QCoreApplication::translate("ui::SettingsDialog", "Model catalog unavailable"),
                         error.isEmpty()
                         ? QCoreApplication::translate("ui::SettingsDialog", "The built-in model catalog is empty.")
                         : error);
    return;
  }

  // 一个扁平的候选表：`提供商 / 模型`。用 QInputDialog 而不是自建对话框——
  // 这里只需要"选一个"，为此写一个窗口不值得。
  QStringList labels;
  QList<QPair<const model::CatalogProvider *, model::CatalogModel>> lookup;
  for(const model::CatalogProvider & provider : providers) {
    for(const model::CatalogModel & catalogModel : provider.models) {
      labels.append(QStringLiteral("%1 / %2").arg(provider.name, catalogModel.id));
      lookup.append({&provider, catalogModel});
    }
  }

  bool accepted = false;
  const QString chosen = QInputDialog::getItem(
                           this, QCoreApplication::translate("ui::SettingsDialog", "Pick a model from the list"),
                           QCoreApplication::translate("ui::SettingsDialog",
                                                       "Models from the built-in catalog (selected ones are added to the current provider):"), labels, 0, false,
                           &accepted, Qt::Popup | Qt::WindowCloseButtonHint);
  if(!accepted || chosen.isEmpty()) {
    return;
  }
  const int index = labels.indexOf(chosen);
  if(index < 0) {
    return;
  }
  const auto &[provider, catalogModel] = lookup.at(index);

  // ① 模型 id 追加到列表（已存在就不重复加）。
  const QStringList existing = parseModelIds(providerModelsEdit_->toPlainText());
  if(!existing.contains(catalogModel.id)) {
    QString text = providerModelsEdit_->toPlainText().trimmed();
    if(!text.isEmpty()) {
      text += QLatin1Char('\n');
    }
    providerModelsEdit_->setPlainText(text + catalogModel.id);
  }

  // ② 顺手把 Provider 的名称、协议与地址补上——用户点"从列表选择"的意图
  //    本来就是"照目录配一个"，让他再手打 base_url 等于把便利又拿走了。
  //    只在空着的时候填，避免覆盖用户已有的配置。
  if(providerNameEdit_->text().trimmed().isEmpty()) {
    providerNameEdit_->setText(provider->name);
  }
  if(providerBaseUrlEdit_->text().trimmed().isEmpty()) {
    providerBaseUrlEdit_->setText(provider->baseUrl);
    const int kindIndex = providerKindCombo_->findData(static_cast<int>(provider->kind));
    if(kindIndex >= 0) {
      providerKindCombo_->setCurrentIndex(kindIndex);
    }
  }

  // ③ 目录里的能力写进覆盖表：否则用户选完模型还得去"模型能力"页再填一遍。
  //    providerId 用当前编辑框里的 id（目录里的名字只是展示名）。
  const QString providerId =
    (currentProvider_ >= 0 && currentProvider_ < settings_.providers.size())
    ? settings_.providers.at(currentProvider_).id
    : QString();
  const ModelOptionOverride override =
    model::ModelCatalog::overrideFor(*provider, catalogModel.id, providerId);
  if(!override.isEmpty() && !providerId.isEmpty()) {
    settings_.setModelOverride(providerId, catalogModel.id, override);
    refreshCapabilityValidation();  // 让"模型能力"页立刻反映出来
  }

  qCInfo(log) << "已从模型目录选择:" << provider->name << catalogModel.id;
}

QWidget * SettingsDialog::buildIntegrationsPage()
{
  auto * page = new QWidget;
  auto * layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  // ── MCP 服务器 ──────────────────────────────────────────────────────────
  auto * mcpTitle = new QLabel(QCoreApplication::translate("ui::SettingsDialog", "MCP servers"));
  mcpTitle->setFont(Theme::instance().font(FontRole::UiLg));
  layout->addWidget(mcpTitle);

  auto * mcpHint = new QLabel(QCoreApplication::translate("ui::SettingsDialog",
                                                          "At startup these servers are launched as child processes. The tools they provide are registered as regular tools the model can call. Reopen the app for changes to reconnect (the child processes are terminated on exit)."));
  mcpHint->setWordWrap(true);
  mcpHint->setFont(Theme::instance().font(FontRole::UiXs));
  layout->addWidget(mcpHint);

  mcpTable_ = new QTableWidget(0, 4, page);
  mcpTable_->setObjectName(QStringLiteral("mcpTable"));
  mcpTable_->setHorizontalHeaderLabels({QCoreApplication::translate("ui::SettingsDialog", "Name"), QCoreApplication::translate("ui::SettingsDialog", "Command"),
                                        QCoreApplication::translate("ui::SettingsDialog", "Arguments"), QCoreApplication::translate("ui::SettingsDialog", "Enabled")});
  mcpTable_->verticalHeader()->setVisible(false);
  mcpTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  mcpTable_->setSelectionMode(QAbstractItemView::SingleSelection);
  mcpTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // 改内容走"编辑"按钮
  mcpTable_->horizontalHeader()->setStretchLastSection(false);
  mcpTable_->setColumnWidth(0, 140);
  mcpTable_->setColumnWidth(1, 160);
  mcpTable_->setColumnWidth(2, 300);
  mcpTable_->setMinimumHeight(150);
  layout->addWidget(mcpTable_, 1);

  auto * mcpButtons = new QHBoxLayout;
  auto * addServer = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Add…"));
  addServer->setObjectName(QStringLiteral("addMcpServer"));
  connect(addServer, &QPushButton::clicked, this, [this]() {
    editMcpServer(-1);
  });
  mcpButtons->addWidget(addServer);

  auto * editServer = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Edit…"));
  editServer->setObjectName(QStringLiteral("editMcpServer"));
  connect(editServer, &QPushButton::clicked, this,
  [this]() {
    editMcpServer(mcpTable_->currentRow());
  });
  mcpButtons->addWidget(editServer);

  auto * removeServer = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Delete"));
  removeServer->setObjectName(QStringLiteral("removeMcpServer"));
  connect(removeServer, &QPushButton::clicked, this, [this]() {
    const int row = mcpTable_->currentRow();
    if(row < 0 || row >= settings_.mcpServers.size()) {
      return;
    }
    settings_.mcpServers.removeAt(row);
    refreshMcpTable();
  });
  mcpButtons->addWidget(removeServer);
  mcpButtons->addStretch(1);
  layout->addLayout(mcpButtons);

  // ── Skills 目录 ─────────────────────────────────────────────────────────
  auto * skillsTitle = new QLabel(QCoreApplication::translate("ui::SettingsDialog", "Skill directories"));
  skillsTitle->setFont(Theme::instance().font(FontRole::UiLg));
  layout->addWidget(skillsTitle);

  auto * skillsHint = new QLabel(QCoreApplication::translate("ui::SettingsDialog",
                                                             "Looks for <name>/SKILL.md or <name>.md in these directories. Empty means the default directories: the user-level <data dir>/skills and the current workspace's .lycode/skills."));
  skillsHint->setWordWrap(true);
  skillsHint->setFont(Theme::instance().font(FontRole::UiXs));
  layout->addWidget(skillsHint);

  skillDirList_ = new QListWidget(page);
  skillDirList_->setObjectName(QStringLiteral("skillDirList"));
  skillDirList_->setMinimumHeight(90);
  layout->addWidget(skillDirList_, 1);

  skillPreview_ = new QLabel;
  skillPreview_->setObjectName(QStringLiteral("skillPreview"));
  skillPreview_->setWordWrap(true);
  skillPreview_->setFont(Theme::instance().font(FontRole::UiXs));
  layout->addWidget(skillPreview_);

  auto * skillButtons = new QHBoxLayout;
  auto * addDir = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Add directory…"));
  addDir->setObjectName(QStringLiteral("addSkillDir"));
  connect(addDir, &QPushButton::clicked, this, &SettingsDialog::addSkillDirectory);
  skillButtons->addWidget(addDir);

  auto * removeDir = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Remove"));
  removeDir->setObjectName(QStringLiteral("removeSkillDir"));
  connect(removeDir, &QPushButton::clicked, this,
          &SettingsDialog::removeSelectedSkillDirectory);
  skillButtons->addWidget(removeDir);

  auto * resetDirs = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Restore default directories"));
  resetDirs->setObjectName(QStringLiteral("resetSkillDirs"));
  connect(resetDirs, &QPushButton::clicked, this, [this]() {
    settings_.skillDirectories.clear();  // 空 = 用内置默认
    refreshSkillDirectories();
  });
  skillButtons->addWidget(resetDirs);
  skillButtons->addStretch(1);
  layout->addLayout(skillButtons);

  refreshMcpTable();
  refreshSkillDirectories();
  return page;
}

void SettingsDialog::refreshMcpTable()
{
  mcpTable_->setRowCount(settings_.mcpServers.size());
  for(int row = 0; row < settings_.mcpServers.size(); ++row) {
    const mcp::ServerConfig & server = settings_.mcpServers.at(row);
    mcpTable_->setItem(row, 0, new QTableWidgetItem(server.id));
    mcpTable_->setItem(row, 1, new QTableWidgetItem(server.command));
    mcpTable_->setItem(row, 2,
                       new QTableWidgetItem(server.args.join(QLatin1Char(' '))));
    auto * enabled = new QTableWidgetItem(server.enabled ? QCoreApplication::translate("ui::SettingsDialog", "Yes")
                                          : QCoreApplication::translate("ui::SettingsDialog", "No"));
    enabled->setTextAlignment(Qt::AlignCenter);
    mcpTable_->setItem(row, 3, enabled);
  }
}

void SettingsDialog::editMcpServer(int row)
{
  const bool isNew = row < 0;
  mcp::ServerConfig server;
  if(!isNew && row < settings_.mcpServers.size()) {
    server = settings_.mcpServers.at(row);
  }
  else {
    server.enabled = true;
  }

  if(!editServerDialog(this, &server, isNew)) {
    return;
  }

  // 名称唯一：重名会导致工具名前缀撞车，注册表里互相覆盖。
  for(int index = 0; index < settings_.mcpServers.size(); ++index) {
    if(index != row && settings_.mcpServers.at(index).id == server.id) {
      QMessageBox::warning(this, QCoreApplication::translate("ui::SettingsDialog", "Duplicate name"),
                           QCoreApplication::translate("ui::SettingsDialog", "A server named \"%1\" already exists.")
                           .arg(server.id));
      refreshMcpTable();
      return;
    }
  }

  if(isNew) {
    settings_.mcpServers.append(server);
  }
  else {
    settings_.mcpServers[row] = server;
  }
  refreshMcpTable();
}

void SettingsDialog::refreshSkillDirectories()
{
  skillDirList_->clear();
  const QStringList directories = settings_.skillDirectories.isEmpty()
                                  ? skills::Library::defaultDirectories(QString())
                                  : settings_.skillDirectories;
  const bool usingDefaults = settings_.skillDirectories.isEmpty();
  for(const QString & directory : directories) {
    auto * item = new QListWidgetItem(directory);
    if(usingDefaults) {
      item->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                   "Default directories (click \"Add directory…\" to use a custom list)"));
    }
    skillDirList_->addItem(item);
  }

  // 立刻扫一遍做预览：让用户当场确认"我配的目录真的被认到了"，
  // 而不是等重启后发现一个技能都没有。
  skills::Library preview;
  preview.rescan(directories);
  if(preview.isEmpty()) {
    skillPreview_->setText(QCoreApplication::translate("ui::SettingsDialog", "No skills found."));
    return;
  }
  QStringList names;
  for(const skills::Skill & skill : preview.skills()) {
    names.append(skill.id);
  }
  skillPreview_->setText(QCoreApplication::translate("ui::SettingsDialog", "Found %1 skill(s): %2")
                         .arg(preview.skills().size())
                         .arg(names.join(QStringLiteral("、"))));
}

void SettingsDialog::addSkillDirectory()
{
  const QString directory = QFileDialog::getExistingDirectory(
                              this, QCoreApplication::translate("ui::SettingsDialog", "Select skill directory"));
  if(directory.isEmpty()) {
    return;
  }
  // 第一次添加时把默认目录也并进来：用户点"添加"的意图是"再加一个"，
  // 而不是"丢掉默认的那两个"。
  if(settings_.skillDirectories.isEmpty()) {
    settings_.skillDirectories = skills::Library::defaultDirectories(QString());
  }
  if(!settings_.skillDirectories.contains(directory)) {
    settings_.skillDirectories.append(directory);
  }
  refreshSkillDirectories();
}

void SettingsDialog::removeSelectedSkillDirectory()
{
  const int row = skillDirList_->currentRow();
  if(row < 0) {
    return;
  }
  if(settings_.skillDirectories.isEmpty()) {
    // 列表里现在显示的是默认目录。用户点"移除"的意思是"别再用它了"，
    // 所以先把默认目录展开成显式列表，再删掉选中的那个。
    settings_.skillDirectories = skills::Library::defaultDirectories(QString());
  }
  if(row < settings_.skillDirectories.size()) {
    settings_.skillDirectories.removeAt(row);
  }
  refreshSkillDirectories();
}

QWidget * SettingsDialog::buildSessionPage()
{
  auto * page = new QWidget;
  auto * layout = new QVBoxLayout(page);
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
  if(modeIndex < 0) {
    // 内部态（auto）或未知值：不写进下拉框，回退 build 并留下告警。
    qCWarning(log) << "默认会话模式是内部态或未知值，回退 build:"
                   << toToken(settings_.defaultSessionMode);
    settings_.defaultSessionMode = SessionMode::Build;
    modeIndex = sessionModeCombo_->findData(static_cast<int>(SessionMode::Build));
  }
  sessionModeCombo_->setCurrentIndex(std::max(modeIndex, 0));

  persistSessionsCheck_ = new QCheckBox(QCoreApplication::translate("ui::SettingsDialog", "Persist sessions"), page);
  persistSessionsCheck_->setObjectName(QStringLiteral("persistSessionsCheck"));
  persistSessionsCheck_->setChecked(settings_.persistSessions);

  // 这个开关值得单独给出来：模型生成标题意味着**每个新会话多一次模型调用**。
  // 不想花这次调用的用户可以关掉，标题会退回"取首条输入的前 40 字符"。
  titleGenerationCheck_ = new QCheckBox(QCoreApplication::translate("ui::SettingsDialog",
                                                                    "Generate session titles with the model"), page);
  titleGenerationCheck_->setObjectName(QStringLiteral("titleGenerationCheck"));
  titleGenerationCheck_->setChecked(settings_.generateSessionTitles);
  titleGenerationCheck_->setToolTip(
    QCoreApplication::translate("ui::SettingsDialog",
                                "When off, the title is the first 40 characters of your first input, with no extra model call"));
  persistSessionsCheck_->setToolTip(
    QCoreApplication::translate("ui::SettingsDialog",
                                "When off, new sessions would exist only in memory (this version still persists them; the switch is kept for future evolution)"));

  addFormField(layout, QCoreApplication::translate("ui::SettingsDialog", "Default session mode"), sessionModeCombo_,
               page);
  layout->addWidget(persistSessionsCheck_);
  layout->addWidget(titleGenerationCheck_);

  auto * separator = new QFrame(page);
  separator->setProperty("role", "separator");
  separator->setFrameShape(QFrame::HLine);
  layout->addWidget(separator);

  auto * captionRow = new QHBoxLayout;
  captionRow->setSpacing(8);
  captionRow->addWidget(makeLabel(QStringLiteral("formFieldCaption"),
                                  QCoreApplication::translate("ui::SettingsDialog", "Recent workspaces"), FontRole::UiSm,
                                  ColorToken::ForegroundSubtle, page));
  captionRow->addStretch(1);
  clearRecentButton_ = new QPushButton(QCoreApplication::translate("ui::SettingsDialog", "Clear the list"), page);
  clearRecentButton_->setToolTip(QCoreApplication::translate("ui::SettingsDialog",
                                                             "Clear the recent workspace list (applies when you click OK)"));
  captionRow->addWidget(clearRecentButton_);
  layout->addLayout(captionRow);

  recentList_ = new QListWidget(page);
  recentList_->setObjectName(QStringLiteral("recentWorkspaceList"));
  recentList_->setSelectionMode(QAbstractItemView::NoSelection);  // 只读列表
  recentList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  recentList_->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(recentList_, 1);

  recentEmptyLabel_ =
    makeLabel(QStringLiteral("formHint"), QCoreApplication::translate("ui::SettingsDialog", "No recent workspaces yet."),
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
  connect(titleGenerationCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    settings_.generateSessionTitles = checked;
    qCDebug(log) << "模型生成会话标题:" << checked;
  });
  connect(clearRecentButton_, &QPushButton::clicked, this,
  [this]() {
    clearRecentWorkspaces();
  });

  reloadRecentWorkspaces();
  return page;
}

void SettingsDialog::reloadRecentWorkspaces()
{
  recentList_->clear();
  for(const QString & path : std::as_const(settings_.recentWorkspaces)) {
    auto * item = new QListWidgetItem(path, recentList_);
    // 路径是技术值 → 等宽字体。
    item->setFont(Theme::instance().font(FontRole::MonoSm));
    item->setToolTip(path);
  }
  const bool empty = recentList_->count() == 0;
  recentEmptyLabel_->setVisible(empty);
  if(clearRecentButton_ != nullptr) {
    clearRecentButton_->setEnabled(!empty);
  }
}

void SettingsDialog::clearRecentWorkspaces()
{
  const int removed = settings_.recentWorkspaces.size();
  if(removed == 0) {
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

void SettingsDialog::buildButtonBox(QVBoxLayout * root)
{
  auto * box = new QDialogButtonBox(this);
  okButton_ = box->addButton(QCoreApplication::translate("ui::SettingsDialog", "OK"), QDialogButtonBox::AcceptRole);
  okButton_->setObjectName(QStringLiteral("settingsOkButton"));
  okButton_->setProperty("accent", true);  // 整个对话框只有这一个主按钮
  okButton_->setDefault(true);
  auto * cancelButton = box->addButton(QCoreApplication::translate("ui::SettingsDialog", "Cancel"),
                                       QDialogButtonBox::RejectRole);
  cancelButton->setAutoDefault(false);
  connect(box, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
  root->addWidget(box);
}

bool SettingsDialog::validateAllProviders()
{
  bool valid = true;
  for(int i = 0; i < settings_.providers.size(); ++i) {
    ProviderConfig & config = settings_.providers[i];
    config.name = config.name.trimmed();
    config.baseUrl = config.baseUrl.trimmed();
    const bool nameEmpty = config.name.isEmpty();
    const bool baseUrlEmpty = config.baseUrl.isEmpty();
    if(!nameEmpty && !baseUrlEmpty) {
      continue;
    }
    qCWarning(log) << "Provider 校验失败:" << config.id << "名称空=" << nameEmpty
                   << "BaseURL 空=" << baseUrlEmpty;
    if(valid) {
      // 选中第一个出问题的项，用户能直接看到字段下方的 destructive 提示。
      providerList_->setCurrentRow(i);
      updateProviderState();
    }
    valid = false;
  }
  return valid;
}

void SettingsDialog::logAcceptedSettings()
{
  // 只记录可审计的配置项。**绝不记录 apiKey**，只记"是否已配置"。
  qCInfo(log) << "设置已确认: themeMode=" << toToken(settings_.themeMode)
              << "uiFontSize=" << settings_.uiFontSize
              << "codeFontSize=" << settings_.codeFontSize
              << "language=" << settings_.language
              << "defaultSessionMode=" << toToken(settings_.defaultSessionMode)
              << "persistSessions=" << settings_.persistSessions
              << "providers=" << settings_.providers.size()
              << "modelOverrides=" << settings_.modelOverrides.size()
              << "recentWorkspaces=" << settings_.recentWorkspaces.size();
  for(const ProviderConfig & config : std::as_const(settings_.providers)) {
    qCInfo(log) << "  provider:" << config.id << config.name << toToken(config.kind)
                << config.baseUrl << "models=" << config.models.size()
                << "enabled=" << config.enabled
                << "apiKeyConfigured=" << !config.apiKey.isEmpty()
                << "availability=" << toToken(config.availability);
  }
}

void SettingsDialog::applyShellStyle()
{
  const Palette & palette = Theme::instance().palette();
  // 对话框外壳 16px 圆角（一级容器），嵌套容器由 10/8px 负责。
  setStyleSheet(
    QStringLiteral("QDialog#%1 { background: %2; border: 1px solid %3; border-radius: 16px; }")
    .arg(objectName(), Theme::css(palette.background), Theme::css(palette.popoverBorder)));
}

void SettingsDialog::refreshTokenStyles()
{
  applyShellStyle();

  // 1) 所有令牌化标签（含 provider 列表项内的主/副标题）。
  const QList<QLabel *> labels = findChildren<QLabel *>();
  for(QLabel * label : labels) {
    applyLabelStyle(label);
  }

  // 2) 等宽技术值输入框。
  const QList<QWidget *> widgets = findChildren<QWidget *>();
  for(QWidget * widget : widgets) {
    if(!widget->property("tokenMonoRole").isValid()) {
      continue;
    }
    const auto role = static_cast<FontRole>(widget->property("tokenMonoRole").toInt());
    const QString selector = widget->property("tokenMonoSelector").toString();
    if(!selector.isEmpty()) {
      enableMonoTypography(widget, selector, role);
    }
  }

  // 3) 列表项文本：字号变了要按新行高重新省略。
  for(int i = 0; i < providerList_->count() && i < settings_.providers.size(); ++i) {
    QWidget * widget = providerList_->itemWidget(providerList_->item(i));
    if(widget != nullptr) {
      fillProviderItemWidget(widget, settings_.providers.at(i));
    }
  }
  for(int i = 0; i < recentList_->count(); ++i) {
    recentList_->item(i)->setFont(Theme::instance().font(FontRole::MonoSm));
  }

  // 4) 模型能力表格：等宽模型列、非法档位边框、行高都吃字号令牌；
  //    网格线颜色也必须走令牌，否则深色主题下默认网格线会发白。
  if(modelCapabilityTable_ != nullptr) {
    const Palette & palette = Theme::instance().palette();
    modelCapabilityTable_->setStyleSheet(
      QStringLiteral("QTableWidget#%1 { gridline-color: %2; }")
      .arg(modelCapabilityTable_->objectName(), Theme::css(palette.border)));
    const QFont monoFont = Theme::instance().font(FontRole::Mono);
    for(int row = 0; row < modelCapabilityTable_->rowCount(); ++row) {
      if(QTableWidgetItem * item = modelCapabilityTable_->item(row, CapabilityModelColumn);
         item != nullptr) {
        item->setFont(monoFont);
      }
      auto * edit = qobject_cast<QLineEdit *>(
                      modelCapabilityTable_->cellWidget(row, CapabilityReasoningColumn));
      if(edit != nullptr) {
        applyReasoningEditStyle(edit);
      }
    }
    applyCapabilityTableHeight();
  }
}

}  // namespace lycode::ui
