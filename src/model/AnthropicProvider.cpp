// LyCode — Anthropic Messages API 流式客户端实现
//
// 一次调用 = 一个 AnthropicStream。它自己持有 QNetworkReply、SSE 解析器与
// 一个空闲看门狗，终止后自行 deleteLater()；调用方只需要消费 event()/finished()。
#include "model/AnthropicProvider.h"

#include "core/Json.h"
#include "core/Logging.h"
#include "model/SseParser.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace lycode
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.model.anthropic")

/// 与设置里"最长单次请求"一致；流式调用整体超时 10 分钟。
constexpr int kTransferTimeoutMs = 600000;
/// 相邻两个网络分块的最大间隔；Anthropic 会周期性发 ping。
constexpr int kIdleTimeoutMs = 60000;
/// 错误响应体只保留前 2KB，避免把整页 HTML 塞进错误信息。
constexpr int kErrorBodyLimit = 2048;
/// 思考预算缺省值（仅当上层没给有效值时使用）。
constexpr int kDefaultThinkingBudget = 4096;

/// baseUrl 归一：去尾部 '/'。
QString normalizeBaseUrl(const QString & baseUrl)
{
  QString base = baseUrl.trimmed();
  while(base.endsWith(QLatin1Char('/'))) {
    base.chop(1);
  }
  return base;
}

/// 拼接 messages 端点；baseUrl 已以 /v1 结尾时不再重复拼 /v1。
QString messagesEndpoint(const QString & baseUrl)
{
  const QString base = normalizeBaseUrl(baseUrl);
  if(base.endsWith(QStringLiteral("/v1"))) {
    return base + QStringLiteral("/messages");
  }
  return base + QStringLiteral("/v1/messages");
}

/// 自建网关的附加请求头；非字符串值忽略。
void applyExtraHeaders(QNetworkRequest & request, const QJsonObject & headers)
{
  for(auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
    if(it.value().isString()) {
      request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    }
    else {
      qCWarning(log) << "extraHeaders 值不是字符串，已忽略; key=" << it.key();
    }
  }
}

/// 工具入参：优先用已解析的 input；否则尝试解析流式累积的原始文本。
QJsonObject toolInputObject(const ToolPart & tool)
{
  if(!tool.input.isEmpty()) {
    return tool.input;
  }
  const QString raw = tool.inputText.trimmed();
  if(raw.isEmpty()) {
    return {};
  }
  QString error;
  const QJsonObject parsed = json::parseObject(raw.toUtf8(), &error);
  if(!error.isEmpty()) {
    qCWarning(log) << "工具入参原始文本解析失败，按空对象回传; callId=" << tool.callId
                   << "error=" << error;
    return {};
  }
  return parsed;
}

/// 工具结果文本：没有输出时退回错误信息，再退回占位符
/// （Anthropic 不接受空 content 的 tool_result）。
QString toolResultContent(const ToolPart & tool)
{
  QString content = tool.output;
  if(content.isEmpty()) {
    content = tool.error;
  }
  if(content.isEmpty()) {
    content = QStringLiteral("(no output)");
  }
  return content;
}

