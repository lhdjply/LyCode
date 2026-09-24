// LyCode — 上下文压缩（实现）
//
// 设计说明见 ContextCompactor.h。这里只补充两个实现上的取舍：
//
// 1. **估算宁可高估**。字符/token 比取 3.0（英文自然语言约 4，中英混排更接近
//    2.5），高估的后果是"压缩早一点发生"，低估的后果是"请求被 provider 拒绝"，
//    两者代价不对称。
// 2. **裁剪点只落在 user 消息边界**。两种 provider 的线格式都要求工具结果紧跟
//    对应的 tool_calls，从中间切断会产生一个悬空的 tool_use，请求会被直接拒。
#include "agent/ContextCompactor.h"

#include "core/Ids.h"
#include "core/Json.h"

#include <algorithm>

namespace lycode
{
namespace
{

/// 估算字符/token 比。见文件头说明。
constexpr double kCharsPerToken = 3.0;

/// 一条消息在下发时的固定开销（role、分隔符等），按 token 计。
constexpr int kPerMessageOverheadTokens = 4;

/// 工具声明与系统提示词的固定余量（token）。估值不必精确，
/// 它只用来把"即将超窗"判断得早一点。
constexpr int kStaticOverheadTokens = 64;

/// 摘要是给"接手的人"看的，所以要求写成结构化的交接文档，而不是流水账。
const char * const kSummaryInstructions =
  "你是会话压缩器。上面是一段 AI 编程助手与用户的对话历史，即将被你的摘要替换——"
  "摘要是你之后唯一能看到的历史，所以必须把所有继续工作所必需的信息写进去。\n"
  "\n"
  "用与对话相同的语言输出，直接给出摘要正文，不要任何开场白或结束语。"
  "按下面的结构写：\n"
  "1. 任务与目标：用户要做什么，有哪些明确约束。\n"
  "2. 已完成的工作：改过/创建过哪些文件（写全路径），关键改动是什么。\n"
  "3. 关键决策：选了什么方案、为什么，以及被否决的替代方案（如果有）。\n"
  "4. 遇到的问题：报错、失败的尝试、以及当前仍未解决的问题。\n"
  "5. 当前状态：代码/任务处在什么状态，是否可运行。\n"
  "6. 下一步：接下来该做什么（具体到文件或命令）。\n"
  "\n"
  "要求：只保留对继续工作有用的信息，省略寒暄与重复；不要编造历史里没有的内容；"
  "拿不准的地方明确写\"不确定\"，不要猜。";

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 策略
// ─────────────────────────────────────────────────────────────────────────────

int CompactionPolicy::microThresholdTokens(int contextWindow) const
{
  if(contextWindow <= 0) {
    return 0;
  }
  return static_cast<int>(static_cast<double>(contextWindow) * autoCompactThreshold);
}

int CompactionPolicy::fullThresholdTokens(int contextWindow) const
{
  if(contextWindow <= 0) {
    return 0;
  }
  return static_cast<int>(static_cast<double>(contextWindow) * fullCompactThreshold);
}

// ─────────────────────────────────────────────────────────────────────────────
// 估算
// ─────────────────────────────────────────────────────────────────────────────

int estimateTextTokens(const QString & text)
{
  if(text.isEmpty()) {
    return 0;
  }
  return static_cast<int>(static_cast<double>(text.size()) / kCharsPerToken) + 1;
}

int estimateContextTokens(const QList<Message> & messages, const QList<ToolSpec> & tools,
                          const QString & systemPrompt)
{
  int chars = 0;
  int messageCount = 0;

  for(const Message & message : std::as_const(messages)) {
    // System 不下发给 provider，不计入。
    if(message.role == MessageRole::System) {
      continue;
    }
    ++messageCount;
    for(const Part & part : message.parts) {
      switch(part.kind) {
        case PartKind::Text:
          chars += part.text.text.size();
          break;
        case PartKind::Reasoning:
          chars += part.reasoning.text.size();
          break;
        case PartKind::Tool:
          chars += part.tool.output.size();
          chars += part.tool.inputText.size();
          break;
        case PartKind::File:
          // 图片走 base64，体积远大于它的字符数；按字符估会严重低估。
          // 一张图按固定成本计（对齐各家的图片计价口径：约 1k token 量级）。
          chars += part.file.isImage() ? 3000 : 0;
          break;
        default:
          // Artifact / Subagent / Timeline / Step 不下发。
          break;
      }
    }
  }

  // 工具声明的体积（名字 + 描述 + schema）。schema 的 JSON 体积与 token 数
  // 大致同阶，按字符估已经够用。
  for(const ToolSpec & spec : tools) {
    chars += spec.name.size();
    chars += spec.description.size();
    chars += json::toBytes(spec.inputSchema).size();
  }
  chars += systemPrompt.size();

  const int bodyTokens = static_cast<int>(static_cast<double>(chars) / kCharsPerToken);
  return bodyTokens + messageCount * kPerMessageOverheadTokens + kStaticOverheadTokens;
}

// ─────────────────────────────────────────────────────────────────────────────
// 微压缩
// ─────────────────────────────────────────────────────────────────────────────

QString toolOutputTrimMarker(const QString & toolName, int originalChars)
{
  const QString name = toolName.isEmpty() ? QStringLiteral("工具") : toolName;
  return QStringLiteral("[%1 的输出已在压缩上下文时裁剪：原 %2 字符；"
                        "需要原文请重新调用该工具。]")
         .arg(name)
         .arg(originalChars);
}

QList<Message> microcompactedMessages(const QList<Message> & messages,
                                      const CompactionPolicy & policy)
{
  QList<Message> projected;
  projected.reserve(messages.size());

  // 先从后往前划出"近处保护窗口"：累计字符（含正文、思考、工具输出）一旦超过
  // 预算就停止，剩下的更早消息才允许裁剪。
  //
  // ⚠ 把**超出预算的那条也留在保护窗口内**（protectedFrom = index 而非 index+1）
  // 会让实际保护量接近两倍预算——它是"最后一条还装得下的消息"的判定，不是
  // "超出者要一起保护"。这条边界写错过一次，表现为旧的工具输出怎么都不被裁剪。
  int protectedFrom = messages.size();
  int carried = 0;
  for(int index = messages.size() - 1; index >= 0; --index) {
    const Message & message = messages.at(index);
    int messageChars = message.plainText().size();
    for(const Part & part : message.parts) {
      switch(part.kind) {
        case PartKind::Tool:
          messageChars += part.tool.output.size();
          break;
        case PartKind::Reasoning:
          messageChars += part.reasoning.text.size();
          break;
        default:
          break;
      }
    }
    if(carried + messageChars > policy.keepRecentContextChars && index < messages.size() - 1) {
      protectedFrom = index + 1;
      break;
    }
    carried += messageChars;
    protectedFrom = index;
  }

  for(int index = 0; index < messages.size(); ++index) {
    const Message & message = messages.at(index);
    const bool isOld = index < protectedFrom;
    if(!isOld) {
      projected.append(message);
      continue;
    }

    bool changed = false;
    Message copy = message;
    for(Part & part : copy.parts) {
      if(part.kind != PartKind::Tool) {
        continue;
      }
      const QString & output = part.tool.output;
      if(output.isEmpty() || output.size() <= policy.toolOutputBudgetChars) {
        continue;   // 本来就小，裁了省不下什么，保留原样更有用
      }
      part.tool.output = toolOutputTrimMarker(part.tool.name, output.size());
      changed = true;
    }
    projected.append(changed ? copy : message);
  }

  return projected;
}

// ─────────────────────────────────────────────────────────────────────────────
// 全压缩
// ─────────────────────────────────────────────────────────────────────────────

QString buildSummaryPrompt()
{
  return QString::fromUtf8(kSummaryInstructions);
}

QString renderTranscriptForSummary(const QList<Message> & messages, int maxChars)
{
  QStringList blocks;
  for(const Message & message : std::as_const(messages)) {
    if(message.role == MessageRole::System) {
      continue;
    }
    const QString speaker =
      message.role == MessageRole::User ? QStringLiteral("用户") : QStringLiteral("助手");
    QStringList lines;

    const QString text = message.plainText().trimmed();
    if(!text.isEmpty()) {
      lines.append(text);
    }
    for(const Part & part : message.parts) {
      if(part.kind != PartKind::Tool) {
        continue;
      }
      const QString name = part.tool.name.isEmpty() ? QStringLiteral("tool") : part.tool.name;
      const QString output = part.tool.output.trimmed();
      lines.append(output.isEmpty()
                   ? QStringLiteral("[调用 %1（无输出）]").arg(name)
                   : QStringLiteral("[调用 %1]\n%2").arg(name, output));
    }
    if(lines.isEmpty()) {
      continue;
    }
    blocks.append(QStringLiteral("### %1\n%2").arg(speaker, lines.join(QStringLiteral("\n"))));
  }

  QString transcript = blocks.join(QStringLiteral("\n\n"));
  if(maxChars > 0 && transcript.size() > maxChars) {
    // 保留**尾部**：近期历史对"接着干活"更重要，开头通常只是任务描述，
    // 而任务描述在摘要要求里已经点名要写清楚，模型会优先保留它。
    transcript = QStringLiteral("…（更早的历史已省略）…\n")
                 + transcript.right(maxChars);
  }
  return transcript;
}

FullCompaction buildFullCompaction(const QList<Message> & messages,
                                   const CompactionPolicy & policy)
{
  FullCompaction result;

  // 只考虑会下发给模型的消息（System 只是本地记录）。
  QList<int> sendable;
  sendable.reserve(messages.size());
  for(int index = 0; index < messages.size(); ++index) {
    if(messages.at(index).role != MessageRole::System) {
      sendable.append(index);
    }
  }
  if(sendable.size() < policy.minCompactionMessages) {
    return result;   // 太短，摘要未必比原文便宜，不值得花这次调用
  }

  // 至少留 keepTailMessages 条，且当前那条用户消息必须留下——否则压缩完
  // 模型看不到用户刚说的话。
  int keep = std::max(policy.keepTailMessages, 1);
  const int lastIndex = sendable.size() - 1;
  if(messages.at(sendable.at(lastIndex)).role == MessageRole::User) {
    keep = std::max(keep, 1);
  }
  int cut = sendable.size() - keep;
  if(cut <= 0) {
    return result;
  }

  // 裁剪点向前吸附到 user 消息：切在 user 边界上，assistant/tool 配对才完整。
  // 不吸附的话，可能把 assistant(tool_calls) 留在尾部而把它的 tool 结果切走。
  int boundary = sendable.at(cut);
  while(cut > 0 && messages.at(boundary).role != MessageRole::User) {
    --cut;
    boundary = sendable.at(cut);
  }
  if(cut <= 0) {
    return result;   // 头部第一条之前没有完整的 user 轮，放弃
  }

  const int absoluteCut = sendable.at(cut);

  QList<Message> head;
  head.reserve(absoluteCut);
  for(int index = 0; index < absoluteCut; ++index) {
    head.append(messages.at(index));
  }
  for(int index = absoluteCut; index < messages.size(); ++index) {
    result.tailMessages.append(messages.at(index));
  }

  result.replacedMessageCount = absoluteCut;
  result.transcript = renderTranscriptForSummary(head, policy.summaryInputChars);
  result.applied = !result.transcript.trimmed().isEmpty();
  if(!result.applied) {
    result.tailMessages.clear();
    result.replacedMessageCount = 0;
  }
  else {
    // 原文交给调用方归档。**不能**在这里丢掉：压缩只是把它移出活动历史，
    // 用户的原始记录必须留档。
    result.headMessages = head;
  }
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 合成消息
// ─────────────────────────────────────────────────────────────────────────────

Message makeCompactionSummaryMessage(const Id & sessionId, const QString & summary,
                                     const QString & modelId)
{
  Message message;
  message.id = newMessageId();
  message.sessionId = sessionId;
  // 必须是 User：System 不下发给 provider，摘要就白写了。
  message.role = MessageRole::User;
  message.status = MessageStatus::Complete;
  // modelOnly：这不是用户说过的话，用户可见的对话流由 TimelinePart 那条分隔行
  // 表达（见 makeCompactionMarkerMessage）。
  message.modelOnly = true;
  message.modelId = modelId;
  message.createdAtMs = nowMs();
  message.updatedAtMs = message.createdAtMs;

  Part text;
  text.id = newPartId();
  text.kind = PartKind::Text;
  text.text.text =
    QStringLiteral("<conversation-summary>\n"
                   "以下是本会话较早历史的压缩摘要（原始消息已归档）。"
                   "把它当作已经发生的事实继续工作：\n\n%1\n"
                   "</conversation-summary>")
    .arg(summary);
  // 标记压缩来源，便于 restoreContextSummary() 在重载会话时认回来。
  // 摘要正文单独存一份：正文本身是包了标签的下发形状，回填时不该还原标签。
  text.metadata.insert(QStringLiteral("kind"), QString::fromLatin1(compaction::kKind));
  text.metadata.insert(QStringLiteral("summary"), summary);
  message.parts.append(text);
  return message;
}

Message makeCompactionMarkerMessage(const Id & sessionId, const QString & summary,
                                    int replacedMessageCount)
{
  Message marker;
  marker.id = newMessageId();
  marker.sessionId = sessionId;
  // System 本地记录：不下发给 provider，也不参与上下文估算。
  marker.role = MessageRole::System;
  marker.status = MessageStatus::Complete;
  marker.createdAtMs = nowMs();
  marker.updatedAtMs = marker.createdAtMs;

  Part part;
  part.id = newPartId();
  part.kind = PartKind::Timeline;
  part.timeline.kind = TimelineKind::ContextCompaction;
  part.timeline.rawType = toToken(TimelineKind::ContextCompaction);
  part.timeline.display = QStringLiteral("separator");
  part.timeline.status = QStringLiteral("completed");
  // reason 留空：触发来源（自动阈值 / 用户 /compact）由调用方在提示文案里说明，
  // 在这里写死一个会让手动压缩的标记说谎。
  part.timeline.reason.clear();
  part.timeline.summary =
    QStringLiteral("上下文已压缩：%1 条较早消息已归档为摘要").arg(replacedMessageCount);
  part.timeline.metadata.insert(QStringLiteral("replacedMessages"), replacedMessageCount);
  // 摘要正文也留一份在分隔行上：这是"压缩过什么"的唯一可见痕迹，
  // 排查"模型为什么忘了某件事"时不用去翻归档表。
  part.timeline.metadata.insert(QStringLiteral("summary"), summary);
  marker.parts.append(part);
  return marker;
}

QString restoreContextSummary(const QList<Message> & messages)
{
  for(auto iterator = messages.crbegin(); iterator != messages.crend(); ++iterator) {
    for(const Part & part : iterator->parts) {
      if(part.kind != PartKind::Text) {
        continue;
      }
      if(json::str(part.metadata, QStringLiteral("kind")) !=
         QString::fromLatin1(compaction::kKind)) {
        continue;
      }
      return json::str(part.metadata, QStringLiteral("summary"));
    }
  }
  return {};
}

}  // namespace lycode
