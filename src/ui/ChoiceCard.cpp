// LyCode — 问询卡片（实现）
#include "ui/ChoiceCard.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

namespace lycode::ui
{
namespace
{

// 文案一律就地写成 QCoreApplication::translate("ui::ChoiceCard", "...")：
// 包一层辅助函数会挡住 lupdate 的抽取（它只认直接作为实参的字面量），
// 表现是界面文案在 .ts 里根本不存在、切到中文后仍是英文。

/// 改过动态属性之后必须重新 polish，QSS 里的 `[selected="true"]` 才会重新求值。
void repolish(QWidget * widget)
{
  if(widget == nullptr || widget->style() == nullptr) {
    return;
  }
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

/// 选项行。继承 QPushButton 是为了"点整行选中"与测试里的 click() 都走 Qt 自己的
/// 路径；但 QPushButton 的 sizeHint 只看文字/图标，**不看**挂上去的布局，
/// 不覆写就会把带说明文字的行压扁、文字被裁掉。
class OptionButton : public QPushButton
{
  public:
    using QPushButton::QPushButton;

    QSize sizeHint() const override
    {
      const QSize base = QPushButton::sizeHint();
      if(layout() == nullptr) {
        return base;
      }
      const QSize fromLayout = layout()->sizeHint();
      return QSize(qMax(base.width(), fromLayout.width()),
                   qMax(base.height(), fromLayout.height()));
    }

    QSize minimumSizeHint() const override
    {
      const QSize fromLayout = layout() != nullptr ? layout()->minimumSize() : QSize();
      // 40px 是参考实现里一行选项的最小高度。
      return QSize(fromLayout.width(), qMax(fromLayout.height(), 40));
    }
};

/// 造一行选项：编号 + 标题（+ 推荐徽标）+ 灰字说明。
///
/// 用按钮而不是自绘控件，代价是内部子控件必须对鼠标透明，否则点在文字上选不中。
QPushButton * makeOptionRow(const Markdown::ChoiceOption & option, int number, QWidget * parent)
{
  auto * row = new OptionButton(parent);
  row->setObjectName(QStringLiteral("choiceOption"));
  row->setCursor(Qt::PointingHandCursor);
  row->setFocusPolicy(Qt::NoFocus);   // Tab 走输入框与页脚按钮，不落在每一行上
  row->setProperty("selected", false);

  auto * layout = new QHBoxLayout(row);
  layout->setContentsMargins(8, 8, 12, 8);
  layout->setSpacing(8);

  auto * numberLabel = new QLabel(QString::number(number), row);
  numberLabel->setObjectName(QStringLiteral("choiceNumber"));
  numberLabel->setAlignment(Qt::AlignCenter);
  numberLabel->setFixedSize(20, 20);
  layout->addWidget(numberLabel, 0, Qt::AlignTop);

  auto * copy = new QWidget(row);
  auto * copyLayout = new QVBoxLayout(copy);
  copyLayout->setContentsMargins(0, 0, 0, 0);
  copyLayout->setSpacing(0);

  auto * line = new QWidget(copy);
  auto * lineLayout = new QHBoxLayout(line);
  lineLayout->setContentsMargins(0, 0, 0, 0);
  lineLayout->setSpacing(6);

  auto * label = new QLabel(option.label, line);
  label->setObjectName(QStringLiteral("choiceOptionLabel"));
  label->setWordWrap(true);
  lineLayout->addWidget(label, 0, Qt::AlignVCenter);

  if(option.recommended) {
    auto * badge = new QLabel(QCoreApplication::translate("ui::ChoiceCard", "Recommended"), line);
    badge->setObjectName(QStringLiteral("choiceBadge"));
    lineLayout->addWidget(badge, 0, Qt::AlignVCenter);
  }
  lineLayout->addStretch(1);
  copyLayout->addWidget(line);

  if(!option.description.trimmed().isEmpty()) {
    auto * description = new QLabel(option.description, copy);
    description->setObjectName(QStringLiteral("choiceDescription"));
    description->setWordWrap(true);
    copyLayout->addWidget(description);
  }
  layout->addWidget(copy, 1);

  for(QWidget * child : row->findChildren<QWidget *>()) {
    child->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  }
  return row;
}

}  // namespace

ChoiceCard::ChoiceCard(QWidget * parent) : QFrame(parent)
{
  setObjectName(QStringLiteral("choiceCard"));
  setFrameShape(QFrame::NoFrame);
  // 与参考实现一致：卡片最高不超过 520px，超出部分由正文区自己滚动，
  // 免得选项多的时候把输入框挤出屏幕。
  setMaximumHeight(520);

  auto * root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 10);
  root->setSpacing(0);

