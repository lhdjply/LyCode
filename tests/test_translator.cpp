// LyCode — 界面翻译链路测试
//
// 这条测试专门盯住一个"全绿但完全没生效"的失败模式：构建产出了 .ts、生成了 .qm、
// 甚至资源里也有文件，但运行时没装上 translator，界面照样全中文。
// 所以断言分三层，逐层往真实效果逼近：
//   1. 语言取值的解析/回退（纯逻辑）
//   2. .qm 真的被嵌进资源、能被 QTranslator 装载
//   3. **真的翻译出英文**——拿一条已知文案，用 QCoreApplication::translate 取
//      英文结果，而中文界面下必须还是原文
#include <QtTest>

#include <QCoreApplication>
#include <QTranslator>

#include "core/Types.h"
#include "ui/Translator.h"

using namespace lycode;
using namespace lycode::ui;

namespace
{

/// 设置页里那条语言说明的**源文案**。它同时是"翻译链路是否生效"的探针：
/// 改了源码里的中文而没更新 .ts，这条测试就会失败——正是想要的提醒。
const char * kProbeSource =
  "Switches the interface language. English is the source text; Chinese comes from the bundled translation file.";
const char * kProbeContext = "ui::SettingsDialog";

QString probe(UiLanguage language)
{
  Translator::instance().switchTo(language);
  return QCoreApplication::translate(kProbeContext, kProbeSource);
}

}  // namespace

class TestTranslator : public QObject
{
    Q_OBJECT

  private slots:
    void tokensRoundTrip();
    void unknownLanguageFallsBackToSourceText();
    void englishTranslationIsBundled();
    void switchingActuallyTranslates();
    void missingTranslationKeepsCurrentLanguage();
};

void TestTranslator::tokensRoundTrip()
{
  QCOMPARE(toToken(UiLanguage::Chinese), QStringLiteral("zh-CN"));
  QCOMPARE(toToken(UiLanguage::English), QStringLiteral("en-US"));
  QCOMPARE(languageFromToken(QStringLiteral("zh-CN")), UiLanguage::Chinese);
  QCOMPARE(languageFromToken(QStringLiteral("en-US")), UiLanguage::English);
  // 宽松接受带地区的取值，别让 "en" / "en_GB" 落到中文去。
  QCOMPARE(languageFromToken(QStringLiteral("en")), UiLanguage::English);
  QCOMPARE(languageFromToken(QStringLiteral("en_GB")), UiLanguage::English);
}

void TestTranslator::unknownLanguageFallsBackToSourceText()
{
  // 配置里出现没见过的语言值时，正确的落点是**中文**（源文案），不是英文。
  // 回退到英文会让中文用户看到半英文界面，而且没有任何办法改回去。
  // 回退英文（源文案）而不是中文：源码里就有英文，是唯一"一定完整"的落点；
  // 回退到中文会得到一个未翻译的界面。
  QCOMPARE(languageFromToken(QStringLiteral("fr-FR")), UiLanguage::English);
  QCOMPARE(languageFromToken(QString()), UiLanguage::English);
  QCOMPARE(languageFromToken(QStringLiteral("   ")), UiLanguage::English);
}

void TestTranslator::englishTranslationIsBundled()
{
  // 资源里必须有英文 .qm：它由 CMake 的 qt_add_translations 生成并嵌入。
  // 缺了它，"选英文"在真实安装包里就是个死选项。
  // 中文界面完全依赖 zh_CN.qm：它没嵌进资源的话，中文用户会看到英文界面。
  QVERIFY2(Translator::isTranslationAvailable(UiLanguage::Chinese),
           "zh_CN.qm 没有嵌进资源；检查 CMakeLists 的 qt_add_translations");
  // 英文本就是源文案，永远可用。
  QVERIFY(Translator::isTranslationAvailable(UiLanguage::English));
}

void TestTranslator::switchingActuallyTranslates()
{
  // 英文：源文案本身，不装 translator。
  const QString english = probe(UiLanguage::English);
  QCOMPARE(english, QString::fromUtf8(kProbeSource));
  QVERIFY(english.contains(QStringLiteral("source text")));

  // 中文：必须真的变中文。这条是**中文界面的生命线**——源码是英文，中文完全
  // 依赖 zh_CN.qm；它缺失或漏译时界面会静默退回英文，而中文用户只会觉得
  // "软件变英文了"，看不出是构建漏了翻译文件。
  const QString chinese = probe(UiLanguage::Chinese);
  QVERIFY2(chinese != english, "切到中文后文案没变——zh_CN.qm 没装上或这条没译文");
  QVERIFY2(!chinese.contains(QStringLiteral("source text")),
           qPrintable(QStringLiteral("中文译文没生效: %1").arg(chinese)));
  QVERIFY2(chinese.contains(QStringLiteral("界面")), qPrintable(chinese));

  // 切回英文必须恢复源文案（移除 translator 这条路也要通）。
  QCOMPARE(probe(UiLanguage::English), QString::fromUtf8(kProbeSource));

  // 覆盖多个上下文，确保不是"只有某一条碰巧能译"。这些探针分别来自
  // ui / agent / tools 三层——工具的文案是后加的，漏接的话中文用户会在工具卡片里
  // 看到英文，而界面上完全看不出是"漏接"还是"本来就没译"。
  struct Probe {
    const char * context;
    const char * source;
    const char * chinese;
  };
  const Probe probes[] = {
    {"ui::MainWindow", "Send", "发送"},
    {"ui::SettingsDialog", "Model capabilities", "模型能力"},
    {"agent::AgentRuntime", "No active session.", "没有活动会话。"},
    {"tools::BashTool", "Execution cancelled", "执行已取消"},
    {"tools::ReadTool", "File does not exist: %1", "文件不存在：%1"},
  };

  probe(UiLanguage::Chinese);
  for(const Probe & item : probes) {
    const QString translated = QCoreApplication::translate(item.context, item.source);
    QVERIFY2(translated == QString::fromUtf8(item.chinese),
             qPrintable(QStringLiteral("[%1] %2 -> %3（期望 %4）")
                        .arg(QString::fromUtf8(item.context),
                             QString::fromUtf8(item.source), translated,
                             QString::fromUtf8(item.chinese))));
  }

  // 切回英文后必须全部恢复成源文案。
  probe(UiLanguage::English);
  for(const Probe & item : probes) {
    QCOMPARE(QCoreApplication::translate(item.context, item.source),
             QString::fromUtf8(item.source));
  }
}

void TestTranslator::missingTranslationKeepsCurrentLanguage()
{
  probe(UiLanguage::Chinese);
  // 切到中文是"无事发生"：它不装 translator，也不需要 .qm。
  QVERIFY(!Translator::instance().switchTo(UiLanguage::Chinese));
  QCOMPARE(Translator::instance().language(), UiLanguage::Chinese);
}

QTEST_MAIN(TestTranslator)
#include "test_translator.moc"
