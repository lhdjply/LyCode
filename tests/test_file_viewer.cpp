// ZCode Qt — 文件查看器测试
//
// 覆盖两件事：
//   1. 查看器能不能正确显示文本 / 图片 / 缺失文件
//   2. 工具卡片有没有给出**可点**的入口（这是用户报的缺口：
//      图片只有 240px 缩略图、代码文件被输出框限高截断，都点不开）
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTextBlock>
#include <QTextFragment>
#include <QTest>
#include <QWidget>

#include "core/Types.h"
#include "ui/DiffView.h"
#include "ui/FileViewerDialog.h"
#include "ui/SyntaxHighlighter.h"
#include "ui/Theme.h"
#include "ui/ToolCallWidget.h"

using namespace zcode;
using namespace zcode::ui;

namespace {

/// 造一个 Read 工具的 part。
Part makeReadPart(const QString &filePath) {
    Part part;
    part.id = QStringLiteral("part_1");
    part.kind = PartKind::Tool;
    part.tool.callId = QStringLiteral("call_1");
    part.tool.name = QStringLiteral("Read");
    part.tool.state = ToolState::Success;
    part.tool.output = QStringLiteral("file content");
    part.tool.metadata.insert(QStringLiteral("filePath"), filePath);
    return part;
}

QByteArray makePngBytes() {
    QImage image(80, 50, QImage::Format_RGB32);
    image.fill(QColor(40, 140, 220));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

}  // namespace

class TestFileViewer : public QObject {
    Q_OBJECT

private slots:
    void showsTextWithLineNumbers();
    void showsImageScaled();
    void reportsMissingFileInsteadOfBlankWindow();
    void toolCardOffersClickableEntryPoints();
    void codeViewIsSyntaxHighlighted();
};

void TestFileViewer::showsTextWithLineNumbers() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sample.cpp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // 故意包含 HTML 与比较运算符：QPlainTextEdit 走 setPlainText，
    // 不做任何 HTML 解析，内容必须原样可见。
    file.write("int main() {\n    if (a < b && c > d) return 1;\n    return 0;\n}\n");
    file.close();

    QScopedPointer<FileViewerDialog> dialog(
        FileViewerDialog::create(nullptr, path, QByteArray()));
    QVERIFY2(dialog->codeView() != nullptr, "文本文件应当用代码视图展示");
    QVERIFY2(dialog->imageLabel() == nullptr, "文本文件不该有图片视图");

    const QString content = dialog->codeView()->toPlainText();
    QVERIFY2(content.contains(QStringLiteral("a < b && c > d")),
             "代码内容必须原样显示，不能被当成 HTML 解析");
    QVERIFY(content.contains(QStringLiteral("int main()")));
    // 打开时定位到开头：否则长文件会停在末尾，用户以为内容不对。
    QCOMPARE(dialog->codeView()->textCursor().position(), 0);
    // 4 行内容。末尾换行不该被算成第 5 行。
    QVERIFY2(dialog->subtitleText().contains(QStringLiteral("4 行")),
             qPrintable(QStringLiteral("行数统计错误，实际副标题：%1")
                            .arg(dialog->subtitleText())));
    QVERIFY2(!dialog->subtitleText().contains(QStringLiteral("5 行")),
             "末尾换行不该多算一行");
    // 行号栏必须真的占了宽度，否则行号画不出来。
    QVERIFY2(dialog->codeView()->lineNumberAreaWidth() > 10,
             "行号栏必须占位");
}

void TestFileViewer::showsImageScaled() {
    const QByteArray bytes = makePngBytes();
    QVERIFY(!bytes.isEmpty());

    // 图片没有磁盘路径也要能看（粘贴来的图片就是这样）。
    QScopedPointer<FileViewerDialog> dialog(
        FileViewerDialog::create(nullptr, QString(), bytes));
    QVERIFY2(dialog->imageLabel() != nullptr, "图片应当用图片视图展示");
    QVERIFY2(dialog->codeView() == nullptr, "图片不该走代码视图");
    // 布局完成后才有缩放后的 pixmap。
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.data()));
    QVERIFY2(!dialog->imageLabel()->pixmap().isNull(), "图片必须被渲染出来");
    QVERIFY2(dialog->subtitleText().contains(QStringLiteral("80×50")),
             qPrintable(QStringLiteral("副标题应当给出原始尺寸，实际：%1")
                            .arg(dialog->subtitleText())));

    const QString dir = QDir(QCoreApplication::applicationDirPath())
                            .filePath(QStringLiteral("ui-screenshots"));
    QDir().mkpath(dir);
    const QString shot = dir + QStringLiteral("/14-image-viewer.png");
    QVERIFY2(dialog->grab().save(shot), qPrintable(shot));
}