  // ── 头部：眉标 + 问题 + 收起/关闭 ────────────────────────────────────────
  auto * header = new QWidget(this);
  header->setObjectName(QStringLiteral("choiceHeader"));
  auto * headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(24, 20, 16, 0);
  headerLayout->setSpacing(16);

  auto * headingBlock = new QVBoxLayout;
  headingBlock->setContentsMargins(0, 0, 0, 0);
  headingBlock->setSpacing(5);

  eyebrowLabel_ = new QLabel(header);
  eyebrowLabel_->setObjectName(QStringLiteral("choiceEyebrow"));
  eyebrowLabel_->setWordWrap(true);
  headingBlock->addWidget(eyebrowLabel_);

  titleLabel_ = new QLabel(header);
  titleLabel_->setObjectName(QStringLiteral("choiceTitle"));
  titleLabel_->setWordWrap(true);
  headingBlock->addWidget(titleLabel_);
  headerLayout->addLayout(headingBlock, 1);

  minimizeButton_ = new QPushButton(QStringLiteral("▾"), header);
  minimizeButton_->setObjectName(QStringLiteral("choiceIconButton"));
  minimizeButton_->setCursor(Qt::PointingHandCursor);
  minimizeButton_->setFixedSize(24, 24);
  minimizeButton_->setToolTip(QCoreApplication::translate("ui::ChoiceCard", "Collapse the question card"));
  headerLayout->addWidget(minimizeButton_, 0, Qt::AlignTop);

  closeButton_ = new QPushButton(QStringLiteral("×"), header);
  closeButton_->setObjectName(QStringLiteral("choiceIconButton"));
  closeButton_->setCursor(Qt::PointingHandCursor);
  closeButton_->setFixedSize(24, 24);
  closeButton_->setToolTip(QCoreApplication::translate("ui::ChoiceCard", "Dismiss all questions"));
  headerLayout->addWidget(closeButton_, 0, Qt::AlignTop);

  root->addWidget(header);

  // ── 正文：选项 + 自定义答案（可滚动） ────────────────────────────────────
  body_ = new QWidget(this);
  auto * bodyLayout = new QVBoxLayout(body_);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);

  optionsLayout_ = new QVBoxLayout;
  optionsLayout_->setContentsMargins(12, 8, 12, 0);
  optionsLayout_->setSpacing(1);
  bodyLayout->addLayout(optionsLayout_);

  customEdit_ = new QLineEdit(body_);
  customEdit_->setObjectName(QStringLiteral("choiceCustomEdit"));
  customEdit_->setPlaceholderText(QCoreApplication::translate("ui::ChoiceCard", "Type your answer"));
  customEdit_->setClearButtonEnabled(false);
  // 自定义答案行自己就是"一行选项"，所以左右内边距与选项行对齐。
  auto * customWrap = new QHBoxLayout;
  customWrap->setContentsMargins(12, 4, 12, 0);
  customWrap->addWidget(customEdit_);
  bodyLayout->addLayout(customWrap);
  bodyLayout->addStretch(1);

