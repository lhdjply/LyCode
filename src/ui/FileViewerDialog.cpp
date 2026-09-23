#include "ui/FileViewerDialog.h"

#include "core/Logging.h"
#include "ui/Markdown.h"
#include "ui/Theme.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBlock>
#include <QUrl>
#include <QVBoxLayout>

namespace lycode::ui
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.ui.fileviewer")

/// 纯文本查看的上限。超过这个大小的文件用本对话框看没有意义
/// （几十万行的日志），明确告知并引导用系统程序打开。
constexpr qint64 kMaxTextViewBytes = 4 * 1024 * 1024;

/// 行号栏。
///
/// 不继承 QPlainTextEdit 的 viewport 而单独做一个控件，是因为行号必须固定
/// 在左侧、不随横向滚动移动；画在 viewport 里会被一起滚走。
/// 刻意不加 Q_OBJECT：它不需要信号槽，只需要覆写虚函数。
class Gutter : public QWidget
{
  public:
    explicit Gutter(CodeView * view) : QWidget(view), view_(view) {}
    QSize sizeHint() const override
    {
      return QSize(view_->lineNumberAreaWidth(), 0);
    }

  protected:
    void paintEvent(QPaintEvent * event) override
    {
      view_->paintLineNumbers(event);
    }

  private:
    CodeView * view_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// CodeView
// ─────────────────────────────────────────────────────────────────────────────

CodeView::CodeView(QWidget * parent) : QPlainTextEdit(parent)
{
  setReadOnly(true);
  setFont(Theme::instance().font(FontRole::Mono));
  // 代码不折行：折行会让一个逻辑行占多行，行号与内容就对不上了。
  setLineWrapMode(QPlainTextEdit::NoWrap);
  gutter_ = new Gutter(this);

  connect(this, &QPlainTextEdit::blockCountChanged, this,
  [this](int) {
    updateGutterWidth();
  });
  connect(this, &QPlainTextEdit::updateRequest, this, &CodeView::updateGutter);
  updateGutterWidth();
}

void CodeView::setCode(const QString & text, const QString & language,
                       const SyntaxColors & colors)
{
  setPlainText(text);

  const QList<Token> tokens = SyntaxHighlighter::tokenize(text, language);
  if(tokens.isEmpty()) {
    return;
  }

  // 关闭重绘再逐个上色：每个 setCharFormat 都会触发一次重新布局，
  // 大文件上会明显卡顿（几千个 token × 一次布局）。
  setUpdatesEnabled(false);
  QTextCursor cursor(document());
  for(const Token & token : tokens) {
    cursor.setPosition(token.start);
    cursor.setPosition(token.start + token.length, QTextCursor::KeepAnchor);

    QTextCharFormat format;
    switch(token.kind) {
      case TokenKind::Keyword:
        format.setForeground(colors.keyword);
        format.setFontWeight(QFont::Bold);
        break;
      case TokenKind::String:
        format.setForeground(colors.string);
        break;
      case TokenKind::Comment:
        format.setForeground(colors.comment);
        format.setFontItalic(true);
        break;
      case TokenKind::Number:
        format.setForeground(colors.number);
        break;
      case TokenKind::Type:
        format.setForeground(colors.type);
        break;
      case TokenKind::Function:
        format.setForeground(colors.function);
        break;
      case TokenKind::Preprocessor:
        format.setForeground(colors.preprocessor);
        break;
      case TokenKind::Plain:
        continue;
    }
    cursor.setCharFormat(format);
  }
  setUpdatesEnabled(true);

  // 光标回到开头：setCharFormat 会把光标留在最后一次编辑处，
  // 不清掉的话打开文件看到的是文件末尾。
  moveCursor(QTextCursor::Start);
}

int CodeView::lineNumberAreaWidth() const
{
  // 宽度按**最大行号**的位数算，而不是固定值：否则文件超过 999 行时
  // 数字会被裁掉，或者留一大片空白。
  const int digits = QString::number(qMax(1, blockCount())).size();
  const int space = 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
  return space;
}

void CodeView::updateGutterWidth()
{
  setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeView::updateGutter(const QRect & rect, int dy)
{
  if(gutter_ == nullptr) {
    return;
  }
  if(dy != 0) {
    gutter_->scroll(0, dy);
  }
  else {
    gutter_->update(0, rect.y(), gutter_->width(), rect.height());
  }
  if(rect.contains(viewport()->rect())) {
    updateGutterWidth();
  }
}

void CodeView::resizeEvent(QResizeEvent * event)
{
  QPlainTextEdit::resizeEvent(event);
  const QRect area = contentsRect();
  gutter_->setGeometry(QRect(area.left(), area.top(), lineNumberAreaWidth(), area.height()));
}

void CodeView::paintEvent(QPaintEvent * event)
{
  QPlainTextEdit::paintEvent(event);
  // 行号栏在 viewport 之外，背景要自己填，否则会露出对话框底色。
  QPainter painter(viewport());
  painter.fillRect(event->rect(), palette().base());
}

void CodeView::paintLineNumbers(QPaintEvent * event)
{
  QPainter painter(gutter_);
  painter.fillRect(event->rect(), Theme::instance().palette().surface);

  const Palette & palette = Theme::instance().palette();
  painter.setPen(Theme::css(palette.foregroundSubtle));

  // 只画可见块：整文件逐行画在大文件上会明显卡顿。
  QTextBlock block = firstVisibleBlock();
  int blockNumber = block.blockNumber();
  int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
  int bottom = top + qRound(blockBoundingRect(block).height());

  while(block.isValid() && top <= event->rect().bottom()) {
    if(block.isVisible() && bottom >= event->rect().top()) {
      painter.drawText(0, top, gutter_->width() - 8,
                       fontMetrics().height(), Qt::AlignRight,
                       QString::number(blockNumber + 1));
    }
    block = block.next();
    top = bottom;
    bottom = top + qRound(blockBoundingRect(block).height());
    ++blockNumber;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// FileViewerDialog
// ─────────────────────────────────────────────────────────────────────────────

FileViewerDialog * FileViewerDialog::create(QWidget * parent, const QString & path,
                                            const QByteArray & bytes)
{
  return new FileViewerDialog(parent, path, bytes);
}

QString FileViewerDialog::subtitleText() const
{
  return subtitle_ != nullptr ? subtitle_->text() : QString();
}

void FileViewerDialog::showContent(QWidget * parent, const QString & path,
                                   const QByteArray & bytes)
{
  // 模态显示：查看器是"看一眼就关"的短生命周期窗口，
  // 非模态会让它被对话流淹没。
  QScopedPointer<FileViewerDialog> dialog(create(parent, path, bytes));
  dialog->exec();
}

void FileViewerDialog::showFromToolMetadata(QWidget * parent, const QString & filePath,
                                            const QByteArray & imageBytes)
{
  showContent(parent, filePath, imageBytes);
}

FileViewerDialog::FileViewerDialog(QWidget * parent, const QString & path,
                                   const QByteArray & bytes)
  : QDialog(parent), path_(path), bytes_(bytes)
{
  setObjectName(QStringLiteral("fileViewer"));
  setWindowTitle(QFileInfo(path).fileName().isEmpty() ? QStringLiteral("查看")
                 : QFileInfo(path).fileName());
  setStyleSheet(Theme::instance().styleSheet());
  resize(880, 620);

  QByteArray payload = bytes_;
  if(payload.isEmpty() && !path.isEmpty()) {
    QFile file(path);
    if(file.open(QIODevice::ReadOnly)) {
      payload = file.readAll();
    }
  }
  bytes_ = payload;

  auto * root = new QVBoxLayout(this);
  root->setContentsMargins(16, 14, 16, 14);
  root->setSpacing(8);

  auto * title = new QLabel(QFileInfo(path).fileName().isEmpty()
                            ? QStringLiteral("查看内容")
                            : QFileInfo(path).fileName());
  title->setFont(Theme::instance().font(FontRole::UiLg));
  root->addWidget(title);

  subtitle_ = new QLabel;
  subtitle_->setFont(Theme::instance().font(FontRole::UiXs));
  subtitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  subtitle_->setWordWrap(true);
  root->addWidget(subtitle_);

  // 判断是图片还是文本：优先看文件名的 MIME，其次看字节内容的魔数。
  // 只看扩展名会漏掉没有扩展名的图片；只看魔数会把文本误判（有些文本
  // 也能被识别成 application/octet-stream）。
  const QMimeDatabase mimeDb;
  QString mime = mimeDb.mimeTypeForFile(path).name();
  if(payload.isEmpty()) {
    mime.clear();
  }
  if(!mime.startsWith(QStringLiteral("image/")) &&
     mimeDb.mimeTypeForData(payload).name().startsWith(QStringLiteral("image/"))) {
    mime = mimeDb.mimeTypeForData(payload).name();
  }

  const bool isImage = !payload.isEmpty() && mime.startsWith(QStringLiteral("image/"));
  if(isImage) {
    buildImageBody(payload);
  }
  else if(payload.isEmpty()) {
    auto * empty = new QLabel(QStringLiteral("无法读取内容：文件不存在或没有权限。\n%1")
                              .arg(path));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    root->addWidget(empty, 1);
  }
  else if(payload.size() > kMaxTextViewBytes) {
    auto * tooBig = new QLabel(
      QStringLiteral("文件过大（%1 MB），不在窗口内展示。\n请用系统程序打开。")
      .arg(QString::number(payload.size() / 1024.0 / 1024.0, 'f', 1)));
    tooBig->setAlignment(Qt::AlignCenter);
    tooBig->setWordWrap(true);
    root->addWidget(tooBig, 1);
  }
  else {
    buildTextBody(QString::fromUtf8(payload));
  }

  root->addStretch(0);

  auto * buttons = new QDialogButtonBox;
  auto * closeButton = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
  connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
  if(!path.isEmpty() && QFileInfo::exists(path)) {
    // 只读查看器不该是唯一出口：想编辑或想用专门的图片工具看时，
    // 交给系统默认程序。仅在文件确实存在时才给这个入口。
    auto * openButton = buttons->addButton(QStringLiteral("用系统程序打开"),
                                           QDialogButtonBox::ActionRole);
    connect(openButton, &QPushButton::clicked, this, [this]() {
      if(!QDesktopServices::openUrl(QUrl::fromLocalFile(path_))) {
        qCWarning(log) << "系统程序打开失败:" << path_;
      }
    });
  }
  root->addWidget(buttons);
}

void FileViewerDialog::buildImageBody(const QByteArray & bytes)
{
  QPixmap pixmap;
  if(!pixmap.loadFromData(bytes)) {
    auto * failed = new QLabel(QStringLiteral("图片无法解码。"));
    failed->setAlignment(Qt::AlignCenter);
    layout()->addWidget(failed);
    return;
  }
  imageSize_ = pixmap.size();

  const QString mime = QMimeDatabase().mimeTypeForData(bytes).name();
  subtitle_->setText(QStringLiteral("%1  ·  %2×%3  ·  %4 KB")
                     .arg(path_.isEmpty() ? mime : path_)
                     .arg(pixmap.width())
                     .arg(pixmap.height())
                     .arg(QString::number(bytes.size() / 1024.0, 'f', 1)));

  imageLabel_ = new QLabel;
  imageLabel_->setAlignment(Qt::AlignCenter);
  // 按窗口大小缩放显示：查看器默认给"看全整张图"，需要看细节时
  // 用户可以放大窗口或点"原始大小"。
  imageLabel_->setPixmap(pixmap);
  imageLabel_->setScaledContents(false);
  imageLabel_->setMinimumSize(1, 1);

  imageArea_ = new QScrollArea;
  imageArea_->setWidgetResizable(true);
  imageArea_->setAlignment(Qt::AlignCenter);
  imageArea_->setWidget(imageLabel_);
  imageArea_->setFrameShape(QFrame::NoFrame);

  // 窗口尺寸确定后按可用空间缩放一次，之后每次 resize 重新适配。
  const auto fit = [this]() {
    if(imageLabel_ == nullptr || imageArea_ == nullptr || imageSize_.isEmpty()) {
      return;
    }
    const QSize available = imageArea_->viewport()->size() - QSize(8, 8);
    if(available.width() <= 0 || available.height() <= 0) {
      return;
    }
    // 小图不放大：放大只会变糊，没有信息增量。
    imageLabel_->setPixmap(QPixmap());
    QPixmap original;
    original.loadFromData(bytes_);
    imageLabel_->setPixmap(original.size().width() > available.width() ||
                           original.size().height() > available.height()
                           ? original.scaled(available, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation)
                           : original);
  };
  connect(this, &QDialog::finished, this, [](int) {});
  imageArea_->installEventFilter(this);
  QMetaObject::invokeMethod(this, fit, Qt::QueuedConnection);

  layout()->addWidget(imageArea_);
}

void FileViewerDialog::buildTextBody(const QString & text)
{
  // 末尾换行不算额外一行。直接 count('\n')+1 会把 4 行的文件报成 5 行，
  // 与 ReadTool / Diff::splitLines 的口径也不一致。
  QStringList lineList = text.split(QLatin1Char('\n'));
  if(!lineList.isEmpty() && lineList.last().isEmpty() && !text.isEmpty()) {
    lineList.removeLast();
  }
  const int lines = lineList.size();
  subtitle_->setText(QStringLiteral("%1  ·  %2 行  ·  %3 KB")
                     .arg(path_)
                     .arg(lines)
                     .arg(QString::number(bytes_.size() / 1024.0, 'f', 1)));

  codeView_ = new CodeView;

  // 语法着色。语言按扩展名猜，猜不出就按通用规则（仍能分出字符串/注释）。
  const Palette & palette = Theme::instance().palette();
  SyntaxColors colors;
  colors.keyword = palette.syntaxKeyword;
  colors.string = palette.syntaxString;
  colors.comment = palette.syntaxComment;
  colors.number = palette.syntaxNumber;
  colors.type = palette.syntaxType;
  colors.function = palette.syntaxFunction;
  colors.preprocessor = palette.syntaxPreproc;

  const QString language = SyntaxHighlighter::languageForFile(path_);
  codeView_->setCode(text, language, colors);
  if(!language.isEmpty()) {
    subtitle_->setText(subtitle_->text() + QStringLiteral("  ·  %1").arg(language));
  }
  layout()->addWidget(codeView_);
}

bool FileViewerDialog::eventFilter(QObject * watched, QEvent * event)
{
  // 窗口尺寸变化时重新适配图片。
  if(watched == imageArea_ && event->type() == QEvent::Resize && imageLabel_ != nullptr) {
    const QSize available = imageArea_->viewport()->size() - QSize(8, 8);
    QPixmap original;
    if(available.width() > 0 && available.height() > 0 &&
       original.loadFromData(bytes_)) {
      imageLabel_->setPixmap(original.size().width() > available.width() ||
                             original.size().height() > available.height()
                             ? original.scaled(available, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation)
                             : original);
    }
  }
  return QDialog::eventFilter(watched, event);
}

}  // namespace lycode::ui
