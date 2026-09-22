#include "ui/SyntaxHighlighter.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>

namespace zcode::ui {
namespace {

/// 注释风格。
struct CommentStyle {
    QString lineToken;    ///< 行注释前缀，如 `//`、`#`（空表示没有）
    bool blockComment = false;  ///< 是否有 `/* … */`
    bool pythonTriple = false;  ///< 是否有 `"""` / `'''` 三引号字符串
};

/// 一门语言的词法配置。
struct LanguageRules {
    QSet<QString> keywords;
    QSet<QString> types;
    CommentStyle comments;
    bool preprocessor = false;  ///< 是否有 `#include` 这类预处理指令
};

QSet<QString> toSet(const QStringList &values) {
    QSet<QString> result;
    for (const QString &value : values) {
        result.insert(value);
    }
    return result;
}

const QHash<QString, LanguageRules> &rulesTable() {
    static const QHash<QString, LanguageRules> table = [] {
        QHash<QString, LanguageRules> result;

        // C / C++ / 类 C 系（Java、C#、Rust、Go、JS 的注释风格都兼容）
        LanguageRules cpp;
        cpp.keywords = toSet({
            QStringLiteral("alignas"), QStringLiteral("alignof"), QStringLiteral("asm"),
            QStringLiteral("auto"), QStringLiteral("break"), QStringLiteral("case"),
            QStringLiteral("catch"), QStringLiteral("class"), QStringLiteral("concept"),
            QStringLiteral("const"), QStringLiteral("consteval"), QStringLiteral("constexpr"),
            QStringLiteral("constinit"), QStringLiteral("const_cast"), QStringLiteral("continue"),
            QStringLiteral("co_await"), QStringLiteral("co_return"), QStringLiteral("co_yield"),
            QStringLiteral("decltype"), QStringLiteral("default"), QStringLiteral("delete"),
            QStringLiteral("do"), QStringLiteral("dynamic_cast"), QStringLiteral("else"),
            QStringLiteral("enum"), QStringLiteral("explicit"), QStringLiteral("export"),
            QStringLiteral("extern"), QStringLiteral("final"), QStringLiteral("for"),
            QStringLiteral("friend"), QStringLiteral("goto"), QStringLiteral("if"),
            QStringLiteral("inline"), QStringLiteral("mutable"), QStringLiteral("namespace"),
            QStringLiteral("new"), QStringLiteral("noexcept"), QStringLiteral("operator"),
            QStringLiteral("override"), QStringLiteral("private"), QStringLiteral("protected"),
            QStringLiteral("public"), QStringLiteral("register"), QStringLiteral("reinterpret_cast"),
            QStringLiteral("requires"), QStringLiteral("return"), QStringLiteral("sizeof"),
            QStringLiteral("static"), QStringLiteral("static_assert"), QStringLiteral("static_cast"),
            QStringLiteral("struct"), QStringLiteral("switch"), QStringLiteral("template"),
            QStringLiteral("this"), QStringLiteral("thread_local"), QStringLiteral("throw"),
            QStringLiteral("try"), QStringLiteral("typedef"), QStringLiteral("typeid"),
            QStringLiteral("typename"), QStringLiteral("union"), QStringLiteral("using"),
            QStringLiteral("virtual"), QStringLiteral("volatile"), QStringLiteral("while"),
            QStringLiteral("nullptr"), QStringLiteral("true"), QStringLiteral("false"),
        });
        cpp.types = toSet({
            QStringLiteral("bool"), QStringLiteral("char"), QStringLiteral("char8_t"),
            QStringLiteral("char16_t"), QStringLiteral("char32_t"), QStringLiteral("double"),
            QStringLiteral("float"), QStringLiteral("int"), QStringLiteral("long"),
            QStringLiteral("short"), QStringLiteral("signed"), QStringLiteral("unsigned"),
            QStringLiteral("void"), QStringLiteral("wchar_t"), QStringLiteral("size_t"),
            QStringLiteral("ssize_t"), QStringLiteral("uint8_t"), QStringLiteral("uint16_t"),
            QStringLiteral("uint32_t"), QStringLiteral("uint64_t"), QStringLiteral("int8_t"),
            QStringLiteral("int16_t"), QStringLiteral("int32_t"), QStringLiteral("int64_t"),
            QStringLiteral("string"), QStringLiteral("vector"), QStringLiteral("map"),
            QStringLiteral("set"), QStringLiteral("optional"), QStringLiteral("unique_ptr"),
            QStringLiteral("shared_ptr"),
        });
        cpp.comments = {QStringLiteral("//"), true, false};
        cpp.preprocessor = true;
        result.insert(QStringLiteral("cpp"), cpp);
        result.insert(QStringLiteral("c"), cpp);
        result.insert(QStringLiteral("java"), cpp);
        result.insert(QStringLiteral("csharp"), cpp);
        result.insert(QStringLiteral("rust"), cpp);
        result.insert(QStringLiteral("go"), cpp);
        result.insert(QStringLiteral("swift"), cpp);
        result.insert(QStringLiteral("kotlin"), cpp);
        result.insert(QStringLiteral("cmake"),
                      LanguageRules{toSet({QStringLiteral("if"), QStringLiteral("else"),
                                           QStringLiteral("endif"), QStringLiteral("foreach"),
                                           QStringLiteral("endforeach"), QStringLiteral("function"),
                                           QStringLiteral("endfunction"), QStringLiteral("macro"),
                                           QStringLiteral("endmacro"), QStringLiteral("set"),
                                           QStringLiteral("option"), QStringLiteral("project"),
                                           QStringLiteral("add_executable"),
                                           QStringLiteral("add_library"),
                                           QStringLiteral("target_link_libraries"),
                                           QStringLiteral("find_package"), QStringLiteral("include"),
                                           QStringLiteral("return"), QStringLiteral("while")}),
                                   toSet({QStringLiteral("ON"), QStringLiteral("OFF"),
                                          QStringLiteral("TRUE"), QStringLiteral("FALSE"),
                                          QStringLiteral("PRIVATE"), QStringLiteral("PUBLIC"),
                                          QStringLiteral("INTERFACE"), QStringLiteral("REQUIRED")}),
                                   {QStringLiteral("#"), true, false}, false});

        // JavaScript / TypeScript
        LanguageRules js;
        js.keywords = toSet({
            QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("break"),
            QStringLiteral("case"), QStringLiteral("catch"), QStringLiteral("class"),
            QStringLiteral("const"), QStringLiteral("continue"), QStringLiteral("debugger"),
            QStringLiteral("default"), QStringLiteral("delete"), QStringLiteral("do"),
            QStringLiteral("else"), QStringLiteral("export"), QStringLiteral("extends"),
            QStringLiteral("finally"), QStringLiteral("for"), QStringLiteral("from"),
            QStringLiteral("function"), QStringLiteral("get"), QStringLiteral("if"),
            QStringLiteral("import"), QStringLiteral("in"), QStringLiteral("instanceof"),
            QStringLiteral("let"), QStringLiteral("new"), QStringLiteral("of"),
            QStringLiteral("return"), QStringLiteral("set"), QStringLiteral("static"),
            QStringLiteral("super"), QStringLiteral("switch"), QStringLiteral("this"),
            QStringLiteral("throw"), QStringLiteral("try"), QStringLiteral("typeof"),
            QStringLiteral("var"), QStringLiteral("void"), QStringLiteral("while"),
            QStringLiteral("with"), QStringLiteral("yield"), QStringLiteral("null"),
            QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("undefined"),
        });
        js.types = toSet({QStringLiteral("any"), QStringLiteral("boolean"), QStringLiteral("never"),
                          QStringLiteral("number"), QStringLiteral("object"),
                          QStringLiteral("string"), QStringLiteral("symbol"),
                          QStringLiteral("unknown"), QStringLiteral("interface"),
                          QStringLiteral("type"), QStringLiteral("enum"), QStringLiteral("implements"),
                          QStringLiteral("readonly"), QStringLiteral("declare")});
        js.comments = {QStringLiteral("//"), true, false};
        result.insert(QStringLiteral("javascript"), js);
        result.insert(QStringLiteral("typescript"), js);

        // Python
        LanguageRules python;
        python.keywords = toSet({
            QStringLiteral("and"), QStringLiteral("as"), QStringLiteral("assert"),
            QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("break"),
            QStringLiteral("class"), QStringLiteral("continue"), QStringLiteral("def"),
            QStringLiteral("del"), QStringLiteral("elif"), QStringLiteral("else"),
            QStringLiteral("except"), QStringLiteral("finally"), QStringLiteral("for"),
            QStringLiteral("from"), QStringLiteral("global"), QStringLiteral("if"),
            QStringLiteral("import"), QStringLiteral("in"), QStringLiteral("is"),
            QStringLiteral("lambda"), QStringLiteral("nonlocal"), QStringLiteral("not"),
            QStringLiteral("or"), QStringLiteral("pass"), QStringLiteral("raise"),
            QStringLiteral("return"), QStringLiteral("try"), QStringLiteral("while"),
            QStringLiteral("with"), QStringLiteral("yield"), QStringLiteral("None"),
            QStringLiteral("True"), QStringLiteral("False"), QStringLiteral("match"),
            QStringLiteral("case"), QStringLiteral("self"), QStringLiteral("cls"),
        });
        python.types = toSet({QStringLiteral("int"), QStringLiteral("float"), QStringLiteral("str"),
                              QStringLiteral("bool"), QStringLiteral("bytes"), QStringLiteral("list"),
                              QStringLiteral("dict"), QStringLiteral("set"), QStringLiteral("tuple"),
                              QStringLiteral("Any"), QStringLiteral("Optional"),
                              QStringLiteral("List"), QStringLiteral("Dict")});
        python.comments = {QStringLiteral("#"), false, true};
        result.insert(QStringLiteral("python"), python);

        // Shell
        LanguageRules shell;
        shell.keywords = toSet({
            QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"),
            QStringLiteral("elif"), QStringLiteral("fi"), QStringLiteral("for"),
            QStringLiteral("while"), QStringLiteral("until"), QStringLiteral("do"),
            QStringLiteral("done"), QStringLiteral("case"), QStringLiteral("esac"),
            QStringLiteral("function"), QStringLiteral("return"), QStringLiteral("exit"),
            QStringLiteral("local"), QStringLiteral("export"), QStringLiteral("source"),
            QStringLiteral("set"), QStringLiteral("unset"), QStringLiteral("test"),
            QStringLiteral("echo"), QStringLiteral("printf"), QStringLiteral("read"),
        });
        shell.comments = {QStringLiteral("#"), false, false};
        result.insert(QStringLiteral("bash"), shell);
        result.insert(QStringLiteral("sh"), shell);
        result.insert(QStringLiteral("shell"), shell);
        result.insert(QStringLiteral("zsh"), shell);

        // JSON
        LanguageRules json;
        json.keywords = toSet({QStringLiteral("true"), QStringLiteral("false"),
                               QStringLiteral("null")});
        // JSON 没有注释，也没有预处理。
        json.comments = {QString(), false, false};
        result.insert(QStringLiteral("json"), json);

        // SQL
        LanguageRules sql;
        sql.keywords = toSet({
            QStringLiteral("select"), QStringLiteral("from"), QStringLiteral("where"),
            QStringLiteral("insert"), QStringLiteral("into"), QStringLiteral("values"),
            QStringLiteral("update"), QStringLiteral("set"), QStringLiteral("delete"),
            QStringLiteral("create"), QStringLiteral("table"), QStringLiteral("index"),
            QStringLiteral("drop"), QStringLiteral("alter"), QStringLiteral("join"),
            QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("inner"),
            QStringLiteral("outer"), QStringLiteral("on"), QStringLiteral("group"),
            QStringLiteral("order"), QStringLiteral("by"), QStringLiteral("having"),
            QStringLiteral("limit"), QStringLiteral("offset"), QStringLiteral("and"),
            QStringLiteral("or"), QStringLiteral("not"), QStringLiteral("null"),
            QStringLiteral("as"), QStringLiteral("distinct"), QStringLiteral("union"),
        });
        sql.types = toSet({QStringLiteral("int"), QStringLiteral("integer"),
                           QStringLiteral("text"), QStringLiteral("varchar"),
                           QStringLiteral("real"), QStringLiteral("blob"),
                           QStringLiteral("primary"), QStringLiteral("key"),
                           QStringLiteral("foreign"), QStringLiteral("references")});
        // `--` 行注释 + `/* */`；`#` 在 MySQL 里也是注释，但会与标识符冲突，不启用。
        sql.comments = {QStringLiteral("--"), true, false};
        result.insert(QStringLiteral("sql"), sql);

        return result;
    }();
    return table;
}

/// 通用兜底：按 C 系风格处理，关键字表取 C++ 的子集。
const LanguageRules &fallbackRules() {
    static const LanguageRules rules = [] {
        LanguageRules result;
        result.keywords = toSet({QStringLiteral("if"), QStringLiteral("else"),
                                 QStringLiteral("for"), QStringLiteral("while"),
                                 QStringLiteral("return"), QStringLiteral("break"),
                                 QStringLiteral("continue"), QStringLiteral("class"),
                                 QStringLiteral("function"), QStringLiteral("def"),
                                 QStringLiteral("import"), QStringLiteral("export"),
                                 QStringLiteral("try"), QStringLiteral("catch"),
                                 QStringLiteral("true"), QStringLiteral("false"),
                                 QStringLiteral("null"), QStringLiteral("nil"),
                                 QStringLiteral("None"), QStringLiteral("True"),
                                 QStringLiteral("False")});
        result.comments = {QStringLiteral("//"), true, false};
        return result;
    }();
    return rules;
}

QString escapeHtml(const QString &text) {
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    escaped.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    escaped.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return escaped;
}

/// 生成一个着色 span：class 供识别/测试，内联 style 负责实际上色。
QString span(const QString &cssClass, const QString &text, const QColor &color,
             bool bold = false, bool italic = false) {
    if (text.isEmpty()) {
        return {};
    }
    QString style;
    if (color.isValid()) {
        style += QStringLiteral("color:%1;").arg(color.name());
    }
    if (bold) {
        style += QStringLiteral("font-weight:bold;");
    }
    if (italic) {
        style += QStringLiteral("font-style:italic;");
    }
    const QString attribute =
        style.isEmpty() ? QString() : QStringLiteral(" style=\"%1\"").arg(style);
    return QStringLiteral("<span class=\"%1\"%2>%3</span>")
        .arg(cssClass, attribute, escapeHtml(text));
}

bool isIdentifierStart(QChar character) {
    return character.isLetter() || character == QLatin1Char('_') ||
           character == QLatin1Char('$');
}

bool isIdentifierPart(QChar character) {
    return character.isLetterOrNumber() || character == QLatin1Char('_') ||
           character == QLatin1Char('$');
}

bool isDigit(QChar character) {
    return character.isDigit();
}

/// 该标识符后面（跳过空白）是否紧跟 '('：据此判为函数调用。
bool looksLikeCall(const QString &code, qsizetype from) {
    qsizetype index = from;
    while (index < code.size() && (code.at(index) == QLatin1Char(' ') ||
                                   code.at(index) == QLatin1Char('\t'))) {
        ++index;
    }
    return index < code.size() && code.at(index) == QLatin1Char('(');
}

/// 尝试匹配一个三引号字符串（Python）。返回结束位置（不含）；不匹配返回 -1。
qsizetype matchTripleQuote(const QString &code, qsizetype start, QChar quote) {
    if (start + 2 >= code.size() || code.at(start + 1) != quote ||
        code.at(start + 2) != quote) {
        return -1;
    }
    qsizetype index = start + 3;
    while (index + 2 < code.size()) {
        if (code.at(index) == quote && code.at(index + 1) == quote &&
            code.at(index + 2) == quote) {
            return index + 3;
        }
        // 反斜杠转义：跳过下一个字符，避免把 \"\"\" 当成结束。
        if (code.at(index) == QLatin1Char('\\')) {
            index += 2;
            continue;
        }
        ++index;
    }
    return code.size();
}

}  // namespace

QStringList SyntaxHighlighter::knownLanguages() {
    QStringList languages = rulesTable().keys();
    languages.sort();
    return languages;
}

QString SyntaxHighlighter::normalizeLanguage(const QString &language) {
    QString normalized = language.trimmed().toLower();
    // 去掉 ``` 后面常见的附加信息（```cpp title="x.cpp"）。
    const qsizetype space = normalized.indexOf(QLatin1Char(' '));
    if (space > 0) {
        normalized = normalized.left(space);
    }

    static const QHash<QString, QString> aliases = {
        {QStringLiteral("c++"), QStringLiteral("cpp")},
        {QStringLiteral("cxx"), QStringLiteral("cpp")},
        {QStringLiteral("cc"), QStringLiteral("cpp")},
        {QStringLiteral("h"), QStringLiteral("cpp")},
        {QStringLiteral("hpp"), QStringLiteral("cpp")},
        {QStringLiteral("objc"), QStringLiteral("cpp")},
        {QStringLiteral("js"), QStringLiteral("javascript")},
        {QStringLiteral("jsx"), QStringLiteral("javascript")},
        {QStringLiteral("mjs"), QStringLiteral("javascript")},
        {QStringLiteral("ts"), QStringLiteral("typescript")},
        {QStringLiteral("tsx"), QStringLiteral("typescript")},
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("python3"), QStringLiteral("python")},
        {QStringLiteral("console"), QStringLiteral("bash")},
        {QStringLiteral("shell-session"), QStringLiteral("bash")},
        {QStringLiteral("cs"), QStringLiteral("csharp")},
        {QStringLiteral("rs"), QStringLiteral("rust")},
        {QStringLiteral("golang"), QStringLiteral("go")},
    };
    return aliases.value(normalized, normalized);
}