  auto * scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("choiceBodyScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(body_);
  bodyScroll_ = scroll;
  root->addWidget(scroll, 1);

  // ── 页脚：分页 + 跳过/提交 ──────────────────────────────────────────────
  footer_ = new QWidget(this);
  footer_->setObjectName(QStringLiteral("choiceFooter"));
  auto * footerLayout = new QHBoxLayout(footer_);
  footerLayout->setContentsMargins(18, 0, 10, 0);
  footerLayout->setSpacing(12);

  auto * pager = new QWidget(footer_);
  auto * pagerLayout = new QHBoxLayout(pager);
  pagerLayout->setContentsMargins(0, 0, 0, 0);
  pagerLayout->setSpacing(6);

  prevButton_ = new QPushButton(QStringLiteral("‹"), pager);
  prevButton_->setObjectName(QStringLiteral("choicePagerButton"));
  prevButton_->setCursor(Qt::PointingHandCursor);
  prevButton_->setFixedSize(24, 24);
  prevButton_->setToolTip(QCoreApplication::translate("ui::ChoiceCard", "Previous question"));
  pagerLayout->addWidget(prevButton_);

  pagerLabel_ = new QLabel(pager);
  pagerLabel_->setObjectName(QStringLiteral("choicePagerLabel"));
  pagerLayout->addWidget(pagerLabel_);

  nextButton_ = new QPushButton(QStringLiteral("›"), pager);
  nextButton_->setObjectName(QStringLiteral("choicePagerButton"));
  nextButton_->setCursor(Qt::PointingHandCursor);
  nextButton_->setFixedSize(24, 24);
  nextButton_->setToolTip(QCoreApplication::translate("ui::ChoiceCard", "Next question"));
  pagerLayout->addWidget(nextButton_);

  footerLayout->addWidget(pager, 0);
  footerLayout->addStretch(1);

  skipButton_ = new QPushButton(QCoreApplication::translate("ui::ChoiceCard", "Skip"), footer_);
  skipButton_->setObjectName(QStringLiteral("choiceSkipButton"));
  skipButton_->setProperty("variant", "ghost");
  skipButton_->setCursor(Qt::PointingHandCursor);
  footerLayout->addWidget(skipButton_, 0);

  submitButton_ = new QPushButton(QCoreApplication::translate("ui::ChoiceCard", "Submit"), footer_);
  submitButton_->setObjectName(QStringLiteral("choiceSubmitButton"));
  submitButton_->setProperty("accent", "true");
  submitButton_->setCursor(Qt::PointingHandCursor);
  footerLayout->addWidget(submitButton_, 0);

  root->addWidget(footer_);

  connect(minimizeButton_, &QPushButton::clicked, this, [this]() {
    minimized_ = !minimized_;
    bodyScroll_->setVisible(!minimized_);
    footer_->setVisible(!minimized_);
    minimizeButton_->setText(minimized_ ? QStringLiteral("▴") : QStringLiteral("▾"));
    minimizeButton_->setToolTip(minimized_ ? QCoreApplication::translate("ui::ChoiceCard", "Expand the question card")
                                           : QCoreApplication::translate("ui::ChoiceCard", "Collapse the question card"));
  });
  connect(closeButton_, &QPushButton::clicked, this, [this]() {
    hideCard();
    emit dismissed();
  });
  connect(prevButton_, &QPushButton::clicked, this, [this]() {
    goTo(index_ - 1);
  });
  connect(nextButton_, &QPushButton::clicked, this, [this]() {
    goTo(index_ + 1);
  });
  connect(skipButton_, &QPushButton::clicked, this, &ChoiceCard::skipCurrent);
  connect(submitButton_, &QPushButton::clicked, this, [this]() {
    if(index_ + 1 < questions_.size()) {
      goTo(index_ + 1);
    }
    else {
      submitAnswers();
    }
  });
  connect(customEdit_, &QLineEdit::textChanged, this, [this](const QString & text) {
    if(drafts_.isEmpty()) {
      return;
    }
    Draft & draft = drafts_[index_];
    draft.custom = text;
    // 手输与选项是互斥的：写了自定义答案就把选项的选中态撤掉。
    if(!text.trimmed().isEmpty() && draft.selected >= 0) {
      draft.selected = -1;
      refreshOptions();
    }
    refreshActions();
  });

  setQuestions({});
}

ChoiceCard::~ChoiceCard() = default;

void ChoiceCard::setQuestions(const QList<Markdown::ChoiceQuestion> & questions)
{
  questions_ = questions;
  drafts_.clear();
  drafts_.reserve(questions.size());
  for(int i = 0; i < questions.size(); ++i) {
    drafts_.append(Draft{});
  }
  index_ = 0;
  minimized_ = false;

  if(questions_.isEmpty()) {
    hide();
    return;
  }

  bodyScroll_->setVisible(true);
  footer_->setVisible(true);
  minimizeButton_->setText(QStringLiteral("▾"));
  minimizeButton_->setToolTip(QCoreApplication::translate("ui::ChoiceCard", "Collapse the question card"));
  rebuildPage();
  show();
}

void ChoiceCard::hideCard()
{
  questions_.clear();
  drafts_.clear();
  optionRows_.clear();
  index_ = 0;
  hide();
}

void ChoiceCard::rebuildPage()
{
  if(questions_.isEmpty() || index_ < 0 || index_ >= questions_.size()) {
    return;
  }

  optionRows_.clear();
  while(QLayoutItem * item = optionsLayout_->takeAt(0)) {
    if(QWidget * widget = item->widget()) {
      widget->deleteLater();
    }
    delete item;
  }

  const Markdown::ChoiceQuestion & question = questions_.at(index_);
  eyebrowLabel_->setText(question.header);
  eyebrowLabel_->setVisible(!question.header.trimmed().isEmpty());
  titleLabel_->setText(question.question);
  titleLabel_->setVisible(!question.question.trimmed().isEmpty());

  for(int i = 0; i < question.options.size(); ++i) {
    QPushButton * row = makeOptionRow(question.options.at(i), i + 1, body_);
    connect(row, &QPushButton::clicked, this, [this, i]() {
      pickOption(i);
    });
    optionsLayout_->addWidget(row);
    optionRows_.append(row);
  }

  {
    // 载入该页草稿时不要反过来触发 textChanged 把草稿又改一遍。
    const QSignalBlocker blocker(customEdit_);
    customEdit_->setText(drafts_.at(index_).custom);
  }

  refreshOptions();
  refreshPager();
  refreshActions();
}

void ChoiceCard::refreshOptions()
{
  const Draft & draft = drafts_.at(index_);
  for(int i = 0; i < optionRows_.size(); ++i) {
    const bool selected = (draft.selected == i);
    QPushButton * row = optionRows_.at(i);
    if(row->property("selected").toBool() != selected) {
      row->setProperty("selected", selected);
      repolish(row);
    }
  }
}

void ChoiceCard::refreshPager()
{
  pagerLabel_->setText(QStringLiteral("%1 / %2").arg(index_ + 1).arg(questions_.size()));
  prevButton_->setEnabled(index_ > 0);
  nextButton_->setEnabled(index_ + 1 < questions_.size());
}

void ChoiceCard::refreshActions()
{
  const bool last = (index_ + 1 >= questions_.size());
  submitButton_->setText(last ? QCoreApplication::translate("ui::ChoiceCard", "Submit") : QCoreApplication::translate("ui::ChoiceCard", "Next"));
  // 没作答就不给提交：跳过是另外一颗按钮，两条路要分得清。
  submitButton_->setEnabled(currentAnswered());
}

void ChoiceCard::stashCurrentDraft()
{
  if(drafts_.isEmpty()) {
    return;
  }
  drafts_[index_].custom = customEdit_->text();
}

void ChoiceCard::goTo(int index)
{
  if(index < 0 || index >= questions_.size() || index == index_) {
    return;
  }
  stashCurrentDraft();
  index_ = index;
  rebuildPage();
}

void ChoiceCard::pickOption(int optionIndex)
{
  if(drafts_.isEmpty()) {
    return;
  }
  Draft & draft = drafts_[index_];
  draft.selected = optionIndex;
  draft.custom.clear();
  {
    const QSignalBlocker blocker(customEdit_);
    customEdit_->clear();
  }
  refreshOptions();
  refreshActions();
}

void ChoiceCard::skipCurrent()
{
  if(drafts_.isEmpty()) {
    return;
  }
  Draft & draft = drafts_[index_];
  draft.selected = -1;
  draft.custom.clear();
  {
    const QSignalBlocker blocker(customEdit_);
    customEdit_->clear();
  }
  if(index_ + 1 < questions_.size()) {
    goTo(index_ + 1);
  }
  else {
    submitAnswers();
  }
}

void ChoiceCard::submitAnswers()
{
  stashCurrentDraft();
  const QString text = composeAnswer();
  hideCard();
  emit answersReady(text);
}

QString ChoiceCard::composeAnswer() const
{
  QStringList lines;
  const bool many = questions_.size() > 1;
  for(int i = 0; i < questions_.size(); ++i) {
    const Draft & draft = drafts_.at(i);
    QString answer = draft.custom.trimmed();
    if(answer.isEmpty() && draft.selected >= 0 &&
       draft.selected < questions_.at(i).options.size()) {
      answer = questions_.at(i).options.at(draft.selected).value;
    }
    if(answer.isEmpty()) {
      continue;   // 跳过的题不参与拼装
    }
    const QString title = questions_.at(i).question.trimmed();
    // 只有一道题时不回显问题本身——用户要的就是那句话。
    lines << (many && !title.isEmpty()
                ? QCoreApplication::translate("ui::ChoiceCard", "%1: %2").arg(title, answer)
                : answer);
  }
  return lines.join(QLatin1Char('\n'));
}

bool ChoiceCard::currentAnswered() const
{
  if(drafts_.isEmpty()) {
    return false;
  }
  const Draft & draft = drafts_.at(index_);
  return draft.selected >= 0 || !draft.custom.trimmed().isEmpty();
}

}  // namespace lycode::ui
