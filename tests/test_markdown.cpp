// ZCode Qt — Markdown 渲染单元测试
//
// 渲染器直接产出 HTML，任何一处转义遗漏都是安全问题（模型输出不可信），
// 任何一处规则顺序错误都会破坏代码块的显示。因此两个方向都要覆盖。
#include <QtTest>

#include "ui/Markdown.h"

using namespace zcode::ui;

namespace {

MarkdownStyle testStyle() {
    MarkdownStyle style;
    style.foreground = QColor(QStringLiteral("#262626"));
    style.foregroundSubtle = QColor(QStringLiteral("#737373"));
    style.foregroundSubtlest = QColor(QStringLiteral("#a3a3a3"));
    style.surface = QColor(QStringLiteral("#f5f5f5"));
    style.border = QColor(QStringLiteral("#e5e5e5"));
    style.codeBackground = QColor(QStringLiteral("#f5f5f5"));
    style.link = QColor(QStringLiteral("#001d3d"));
    style.quoteBar = QColor(QStringLiteral("#d4d4d4"));
    style.tableHeader = QColor(QStringLiteral("#f5f5f5"));
    style.baseFontPx = 14;
    style.codeFontPx = 14;
    style.sansFamily = QStringLiteral("Noto Sans CJK SC");
    style.monoFamily = QStringLiteral("Noto Sans Mono CJK SC");
    return style;
}

}  // namespace

class TestMarkdown : public QObject {
    Q_OBJECT

private slots:
    void escapesHtmlInText();
    void escapesHtmlInsideCodeBlock();
    void rejectsUnsafeLinkSchemes();
    void keepsSafeLinks();
    void rendersHeadings();
    void rendersFencedCodeBlockWithLanguage();
    void rendersIndentedFenceWithTildes();
    void inlineCodeSurvivesEmphasisRules();
    void rendersEmphasis();
    void rendersLists();
    void rendersTaskList();
    void rendersBlockquote();
    void rendersHorizontalRule();
    void rendersPipeTable();
    void rendersNestedList();
    void styleSheetHasNoPlaceholders();
    void plainPreviewStripsMarkup();
};

void TestMarkdown::escapesHtmlInText() {
    // 模型输出不可信：标签必须被转义而不是直通。
    const QString html = Markdown::toHtml(
        QStringLiteral("hello <script>alert(1)</script> & \"quoted\""), testStyle());

    QVERIFY(!html.contains(QStringLiteral("<script>")));
    QVERIFY(html.contains(QStringLiteral("&lt;script&gt;")));
    QVERIFY(html.contains(QStringLiteral("&amp;")));
    QVERIFY(html.contains(QStringLiteral("&quot;")));
}

void TestMarkdown::escapesHtmlInsideCodeBlock() {
    const QString html =
        Markdown::toHtml(QStringLiteral("```html\n<b>bold</b>\n```"), testStyle());

    QVERIFY(html.contains(QStringLiteral("code-block")));
    QVERIFY(html.contains(QStringLiteral("&lt;b&gt;bold&lt;/b&gt;")));
    QVERIFY(!html.contains(QStringLiteral("<b>bold</b>")));
}

void TestMarkdown::rejectsUnsafeLinkSchemes() {
    // javascript: 链接必须降级为纯文本。
    const QString html =
        Markdown::toHtml(QStringLiteral("[click](javascript:alert(1))"), testStyle());

    QVERIFY(!html.contains(QStringLiteral("javascript:")));
    QVERIFY(html.contains(QStringLiteral("click")));
}

void TestMarkdown::keepsSafeLinks() {
    const QString html =
        Markdown::toHtml(QStringLiteral("[docs](https://example.com/a)"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<a href=\"https://example.com/a\">docs</a>")));
}

void TestMarkdown::rendersHeadings() {
    const QString html = Markdown::toHtml(
        QStringLiteral("# Title\n\n## Sub\n\n### Third\n"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<h1>Title</h1>")));
    QVERIFY(html.contains(QStringLiteral("<h2>Sub</h2>")));
    QVERIFY(html.contains(QStringLiteral("<h3>Third</h3>")));
}

void TestMarkdown::rendersFencedCodeBlockWithLanguage() {
    const QString html = Markdown::toHtml(
        QStringLiteral("before\n\n```cpp\nint main() {}\n```\n\nafter"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<div class=\"code-lang\">cpp</div>")));
    QVERIFY(html.contains(QStringLiteral("<pre class=\"code\">int main() {}")));
    // 代码块前后必须是独立段落，不能被并进代码块。
    QVERIFY(html.contains(QStringLiteral("<p>before</p>")));
    QVERIFY(html.contains(QStringLiteral("<p>after</p>")));
}

void TestMarkdown::rendersIndentedFenceWithTildes() {
    const QString html =
        Markdown::toHtml(QStringLiteral("  ~~~python\nprint(1)\n  ~~~\n"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<div class=\"code-lang\">python</div>")));
    QVERIFY(html.contains(QStringLiteral("print(1)")));
}

void TestMarkdown::inlineCodeSurvivesEmphasisRules() {
    // 行内代码里的 * 不能被当成强调标记——这是规则顺序的经典陷阱。
    const QString html = Markdown::toHtml(
        QStringLiteral("use `a * b * c` here"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<code class=\"inline\">a * b * c</code>")));
    QVERIFY(!html.contains(QStringLiteral("<i>")));
}

