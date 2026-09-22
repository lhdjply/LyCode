#include "ui/Markdown.h"

#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

namespace zcode::ui {
namespace {

/// HTML 转义。所有进入输出的用户/模型文本都必须经过这里——
/// 模型可能输出 `<script>` 或破坏布局的标签，直通会同时造成安全与显示问题。
QString escape(const QString &text) {
    QString result = text;
    result.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    result.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    result.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    result.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return result;
}

/// 行内标记：代码 → 粗体 → 斜体 → 删除线 → 链接。
/// 顺序重要：先处理行内代码，否则代码里的 `*` 会被当成强调标记。
QString renderInline(const QString &raw) {
    // 先用占位符把行内代码摘出来，避免其内容被后续规则改写。
    QStringList codeSpans;
    QString text = raw;

    static const QRegularExpression codePattern(QStringLiteral("`([^`]+)`"));
    {
        QString replaced;
        qsizetype last = 0;
        auto iterator = codePattern.globalMatch(text);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch match = iterator.next();
            replaced += text.mid(last, match.capturedStart() - last);
            const QString placeholder =
                QStringLiteral("\u0001CODE%1\u0001").arg(codeSpans.size());
            codeSpans.append(match.captured(1));
            replaced += placeholder;
            last = match.capturedEnd();
        }
        replaced += text.mid(last);
        text = replaced;
    }

    text = escape(text);

    // 图片语法降级为链接（我们不内联远程图片：那会发起网络请求且无法离线渲染）。
    static const QRegularExpression imagePattern(QStringLiteral("!\\[([^\\]]*)\\]\\(([^)]+)\\)"));
    text.replace(imagePattern, QLatin1String("[图片: \\1](\\2)"));

    static const QRegularExpression linkPattern(QStringLiteral("\\[([^\\]]+)\\]\\(([^)]+)\\)"));
    {
        QString replaced;
        qsizetype last = 0;
        auto iterator = linkPattern.globalMatch(text);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch match = iterator.next();
            replaced += text.mid(last, match.capturedStart() - last);
            const QString label = match.captured(1);
            const QString href = match.captured(2).trimmed();
            // 只允许安全 scheme，避免 javascript: 之类的链接。
            const QUrl url(href);
            const bool safe = url.isLocalFile() || url.scheme().isEmpty() ||
                              url.scheme() == QLatin1String("http") ||
                              url.scheme() == QLatin1String("https") ||
                              url.scheme() == QLatin1String("mailto");
            if (safe) {
                replaced += QStringLiteral("<a href=\"%1\">%2</a>").arg(escape(href), label);
            } else {
                replaced += label;
            }
            last = match.capturedEnd();
        }
        replaced += text.mid(last);
        text = replaced;
    }

    text.replace(QRegularExpression(QStringLiteral("\\*\\*([^*]+)\\*\\*")),
                 QLatin1String("<b>\\1</b>"));
    text.replace(QRegularExpression(QStringLiteral("__([^_]+)__")), QLatin1String("<b>\\1</b>"));
    text.replace(QRegularExpression(QStringLiteral("(?<![*\\w])\\*([^*\\n]+)\\*(?![*\\w])")),
                 QLatin1String("<i>\\1</i>"));
    text.replace(QRegularExpression(QStringLiteral("(?<![_\\w])_([^_\\n]+)_(?![_\\w])")),
                 QLatin1String("<i>\\1</i>"));
    text.replace(QRegularExpression(QStringLiteral("~~([^~]+)~~")), QLatin1String("<s>\\1</s>"));

    // 还原行内代码。
    for (qsizetype index = 0; index < codeSpans.size(); ++index) {
        text.replace(QStringLiteral("\u0001CODE%1\u0001").arg(index),
                     QStringLiteral("<code class=\"inline\">%1</code>").arg(escape(codeSpans.at(index))));
    }

    return text;
}

/// GFM 任务列表前缀 `- [ ]` / `- [x]`。
/// 返回替换用的复选框字符，并通过 out 参数给出**去掉前缀后**的正文——
/// 不清掉 `[x]` 会渲染成"☑ [x] done"这种重复。
QString taskPrefix(const QString &item, QString *strippedOut) {
    static const QRegularExpression pattern(QStringLiteral("^\\[([ xX])\\]\\s+"));
    const QRegularExpressionMatch match = pattern.match(item);
    if (!match.hasMatch()) {
        if (strippedOut != nullptr) {
            *strippedOut = item;
        }
        return {};
    }
    if (strippedOut != nullptr) {
        *strippedOut = item.mid(match.capturedEnd());
    }
    return match.captured(1).trimmed().toLower() == QLatin1String("x")
               ? QStringLiteral("\u2611 ")
               : QStringLiteral("\u2610 ");
}

