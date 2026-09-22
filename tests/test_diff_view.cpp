// LyCode — Diff 视图测试
//
// 关注三件容易出错的事：
//   1. 行/增删统计是否准确（卡片头部的 "+N −M" 直接来自它）
//   2. 文件内容里的 HTML 是否被转义（源码里的 `<` 会被当成标签吃掉）
//   3. 超大补丁是否被截断并如实告知剩余行数
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include "tools/Diff.h"
#include "ui/DiffView.h"
#include "ui/Theme.h"

using namespace lycode;
using namespace lycode::ui;

namespace {

/// 构造一个 hunk。
QJsonObject makeHunk(int oldStart, int newStart, const QJsonArray &lines) {
    QJsonObject hunk;
    hunk.insert(QStringLiteral("oldStart"), oldStart);
    hunk.insert(QStringLiteral("oldLines"), 3);
    hunk.insert(QStringLiteral("newStart"), newStart);
    hunk.insert(QStringLiteral("newLines"), 4);
    hunk.insert(QStringLiteral("lines"), lines);
    return hunk;
}

QJsonObject makeLine(const QString &type, const QString &text) {
    QJsonObject line;
    line.insert(QStringLiteral("type"), type);
    line.insert(QStringLiteral("text"), text);
    return line;
}

}  // namespace

class TestDiffView : public QObject {
    Q_OBJECT

private slots:
    void countsAdditionsAndDeletions();
    void escapesHtmlFromFileContent();
    void truncatesHugePatches();
    void rendersRealDiffFromTool();

private:
    static QString screenshotDir();
};

QString TestDiffView::screenshotDir() {
    const QString path = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("ui-screenshots"));
    QDir().mkpath(path);
    return path;
}

void TestDiffView::countsAdditionsAndDeletions() {
    QJsonArray lines;
    lines.append(makeLine(QStringLiteral("context"), QStringLiteral("keep")));
    lines.append(makeLine(QStringLiteral("remove"), QStringLiteral("old-a")));
    lines.append(makeLine(QStringLiteral("remove"), QStringLiteral("old-b")));
    lines.append(makeLine(QStringLiteral("add"), QStringLiteral("new-a")));
    lines.append(makeLine(QStringLiteral("context"), QStringLiteral("tail")));

    DiffView view;
    view.setHunks(QJsonArray{makeHunk(1, 1, lines)});

    QCOMPARE(view.additions(), 1);   // 只有 "new-a" 是新增
    QCOMPARE(view.deletions(), 2);   // "old-a" 与 "old-b"
    QCOMPARE(view.renderedLines(), 5);
    QCOMPARE(view.truncatedLines(), 0);
    QVERIFY(!view.isEmpty());

    // 空补丁必须回到"没有内容"的状态，否则卡片会留一块空白。
    view.setHunks(QJsonArray{});
    QVERIFY(view.isEmpty());
    QCOMPARE(view.additions(), 0);
    QCOMPARE(view.deletions(), 0);

    // 纯新增（新文件）不能把删除数算错。
    QJsonArray added;
    added.append(makeLine(QStringLiteral("add"), QStringLiteral("a")));
    added.append(makeLine(QStringLiteral("add"), QStringLiteral("b")));
    view.setHunks(QJsonArray{makeHunk(0, 1, added)});
    QCOMPARE(view.additions(), 2);
    QCOMPARE(view.deletions(), 0);
}

void TestDiffView::escapesHtmlFromFileContent() {
    // 文件里出现 HTML 是常态（比如改一个 .html 模板，或者代码里比较 `a < b`）。
    // 不转义的话 `<script>` 会被当成标签，那一行直接消失或错乱。
    QJsonArray lines;
    lines.append(makeLine(QStringLiteral("add"),
                          QStringLiteral("<script>alert('x')</script>")));
    lines.append(makeLine(QStringLiteral("remove"), QStringLiteral("if (a < b && c > d)")));

    DiffView view;
    view.setHunks(QJsonArray{makeHunk(1, 1, lines)});

    const QString html = view.toHtml();
    QVERIFY2(html.contains(QStringLiteral("&lt;script&gt;")),
             "文件内容里的尖括号必须被转义");
    QVERIFY2(!html.contains(QStringLiteral("<script>alert")),
             "未转义的 <script> 会破坏渲染");
    QVERIFY2(html.contains(QStringLiteral("a &lt; b &amp;&amp; c &gt; d")),
             "代码里的比较运算符必须原样可见");
}

void TestDiffView::truncatesHugePatches() {
    // 一个改动几千行的补丁不该把整条对话流撑爆。
    QJsonArray lines;
    const int total = DiffView::kMaxRenderedLines + 120;
    for (int index = 0; index < total; ++index) {
        lines.append(makeLine(QStringLiteral("add"), QStringLiteral("line %1").arg(index)));
    }

    DiffView view;
    view.setHunks(QJsonArray{makeHunk(1, 1, lines)});

    QCOMPARE(view.renderedLines(), DiffView::kMaxRenderedLines);
    QCOMPARE(view.truncatedLines(), total - DiffView::kMaxRenderedLines);
    // 统计仍然要按**完整**补丁算：被截断的是显示，不是事实。
    QCOMPARE(view.additions(), total);
    QVERIFY2(view.toHtml().contains(QStringLiteral("未显示")),
             "截断必须如实告知，不能悄悄丢内容");
}

void TestDiffView::rendersRealDiffFromTool() {
    // 用真实的 Diff::unified 输出，而不是手搓的 hunk：
    // 保证视图能吃下工具真正产出的格式。
    const QString before = QStringLiteral("alpha\nbeta\ngamma\n");
    const QString after = QStringLiteral("alpha\nBETA\ngamma\ndelta\n");
    const QJsonArray hunks = Diff::unified(before, after, QStringLiteral("sample.txt"));
    QVERIFY2(!hunks.isEmpty(), "两份不同内容应当产生补丁");

    // 放进一个真正显示出来的窗口再截图：脱离窗口树的控件会退回系统浅色调色板。
    QWidget window;
    window.setStyleSheet(Theme::instance().styleSheet());
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(12, 12, 12, 12);
    auto *view = new DiffView(&window);
    view->setHunks(hunks);
    layout->addWidget(view);
    window.resize(560, 220);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QCOMPARE(view->additions(), 2);   // BETA + delta
    QCOMPARE(view->deletions(), 1);   // beta

    const QString shot = screenshotDir() + QStringLiteral("/13-diff-view.png");
    QVERIFY2(window.grab().save(shot), qPrintable(shot));
}

QTEST_MAIN(TestDiffView)

#include "test_diff_view.moc"