/// 领域消息 → Anthropic messages 数组。
QJsonArray buildMessages(const QList<Message> & messages)
{
  QJsonArray result;
  for(const Message & message : messages) {
    // system 走顶层 system 字段，不参与 messages 序列。
    if(message.role == MessageRole::System) {
      continue;
    }
    const bool isAssistant = message.role == MessageRole::Assistant;

    QJsonArray blocks;
    if(isAssistant) {
      // Anthropic 只在带 signature 时才接受回传 thinking，且必须位于该
      // assistant 消息的最前面（thinking 块必须在 text/tool_use 之前）。
      for(const Part & part : message.parts) {
        if(part.kind != PartKind::Reasoning) {
          continue;
        }
        if(part.reasoning.signature.isEmpty() || part.reasoning.text.isEmpty()) {
          continue;
        }
        QJsonObject thinking;
        thinking.insert(QStringLiteral("type"), QStringLiteral("thinking"));
        thinking.insert(QStringLiteral("thinking"), part.reasoning.text);
        thinking.insert(QStringLiteral("signature"), part.reasoning.signature);
        blocks.append(thinking);
      }
    }

    for(const Part & part : message.parts) {
      if(part.kind == PartKind::Text) {
        if(part.text.text.isEmpty()) {
          continue;
        }
        QJsonObject block;
        block.insert(QStringLiteral("type"), QStringLiteral("text"));
        block.insert(QStringLiteral("text"), part.text.text);
        blocks.append(block);
      }
      else if(part.kind == PartKind::File) {
        // 图片以 base64 内联（Anthropic 的 image source）。
        // assistant 侧不回传图片：模型没有"发出"过图片，硬塞会被拒。
        if(isAssistant) {
          continue;
        }
        if(!part.file.isImage()) {
          // 非图片附件本实现不上传。明确告警而不是静默丢弃，
          // 否则用户会以为文件已经给模型看过了。
          qCWarning(log) << "跳过非图片附件（本实现不支持上传）; mime="
                         << part.file.mimeType << "name=" << part.file.fileName;
          continue;
        }
        if(part.file.base64.isEmpty()) {
          qCWarning(log) << "图片附件缺少 base64 数据，已跳过; name="
                         << part.file.fileName;
          continue;
        }
        QJsonObject source;
        source.insert(QStringLiteral("type"), QStringLiteral("base64"));
        source.insert(QStringLiteral("media_type"), part.file.mimeType);
        source.insert(QStringLiteral("data"), part.file.base64);
        QJsonObject block;
        block.insert(QStringLiteral("type"), QStringLiteral("image"));
        block.insert(QStringLiteral("source"), source);
        blocks.append(block);
      }
      else if(part.kind == PartKind::Tool) {
        if(isAssistant) {
          if(part.tool.callId.isEmpty() || part.tool.name.isEmpty()) {
            qCWarning(log) << "assistant 工具块缺少 callId/name，已跳过; callId="
                           << part.tool.callId;
            continue;
          }
          QJsonObject block;
          block.insert(QStringLiteral("type"), QStringLiteral("tool_use"));
          block.insert(QStringLiteral("id"), part.tool.callId);
          block.insert(QStringLiteral("name"), part.tool.name);
          block.insert(QStringLiteral("input"), toolInputObject(part.tool));
          blocks.append(block);
        }
        else {
          // user 消息里的工具块 = 工具执行结果。
          if(part.tool.callId.isEmpty()) {
            qCWarning(log) << "工具结果缺少 callId，已跳过";
            continue;
          }
          QJsonObject block;
          block.insert(QStringLiteral("type"), QStringLiteral("tool_result"));
          block.insert(QStringLiteral("tool_use_id"), part.tool.callId);
          block.insert(QStringLiteral("content"), toolResultContent(part.tool));
          blocks.append(block);
        }
      }
      // File / Artifact / Subagent / Timeline / Step / Reasoning(非 assistant)
      // 不参与 provider 消息序列。
    }

    if(blocks.isEmpty()) {
      qCDebug(log) << "消息内容为空，已从请求中跳过; role=" << toToken(message.role);
      continue;
    }

    QJsonObject mapped;
    mapped.insert(QStringLiteral("role"),
                  isAssistant ? QStringLiteral("assistant") : QStringLiteral("user"));
    mapped.insert(QStringLiteral("content"), blocks);
    result.append(mapped);
  }
  return result;
}

/// 工具声明 → Anthropic tools 数组。
QJsonArray buildTools(const QList<ToolSpec> & tools)
{
  QJsonArray result;
  for(const ToolSpec & spec : tools) {
    if(spec.name.isEmpty()) {
      qCWarning(log) << "工具声明缺少 name，已跳过";
      continue;
    }
    QJsonObject schema = spec.inputSchema;
    if(schema.isEmpty()) {
      schema.insert(QStringLiteral("type"), QStringLiteral("object"));
      schema.insert(QStringLiteral("properties"), QJsonObject());
    }
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), spec.name);
    if(!spec.description.isEmpty()) {
      entry.insert(QStringLiteral("description"), spec.description);
    }
    entry.insert(QStringLiteral("input_schema"), schema);
    result.append(entry);
  }
  return result;
}

