// LyCode — 工具调用卡片
//
// 对话流里每次工具调用渲染成一张可折叠的卡片：
//   头部：状态灯 + 工具名 + 一行摘要 + 耗时 + 展开箭头
//   主体：入参（等宽）与输出（等宽、限高、可滚动）
//
// ── 折叠行为 ────────────────────────────────────────────────────────────
// **默认收起，并且不会自己展开**：一次任务里工具卡片往往十几张，自动展开会把
// 正文顶出屏幕。状态变化（等待批准 → 执行中 → 成功/失败）只更新头部，
// 不碰用户的展开选择。
// 头部**整行可点**（不是只有那个十几像素的箭头）：标题上拖动仍是选文字，
// 只有"点一下"才切换。
//
// 状态灯的颜色严格按 ToolState 语义取令牌，不做装饰性用色：
//   InputStreaming / PendingApproval → warning（等待中）
//   Running                          → warning（进行中）
//   Success                          → success
//   Error                            → destructive
//   Cancelled                        → foregroundSubtlest（弱化，不报错）
#pragma once

#include <QFrame>
#include <QPoint>
#include <QString>

#include "core/Types.h"

class QHBoxLayout;
class QLabel;
class QPlainTextEdit;

namespace lycode::ui
{
class DiffView;
}

class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace lycode::ui
{

class ToolCallWidget : public QFrame
{
    Q_OBJECT

  public:
    explicit ToolCallWidget(const Part & part, QWidget * parent = nullptr);
    ~ToolCallWidget() override;

    /// 用最新的 part 刷新卡片。必须是同一个 callId，否则忽略。
    void applyPart(const Part & part);

    QString callId() const
    {
      return callId_;
    }
    /// 当前是否展开。
    bool isExpanded() const;

    /// 展开/收起。用户点击头部或调用此方法都会切换。
    void setExpanded(bool expanded);

  protected:
    /// 点击头部任意位置开合卡片；点击图片缩略图打开查看器。
    bool eventFilter(QObject * watched, QEvent * event) override;

  private:
    void buildUi();
    void refreshHeader();
    void refreshBodies();
    /// 同步工具读到的图片缩略图。
    void syncImages();
    /// 打开第 index 张图片的查看器。
    void openImageViewer(QWidget * source);

    /// 这个对象是否属于"点它应当开合卡片"的头部区域。
    bool isHeaderHitTarget(const QObject * watched) const;

    /// 状态灯的颜色。
    QColor stateColor() const;
    /// 状态的中文文案。
    QString stateText() const;

    Part part_;
    QString callId_;

    QWidget * header_ = nullptr;
    QToolButton * toggleButton_ = nullptr;
    QLabel * stateDot_ = nullptr;
    QLabel * nameLabel_ = nullptr;
    QLabel * titleLabel_ = nullptr;
    QLabel * metaLabel_ = nullptr;
    /// 头部一次点击的起点：用来区分"点一下"与"在标题上拖选文字"。
    QPoint headerPressPos_;
    bool headerPressed_ = false;

    QWidget * body_ = nullptr;
    QLabel * inputCaption_ = nullptr;
    QPlainTextEdit * inputView_ = nullptr;
    QLabel * outputCaption_ = nullptr;
    QPlainTextEdit * outputView_ = nullptr;
    QLabel * emptyHint_ = nullptr;

    /// 改动补丁。Write/Edit 会产出 structuredPatch，用专门的视图渲染，
    /// 而不是把它当普通输出文本塞进等宽框。无补丁时整块隐藏。
    QLabel * diffCaption_ = nullptr;
    DiffView * diffView_ = nullptr;
    /// 同步补丁视图。
    void syncDiff();
    /// 非图片文件：给出"查看完整内容"的入口。
    void syncFileLink();
    QPushButton * fileCaption_ = nullptr;
    /// 工具读到的图片（Read 读图片文件时产生）。空时整块隐藏。
    QWidget * imageStrip_ = nullptr;
    QHBoxLayout * imageLayout_ = nullptr;
};

}  // namespace lycode::ui
