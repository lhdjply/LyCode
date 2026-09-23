// LyCode — Server-Sent Events 解析器
//
// 所有主流模型 API 的流式响应都用 SSE。QNetworkReply::readyRead 给出的是
// 任意切分的字节块，可能把一个事件劈成两半，也可能一次给出多个事件。
// 本解析器负责把字节流还原成完整事件。
//
// 关键边界：帧分隔符是空行（\n\n 或 \r\n\r\n），不是单个换行；
// 且必须按字节缓冲，不能先转成 QString —— UTF-8 多字节字符同样可能被切开。
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace lycode
{

/// 一个 SSE 事件：event 名 + 拼接后的 data 负载。
struct SseEvent {
  /// `event:` 字段；缺省为 "message"。
  QString event;
  /// 多行 `data:` 用 \n 拼接后的结果。
  QString data;
  QString id;
  /// `retry:` 字段，未知为 -1。
  int retryMs = -1;

  bool isValid() const
  {
    return !data.isEmpty() || !event.isEmpty();
  }
};

/// 增量式 SSE 解析器。
///
/// 用法：
///     SseParser parser;
///     parser.feed(reply->readAll());
///     while (parser.hasNext()) { const SseEvent e = parser.next(); ... }
class SseParser
{
  public:
    SseParser() = default;

    /// 追加一段原始字节。可被任意次数、任意边界调用。
    void feed(const QByteArray & chunk);

    /// 是否已有完整事件可取出。
    bool hasNext() const
    {
      return !readyEvents_.isEmpty();
    }

    /// 取出下一个完整事件。调用前必须确认 hasNext()。
    SseEvent next();

    /// 流结束时调用：把缓冲区里最后一个没有以空行收尾的事件也交出来。
    /// 部分服务端在最后一个事件后直接关闭连接而不补空行。
    void finish();

    /// 清空所有内部状态，复用解析器。
    void reset();

    /// 当前未被消费的原始缓冲字节数（用于诊断）。
    int pendingBytes() const
    {
      return buffer_.size();
    }

  private:
    /// 从缓冲区头部尽可能多地切出完整事件。
    void drainCompleteEvents();

    /// 解析单个原始事件块（不含分隔空行），追加到 readyEvents_。
    void parseBlock(const QByteArray & block);

    QByteArray buffer_;
    QList<SseEvent> readyEvents_;
};

}  // namespace lycode