/// 组装请求体。
QJsonObject buildRequestBody(const ModelRequest & request)
{
  QJsonObject body;
  body.insert(QStringLiteral("model"), request.modelId);
  body.insert(QStringLiteral("max_tokens"), request.maxOutputTokens);
  body.insert(QStringLiteral("stream"), true);
  if(!request.systemPrompt.isEmpty()) {
    body.insert(QStringLiteral("system"), request.systemPrompt);
  }
  body.insert(QStringLiteral("messages"), buildMessages(request.messages));

  const QJsonArray tools = buildTools(request.tools);
  if(!tools.isEmpty()) {
    body.insert(QStringLiteral("tools"), tools);
  }

  if(request.enableReasoning) {
    QJsonObject thinking;
    thinking.insert(QStringLiteral("type"), QStringLiteral("enabled"));
    const int budget =
      request.reasoningBudgetTokens > 0 ? request.reasoningBudgetTokens : kDefaultThinkingBudget;
    thinking.insert(QStringLiteral("budget_tokens"), budget);
    body.insert(QStringLiteral("thinking"), thinking);
    // 开启 thinking 时 Anthropic 拒绝 temperature，这里刻意不带。
  }
  else if(request.temperature != 1.0) {
    // 仅在偏离默认值时才带，避免无意改变服务端默认行为。
    body.insert(QStringLiteral("temperature"), request.temperature);
  }
  return body;
}

/// 错误体截断为可读文本。
QString truncateBody(const QByteArray & body)
{
  QString text = QString::fromUtf8(body.left(kErrorBodyLimit)).trimmed();
  if(body.size() > kErrorBodyLimit) {
    text += QStringLiteral("…[truncated]");
  }
  return text;
}

/// stop_reason 归一：工具调用保留 tool_use，其余原样透传（未知值只告警不篡改）。
QString normalizeStopReason(const QString & reason)
{
  static const QStringList known = {
    QStringLiteral("end_turn"),   QStringLiteral("tool_use"), QStringLiteral("max_tokens"),
    QStringLiteral("stop_sequence"), QStringLiteral("pause_turn"), QStringLiteral("refusal"),
  };
  if(!known.contains(reason)) {
    qCWarning(log) << "未知 stop_reason，原样透传:" << reason;
  }
  return reason;
}

// ─────────────────────────────────────────────────────────────────────────────
// 单次流式调用
// ─────────────────────────────────────────────────────────────────────────────

