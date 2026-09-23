// LyCode — 语法高亮测试
//
// 分词器是手写的，最容易错的是**规则优先级**：
//   * 字符串/注释里的关键字不能着色（`"for"`、`// for`）
//   * 跨行未闭合的引号不能把后面整段代码吞掉
//   * 预处理指令只在行首生效
// 另外内容完整性是底线：高亮只加 span，绝不丢字符。
#include <QApplication>
#include <QRegularExpression>
#include <QDir>
#include <QSet>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/Markdown.h"
#include "ui/SyntaxHighlighter.h"
#include "ui/Theme.h"

using namespace lycode::ui;

class TestSyntaxHighlighter : public QObject
{
    Q_OBJECT

  private slots:
    void stringsAndCommentsBeatKeywords();
    void escapesHtmlAndKeepsEveryCharacter();
    void handlesPythonTripleQuotes();
    void preprocessorOnlyAtLineStart();
    void unknownLanguageFallsBackGracefully();
    void rendersHighlightedCodeBlock();
};

/// 测试用的 MarkdownStyle：直接从主题调色板取，保证与产品一致。
static MarkdownStyle markdownStyleForTest()
{
  const Theme & theme = Theme::instance();
  const Palette & palette = theme.palette();
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
  style.syntaxKeyword = palette.syntaxKeyword;
  style.syntaxString = palette.syntaxString;
  style.syntaxComment = palette.syntaxComment;
  style.syntaxNumber = palette.syntaxNumber;
  style.syntaxType = palette.syntaxType;
  style.syntaxFunction = palette.syntaxFunction;
  style.syntaxPreproc = palette.syntaxPreproc;
  style.baseFontPx = theme.fontPixelSize(FontRole::UiBase);
  style.codeFontPx = theme.fontPixelSize(FontRole::Mono);
  style.sansFamily = theme.font(FontRole::UiBase).family();
  style.monoFamily = theme.font(FontRole::Mono).family();
  return style;
}

/// 某个 token 是否被产出了指定文本。
/// 对 span 上有没有 style 属性不敏感——颜色由调用方决定，
/// 分词测试只关心"分对了类"。
static bool hasToken(const QString & html, const QString & cssClass, const QString & text)
{
  // 转义必须与高亮器一致：它只转义 & < >，**不**转义引号
  //（文本节点里的引号无需转义）。用 toHtmlEscaped() 会把 " 变成 &quot;，
  //于是永远匹配不上。
  QString expected = text;
  expected.replace(QLatin1Char('&'), QLatin1String("&amp;"));
  expected.replace(QLatin1Char('<'), QLatin1String("&lt;"));
  expected.replace(QLatin1Char('>'), QLatin1String("&gt;"));

  const QRegularExpression pattern(QStringLiteral("<span class=\"%1\"[^>]*>%2</span>")
                                   .arg(QRegularExpression::escape(cssClass),
                                        QRegularExpression::escape(expected)));
  return html.contains(pattern);
}

/// 去掉所有 span 标签，用于验证"内容一个字符都没丢"。
static QString stripTags(const QString & html)
{
  QString text = html;
  text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
  text.replace(QLatin1String("&lt;"), QLatin1String("<"));
  text.replace(QLatin1String("&gt;"), QLatin1String(">"));
  text.replace(QLatin1String("&amp;"), QLatin1String("&"));
  return text;
}

void TestSyntaxHighlighter::stringsAndCommentsBeatKeywords()
{
  // 这是最容易做错的地方：先认关键字的话，字符串和注释里的 for/if 会被
  // 染成关键字色，看起来像是"注释里有关键字"。
  const QString html = SyntaxHighlighter::highlight(
                         QStringLiteral("for (;;) { s = \"for while if\"; } // for loop"), QStringLiteral("cpp"));

  // 真正的关键字被着色（for 出现两次都是关键字，加上 if 一次）。
  QVERIFY2(hasToken(html, QStringLiteral("tok-keyword"), QStringLiteral("for")),
           qPrintable(html));
  // 字符串整体着色，里面的 for/while/if 不再单独着色。
  QVERIFY2(hasToken(html, QStringLiteral("tok-string"), QStringLiteral("\"for while if\"")),
           qPrintable(html));
  // 注释整体着色。
  QVERIFY2(hasToken(html, QStringLiteral("tok-comment"), QStringLiteral("// for loop")),
           qPrintable(html));
  // 字符串断言里的关键字必须没有独立的 span。
  QVERIFY2(!hasToken(html, QStringLiteral("tok-keyword"), QStringLiteral("while")),
           "字符串里的 while 不该被当成关键字");
}

