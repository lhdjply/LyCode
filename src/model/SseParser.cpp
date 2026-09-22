// ZCode Qt — Server-Sent Events 解析器实现
//
// 实现要点（与 npm 版行为对齐）：
//   * 帧分隔符是空行（\n\n / \r\n\r\n），不是单个换行；
//   * 全程按字节缓冲：UTF-8 多字节字符可能被网络分块切开，先转 QString 会乱码；
//   * `data:` 多行用 \n 拼接；`event:` 缺省为 "message"；以 `:` 开头的是注释；
//   * `finish()` 负责把最后一个没有以空行收尾的事件也交出来；
//   * `[DONE]` 这类负载语义由 Provider 层解释，解析器只还原字段。
#include "model/SseParser.h"

#include <QList>
#include <QLoggingCategory>

namespace zcode {
namespace {

Q_LOGGING_CATEGORY(log, "zcode.model")

/// 在 buffer 中从 `from` 开始寻找"空行"帧边界。
///
/// 一行以 \n 或 \r\n 结束；空行 = 连续两个行结束符。这里把四种混排都认掉
/// （\n\n、\n\r\n、\r\n\n、\r\n\r\n），避免代理网关混用换行时整帧粘连。
/// 若缓冲区尾部只有半个分隔符（例如以 "\r\n\r" 结尾），本次不消费，
/// 留待下一段字节补齐后再判。
bool findFrameBoundary(const QByteArray &buffer, qsizetype from, qsizetype *index,
                       qsizetype *length) {
    const qsizetype size = buffer.size();
    for (qsizetype i = from; i < size; ++i) {
        const char c = buffer.at(i);
        if (c == '\n') {
            if (i + 1 < size && buffer.at(i + 1) == '\n') {
                *index = i;
                *length = 2;
                return true;
            }
            if (i + 2 < size && buffer.at(i + 1) == '\r' && buffer.at(i + 2) == '\n') {
                *index = i;
                *length = 3;
                return true;
            }
        } else if (c == '\r' && i + 1 < size && buffer.at(i + 1) == '\n') {
            if (i + 2 < size && buffer.at(i + 2) == '\n') {
                *index = i;
                *length = 3;
                return true;
            }
            if (i + 3 < size && buffer.at(i + 2) == '\r' && buffer.at(i + 3) == '\n') {
                *index = i;
                *length = 4;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void SseParser::feed(const QByteArray &chunk) {
    if (chunk.isEmpty()) {
        return;
    }
    buffer_.append(chunk);
    drainCompleteEvents();
}

SseEvent SseParser::next() {
    if (readyEvents_.isEmpty()) {
        // 头文件约定调用前必须 hasNext()；这里只做防御，不崩溃。
        qCWarning(log) << "SseParser::next() 在没有就绪事件时被调用";
        return {};
    }
    return readyEvents_.takeFirst();
}

void SseParser::finish() {
    // 先切出仍然完整的帧（正常路径下 feed 已切完，这里是兜底）。
    drainCompleteEvents();
    if (buffer_.isEmpty()) {
        return;
    }
    // 服务端直接关闭连接、最后一个事件没有尾随空行时的收尾路径。
    qCDebug(log) << "SSE 流结束时仍有未收尾事件，按最后一块解析; bytes=" << buffer_.size();
    parseBlock(buffer_);
    buffer_.clear();
}

void SseParser::reset() {
    buffer_.clear();
    readyEvents_.clear();
}

void SseParser::drainCompleteEvents() {
    qsizetype consumed = 0;
    while (true) {
        qsizetype separatorIndex = -1;
        qsizetype separatorLength = 0;
        if (!findFrameBoundary(buffer_, consumed, &separatorIndex, &separatorLength)) {
            break;
        }
        parseBlock(buffer_.mid(consumed, separatorIndex - consumed));
        consumed = separatorIndex + separatorLength;
    }
    if (consumed > 0) {
        buffer_.remove(0, consumed);
    }
}

void SseParser::parseBlock(const QByteArray &block) {
    if (block.isEmpty()) {
        // 纯分隔空行不产生事件。
        return;
    }

    SseEvent event;
    event.event = QStringLiteral("message");

    QStringList dataLines;
    bool sawField = false;

    const QList<QByteArray> lines = block.split('\n');
    for (QByteArray line : lines) {
        // 逐行去掉行尾的 \r，兼容 \r\n。
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        // 以 ':' 开头的是注释（常用于心跳），整行忽略。
        if (line.startsWith(':')) {
            continue;
        }

        const qsizetype colonIndex = line.indexOf(':');
        const QByteArray field = colonIndex < 0 ? line : line.left(colonIndex);
        QByteArray value = colonIndex < 0 ? QByteArray() : line.mid(colonIndex + 1);
        // 规范：冒号后若有一个空格，属于分隔符，不算值。
        if (value.startsWith(' ')) {
            value.remove(0, 1);
        }

        if (field == "data") {
            // 多行 data 按 \n 拼接；UTF-8 在此处才解码，字节边界已保证完整。
            dataLines.append(QString::fromUtf8(value));
            sawField = true;
        } else if (field == "event") {
            event.event = QString::fromUtf8(value);
            sawField = true;
        } else if (field == "id") {
            event.id = QString::fromUtf8(value);
            sawField = true;
        } else if (field == "retry") {
            bool ok = false;
            const int retryMs = value.toInt(&ok);
            if (ok) {
                event.retryMs = retryMs;
            } else {
                qCWarning(log) << "SSE retry 字段不是整数，已忽略:" << QString::fromUtf8(value);
            }
            sawField = true;
        } else {
            // 未知字段按规范忽略，不影响本事件其余字段。
            qCDebug(log) << "SSE 未知字段，已忽略:" << QString::fromUtf8(field);
        }
    }

    if (!sawField) {
        return;
    }
    if (event.event.isEmpty()) {
        event.event = QStringLiteral("message");
    }
    event.data = dataLines.join(QLatin1Char('\n'));
    readyEvents_.append(event);
}

}  // namespace zcode
