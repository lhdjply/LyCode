// LyCode — 问询卡片
//
// 模型用 `choices` 围栏给出选项时，在输入框上方渲染成一张卡片。视觉与交互对齐
// 参考实现：眉标 + 问题标题 + 编号选项（可带灰字说明与「推荐」徽标）+ 自定义答案行
// + 页脚（分页 / 跳过 / 提交）。
//
// 为什么不直接把选项塞进对话流：选项是"针对当前这一步该选什么"，用户答完它就该
// 消失；摆在正文里会跟着历史一直往下滚。卡片独立成控件也让它可被单测直接驱动。
#pragma once

#include <QFrame>
#include <QList>
#include <QString>

#include "ui/Markdown.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace lycode::ui
{

class ChoiceCard : public QFrame
{
    Q_OBJECT

  public:
    explicit ChoiceCard(QWidget * parent = nullptr);
    ~ChoiceCard() override;

    /// 用一组问题重建卡片。传空列表等价于 hideCard()。
    void setQuestions(const QList<Markdown::ChoiceQuestion> & questions);
    /// 收起卡片，不发任何信号。发送消息、切换会话时用。
    void hideCard();

  signals:
    /// 用户按下「提交」或「跳过」走完最后一题：回答已按题序拼好。
    /// 全部跳过的场合 text 为空串——调用方据此不要发送（空消息没有意义）。
    void answersReady(const QString & text);
    /// 用户按下 ✕：放弃整组问题。
    void dismissed();

  private:
    /// 一道题的作答状态。分页切换时保留，所以按题存而不是只存当前页。
    struct Draft {
      int selected = -1;   ///< 选中的选项下标，-1 表示没选
      QString custom;      ///< 自定义答案
    };

    void rebuildPage();
    void refreshOptions();
    void refreshPager();
    void refreshActions();
    void stashCurrentDraft();
    void goTo(int index);
    void pickOption(int optionIndex);
    void submitAnswers();
    void skipCurrent();
    /// 按题序拼出要发出去的文本；跳过的题不参与。
    QString composeAnswer() const;
    bool currentAnswered() const;

    QList<Markdown::ChoiceQuestion> questions_;
    QList<Draft> drafts_;
    int index_ = 0;
    bool minimized_ = false;
    /// 选项行：与 options_ 同序，用于刷新选中态。
    QList<QPushButton *> optionRows_;

    QLabel * eyebrowLabel_ = nullptr;
    QLabel * titleLabel_ = nullptr;
    QWidget * body_ = nullptr;
    /// 正文的滚动容器。收起卡片时隐藏的是它，不是 body_ 的父控件
    ///（setWidget 之后 body_ 会被挂到 viewport 下）。
    QScrollArea * bodyScroll_ = nullptr;
    QVBoxLayout * optionsLayout_ = nullptr;
    QLineEdit * customEdit_ = nullptr;
    QPushButton * minimizeButton_ = nullptr;
    QPushButton * closeButton_ = nullptr;
    QWidget * footer_ = nullptr;
    QPushButton * prevButton_ = nullptr;
    QPushButton * nextButton_ = nullptr;
    QLabel * pagerLabel_ = nullptr;
    QPushButton * skipButton_ = nullptr;
    QPushButton * submitButton_ = nullptr;
};

}  // namespace lycode::ui