bool SyntaxHighlighter::isKnownLanguage(const QString &language) {
    return rulesTable().contains(normalizeLanguage(language));
}

QString SyntaxHighlighter::languageForFile(const QString &path) {
    // 按扩展名映射到内部分词语言。用扩展名而不是 MIME：MIME 对
    // .h/.cpp/.m 这类源码给的是很粗的类别，分不出该用哪套关键字表。
    static const QHash<QString, QString> bySuffix = {
        {QStringLiteral("c"), QStringLiteral("c")},
        {QStringLiteral("cc"), QStringLiteral("cpp")},
        {QStringLiteral("cpp"), QStringLiteral("cpp")},
        {QStringLiteral("cxx"), QStringLiteral("cpp")},
        {QStringLiteral("h"), QStringLiteral("cpp")},
        {QStringLiteral("hh"), QStringLiteral("cpp")},
        {QStringLiteral("hpp"), QStringLiteral("cpp")},
        {QStringLiteral("hxx"), QStringLiteral("cpp")},
        {QStringLiteral("m"), QStringLiteral("cpp")},
        {QStringLiteral("mm"), QStringLiteral("cpp")},
        {QStringLiteral("java"), QStringLiteral("java")},
        {QStringLiteral("cs"), QStringLiteral("csharp")},
        {QStringLiteral("rs"), QStringLiteral("rust")},
        {QStringLiteral("go"), QStringLiteral("go")},
        {QStringLiteral("swift"), QStringLiteral("swift")},
        {QStringLiteral("kt"), QStringLiteral("kotlin")},
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("pyw"), QStringLiteral("python")},
        {QStringLiteral("js"), QStringLiteral("javascript")},
        {QStringLiteral("mjs"), QStringLiteral("javascript")},
        {QStringLiteral("cjs"), QStringLiteral("javascript")},
        {QStringLiteral("jsx"), QStringLiteral("javascript")},
        {QStringLiteral("ts"), QStringLiteral("typescript")},
        {QStringLiteral("tsx"), QStringLiteral("typescript")},
        {QStringLiteral("sh"), QStringLiteral("bash")},
        {QStringLiteral("bash") , QStringLiteral("bash")},
        {QStringLiteral("zsh"), QStringLiteral("bash")},
        {QStringLiteral("json"), QStringLiteral("json")},
        {QStringLiteral("sql"), QStringLiteral("sql")},
        {QStringLiteral("cmake"), QStringLiteral("cmake")},
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) {
        return {};
    }
    // CMakeLists.txt 没有扩展名，单独认一下（它是最常看的构建文件之一）。
    if (QFileInfo(path).fileName().compare(QLatin1String("CMakeLists.txt"),
                                           Qt::CaseInsensitive) == 0) {
        return QStringLiteral("cmake");
    }
    return bySuffix.value(suffix);
}

