// ZCode Qt — 文件查看器
//
// 点击工具卡片里的图片缩略图 / "查看文件"按钮打开。
//
// 存在的理由：工具卡片里的内容是**给模型看的**——正文被 output 预算截断、
// 输出框限高、图片只画 240px 缩略图。用户要核对"到底读到了什么"时
// （尤其是想确认模型看图看得对不对、或者想自己看完整源码），
// 需要一个能看到完整内容的地方。
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QPlainTextEdit>

#include "ui/SyntaxHighlighter.h"
#include <QString>

class QLabel;
class QScrollArea;

namespace zcode::ui {

/// 带行号的只读代码视图。
class CodeView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeView(QWidget *parent = nullptr);

    /// 载入代码并**着色**。
    ///
    /// 用 `setPlainText` + 逐 token 设置 QTextCharFormat，而不是把文件查看器
    /// 也换成 QTextBrowser：行号栏是挂在 QPlainTextEdit 的 viewport 边距上的，
    /// 换控件就得重做一遍。整篇一次性分词（不是逐行），块注释这种跨行结构
    /// 才能正确着色。
    void setCode(const QString &text, const QString &language,
                 const SyntaxColors &colors);

    /// 行号栏需要的宽度（随行数位数变化）。
    int lineNumberAreaWidth() const;
    /// 由行号栏调用，把自己的绘制委托回来。
    void paintLineNumbers(QPaintEvent *event);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QWidget *gutter_ = nullptr;

    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);
};

/// 文件查看器对话框。
class FileViewerDialog : public QDialog {
    Q_OBJECT

public:
    /// 打开查看器。
    ///
    /// `bytes` 非空时优先使用它（工具已经读到的内容，不依赖磁盘状态），
    /// 否则从 `path` 读取。两者都拿不到内容时给出明确提示，而不是空白窗口。
    static void showContent(QWidget *parent, const QString &path, const QByteArray &bytes);

    /// 从工具卡片的 metadata 直接打开：自动取 filePath 与（可选）images。
    static void showFromToolMetadata(QWidget *parent, const QString &filePath,
                                     const QByteArray &imageBytes);

    /// 构造一个**尚未显示**的查看器（调用方持有所有权）。
    /// showContent() 就是"create + exec + delete"；把构造单独暴露出来是为了
    /// 可测试——模态 exec() 会阻塞测试，没法检查里面的内容。
    static FileViewerDialog *create(QWidget *parent, const QString &path,
                                    const QByteArray &bytes);

    // ── 供测试检查内部状态 ──────────────────────────────────────────────────
    CodeView *codeView() const { return codeView_; }
    QLabel *imageLabel() const { return imageLabel_; }
    QString subtitleText() const;

protected:
    /// 图片区域尺寸变化时重新适配。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    FileViewerDialog(QWidget *parent, const QString &path, const QByteArray &bytes);

    void buildImageBody(const QByteArray &bytes);
    void buildTextBody(const QString &text);

    QString path_;
    QByteArray bytes_;
    QLabel *subtitle_ = nullptr;
    QScrollArea *imageArea_ = nullptr;
    QLabel *imageLabel_ = nullptr;
    CodeView *codeView_ = nullptr;
    /// 图片的原始尺寸，用于"适应窗口"与"原始大小"之间切换。
    QSize imageSize_;
};

}  // namespace zcode::ui