void TestFileViewer::reportsMissingFileInsteadOfBlankWindow() {
    // 文件不存在且没有备用内容时，必须给出明确说明。
    // 一个空白窗口会让用户以为"文件是空的"，而实际上根本没读到。
    QScopedPointer<FileViewerDialog> dialog(FileViewerDialog::create(
        nullptr, QStringLiteral("/nonexistent/definitely-missing.txt"), QByteArray()));
    QVERIFY2(dialog->codeView() == nullptr, "读不到内容时不该给出空的代码视图");
    QVERIFY(dialog->imageLabel() == nullptr);

    bool foundHint = false;
    for (const QLabel *label : dialog->findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("无法读取内容"))) {
            foundHint = true;
            break;
        }
    }
    QVERIFY2(foundHint, "必须明确告知读不到内容");
    // 文件不存在时不该给"用系统程序打开"。
    for (const QPushButton *button : dialog->findChildren<QPushButton *>()) {
        QVERIFY2(!button->text().contains(QStringLiteral("系统程序")),
                 "文件不存在时不应提供系统程序打开");
    }
}

void TestFileViewer::toolCardOffersClickableEntryPoints() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // ① 文本文件：卡片里必须有"查看文件"入口。
    const QString textPath = dir.filePath(QStringLiteral("code.py"));
    QFile file(textPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("print('hi')\n");
    file.close();

    ToolCallWidget card(makeReadPart(textPath));
    auto *link = card.findChild<QPushButton *>(QStringLiteral("toolFileLink"));
    QVERIFY2(link != nullptr, "读文本文件后卡片必须给出查看入口");
    // 卡片默认是收起的，展开后入口才可见——所以要断言"展开后可见"，
    // 而不是直接断言可见。
    card.setExpanded(true);
    QVERIFY(card.isExpanded());
    QVERIFY2(link->isVisibleTo(&card), "展开卡片后查看入口必须可见");
    QVERIFY2(link->toolTip().contains(textPath), "入口应当说明会打开哪个文件");

    // ② 图片：缩略图本身必须可点，并且不能再多一个"查看文件"按钮（噪音）。
    Part imagePart = makeReadPart(dir.filePath(QStringLiteral("shot.png")));
    FilePart image;
    image.mimeType = QStringLiteral("image/png");
    image.fileName = QStringLiteral("shot.png");
    image.base64 = QString::fromLatin1(makePngBytes().toBase64());
    image.sizeBytes = makePngBytes().size();
    imagePart.tool.images.append(image);

    ToolCallWidget imageCard(imagePart);
    QLabel *thumb = nullptr;
    for (QLabel *label : imageCard.findChildren<QLabel *>(QStringLiteral("toolImage"))) {
        thumb = label;
        break;
    }
    QVERIFY2(thumb != nullptr, "图片必须渲染成缩略图");
    QVERIFY2(thumb->cursor().shape() == Qt::PointingHandCursor,
             "缩略图必须看起来可点");
    QVERIFY2(!thumb->toolTip().isEmpty(), "缩略图应当提示可以点开看原图");
    // 按钮在构造时就建好了，所以查的是**可见性**而不是存在性。
    imageCard.setExpanded(true);
    auto *imageFileLink = imageCard.findChild<QPushButton *>(QStringLiteral("toolFileLink"));
    QVERIFY(imageFileLink != nullptr);
    QVERIFY2(!imageFileLink->isVisibleTo(&imageCard),
             "图片已经可点，不该再多一个查看文件按钮");

    // ③ 没有 filePath 的调用（例如 Bash）不该出现查看入口。
    Part bashPart;
    bashPart.kind = PartKind::Tool;
    bashPart.tool.name = QStringLiteral("Bash");
    bashPart.tool.state = ToolState::Success;
    ToolCallWidget bashCard(bashPart);
    bashCard.setExpanded(true);
    auto *bashFileLink = bashCard.findChild<QPushButton *>(QStringLiteral("toolFileLink"));
    QVERIFY(bashFileLink != nullptr);
    QVERIFY2(!bashFileLink->isVisibleTo(&bashCard), "没有文件路径时不该给出查看入口");
}

