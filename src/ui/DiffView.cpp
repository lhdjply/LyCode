#include "ui/DiffView.h"

#include "core/Json.h"
#include "core/Logging.h"
#include "ui/Theme.h"

#include <QLoggingCategory>
#include <QScrollBar>
#include <QTextBlock>
#include <QCoreApplication>

namespace lycode::ui
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.ui.diff")

/// HTML 转义。行内容来自文件，必须转义——否则文件里的 `<` 会被当成标签，
/// 轻则显示错乱，重则把一段源码整块吃掉。
QString escapeHtml(const QString & text)
{
  QString escaped = text;
  escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"));
  escaped.replace(QLatin1Char('<'), QLatin1String("&lt;"));
  escaped.replace(QLatin1Char('>'), QLatin1String("&gt;"));
  // 制表符在等宽字体里宽度不定，统一展开成 4 空格，让缩进对齐可预测。
  escaped.replace(QLatin1Char('\t'), QLatin1String("    "));
  return escaped;
}

/// 把行号渲染成固定宽度的字符串，避免行号位数变化时整列错位。
QString lineNumber(int value)
{
  if(value <= 0) {
    return QStringLiteral("&nbsp;&nbsp;&nbsp;");
  }
  return QString::number(value).rightJustified(4, QLatin1Char(' '));
}

}  // namespace

DiffView::DiffView(QWidget * parent) : QTextBrowser(parent)
{
  setObjectName(QStringLiteral("diffView"));
  setReadOnly(true);
  setFrameShape(QFrame::NoFrame);
  setOpenExternalLinks(false);
  setFont(Theme::instance().font(FontRole::Mono));
  // 长行走横向滚动而不是折行：折行会让"一个逻辑行"占多行，
  // 行号与内容就对不上了。
  setLineWrapMode(QTextBrowser::NoWrap);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  document()->setDocumentMargin(0);

  connect(&Theme::instance(), &Theme::changed, this, &DiffView::rebuild);
}

void DiffView::setHunks(const QJsonArray & hunks)
{
  hunks_ = hunks;
  rebuild();
}

void DiffView::rebuild()
{
  const Palette & palette = Theme::instance().palette();

  additions_ = 0;
  deletions_ = 0;
  renderedLines_ = 0;
  truncatedLines_ = 0;

  const QString addedBg = Theme::css(palette.diffAdded);
  const QString removedBg = Theme::css(palette.diffRemoved);
  const QString addedFg = Theme::css(palette.diffAddedForeground);
  const QString removedFg = Theme::css(palette.diffRemovedForeground);
  const QString muted = Theme::css(palette.foregroundSubtle);
  const QString normal = Theme::css(palette.foreground);
  const QString hunkBg = Theme::css(palette.surface);

  const QString monoFamily =
    Theme::instance().font(FontRole::Mono).family();
  const int monoSize = Theme::instance().font(FontRole::Mono).pointSize();

  QString html;
  html += QStringLiteral("<html><body style=\"margin:0; font-family:'%1'; "
                         "font-size:%2pt; white-space:pre;\">")
          .arg(monoFamily)
          .arg(monoSize);

  int budget = kMaxRenderedLines;
  for(const QJsonValue & hunkValue : hunks_) {
    const QJsonObject hunk = hunkValue.toObject();
    const int oldStart = json::integer(hunk, QStringLiteral("oldStart"), 1);
    const int oldLines = json::integer(hunk, QStringLiteral("oldLines"), 0);
    const int newStart = json::integer(hunk, QStringLiteral("newStart"), 1);
    const int newLines = json::integer(hunk, QStringLiteral("newLines"), 0);

    if(budget <= 0) {
      break;
    }

    // hunk 头：与 git diff 的 @@ 行同形，便于对照熟悉的输出。
    html += QStringLiteral("<div style=\"background:%1; color:%2;\">"
                           "%3 %3 &nbsp;@@ -%4,%5 +%6,%7 @@</div>")
            .arg(hunkBg, muted, QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;"))
            .arg(oldStart)
            .arg(oldLines)
            .arg(newStart)
            .arg(newLines);

    int oldLine = oldStart;
    int newLine = newStart;
    for(const QJsonValue & lineValue : json::array(hunk, QStringLiteral("lines"))) {
      const QJsonObject line = lineValue.toObject();
      // 统计**先于**预算判断：卡片头部的 "+N −M" 描述的是这个补丁本身，
      // 被截断的只是显示。先判预算会让一个改动 5000 行的补丁显示成
      // "+400 −0"，把改动规模严重低估。
      const QString lineType = json::str(line, QStringLiteral("type"));
      if(lineType == QLatin1String("add")) {
        ++additions_;
      }
      else if(lineType == QLatin1String("remove")) {
        ++deletions_;
      }
      if(budget <= 0) {
        truncatedLines_ += 1;
        continue;
      }
      const QString type = json::str(line, QStringLiteral("type"));
      const QString text = escapeHtml(json::str(line, QStringLiteral("text")));

      QString left;
      QString right;
      QString marker;
      QString background;
      QString foreground = normal;

      if(type == QLatin1String("add")) {
        left = lineNumber(0);
        right = lineNumber(newLine);
        marker = QStringLiteral("+");
        background = addedBg;
        foreground = addedFg;
        ++newLine;
      }
      else if(type == QLatin1String("remove")) {
        left = lineNumber(oldLine);
        right = lineNumber(0);
        marker = QStringLiteral("-");
        background = removedBg;
        foreground = removedFg;
        ++oldLine;
      }
      else {
        left = lineNumber(oldLine);
        right = lineNumber(newLine);
        marker = QStringLiteral("&nbsp;");
        ++oldLine;
        ++newLine;
      }

      // 未变行不给背景：背景只用来标出"改了什么"，全都有底色等于没有重点。
      // 占位符个数必须与 .arg() 的参数个数一致：多写一个 %8 而少传一个
      // 参数，那一行内容末尾会原样多出一个字面量 "%8"（截图里抓到过）。
      html += QStringLiteral("<div style=\"color:%1;%2\">"
                             "<span style=\"color:%3;\">%4 %5 %6</span> %7</div>")
              .arg(foreground,
                   background.isEmpty() ? QString()
                   : QStringLiteral(" background:%1;")
                   .arg(background),
                   muted, left, right, marker, text);
      ++renderedLines_;
      --budget;
    }
  }

  if(truncatedLines_ > 0) {
    html += QCoreApplication::translate("ui::DiffView",
                                        "<div style=\"color:%1;\">… %2 more lines not shown (at most %3 lines are rendered at once)</div>")
            .arg(muted)
            .arg(truncatedLines_)
            .arg(kMaxRenderedLines);
  }
  html += QStringLiteral("</body></html>");

  const int previousScroll = verticalScrollBar()->value();
  setHtml(html);
  verticalScrollBar()->setValue(previousScroll);

  qCDebug(log) << "diff 渲染完成; hunks=" << hunks_.size() << "lines=" << renderedLines_
               << "truncated=" << truncatedLines_ << "+" << additions_ << "-" << deletions_;
}

}  // namespace lycode::ui