class AnthropicStream final : public ModelStream
{
  public:
    AnthropicStream(QNetworkAccessManager * network, QUrl url, QByteArray payload, QByteArray apiKey,
                    QJsonObject extraHeaders)
      : network_(network),
        url_(std::move(url)),
        payload_(std::move(payload)),
        apiKey_(std::move(apiKey)),
        extraHeaders_(std::move(extraHeaders))
    {
      idleTimer_ = new QTimer(this);
      idleTimer_->setSingleShot(true);
      idleTimer_->setInterval(kIdleTimeoutMs);
      connect(idleTimer_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("流空闲超时：%1 毫秒内未收到任何数据").arg(kIdleTimeoutMs));
      });
    }

    void start()
    {
      if(terminal_) {
        return;
      }
      if(network_ == nullptr || url_.isEmpty()) {
        fail(QStringLiteral("Anthropic provider 未正确配置 baseUrl"));
        return;
      }

      QNetworkRequest request(url_);
      request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
      request.setRawHeader("accept", "text/event-stream");
      request.setRawHeader("x-api-key", apiKey_);
      request.setRawHeader("anthropic-version", "2023-06-01");
      applyExtraHeaders(request, extraHeaders_);
      request.setTransferTimeout(kTransferTimeoutMs);

      qCInfo(log) << "请求开始; url=" << url_.toString();
      reply_ = network_->post(request, payload_);
      if(reply_ == nullptr) {
        fail(QStringLiteral("无法创建网络请求"));
        return;
      }
      connect(reply_, &QNetworkReply::readyRead, this, [this] { onReadyRead(); });
      connect(reply_, &QNetworkReply::finished, this, [this] { onReplyFinished(); });
      // 网络管理器若先被销毁，reply 会被直接 delete，需要兜底终止，避免悬挂。
      connect(reply_, &QObject::destroyed, this, [this] {
        if(terminal_) {
          return;
        }
        reply_ = nullptr;
        fail(QStringLiteral("网络管理器已销毁，请求中断"));
      });
      idleTimer_->start();
    }

    void abort() override
    {
      if(terminal_ || isFinished()) {
        return;
      }
      abortRequested_ = true;
      // 注意：abort() 可能同步发出 finished()，从而在 onReplyFinished 里把
      // reply_ 置空并终止本流，因此必须用局部指针并先检查 terminal_。
      QNetworkReply * reply = reply_;
      if(reply != nullptr) {
        reply->abort();
        if(!terminal_ && reply->isFinished()) {
          onReplyFinished();
        }
      }
      else {
        fail(QStringLiteral("请求已取消"));
      }
    }

  private:
    void onReadyRead()
    {
      if(terminal_ || reply_ == nullptr || !reply_->isOpen()) {
        return;
      }
      const QByteArray chunk = reply_->readAll();
      if(chunk.isEmpty()) {
        return;
      }
      idleTimer_->start();

      const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if(status >= 400) {
        // 非 2xx 的响应体通常是 JSON/HTML 错误页，不喂给 SSE 解析器。
        errorBody_.append(chunk);
        if(errorBody_.size() > kErrorBodyLimit * 4) {
          errorBody_ = errorBody_.left(kErrorBodyLimit * 4);
        }
        return;
      }
      parser_.feed(chunk);
      processParser();
    }

    void onReplyFinished()
    {
      if(terminal_ || reply_ == nullptr) {
        return;
      }
      idleTimer_->stop();

      const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      const QNetworkReply::NetworkError netError = reply_->error();
      const QString netErrorString = reply_->errorString();

      // finished 之后仍可能有未读字节（最后一个事件常常没有尾随空行）。
      // 被 abort 的 reply 已关闭，此时不能读，否则 Qt 会打印设备未打开的告警。
      QByteArray rest;
      if(reply_->isOpen() && reply_->bytesAvailable() > 0) {
        rest = reply_->readAll();
      }
      if(!rest.isEmpty()) {
        if(status >= 400) {
          errorBody_.append(rest);
        }
        else {
          parser_.feed(rest);
        }
      }

      if(status >= 400) {
        if(status == 401 || status == 403) {
          qCCritical(log) << "鉴权失败; HTTP" << status;
        }
        fail(QStringLiteral("HTTP %1: %2").arg(status).arg(truncateBody(errorBody_)));
        return;
      }

      if(netError != QNetworkReply::NoError) {
        if(abortRequested_ || netError == QNetworkReply::OperationCanceledError) {
          fail(QStringLiteral("请求已取消"));
        }
        else {
          qCCritical(log) << "连接失败:" << netErrorString;
          fail(netErrorString);
        }
        return;
      }

      parser_.finish();
      processParser();
      if(terminal_) {
        return;
      }
      // 服务端正常关闭但没发 message_stop：按已记录的 stop_reason 收尾。
      finishStream();
    }

    void processParser()
    {
      while(!terminal_ && parser_.hasNext()) {
        handleEvent(parser_.next());
      }
    }

    /// 解析事件负载；空 data 视为空对象，非法 JSON 直接失败。
    bool parsePayload(const SseEvent & event, QJsonObject * out)
    {
      if(event.data.trimmed().isEmpty()) {
        *out = QJsonObject();
        return true;
      }
      QString error;
      const QJsonObject payload = json::parseObject(event.data.toUtf8(), &error);
      if(!error.isEmpty()) {
        fail(QStringLiteral("无法解析 %1 事件负载: %2; 原始片段: %3")
             .arg(event.event, error, json::truncate(event.data, 300)));
        return false;
      }
      *out = payload;
      return true;
    }

    void handleEvent(const SseEvent & event)
    {
      if(terminal_) {
        return;
      }
      qCDebug(log) << "SSE 事件; event=" << event.event << "data=" << event.data.left(500);

      if(event.event == QStringLiteral("ping")) {
        return;
      }
      if(event.event == QStringLiteral("error")) {
        QJsonObject payload;
        if(!parsePayload(event, &payload)) {
          return;
        }
        const QJsonObject error = json::object(payload, QStringLiteral("error"));
        QString message = json::str(error, QStringLiteral("message"));
        if(message.isEmpty()) {
          message = json::str(payload, QStringLiteral("message"));
        }
        if(message.isEmpty()) {
          message = json::truncate(event.data, 300);
        }
        fail(message.isEmpty() ? QStringLiteral("Anthropic 返回未知错误") : message);
        return;
      }
      if(event.event == QStringLiteral("message_start")) {
        QJsonObject payload;
        if(parsePayload(event, &payload)) {
          handleMessageStart(payload);
        }
        return;
      }
      if(event.event == QStringLiteral("content_block_start")) {
        QJsonObject payload;
        if(parsePayload(event, &payload)) {
          handleContentBlockStart(payload);
        }
        return;
      }
      if(event.event == QStringLiteral("content_block_delta")) {
        QJsonObject payload;
        if(parsePayload(event, &payload)) {
          handleContentBlockDelta(payload);
        }
        return;
      }
      if(event.event == QStringLiteral("content_block_stop")) {
        QJsonObject payload;
        if(!parsePayload(event, &payload)) {
          return;
        }
        handleContentBlockStop();
        return;
      }
      if(event.event == QStringLiteral("message_delta")) {
        QJsonObject payload;
        if(parsePayload(event, &payload)) {
          handleMessageDelta(payload);
        }
        return;
      }
      if(event.event == QStringLiteral("message_stop")) {
        finishStream();
        return;
      }
      qCWarning(log) << "未知 SSE 事件类型，已忽略:" << event.event;
    }

    void handleMessageStart(const QJsonObject & payload)
    {
      const QJsonObject message = json::object(payload, QStringLiteral("message"));
      const QString model = json::str(message, QStringLiteral("model"));
      if(!model.isEmpty()) {
        qCInfo(log) << "模型已确认; model=" << model;
      }
      const QJsonObject usage = json::object(message, QStringLiteral("usage"));
      // Anthropic 的 input_tokens 本身就是"未命中缓存"的口径（缓存读/写分别由
      // cache_read_input_tokens / cache_creation_input_tokens 单列），
      // 与 Usage 的约定一致，不需要像 OpenAI 那样再减一次。
      usage_.inputTokens = json::integer(usage, QStringLiteral("input_tokens"));
      usage_.cacheReadTokens = json::integer(usage, QStringLiteral("cache_read_input_tokens"));
      usage_.cacheWriteTokens =
        json::integer(usage, QStringLiteral("cache_creation_input_tokens"));
      const int outputTokens = json::integer(usage, QStringLiteral("output_tokens"));
      if(outputTokens > 0) {
        usage_.outputTokens = outputTokens;
      }
      if(!startedEmitted_) {
        startedEmitted_ = true;
        emitSafe(StreamEvent::started());
      }
    }

    void handleContentBlockStart(const QJsonObject & payload)
    {
      const QJsonObject block = json::object(payload, QStringLiteral("content_block"));
      currentBlockType_ = json::str(block, QStringLiteral("type"));
      currentToolJson_.clear();
      currentThinkingSignature_.clear();

      if(currentBlockType_ == QStringLiteral("tool_use")) {
        currentToolId_ = json::str(block, QStringLiteral("id"));
        currentToolName_ = json::str(block, QStringLiteral("name"));
        StreamEvent event;
        event.kind = StreamEventKind::ToolCallStart;
        event.toolCallId = currentToolId_;
        event.toolName = currentToolName_;
        emitSafe(event);
      }
    }

    void handleContentBlockDelta(const QJsonObject & payload)
    {
      const QJsonObject delta = json::object(payload, QStringLiteral("delta"));
      const QString type = json::str(delta, QStringLiteral("type"));

      if(type == QStringLiteral("text_delta")) {
        const QString text = json::str(delta, QStringLiteral("text"));
        if(!text.isEmpty()) {
          emitSafe(StreamEvent::textDelta(text));
        }
      }
      else if(type == QStringLiteral("thinking_delta")) {
        const QString text = json::str(delta, QStringLiteral("thinking"));
        if(!text.isEmpty()) {
          emitSafe(StreamEvent::reasoningDelta(text));
        }
      }
      else if(type == QStringLiteral("signature_delta")) {
        // 签名通常紧跟在 thinking 之后单独下发；单独发一个 text 为空的
        // ReasoningDelta 让消费者把 signature 存到对应 reasoning part 上。
        currentThinkingSignature_ += json::str(delta, QStringLiteral("signature"));
        StreamEvent event;
        event.kind = StreamEventKind::ReasoningDelta;
        event.signature = currentThinkingSignature_;
        emitSafe(event);
      }
      else if(type == QStringLiteral("input_json_delta")) {
        const QString partial = json::str(delta, QStringLiteral("partial_json"));
        if(partial.isEmpty()) {
          return;
        }
        currentToolJson_ += partial;
        StreamEvent event;
        event.kind = StreamEventKind::ToolCallDelta;
        event.toolCallId = currentToolId_;
        event.argumentsDelta = partial;
        emitSafe(event);
      }
      else {
        qCWarning(log) << "未知 content_block_delta 类型，已忽略:" << type;
      }
    }

    void handleContentBlockStop()
    {
      if(currentBlockType_ == QStringLiteral("tool_use")) {
        QJsonObject input;
        const QString raw = currentToolJson_.trimmed();
        if(!raw.isEmpty()) {
          QString error;
          input = json::parseObject(raw.toUtf8(), &error);
          if(!error.isEmpty()) {
            fail(QStringLiteral("工具入参 JSON 解析失败: %1; 原始片段: %2")
                 .arg(error, json::truncate(raw, 300)));
            return;
          }
        }
        StreamEvent event;
        event.kind = StreamEventKind::ToolCallEnd;
        event.toolCallId = currentToolId_;
        event.toolName = currentToolName_;
        event.toolInput = input;
        emitSafe(event);
      }
      currentBlockType_.clear();
      currentToolJson_.clear();
      currentThinkingSignature_.clear();
    }

    void handleMessageDelta(const QJsonObject & payload)
    {
      const QJsonObject delta = json::object(payload, QStringLiteral("delta"));
      const QString stopReason = json::str(delta, QStringLiteral("stop_reason"));
      if(!stopReason.isEmpty()) {
        stopReason_ = normalizeStopReason(stopReason);
      }
      const QJsonObject usage = json::object(payload, QStringLiteral("usage"));
      if(json::has(usage, QStringLiteral("output_tokens"))) {
        usage_.outputTokens = json::integer(usage, QStringLiteral("output_tokens"));
      }
      if(json::has(usage, QStringLiteral("input_tokens"))) {
        usage_.inputTokens = json::integer(usage, QStringLiteral("input_tokens"));
      }
    }

    /// message_stop / 连接正常关闭的统一收尾：Usage 之后恰好一个 Completed。
    void finishStream()
    {
      if(terminal_) {
        return;
      }
      if(!usageSent_) {
        usageSent_ = true;
        StreamEvent usageEvent;
        usageEvent.kind = StreamEventKind::Usage;
        usageEvent.usage = usage_;
        emitSafe(usageEvent);
      }
      if(terminal_) {
        return;
      }
      const QString reason = stopReason_.isEmpty() ? QStringLiteral("end_turn") : stopReason_;
      qCInfo(log) << "请求结束; finishReason=" << reason << "inputTokens=" << usage_.inputTokens
                  << "outputTokens=" << usage_.outputTokens
                  << "cacheReadTokens=" << usage_.cacheReadTokens;
      terminate(StreamEvent::completed(reason));
    }

    void fail(const QString & message)
    {
      if(terminal_) {
        return;
      }
      qCWarning(log) << "流失败:" << message;
      terminate(StreamEvent::failed(message));
    }

    void emitSafe(const StreamEvent & event)
    {
      if(!terminal_) {
        emitEvent(event);
      }
    }

    /// 唯一的终止出口：保证恰好一次终止事件，并在终止后释放网络资源。
    void terminate(const StreamEvent & event)
    {
      if(terminal_) {
        return;
      }
      terminal_ = true;
      idleTimer_->stop();
      if(reply_ != nullptr) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
      }
      // 先登记延迟删除：即使调用方在事件回调里 delete 本对象，Qt 也会撤销
      // 这个 DeferredDelete，不存在二次释放。之后不得再访问任何成员。
      deleteLater();
      emitEvent(event);
    }

    QNetworkAccessManager * network_ = nullptr;
    QUrl url_;
    QByteArray payload_;
    QByteArray apiKey_;
    QJsonObject extraHeaders_;

    QNetworkReply * reply_ = nullptr;
    QTimer * idleTimer_ = nullptr;
    SseParser parser_;

    bool terminal_ = false;
    bool abortRequested_ = false;
    bool startedEmitted_ = false;
    bool usageSent_ = false;

    Usage usage_;
    QString stopReason_;

    QString currentBlockType_;
    QString currentToolId_;
    QString currentToolName_;
    QString currentToolJson_;
    QString currentThinkingSignature_;
    QByteArray errorBody_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// AnthropicProvider
