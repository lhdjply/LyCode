// LyCode — 上下文压缩
//
// 解决的问题：会话越长，历史消息越占上下文窗口，最终要么被 provider 拒绝
// （超窗），要么把有效预算挤到几乎没有。这里提供两级压缩：
//
//   microcompact  只裁掉**旧工具输出**，不调模型、不丢消息。工具输出是上下文里
//                 最"重"也最可再生的部分（一条 `cat` 能顶几千 token，而它要
//                 表达的信息通常已经进了模型接下来的推理）。近处几轮保持原样。
//   full compact  把早期历史交给模型摘要成一份"交接文档"，用一份合成消息替换
//                 掉那段历史。真正把体积降下来，代价是一次模型调用。
//
// 本模块刻意做成**纯函数 + 无状态**：不碰 QObject、不发信号、不做持久化，
// 因此可以用普通单元测试直接验证边界（哪些消息被替换、合成消息长什么样）。
// 触发时机、异步摘要调用、落盘与界面通知都在 AgentRuntime 里。
//
// 与 provider 协议的约束（改了投影逻辑必须回头核对这里）：
//   - 工具结果与工具调用是**成对**的：assistant 的 tool_calls 必须紧跟 role=tool
//     的结果。microcompact 只改 ToolPart::output，绝不移除 part；full compact 的
//     裁剪点只落在 user 消息边界上，因此不会把一对拆开。
//   - MessageRole::System 不下发给 provider（两家 provider 都显式跳过），
//     所以合成消息必须是 User 角色；`modelOnly` 让它不出现在用户可见的对话流里。
#pragma once

#include <QList>
#include <QString>

#include "core/Types.h"
#include "model/ModelProvider.h"

namespace lycode
{

/// 压缩策略。默认值按"上下文窗口的百分比 + 少量绝对下限"给出。
struct CompactionPolicy {
  /// 自动微压缩阈值（占窗口比例）。超过它就开始裁剪旧工具输出。
  double autoCompactThreshold = 0.70;
  /// 自动全压缩阈值（占窗口比例）。超过它才花一次模型调用做摘要。
  double fullCompactThreshold = 0.90;
  /// 近处保护窗口（字符）。这一段里的消息一律不动——模型手头正在处理的
  /// 工具结果是最不能丢的。
  int keepRecentContextChars = 40000;
  /// 单个工具输出在下发给模型时的上限（字符）。
  int toolOutputBudgetChars = 8000;
  /// 全压缩后至少保留的尾部消息数。
  int keepTailMessages = 4;
  /// 全压缩至少要有这么多条消息才值得做（否则摘要比原文还贵）。
  int minCompactionMessages = 8;
  /// 送给摘要模型的历史字符上限。
  int summaryInputChars = 60000;
  /// 摘要正文的长度上限（字符），超出部分截断。
  int summaryMaxChars = 4000;

  /// 按上下文窗口算出微压缩阈值（token）。
  int microThresholdTokens(int contextWindow) const;
  /// 按上下文窗口算出全压缩阈值（token）。
  int fullThresholdTokens(int contextWindow) const;
};

/// 先估算一段文本的 token 数，再累加。
int estimateTextTokens(const QString & text);

/// 估算当前会话真正会下发给模型的上下文体积（token）。
///
/// 计入：正文、思考、工具输出，以及工具声明与系统提示词——它们同样占窗口，
/// 不计的话会在窗口即将用满时低估，压缩永远不触发。
/// 刻意高估（字符/token 比取得比真实更"密"）：高估只会让压缩更早发生，
/// 低估则可能导致请求直接被 provider 拒绝。
int estimateContextTokens(const QList<Message> & messages, const QList<ToolSpec> & tools,
                          const QString & systemPrompt);

/// 工具输出被裁剪后替换成的标记文本。
QString toolOutputTrimMarker(const QString & toolName, int originalChars);

/// 微压缩：构造一份**只用于下发**的消息序列。
///
/// 返回的列表与入参一一对应（不增删消息），差别只在被裁剪的 ToolPart::output。
/// 本地 messages_ 不受影响——工具卡片要继续显示完整输出，必须自己留着。
QList<Message> microcompactedMessages(const QList<Message> & messages,
                                      const CompactionPolicy & policy);

/// 全压缩的结果。
struct FullCompaction {
  /// 早期消息的条数（= 从列表头部裁掉的条数）。
  int replacedMessageCount = 0;
  /// 被裁掉的早期消息原文。调用方负责把它们归档，**不能直接丢弃**。
  QList<Message> headMessages;
  /// 剩下的尾部消息。
  QList<Message> tailMessages;
  /// 被摘出来的早期历史渲染成的纯文本（送给摘要模型）。
  QString transcript;
  /// 是否真的可以压缩（消息太少或没有可压缩区间时为 false）。
  bool applied = false;
};

/// 构造全压缩的素材：把头部历史摘出来、留下尾部。
///
/// 裁剪点只落在 User 消息边界上，保证 assistant/tool 配对不被拆开；
/// 同时至少留下 `policy.keepTailMessages` 条（以及一条当前用户消息，
/// 否则压缩完模型会看不到用户刚说的话）。
FullCompaction buildFullCompaction(const QList<Message> & messages,
                                   const CompactionPolicy & policy);

/// 构造替换掉早期历史的**合成摘要消息**（User 角色 + modelOnly，会下发给模型）。
Message makeCompactionSummaryMessage(const Id & sessionId, const QString & summary,
                                     const QString & modelId);

/// 构造对话流里的压缩分隔行（System 角色 + TimelinePart，不下发给模型）。
Message makeCompactionMarkerMessage(const Id & sessionId, const QString & summary,
                                    int replacedMessageCount);

/// 摘要消息的 metadata 标记见 lycode::compaction::kKind（core/Types.h）。

/// 从消息列表里恢复出最近一次压缩摘要。
/// 返回空串表示这段历史没有被压缩过。
QString restoreContextSummary(const QList<Message> & messages);

/// 为摘要模型构造提示词。
QString buildSummaryPrompt();

/// 把要摘要的历史渲染成纯文本（工具调用与结果都带工具名，便于模型引用）。
QString renderTranscriptForSummary(const QList<Message> & messages, int maxChars);

}  // namespace lycode
