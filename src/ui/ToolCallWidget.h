// ZCode Qt — 工具调用卡片
//
// 对话流里每次工具调用渲染成一张可折叠的卡片：
//   头部：状态灯 + 工具名 + 一行摘要 + 耗时 + 展开箭头
//   主体：入参（等宽）与输出（等宽、限高、可滚动）
//
// 状态灯的颜色严格按 ToolState 语义取令牌，不做装饰性用色：
//   InputStreaming / PendingApproval → warning（等待中）
//   Running                          → warning（进行中）
//   Success                          → success
//   Error                            → destructive
//   Cancelled                        → foregroundSubtlest（弱化，不报错）
#pragma once

#include <QFrame>
#include <QString>

#include "core/Types.h"

class QLabel;
class QPlainTextEdit;
class QToolButton;
class QVBoxLayout;

namespace zcode::ui {

class ToolCallWidget : public QFrame {
    Q_OBJECT

public:
    explicit ToolCallWidget(const Part &part, QWidget *parent = nullptr);
    ~ToolCallWidget() override;

    /// 用最新的 part 刷新卡片。必须是同一个 callId，否则忽略。
    void applyPart(const Part &part);

    QString callId() const { return callId_; }
    /// 当前是否展开。
    bool isExpanded() const;

    /// 展开/收起。用户点击头部或调用此方法都会切换。
    void setExpanded(bool expanded);

private:
    void buildUi();
    void refreshHeader();
    void refreshBodies();

    /// 状态灯的颜色。
    QColor stateColor() const;
    /// 状态的中文文案。
    QString stateText() const;

    Part part_;
    QString callId_;

    QToolButton *toggleButton_ = nullptr;
    QLabel *stateDot_ = nullptr;
    QLabel *nameLabel_ = nullptr;
    QLabel *titleLabel_ = nullptr;
    QLabel *metaLabel_ = nullptr;

    QWidget *body_ = nullptr;
    QLabel *inputCaption_ = nullptr;
    QPlainTextEdit *inputView_ = nullptr;
    QLabel *outputCaption_ = nullptr;
    QPlainTextEdit *outputView_ = nullptr;
    QLabel *emptyHint_ = nullptr;
};

}  // namespace zcode::ui