// ─────────────────────────────────────────────────────────────────────────────

AnthropicProvider::AnthropicProvider(QObject * parent)
  : ModelProvider(parent), network_(new QNetworkAccessManager(this)) {}

AnthropicProvider::~AnthropicProvider() = default;

QString AnthropicProvider::providerId() const
{
  return config_.id;
}

ProviderKind AnthropicProvider::kind() const
{
  return ProviderKind::Anthropic;
}

QStringList AnthropicProvider::modelIds() const
{
  return config_.models;
}

bool AnthropicProvider::configure(const ProviderConfig & config)
{
  config_ = config;
  const QString baseUrl = normalizeBaseUrl(config_.baseUrl);
  if(baseUrl.isEmpty()) {
    qCWarning(log) << "配置缺少 baseUrl; providerId=" << config_.id;
    return false;
  }
  if(config_.apiKey.trimmed().isEmpty()) {
    qCWarning(log) << "Anthropic 配置缺少 apiKey; providerId=" << config_.id;
    return false;
  }
  // 绝不记录明文 key。
  qCInfo(log) << "配置完成; providerId=" << config_.id << "baseUrl=" << baseUrl
              << "apiKey=" << logging::redact(config_.apiKey)
              << "models=" << config_.models.size();
  return true;
}

ProviderConfig AnthropicProvider::configuration() const
{
  return config_;
}