void TestSyntaxHighlighter::escapesHtmlAndKeepsEveryCharacter()
{
  const QString code = QStringLiteral(
                         "#include <cstdio>\nint x = a < b && c > d;\nconst char *s = \"<script>\";");
  const QString html = SyntaxHighlighter::highlight(code, QStringLiteral("cpp"));

  QVERIFY2(!html.contains(QStringLiteral("<script>")), "文件内容里的标签必须被转义");
  QVERIFY(html.contains(QStringLiteral("&lt;cstdio&gt;")));

  // 底线：去掉 span 并反转义之后，必须与原文本**完全一致**。
  QCOMPARE(stripTags(html), code);
}

void TestSyntaxHighlighter::handlesPythonTripleQuotes()
{
  const QString code = QStringLiteral("def f():\n    \"\"\"doc for x\"\"\"\n    return 1");
  const QString html = SyntaxHighlighter::highlight(code, QStringLiteral("python"));

  QVERIFY2(hasToken(html, QStringLiteral("tok-string"), QStringLiteral("\"\"\"doc for x\"\"\"")),
           qPrintable(html));
  QVERIFY2(hasToken(html, QStringLiteral("tok-keyword"), QStringLiteral("def")),
           qPrintable(html));
  QCOMPARE(stripTags(html), code);

  // 未闭合的引号不能吞掉后面整段：单引号字符串不跨行。
  const QString broken = QStringLiteral("x = \"unterminated\nfor i in y:\n    pass");
  const QString brokenHtml = SyntaxHighlighter::highlight(broken, QStringLiteral("python"));
  QVERIFY2(hasToken(brokenHtml, QStringLiteral("tok-keyword"), QStringLiteral("for")),
           "漏了引号之后，后面的代码不该整段被染成字符串");
  QCOMPARE(stripTags(brokenHtml), broken);
}

void TestSyntaxHighlighter::preprocessorOnlyAtLineStart()
{
  const QString html = SyntaxHighlighter::highlight(
                         QStringLiteral("#include <string>\nint a = b # c;"), QStringLiteral("cpp"));
  QVERIFY2(hasToken(html, QStringLiteral("tok-preproc"), QStringLiteral("#include <string>")),
           qPrintable(html));
  // 行中间出现的 # 是运算/标记，不是预处理指令。
  QVERIFY2(!html.contains(QStringLiteral("tok-preproc\"># c")), "行中的 # 不该当成预处理");
}

void TestSyntaxHighlighter::unknownLanguageFallsBackGracefully()
{
  // 未知语言走 C 系兜底：能认出字符串与 `//` 注释。
  const QString html = SyntaxHighlighter::highlight(
                         QStringLiteral("if x: // note\n    s = \"hi\""), QStringLiteral("brainfuck"));
  QVERIFY2(html.contains(QStringLiteral("tok-string")), "未知语言也要能认出字符串");
  QVERIFY2(html.contains(QStringLiteral("tok-comment")), "未知语言也要能认出 C 系注释");
  QCOMPARE(stripTags(html), QStringLiteral("if x: // note\n    s = \"hi\""));

  // 刻意的取舍：兜底规则**不**把 `#` 当注释。未知语言里 `#` 也可能是
  // 运算符或标记，误判会让一整行被染成注释色，比不着色更糟。
  const QString hashLine =
    SyntaxHighlighter::highlight(QStringLiteral("a = b # c"), QStringLiteral("brainfuck"));
  QVERIFY2(!hashLine.contains(QStringLiteral("tok-comment")),
           "兜底规则不该把 # 当注释");

  // 空输入不产生任何标签。
  QVERIFY(SyntaxHighlighter::highlight(QString(), QStringLiteral("cpp")).isEmpty());
  // 语言别名归一化。
  QCOMPARE(SyntaxHighlighter::normalizeLanguage(QStringLiteral("C++")), QStringLiteral("cpp"));
  QCOMPARE(SyntaxHighlighter::normalizeLanguage(QStringLiteral("ts")),
           QStringLiteral("typescript"));
  QCOMPARE(SyntaxHighlighter::normalizeLanguage(QStringLiteral("cpp title=\"x\"")),
           QStringLiteral("cpp"));
  QVERIFY(SyntaxHighlighter::isKnownLanguage(QStringLiteral("py")));
  QVERIFY(!SyntaxHighlighter::isKnownLanguage(QStringLiteral("cobol")));
}

