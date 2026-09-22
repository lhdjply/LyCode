// LyCode — Diff 视图
//
// 渲染 Write / Edit 工具产生的结构化补丁（`metadata.structuredPatch`）。
//
// 为什么不直接把 Diff::render() 的文本丢进输出框：
//   * 结构化补丁里每行都带 type，渲染端不需要解析文本 diff，也就不会
//     因为格式细节（`\ No newline`、行号宽度）出现展示偏差。
//   * 增删行需要**背景色块**而不是字符前缀——满屏的 +/- 前缀在深色主题下
//     几乎看不出结构，色块才能让人一眼扫出改动范围。
//
// 用 QTextBrowser + 逐行 HTML 而不是自绘：白送选中复制、滚动、
// 长行横向滚动，这几样自绘都要重新实现一遍。
#pragma once

#include <QJsonArray>
#include <QTextBrowser>

namespace lycode::ui {

class DiffView : public QTextBrowser {
    Q_OBJECT

public:
    /// 单次渲染的行数上限。超出时截断并明确告知剩余行数——
    /// 一个改动 5000 行的补丁不该把整条对话流撑爆。
    static constexpr int kMaxRenderedLines = 400;

    explicit DiffView(QWidget *parent = nullptr);

    /// 渲染结构化补丁（Diff::unified 的输出格式）。
    /// hunks 为空或格式不对时清空自身。
    void setHunks(const QJsonArray &hunks);

    /// 是否有内容可显示。
    bool isEmpty() const { return renderedLines_ == 0; }
    int renderedLines() const { return renderedLines_; }
    /// 被截断的行数；未截断为 0。
    int truncatedLines() const { return truncatedLines_; }
    /// 本次补丁的新增/删除行数（用于卡片头部的 +N −M）。
    int additions() const { return additions_; }
    int deletions() const { return deletions_; }

private:
    /// 重新按主题生成 HTML。
    void rebuild();

    QJsonArray hunks_;
    int renderedLines_ = 0;
    int truncatedLines_ = 0;
    int additions_ = 0;
    int deletions_ = 0;
};

}  // namespace lycode::ui