QList<Token> SyntaxHighlighter::tokenize(const QString &code, const QString &language) {
    QList<Token> tokens;
    if (code.isEmpty()) {
        return tokens;
    }

    const LanguageRules &rules = rulesTable().value(normalizeLanguage(language),
                                                    fallbackRules());
    const QString lineComment = rules.comments.lineToken;

    const auto record = [&tokens](qsizetype start, qsizetype end, TokenKind kind) {
        if (end > start) {
            tokens.append(Token{static_cast<int>(start), static_cast<int>(end - start), kind});
        }
    };

    qsizetype index = 0;
    const qsizetype size = code.size();
    // 行首判定：`#` 只在行首才是预处理指令，`a # b` 不是。
    bool atLineStart = true;

    while (index < size) {
        const QChar current = code.at(index);

        // ── 三引号字符串（Python）─────────────────────────────────────────
        if (rules.comments.pythonTriple &&
            (current == QLatin1Char('"') || current == QLatin1Char('\''))) {
            const qsizetype end = matchTripleQuote(code, index, current);
            if (end > 0) {
                record(index, end, TokenKind::String);
                for (qsizetype cursor = index; cursor < end; ++cursor) {
                    if (code.at(cursor) == QLatin1Char('\n')) {
                        atLineStart = true;
                    }
                }
                index = end;
                continue;
            }
        }

        // ── 行注释 ────────────────────────────────────────────────────────
        if (!lineComment.isEmpty() && code.mid(index, lineComment.size()) == lineComment) {
            qsizetype end = code.indexOf(QLatin1Char('\n'), index);
            if (end < 0) {
                end = size;
            }
            record(index, end, TokenKind::Comment);
            index = end;
            continue;
        }

        // ── 块注释 ────────────────────────────────────────────────────────
        if (rules.comments.blockComment && current == QLatin1Char('/') && index + 1 < size &&
            code.at(index + 1) == QLatin1Char('*')) {
            qsizetype end = code.indexOf(QLatin1String("*/"), index + 2);
            end = (end < 0) ? size : end + 2;
            record(index, end, TokenKind::Comment);
            index = end;
            continue;
        }

        // ── 预处理指令（整行，含续行）─────────────────────────────────────
        if (rules.preprocessor && current == QLatin1Char('#') && atLineStart) {
            qsizetype end = code.indexOf(QLatin1Char('\n'), index);
            if (end < 0) {
                end = size;
            }
            // 续行（反斜杠结尾）要一起吃掉，否则第二行会被当成普通代码。
            while (end < size && end > 0 && code.at(end - 1) == QLatin1Char('\\')) {
                const qsizetype next = code.indexOf(QLatin1Char('\n'), end + 1);
                if (next < 0) {
                    end = size;
                    break;
                }
                end = next;
            }
            record(index, end, TokenKind::Preprocessor);
            index = end;
            continue;
        }

        // ── 字符串 ────────────────────────────────────────────────────────
        if (current == QLatin1Char('"') || current == QLatin1Char('\'') ||
            current == QLatin1Char('`')) {
            qsizetype cursor = index + 1;
            while (cursor < size) {
                const QChar character = code.at(cursor);
                if (character == QLatin1Char('\\') && cursor + 1 < size) {
                    cursor += 2;
                    continue;
                }
                if (character == current) {
                    ++cursor;
                    break;
                }
                // 单双引号字符串不跨行（模板串可以）：跨行通常是漏了引号，
                // 继续吃下去会把后面整段代码都染成字符串。
                if (character == QLatin1Char('\n') && current != QLatin1Char('`')) {
                    break;
                }
                ++cursor;
            }
            record(index, cursor, TokenKind::String);
            index = cursor;
            atLineStart = false;
            continue;
        }

        // ── 数字 ──────────────────────────────────────────────────────────
        if (isDigit(current)) {
            qsizetype cursor = index;
            while (cursor < size &&
                   (isIdentifierPart(code.at(cursor)) || code.at(cursor) == QLatin1Char('.') ||
                    code.at(cursor) == QLatin1Char('\''))) {
                ++cursor;
            }
            record(index, cursor, TokenKind::Number);
            index = cursor;
            atLineStart = false;
            continue;
        }

        // ── 标识符 / 关键字 ───────────────────────────────────────────────
        if (isIdentifierStart(current)) {
            qsizetype cursor = index;
            while (cursor < size && isIdentifierPart(code.at(cursor))) {
                ++cursor;
            }
            const QString word = code.mid(index, cursor - index);
            // 关键字表里是小写；Python 的 None/True/False 与 SQL 的关键字
            // 习惯大写，所以两种都查。
            const QString lowered = word.toLower();
            const bool isKeyword = rules.keywords.contains(word) || rules.keywords.contains(lowered);
            const bool isType = !isKeyword && (rules.types.contains(word) ||
                                               rules.types.contains(lowered));

            if (isKeyword) {
                record(index, cursor, TokenKind::Keyword);
            } else if (isType) {
                record(index, cursor, TokenKind::Type);
            } else if (looksLikeCall(code, cursor)) {
                record(index, cursor, TokenKind::Function);
            }
            index = cursor;
            atLineStart = false;
            continue;
        }

        // ── 其它字符 ──────────────────────────────────────────────────────
        atLineStart = (current == QLatin1Char('\n'));
        ++index;
    }

    return tokens;
}

