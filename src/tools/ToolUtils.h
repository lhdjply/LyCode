// LyCode — 内置工具共享辅助（内部头，非公共契约）
//
// 这里的函数只服务于 src/tools 下的实现，刻意不放进 Tool.h：
// 它们描述的是"怎么实现一个工具"的细节（路径归一、glob 展开、输出预算），
// 而不是"工具是什么"的契约。公共头保持稳定，实现细节可以自由演进。
//
// 为什么要集中在一处：
//   * 路径安全（防 `../` 逃逸）必须每个落盘工具都走同一条代码路径，
//     分散实现迟早会出现某个工具忘记校验。
//   * glob→regex、输出截断这些逻辑在 Glob/Grep/Bash/Read 之间重复，
//     复制粘贴会带来行为漂移。
//
// 实现位置说明：这些函数**实现在 Tool.cpp**，而不是单独的 ToolUtils.cpp。
// 工具层因此只有一个"必定被链接进去"的基础翻译单元，测试/验证程序只需要
// 链接 Tool.cpp 就能拿到全部共享辅助，不必再维护一份源文件清单。
#pragma once

#include <QByteArray>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "tools/Tool.h"

namespace lycode::toolutil {

/// 目录名跳过表：这些目录只包含构建产物或依赖，递归扫描它们纯属浪费
/// （node_modules 常常有几十万文件）。Glob/Grep 会跳过并在 metadata 里说明。
bool shouldSkipDirectory(const QString &dirName);

/// 二进制内容判定：含 NUL 字节即视为二进制。
/// 用 NUL 而不是"不可打印字符比例"是因为后者在不同编码下误判率高，
/// 而 NUL 在文本文件里几乎不可能出现。
bool looksBinary(const QByteArray &bytes);

/// 把 glob 转成锚定的 QRegularExpression。
/// 支持 `**`（跨目录）、`*`（不跨 `/`）、`?`、`{a,b}`、`[abc]`。
/// 非法 pattern 返回一个永不匹配的正则（而不是抛异常）。
QRegularExpression globToRegex(const QString &pattern, bool caseInsensitive = false);

/// 去掉 glob 的开头 `./`，并把 '\\' 统一成 '/'。
QString normalizeGlob(const QString &pattern);

/// 判断某个（相对基目录的、以 '/' 分隔的）路径是否匹配 glob。
bool globMatches(const QRegularExpression &regex, const QString &relativePath);

/// 路径前缀拼接，保证不会出现双斜杠（如 base="/a/" + "b" → "/a/b"）。
QString joinPath(const QString &base, const QString &relative);

/// 该路径是否位于 root 之内（root 自身算在内）。
/// 用带分隔符的前缀比较，避免 `/foo/bar` 被 `/foo/b` 误判为包含。
bool pathWithin(const QString &root, const QString &absolutePath);

/// 命令行首词（用于生成 `always allow Bash(make:*)` 这类规则）。
QString commandFirstWord(const QString &command);

/// 目录前缀（用于生成 `always allow Write(/src)` 这类规则）。
QString directoryPrefix(const QString &path);

/// 让子进程成为独立会话的组长（Unix 下 setsid）。
///
/// 命令可能自己再 fork（`make -j` 会拉起一堆子进程），只 kill 直接子进程
/// 会留下孤儿继续占着端口或文件锁。建立独立进程组后 killProcessGroup 能命中整组。
/// 由 BashTool 与 BackgroundTaskRegistry 共用——两处各写一份迟早会漂移。
void configureProcessGroup(QProcess *process);

/// 终止整个进程组。SIGKILL 而不是 SIGTERM：工具被取消/超时/后台任务被停止后
/// 必须立即释放资源，不做"优雅退出"协商（模型可以自己再发一条命令收尾）。
void killProcessGroup(QProcess *process);

/// 把字符串包成 shell 单引号字面量（内含的单引号按 '\'' 转义）。
/// 用于把路径安全地拼进 shell 重定向表达式，避免空格或特殊字符改变语义。
QString shellSingleQuote(const QString &value);

/// 参数日志用：把敏感/超长内容截断到 200 字符，避免把整篇文件写进日志。
QString redactForLog(const QString &value, int maxChars = 200);

/// 只读模式（plan / ask）下是否必须拒绝这次调用。
/// 判据只有一条：readOnly 且该工具的副作用范围属于写入集
/// （Workspace/Git/System）。绝不按工具名判断。
bool shouldRejectForReadOnly(const ToolMetadata &metadata, const ToolContext &context);

/// 只读模式下的统一拒绝结果（errorCode = read_only_mode）。
ToolResult readOnlyModeFailure(const ToolMetadata &metadata);

/// 原子写文件：QSaveFile 先写临时文件再 rename。
/// 这样"写到一半进程被杀"不会留下半个文件，也不会破坏原文件。
bool writeFileAtomic(const QString &absolutePath, const QByteArray &bytes, QString *errorOut);

/// 扩展名 → mime type。只覆盖工具真正会特殊处理的类型，其余按文本处理。
QString mimeTypeForPath(const QString &path);

/// 去掉字符串末尾的换行符。
/// 行式输出（Read 的行号正文、Glob/Grep 的结果行）末尾的 '\n' 只是分隔符：
/// 保留它会让 `output.split('\n')` 多出一个空元素，UI 也会多出一行空白。
QString chompTrailingNewlines(QString text);

/// 字节预算累加器。
///
/// 工具的 maxOutputBytes 是硬上限：宁可截断也不能让一次 `cat 大文件` 把
/// 整个会话的内存和 UI 拖垮。append() 在超预算后只统计不再存储，
/// 这样 metadata 里的字节数仍是"真实看到多少"，而 text() 是"保留了多少"。
class OutputBudget {
public:
    explicit OutputBudget(qint64 maxBytes);

    void append(const QString &chunk);
    void appendBytes(const QByteArray &bytes);
    /// 追加一行（自动补 '\n'）。
    void appendLine(const QString &line);

    /// 进入累加器的字节总量（含被丢弃的部分）。
    qint64 totalBytes() const { return totalBytes_; }
    bool truncated() const { return truncated_; }
    int maxBytes() const { return static_cast<int>(maxBytes_); }

    /// 保留的内容；发生截断时追加一行明确的提示。
    QString text() const;
    bool isEmpty() const { return kept_.isEmpty(); }

private:
    QByteArray kept_;
    qint64 maxBytes_ = 0;
    qint64 totalBytes_ = 0;
    bool truncated_ = false;
};

/// 把可能被 OutputBudget 硬切开的 UTF-8 尾巴修掉，避免尾部出现半个字符。
QByteArray trimIncompleteUtf8(QByteArray bytes);

}  // namespace lycode::toolutil
