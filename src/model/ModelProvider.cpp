// LyCode — 模型 Provider 抽象实现
//
// 这里只放与具体协议无关的部分：StreamEvent 的静态工厂、终止判定、
// 事件类型词表、ModelStream 的事件投递，以及 ModelInfo 的默认推断。
#include "model/ModelProvider.h"

#include <QLoggingCategory>

#include <iterator>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.model")

/// 事件类型 ↔ 字符串。与 Types.cpp 的表驱动风格保持一致，
/// 便于日志与测试断言使用稳定字面量。
constexpr const char * kEventTokens[] = {
  "started",       // StreamEventKind::Started
  "textDelta",     // StreamEventKind::TextDelta
  "reasoningDelta",  // StreamEventKind::ReasoningDelta
  "toolCallStart",   // StreamEventKind::ToolCallStart
  "toolCallDelta",   // StreamEventKind::ToolCallDelta
  "toolCallEnd",     // StreamEventKind::ToolCallEnd
  "usage",           // StreamEventKind::Usage
  "completed",       // StreamEventKind::Completed
  "failed",          // StreamEventKind::Failed
};

}  // namespace

QString toToken(StreamEventKind kind)
{
  const auto index = static_cast<size_t>(kind);
  if(index < std::size(kEventTokens)) {
    return QString::fromLatin1(kEventTokens[index]);
  }
  qCWarning(log) << "未知 StreamEventKind，使用回退名; value=" << static_cast<int>(kind);
  return QStringLiteral("failed");
}

StreamEvent StreamEvent::started()
{
  StreamEvent event;
  event.kind = StreamEventKind::Started;
  return event;
}

StreamEvent StreamEvent::textDelta(const QString & value)
{
  StreamEvent event;
  event.kind = StreamEventKind::TextDelta;
  event.text = value;
  return event;
}

StreamEvent StreamEvent::reasoningDelta(const QString & value)
{
  StreamEvent event;
  event.kind = StreamEventKind::ReasoningDelta;
  event.text = value;
  return event;
}

StreamEvent StreamEvent::completed(const QString & finishReason)
{
  StreamEvent event;
  event.kind = StreamEventKind::Completed;
  event.finishReason = finishReason;
  return event;
}

StreamEvent StreamEvent::failed(const QString & message)
{
  StreamEvent event;
  event.kind = StreamEventKind::Failed;
  event.errorMessage = message;
  return event;
}

bool StreamEvent::isTerminal() const
{
  return kind == StreamEventKind::Completed || kind == StreamEventKind::Failed;
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelStream
// ─────────────────────────────────────────────────────────────────────────────

ModelStream::ModelStream(QObject * parent) : QObject(parent) {}

ModelStream::~ModelStream() = default;

void ModelStream::emitEvent(const StreamEvent & streamEvent)
{
  // 终止事件之后一律不再投递：保证消费者看到的序列以唯一一个
  // Completed/Failed 收尾，子类不需要自己维护这个不变量。
  if(finished_) {
    qCWarning(log) << "已终止的流收到后续事件，已丢弃; kind=" << toToken(streamEvent.kind);
    return;
  }
  emit event(streamEvent);
  if(streamEvent.isTerminal()) {
    finished_ = true;
    emit finished();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelProvider
// ─────────────────────────────────────────────────────────────────────────────

ModelProvider::ModelProvider(QObject * parent) : QObject(parent) {}

ModelProvider::~ModelProvider() = default;

ModelInfo ModelProvider::modelInfo(const QString & modelId) const
{
  // 默认只给保守值；具体 provider 覆写本方法以补上厂商知识。
  ModelInfo info;
  info.providerId = providerId();
  info.modelId = modelId;
  info.displayName = modelId;
  info.contextWindow = 128000;
  info.maxOutputTokens = 8192;
  info.supportsTools = true;
  return info;
}

}  // namespace lycode
