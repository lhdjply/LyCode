// LyCode — 界面翻译（实现）
//
// 资源路径约定：:/i18n/lycode_zh_CN.qm —— 由 CMakeLists 的 qt_add_translations
// 生成并嵌入（RESOURCE_PREFIX "/i18n"）。这里刻意只认这一条路径，
// 不做"多个候选目录依次尝试"的模糊加载：装没装上应当是确定的，不是碰运气。
//
// 源语言是英文，所以**英文界面不装 translator**；中文界面必须装上 zh_CN，
// 装不上就保持英文——宁可显示英文（可读），也不要显示未翻译的中间态。
#include "ui/Translator.h"

#include <QCoreApplication>
#include <QFile>
#include <QLocale>
#include <QLoggingCategory>
#include <QTranslator>

namespace lycode::ui
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.ui.i18n")

/// 当前 QApplication 上是否真的挂着一个 .qm（用一条已知文案实测）。
///
/// 只信 translator_ 指针不够：指针非空也可能已被 removeTranslator 摘掉。
/// 取一条**必然存在于 zh_CN.qm** 的文案来试——探针若与源码文案不一致会立刻
/// 在测试里暴露（见 tests/test_translator.cpp），比读内部状态可靠。
bool englishProbeIsTranslated()
{
  static const char * kProbeContext = "ui::MainWindow";
  static const char * kProbeSource = "Send";
  return QCoreApplication::translate(kProbeContext, kProbeSource)
         != QString::fromUtf8(kProbeSource);
}

/// 资源里 .qm 的路径。与 CMakeLists 的 RESOURCE_PREFIX + 语言取值对应。
QString translationResourcePath(UiLanguage language)
{
  return QStringLiteral(":/i18n/lycode_%1.qm").arg(toToken(language).replace(
                                                     QLatin1Char('-'), QLatin1Char('_')));
}

}  // namespace

QString toToken(UiLanguage language)
{
  switch(language) {
    case UiLanguage::Chinese:
      return QStringLiteral("zh-CN");
    case UiLanguage::English:
      return QStringLiteral("en-US");
  }
  return QStringLiteral("en-US");
}

UiLanguage languageFromToken(const QString & token)
{
  const QString normalized = token.trimmed().toLower();
  if(normalized.startsWith(QStringLiteral("zh"))) {
    return UiLanguage::Chinese;
  }
  // 未知值回退英文：源文案就在源码里，是唯一"一定完整"的落点。
  return UiLanguage::English;
}

UiLanguage systemLanguage()
{
  const QString name = QLocale::system().name().toLower();
  return name.startsWith(QStringLiteral("zh")) ? UiLanguage::Chinese : UiLanguage::English;
}

bool Translator::isTranslationAvailable(UiLanguage language)
{
  if(language == UiLanguage::English) {
    return true;   // 源文案，天然可用
  }
  // QFile 认 Qt 资源路径，用它做存在性判定不必真的构造 translator。
  return QFile::exists(translationResourcePath(language));
}

Translator & Translator::instance()
{
  static Translator translator;
  return translator;
}

Translator::Translator() = default;

Translator::~Translator()
{
  // QApplication 可能已经先析构（静态析构顺序），此时 installTranslator 不可用。
  if(translator_ != nullptr && qApp != nullptr) {
    qApp->removeTranslator(translator_);
  }
  delete translator_;
}

bool Translator::switchTo(UiLanguage language)
{
  if(qApp == nullptr) {
    qCWarning(log) << "QApplication 尚未建立，无法切换语言";
    return false;
  }
  // 早退必须同时确认"状态与实际装载一致"。
  //
  // ⚠ 只比 language_ 是不够的：存在 language_ 已经是目标语言、但 translator 并没有
  // 真的装上的窗口（例如上一次 load 失败后状态被别的路径改过）。那时早退会让界面
  // 停在英文，而语言字段却写着中文——设置页显示中文、界面是英文，最难查的一类问题。
  // 用 qApp->translate() 实测一下：装了翻译时同一源文案应当能取到不同结果。
  const bool probeTranslated = englishProbeIsTranslated();
  if(language == language_ && probeTranslated == (language != UiLanguage::English)) {
    return false;
  }

  // 先摘掉旧的：装两个同族 translator 时，谁先命中不确定。
  if(translator_ != nullptr) {
    qApp->removeTranslator(translator_);
    delete translator_;
    translator_ = nullptr;
  }

  if(language == UiLanguage::English) {
    // 源文案即英文，不装 translator 就是英文界面。
    language_ = language;
    qCInfo(log) << "界面语言 -> en-US（源文案）";
    return true;
  }

  auto * loaded = new QTranslator;
  if(!loaded->load(translationResourcePath(language))) {
    // 关键：**不**把语言状态改掉。改成"选了中文但界面还是英文"会让用户以为设置
    // 坏了，而且下次启动还会静默沿用这个假状态。
    qCWarning(log) << "中文翻译文件缺失，语言未切换:" << translationResourcePath(language);
    delete loaded;
    return false;
  }

  qApp->installTranslator(loaded);
  translator_ = loaded;
  language_ = language;
  qCInfo(log) << "界面语言 ->" << toToken(language);
  return true;
}

}  // namespace lycode::ui