void TestMarkdown::rendersEmphasis() {
    const QString html = Markdown::toHtml(
        QStringLiteral("**bold** and *italic* and ~~gone~~ and __also bold__"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<b>bold</b>")));
    QVERIFY(html.contains(QStringLiteral("<i>italic</i>")));
    QVERIFY(html.contains(QStringLiteral("<s>gone</s>")));
    QVERIFY(html.contains(QStringLiteral("<b>also bold</b>")));
}

void TestMarkdown::rendersLists() {
    const QString html = Markdown::toHtml(
        QStringLiteral("- one\n- two\n- three\n\n1. first\n2. second\n"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<ul>")));
    QVERIFY(html.contains(QStringLiteral("<li>one</li>")));
    QVERIFY(html.contains(QStringLiteral("<ol>")));
    QVERIFY(html.contains(QStringLiteral("<li>first</li>")));
}

void TestMarkdown::rendersTaskList() {
    const QString html =
        Markdown::toHtml(QStringLiteral("- [x] done\n- [ ] todo\n"), testStyle());

    QVERIFY(html.contains(QStringLiteral("\u2611 done")));
    QVERIFY(html.contains(QStringLiteral("\u2610 todo")));
}

void TestMarkdown::rendersBlockquote() {
    const QString html = Markdown::toHtml(QStringLiteral("> quoted line\n"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<blockquote>")));
    QVERIFY(html.contains(QStringLiteral("quoted line")));
    QVERIFY(html.contains(QStringLiteral("</blockquote>")));
}

void TestMarkdown::rendersHorizontalRule() {
    const QString html = Markdown::toHtml(QStringLiteral("a\n\n---\n\nb"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<hr/>")));
}

void TestMarkdown::rendersPipeTable() {
    const QString markdown = QStringLiteral(
        "| name | value |\n"
        "| --- | --- |\n"
        "| alpha | 1 |\n"
        "| beta | 2 |\n"
        "\n"
        "after");

    const QString html = Markdown::toHtml(markdown, testStyle());

    QVERIFY(html.contains(QStringLiteral("<table class=\"md-table\">")));
    QVERIFY(html.contains(QStringLiteral("<th>name</th>")));
    QVERIFY(html.contains(QStringLiteral("<td>alpha</td>")));
    QVERIFY(html.contains(QStringLiteral("<td>2</td>")));
    // 表格之后的内容不能被吞进表格。
    QVERIFY(html.contains(QStringLiteral("<p>after</p>")));
}

void TestMarkdown::rendersNestedList() {
    const QString html =
        Markdown::toHtml(QStringLiteral("- outer\n  - inner\n"), testStyle());

    QVERIFY(html.contains(QStringLiteral("<li>outer</li>")));
    QVERIFY(html.contains(QStringLiteral("<li class=\"nested\">inner</li>")));
}

void TestMarkdown::styleSheetHasNoPlaceholders() {
    const QString css = Markdown::styleSheet(testStyle());

    QVERIFY(!css.isEmpty());
    // .arg() 链漏掉一个就会留下 %1 之类的痕迹，必须显式检查。
    QVERIFY(!css.contains(QStringLiteral("%1")));
    QVERIFY(!css.contains(QStringLiteral("%2")));
    QVERIFY(!css.contains(QStringLiteral("var(--")));
    // 关键选择器必须在场，否则 class 规则不生效。
    QVERIFY(css.contains(QStringLiteral("pre.code")));
    QVERIFY(css.contains(QStringLiteral("code.inline")));
    QVERIFY(css.contains(QStringLiteral("blockquote")));
    QVERIFY(css.contains(QStringLiteral("table.md-table")));
    // 字号阶梯：h1 应比正文大 4px，h2 大 2px（与设计规范一致）。
    QVERIFY(css.contains(QStringLiteral("font-size: 18px")));
    QVERIFY(css.contains(QStringLiteral("font-size: 16px")));
}

void TestMarkdown::plainPreviewStripsMarkup() {
    const QString markdown = QStringLiteral(
        "# Heading\n\nSome **bold** and `code` and [link](https://x.y).\n\n"
        "```cpp\nint x;\n```\n");

    const QString preview = Markdown::toPlainPreview(markdown, 120);

    QVERIFY(!preview.contains(QLatin1Char('#')));
    QVERIFY(!preview.contains(QStringLiteral("**")));
    QVERIFY(!preview.contains(QLatin1Char('`')));
    QVERIFY(!preview.contains(QStringLiteral("int x;")));
    QVERIFY(preview.contains(QStringLiteral("Heading")));
    QVERIFY(preview.contains(QStringLiteral("bold")));
    QVERIFY(preview.contains(QStringLiteral("link")));

    // 超长时必须截断并带省略标记。
    const QString longPreview =
        Markdown::toPlainPreview(QString(500, QLatin1Char('a')), 40);
    QVERIFY(longPreview.size() <= 41);
    QVERIFY(longPreview.endsWith(QStringLiteral("…")));
}

QTEST_MAIN(TestMarkdown)
#include "test_markdown.moc"
