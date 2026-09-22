// ZCode Qt — 对话流视图
//
// 由若干 MessageWidget 纵向排列组成，每个消息内部再按 Part 的真实顺序
// 交错渲染「思考 → 正文 → 工具调用 → 正文」。
//
// ── 流式渲染策略（重要）────────────────────────────────────────────────
// 逐 token 重新 setMarkdown() 会导致整个文档重新解析与排版，长回答会明显卡顿。
// 因此采用两阶段：
//   流式中  —— `insertPlainText()` 直接追加纯文本，代价 O(增量)
//   完成时  —— 用累积的 Markdown 源做一次 `setMarkdown()`，得到正确的
//              标题/列表/代码块/表格排版
// 代价是流式过程中标记符号（`**`、```）会短暂可见，这在同类产品里是常规取舍。
//
// ── 自动滚动 ────────────────────────────────────────────────────────────
// 只在用户本来就贴着底部时自动跟随。用户向上翻阅历史时不得抢走滚动位置。
#pragma once

#include <QHash>
#include <QWidget>

#include "core/Types.h"

class QLabel;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

namespace zcode::ui {

class ToolCallWidget;

/// 单条消息的渲染单元。
class MessageWidget : public QWidget {
    Q_OBJECT

public:
    explicit MessageWidget(const Message &message, QWidget *parent = nullptr);
    ~MessageWidget() override;

    QString messageId() const { return message_.id; }

    /// 用完整消息刷新（终态渲染：Markdown 重新排版、工具卡片更新状态）。
    void applyMessage(const Message &message);
    /// 追加流式增量。
    void appendDelta(const Id &partId, const QString &delta, bool reasoning);
    /// 更新单个 part（工具状态变化）。
    void applyPart(const Part &part);
    /// 新出现的 part 直接挂上去（工具调用在流式中途产生）。
    void ensurePartWidget(const Part &part);

private:
    void buildUi();
    void buildPartWidget(const Part &part);
    void refreshHeader();
    void refreshTheme();

    /// 正文/思考用的富文本视图。
    QTextBrowser *createRichTextView(bool reasoning);
    /// 用累积的 Markdown 源做终态排版。
    void renderRichText(const Id &partId);

    Message message_;

    QLabel *roleLabel_ = nullptr;
    QLabel *metaLabel_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;

    /// partId → 富文本视图（正文与思考共用这条路径）。
    QHash<Id, QTextBrowser *> richViews_;
    /// partId → 累积的 Markdown 源。流式追加纯文本，终态用它重新排版。
    QHash<Id, QString> richSources_;
    /// partId → 工具卡片。
    QHash<Id, ToolCallWidget *> toolViews_;
    /// 已经建过控件的 part 集合，防止重复插入。
    QHash<Id, bool> renderedParts_;
};

/// 对话流。负责消息顺序、滚动与空状态。
class ConversationView : public QWidget {
    Q_OBJECT

public:
    explicit ConversationView(QWidget *parent = nullptr);
    ~ConversationView() override;

    void clear();
    void addMessage(const Message &message);
    void applyMessage(const Message &message);
    void appendDelta(const Id &messageId, const Id &partId, const QString &delta, bool reasoning);
    void applyPart(const Id &messageId, const Part &part);

    /// 是否应该自动跟随到底部（用户当前贴着底部）。
    bool shouldFollowBottom() const;
    void scrollToBottom();

    int messageCount() const { return static_cast<int>(messages_.size()); }

signals:
    /// 用户点击"重试"时发出（用于重新发起上一条输入）。
    void retryRequested(const zcode::Id &messageId);

private:
    void refreshEmptyState();
    void applyTheme();

    QScrollArea *scroll_ = nullptr;
    QWidget *container_ = nullptr;
    QVBoxLayout *containerLayout_ = nullptr;
    QLabel *emptyState_ = nullptr;
    QHash<Id, MessageWidget *> messages_;
    QStringList messageOrder_;
};

}  // namespace zcode::ui