ModelStream * AnthropicProvider::stream(const ModelRequest & request)
{
  const QByteArray payload = json::toBytes(buildRequestBody(request));
  qCInfo(log) << "发起调用; model=" << request.modelId << "messages=" << request.messages.size()
              << "tools=" << request.tools.size() << "reasoning=" << request.enableReasoning;

  auto * stream = new AnthropicStream(network_, QUrl(messagesEndpoint(config_.baseUrl)), payload,
                                      config_.apiKey.toUtf8(), config_.extraHeaders);
  // 延迟到事件循环再启动：确保调用方先连上 event()/finished() 再收到任何事件
  // （配置缺失等同步失败路径也一样）。
  QTimer::singleShot(0, stream, [stream] { stream->start(); });
  return stream;
}

ModelInfo AnthropicProvider::modelInfo(const QString & modelId) const
{
  ModelInfo info = ModelProvider::modelInfo(modelId);
  const QString lower = modelId.toLower();
  if(lower.contains(QStringLiteral("claude"))) {
    // Claude 全系上下文 200K，且支持 extended thinking。
    info.contextWindow = 200000;
    info.supportsReasoning = true;
  }
  else if(lower.contains(QStringLiteral("gpt-4")) || lower.contains(QStringLiteral("o1")) ||
          lower.contains(QStringLiteral("o3"))) {
    info.contextWindow = 128000;
    info.supportsReasoning = true;
  }
  return info;
}

}  // namespace lycode