bool isTableSeparator(const QString &line) {
    static const QRegularExpression pattern(QStringLiteral("^\\s*\\|?[\\s:-]*-[\\s:|-]*\\|?\\s*$"));
    return pattern.match(line).hasMatch() && line.contains(QLatin1Char('-'));
}

QStringList splitTableRow(const QString &line) {
    QString trimmed = line.trimmed();
    if (trimmed.startsWith(QLatin1Char('|'))) {
        trimmed = trimmed.mid(1);
    }
    if (trimmed.endsWith(QLatin1Char('|'))) {
        trimmed.chop(1);
    }
    return trimmed.split(QLatin1Char('|'));
}

}  // namespace

QString Markdown::toHtml(const QString &markdown, const MarkdownStyle &style) {
    Q_UNUSED(style)

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString html;

    bool inCodeBlock = false;
    QString codeLanguage;
    QStringList codeLines;
    bool inList = false;
    QString listTag;
    bool inQuote = false;
    bool inParagraph = false;
    QStringList paragraphLines;

    // 统一收口：把悬空的段落/列表/引用块闭合。集中在一处，避免每条分支各写一遍。
    const auto closeParagraph = [&]() {
        if (inParagraph) {
            html += QStringLiteral("<p>") + renderInline(paragraphLines.join(QLatin1Char(' '))) +
                    QStringLiteral("</p>\n");
            paragraphLines.clear();
            inParagraph = false;
        }
    };
    const auto closeList = [&]() {
        if (inList) {
            html += QStringLiteral("</%1>\n").arg(listTag);
            inList = false;
            listTag.clear();
        }
    };
    const auto closeQuote = [&]() {
        if (inQuote) {
            html += QStringLiteral("</blockquote>\n");
            inQuote = false;
        }
    };
    const auto closeCode = [&]() {
        if (!inCodeBlock) {
            return;
        }
        const QString languageLabel =
            codeLanguage.isEmpty() ? QString() : QStringLiteral("<div class=\"code-lang\">%1</div>")
                                                  .arg(escape(codeLanguage));
        html += QStringLiteral("<div class=\"code-block\">%1<pre class=\"code\">%2</pre></div>\n")
                    .arg(languageLabel, escape(codeLines.join(QLatin1Char('\n'))));
        codeLines.clear();
        codeLanguage.clear();
        inCodeBlock = false;
    };
    const auto closeAllBlocks = [&]() {
        closeParagraph();
        closeList();
        closeQuote();
    };

    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QString line = lines.at(index);
        const QString trimmed = line.trimmed();

        // ── 围栏代码块 ──────────────────────────────────────────────────────
        static const QRegularExpression fencePattern(QStringLiteral("^\\s*(```+|~~~+)\\s*(.*)$"));
        const QRegularExpressionMatch fenceMatch = fencePattern.match(line);
        if (fenceMatch.hasMatch()) {
            if (inCodeBlock) {
                closeCode();
            } else {
                closeAllBlocks();
                inCodeBlock = true;
                codeLanguage = fenceMatch.captured(2).trimmed();
            }
            continue;
        }
        if (inCodeBlock) {
            codeLines.append(line);
            continue;
        }

        // ── 空行 ────────────────────────────────────────────────────────────
        if (trimmed.isEmpty()) {
            closeAllBlocks();
            continue;
        }

        // ── 水平线 ──────────────────────────────────────────────────────────
        if (trimmed == QLatin1String("---") || trimmed == QLatin1String("***") ||
            trimmed == QLatin1String("___")) {
            closeAllBlocks();
            html += QStringLiteral("<hr/>\n");
            continue;
        }

        // ── 标题 ────────────────────────────────────────────────────────────
        static const QRegularExpression headingPattern(QStringLiteral("^(#{1,6})\\s+(.*)$"));
        const QRegularExpressionMatch headingMatch = headingPattern.match(trimmed);
        if (headingMatch.hasMatch()) {
            closeAllBlocks();
            const int level = static_cast<int>(headingMatch.captured(1).size());
            html += QStringLiteral("<h%1>%2</h%1>\n")
                        .arg(QString::number(level), renderInline(headingMatch.captured(2)));
            continue;
        }

        // ── 引用块 ──────────────────────────────────────────────────────────
        if (trimmed.startsWith(QLatin1Char('>'))) {
            closeParagraph();
            closeList();
            if (!inQuote) {
                html += QStringLiteral("<blockquote>\n");
                inQuote = true;
            }
            html += QStringLiteral("<p>") +
                    renderInline(trimmed.mid(1).trimmed()) + QStringLiteral("</p>\n");
            continue;
        }
        closeQuote();

        // ── 列表 ────────────────────────────────────────────────────────────
        static const QRegularExpression bulletPattern(QStringLiteral("^(\\s*)[-*+]\\s+(.*)$"));
        static const QRegularExpression orderedPattern(QStringLiteral("^(\\s*)(\\d+)[.)]\\s+(.*)$"));
        const QRegularExpressionMatch bulletMatch = bulletPattern.match(line);
        const QRegularExpressionMatch orderedMatch = orderedPattern.match(line);

        if (bulletMatch.hasMatch() || orderedMatch.hasMatch()) {
            closeParagraph();
            const bool ordered = orderedMatch.hasMatch();
            const QString tag = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
            const int indent = ordered ? static_cast<int>(orderedMatch.captured(1).size())
                                       : static_cast<int>(bulletMatch.captured(1).size());
            QString content = ordered ? orderedMatch.captured(3) : bulletMatch.captured(2);

            // 单层缩进降级为嵌套列表；更深层不再处理（有意取舍）。
            const int level = indent >= 2 ? 1 : 0;
            if (inList && level == 1 && listTag == tag) {
                html += QStringLiteral("<li class=\"nested\">");
            } else {
                closeList();
                html += QStringLiteral("<%1>\n").arg(tag);
                inList = true;
                listTag = tag;
                html += QStringLiteral("<li>");
            }
            // taskPrefix 会就地去掉 `[x]` 前缀，避免渲染成"☑ [x] done"。
            html += taskPrefix(content, &content);
            html += renderInline(content);
            html += QStringLiteral("</li>\n");
            continue;
        }
        closeList();

        // ── 管道表格 ────────────────────────────────────────────────────────
        if (trimmed.contains(QLatin1Char('|')) && index + 1 < lines.size() &&
            isTableSeparator(lines.at(index + 1))) {
            closeAllBlocks();
            const QStringList headers = splitTableRow(trimmed);
            html += QStringLiteral("<table class=\"md-table\">\n<thead><tr>");
            for (const QString &header : headers) {
                html += QStringLiteral("<th>") + renderInline(header.trimmed()) +
                        QStringLiteral("</th>");
            }
            html += QStringLiteral("</tr></thead>\n<tbody>\n");

            qsizetype rowIndex = index + 2;
            while (rowIndex < lines.size() && lines.at(rowIndex).contains(QLatin1Char('|')) &&
                   !lines.at(rowIndex).trimmed().isEmpty()) {
                const QStringList cells = splitTableRow(lines.at(rowIndex));
                html += QStringLiteral("<tr>");
                for (const QString &cell : cells) {
                    html += QStringLiteral("<td>") + renderInline(cell.trimmed()) +
                            QStringLiteral("</td>");
                }
                html += QStringLiteral("</tr>\n");
                ++rowIndex;
            }
            html += QStringLiteral("</tbody></table>\n");
            index = rowIndex - 1;  // 跳过已消费的行
            continue;
        }

        // ── 普通段落 ────────────────────────────────────────────────────────
        paragraphLines.append(trimmed);
        inParagraph = true;
    }

    // 收尾：文件结束时所有悬空块都要闭合。
    closeCode();
    closeAllBlocks();

    return html;
}