void TestFileViewer::codeViewIsSyntaxHighlighted() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sample.cpp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("// note\n"
               "static int factorial(int n) {\n"
               "    const char *label = \"recursive\";\n"
               "    if (n <= 1) return 1;\n"
               "    return n * factorial(n - 1);\n"
               "}\n");
    file.close();

    QScopedPointer<FileViewerDialog> dialog(
        FileViewerDialog::create(nullptr, path, QByteArray()));
    CodeView *view = dialog->codeView();
    QVERIFY(view != nullptr);

    // 断言**文档里真的出现了多种前景色**，而不是只看截图。
    // 之前文件查看器就是 setPlainText，标题说支持高亮但一个字都没上色。
    QSet<QString> colors;
    for (QTextBlock block = view->document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const QTextFragment piece = fragment.fragment();
            if (piece.isValid() && !piece.text().trimmed().isEmpty()) {
                colors.insert(piece.charFormat().foreground().color().name());
            }
        }
    }
    QVERIFY2(colors.size() >= 4,
             qPrintable(QStringLiteral("代码查看器必须着色（注释/关键字/字符串/数字），"
                                       "实际只有 %1 种前景色：%2")
                            .arg(colors.size())
                            .arg(QStringList(colors.begin(), colors.end())
                                     .join(QStringLiteral(", ")))));

    // 注释必须是斜体、关键字必须是粗体——否则只算"有颜色"，不算语法高亮。
    bool foundItalicComment = false;
    bool foundBoldKeyword = false;
    for (QTextBlock block = view->document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const QTextFragment piece = fragment.fragment();
            if (!piece.isValid()) {
                continue;
            }
            const QString text = piece.text();
            if (text.contains(QStringLiteral("// note"))) {
                foundItalicComment = piece.charFormat().fontItalic();
            }
            if (text == QStringLiteral("static")) {
                foundBoldKeyword = piece.charFormat().fontWeight() >= QFont::Bold;
            }
        }
    }
    QVERIFY2(foundItalicComment, "注释应当是斜体");
    QVERIFY2(foundBoldKeyword, "关键字应当是粗体");

    // 内容完整性：着色只加格式，不改字符。
    QVERIFY(view->toPlainText().contains(QStringLiteral("const char *label = \"recursive\";")));
    // 载入后定位到开头。
    QCOMPARE(view->textCursor().position(), 0);
    // 副标题标出识别到的语言。
    QVERIFY2(dialog->subtitleText().contains(QStringLiteral("cpp")),
             qPrintable(dialog->subtitleText()));

    // 语言探测
    QCOMPARE(SyntaxHighlighter::languageForFile(QStringLiteral("/a/b/main.cpp")),
             QStringLiteral("cpp"));
    QCOMPARE(SyntaxHighlighter::languageForFile(QStringLiteral("x.py")), QStringLiteral("python"));
    QCOMPARE(SyntaxHighlighter::languageForFile(QStringLiteral("CMakeLists.txt")),
             QStringLiteral("cmake"));
    QVERIFY(SyntaxHighlighter::languageForFile(QStringLiteral("notes.unknownext")).isEmpty());

    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.data()));
    const QString dir2 = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("ui-screenshots"));
    QDir().mkpath(dir2);
    const QString shot = dir2 + QStringLiteral("/16-code-viewer-highlight.png");
    QVERIFY2(dialog->grab().save(shot), qPrintable(shot));
}

QTEST_MAIN(TestFileViewer)

#include "test_file_viewer.moc"