QString SyntaxHighlighter::highlight(const QString &code, const QString &language) {
    return highlight(code, language, SyntaxColors{});
}

QString SyntaxHighlighter::highlight(const QString &code, const QString &language,
                                     const SyntaxColors &colors) {
    if (code.isEmpty()) {
        return {};
    }

    const QList<Token> tokens = tokenize(code, language);

    QString html;
    html.reserve(code.size() * 2);

    qsizetype cursor = 0;
    const auto flushPlain = [&html, &code, &cursor](qsizetype end) {
        if (end > cursor) {
            html += escapeHtml(code.mid(cursor, end - cursor));
        }
    };

    for (const Token &token : tokens) {
        // token 之间（以及文档末尾）的空隙就是 Plain 文本。
        flushPlain(token.start);

        const QString text = code.mid(token.start, token.length);
        switch (token.kind) {
            case TokenKind::Keyword:
                html += span(QStringLiteral("tok-keyword"), text, colors.keyword, true);
                break;
            case TokenKind::String:
                html += span(QStringLiteral("tok-string"), text, colors.string);
                break;
            case TokenKind::Comment:
                html += span(QStringLiteral("tok-comment"), text, colors.comment, false, true);
                break;
            case TokenKind::Number:
                html += span(QStringLiteral("tok-number"), text, colors.number);
                break;
            case TokenKind::Type:
                html += span(QStringLiteral("tok-type"), text, colors.type);
                break;
            case TokenKind::Function:
                html += span(QStringLiteral("tok-function"), text, colors.function);
                break;
            case TokenKind::Preprocessor:
                html += span(QStringLiteral("tok-preproc"), text, colors.preprocessor);
                break;
            case TokenKind::Plain:
                html += escapeHtml(text);
                break;
        }
        cursor = token.start + token.length;
    }
    flushPlain(code.size());

    return html;
}

}  // namespace zcode::ui