void TestSyntaxHighlighter::rendersHighlightedCodeBlock()
{
  // 截图：代码块在真实主题下的观感（深浅两套配色都靠 CSS 令牌，
  // 这里只验证浅色，深色由 ui-screenshots 的人工检查覆盖）。
  const QString code = QStringLiteral(
                         "// 计算阶乘\n"
                         "static int factorial(int n) {\n"
                         "    if (n <= 1) return 1;\n"
                         "    const char *note = \"recursive\";\n"
                         "    return n * factorial(n - 1);\n"
                         "}");

  QWidget window;
  window.setStyleSheet(Theme::instance().styleSheet());
  auto * layout = new QVBoxLayout(&window);
  layout->setContentsMargins(14, 14, 14, 14);
  // 必须用 QTextBrowser 并设置**文档样式表**：控件样式表
  // （setStyleSheet）不会作用到富文本内容上，token span 会一个都不上色。
  auto * view = new QTextBrowser(&window);
  view->setFrameShape(QFrame::NoFrame);
  view->setFont(Theme::instance().font(FontRole::Mono));
  const MarkdownStyle style = markdownStyleForTest();
  view->document()->setDefaultStyleSheet(Markdown::styleSheet(style));
  SyntaxColors tokenColors;
  tokenColors.keyword = style.syntaxKeyword;
  tokenColors.string = style.syntaxString;
  tokenColors.comment = style.syntaxComment;
  tokenColors.number = style.syntaxNumber;
  tokenColors.type = style.syntaxType;
  tokenColors.function = style.syntaxFunction;
  tokenColors.preprocessor = style.syntaxPreproc;
  view->setHtml(QStringLiteral("<pre class=\"code\">%1</pre>")
                .arg(SyntaxHighlighter::highlight(code, QStringLiteral("cpp"), tokenColors)));
  layout->addWidget(view);
  window.resize(520, 160);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  // 用**文档片段的前景色**来断言着色是否真的生效，而不是靠肉眼看截图：
  // 截图看不出"两种颜色差一点点"，而少上一色是很容易发生的回归。
  QSet<QString> colors;
  for(QTextBlock block = view->document()->begin(); block.isValid(); block = block.next()) {
    for(QTextBlock::iterator fragment = block.begin(); !fragment.atEnd(); ++fragment) {
      const QTextFragment piece = fragment.fragment();
      if(piece.isValid() && !piece.text().trimmed().isEmpty()) {
        colors.insert(piece.charFormat().foreground().color().name());
      }
    }
  }
  QVERIFY2(colors.size() >= 3,
           qPrintable(QStringLiteral("代码块必须出现多种前景色，实际只有 %1 种：%2")
                      .arg(colors.size())
                      .arg(QStringList(colors.begin(), colors.end())
                           .join(QStringLiteral(", ")))));

  const QString dir = QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("ui-screenshots"));
  QDir().mkpath(dir);
  const QString dark = dir + QStringLiteral("/15-syntax-highlight.png");
  QVERIFY2(window.grab().save(dark), qPrintable(dark));
}

QTEST_MAIN(TestSyntaxHighlighter)

#include "test_syntax_highlighter.moc"