QString Markdown::styleSheet(const MarkdownStyle &style) {
    const auto family = [](const QString &name) {
        // Qt 富文本的 font-family 需要带引号，否则含空格的族名会解析失败。
        return QStringLiteral("'%1'").arg(name);
    };

    QString css;
    css += QStringLiteral("body, p, li, td, th { color: %1; font-family: %2; font-size: %3px; "
                          "line-height: 150%%; }\n")
               .arg(style.foreground.name(), family(style.sansFamily))
               .arg(style.baseFontPx);

    // 标题：h1/h2 逐级放大，h3 及以下同正文尺码，靠字重区分（与设计规范一致）。
    css += QStringLiteral("h1 { font-size: %1px; font-weight: 600; margin: 16px 0 8px 0; }\n")
               .arg(style.baseFontPx + 4);
    css += QStringLiteral("h2 { font-size: %1px; font-weight: 600; margin: 14px 0 6px 0; }\n")
               .arg(style.baseFontPx + 2);
    css += QStringLiteral("h3, h4 { font-size: %1px; font-weight: 600; margin: 12px 0 6px 0; }\n")
               .arg(style.baseFontPx);
    css += QStringLiteral("h5 { font-size: %1px; font-weight: 500; margin: 12px 0 6px 0; }\n")
               .arg(style.baseFontPx);
    css += QStringLiteral("h6 { font-size: %1px; font-weight: 400; margin: 12px 0 6px 0; }\n")
               .arg(style.baseFontPx);

    css += QStringLiteral("p { margin: 6px 0; }\n");
    css += QStringLiteral("a { color: %1; text-decoration: none; }\n").arg(style.link.name());

    // 行内代码：等宽 + 弱底色，尺码比正文小一档。
    css += QStringLiteral("code.inline { font-family: %1; font-size: %2px; "
                          "background-color: %3; color: %4; }\n")
               .arg(family(style.monoFamily))
               .arg(style.baseFontPx - 2)
               .arg(style.codeBackground.name(), style.foreground.name());

    // 代码块外壳：独立卡片 + 语言标签。
    css += QStringLiteral("div.code-block { background-color: %1; margin: 8px 0; }\n")
               .arg(style.codeBackground.name());
    css += QStringLiteral("div.code-lang { color: %1; font-family: %2; font-size: %3px; "
                          "margin: 0 0 2px 0; }\n")
               .arg(style.foregroundSubtlest.name(), family(style.monoFamily))
               .arg(style.baseFontPx - 2);
    css += QStringLiteral("pre.code { font-family: %1; font-size: %2px; color: %3; "
                          "margin: 0; line-height: 140%%; }\n")
               .arg(family(style.monoFamily))
               .arg(style.codeFontPx)
               .arg(style.foreground.name());

    // 引用块：左侧色条 + 弱化文字。
    css += QStringLiteral("blockquote { border-left: 2px solid %1; margin: 8px 0; "
                          "padding-left: 10px; color: %2; }\n")
               .arg(style.quoteBar.name(), style.foregroundSubtle.name());

    css += QStringLiteral("ul, ol { margin: 6px 0; }\n");
    css += QStringLiteral("li { margin: 2px 0; }\n");
    css += QStringLiteral("li.nested { margin-left: 16px; }\n");
    css += QStringLiteral("hr { border: 0; border-top: 1px solid %1; margin: 12px 0; }\n")
               .arg(style.border.name());

    // 表格：只画横向分隔线，避免网格感过重。
    css += QStringLiteral("table.md-table { border-collapse: collapse; margin: 8px 0; }\n");
    css += QStringLiteral("th { background-color: %1; font-weight: 600; "
                          "border-bottom: 1px solid %2; padding: 4px 8px; }\n")
               .arg(style.tableHeader.name(), style.border.name());
    css += QStringLiteral("td { border-bottom: 1px solid %1; padding: 4px 8px; }\n")
               .arg(style.border.name());

    return css;
}

QString Markdown::toPlainPreview(const QString &markdown, int maxChars) {
    // 会话列表只需要一行无标记的文本。
    QString text = markdown;
    text.remove(QRegularExpression(QStringLiteral("```[\\s\\S]*?```")));
    text.remove(QRegularExpression(QStringLiteral("`([^`]*)`")));
    // 链接与图片只去掉 URL，**保留链接文字**——文字才是内容，
    // 整段删掉会让预览丢掉有效信息。
    text.replace(QRegularExpression(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)")),
                 QLatin1String("\\1"));
    text.replace(QRegularExpression(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)")),
                 QLatin1String("\\1"));
    text.remove(QRegularExpression(QStringLiteral("[#*_>~]")));
    text = text.simplified();
    if (maxChars > 0 && text.size() > maxChars) {
        text = text.left(maxChars) + QStringLiteral("…");
    }
    return text;
}

}  // namespace zcode::ui
