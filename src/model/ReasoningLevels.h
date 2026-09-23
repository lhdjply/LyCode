// LyCode — 思考等级
//
// "思考等级"是同一件事在两种协议里的不同表达：
//   * Anthropic Messages：`thinking: { type: "enabled", budget_tokens: N }`
//     —— 用一个 token 预算表达强度，必须 >= 1024 且 < max_tokens。
//   * OpenAI 兼容：`reasoning_effort: "low" | "medium" | "high"`
//     —— 用枚举表达强度，不需要算预算。
//
// 本文件把两者收敛到一张**稳定的档位表**上：档位 id 是持久化用的键
// （存进 ModelSelection::reasoningLevel 与配置文件），因此一旦发布就不能改名。
// 新增档位只能往后加，不能改已有 id 的含义。
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/Types.h"

namespace lycode
{

/// 一个思考档位。
struct ReasoningLevel {
  /// 稳定 id，用于持久化。取值：off / low / medium / high / max。
  QString id;
  /// 面向用户的标签（中文）。
  QString label;
  /// 传给 Anthropic 的 thinking.budget_tokens。0 表示不开启思考。
  int budgetTokens = 0;
  /// 传给 OpenAI 兼容协议的 reasoning_effort。空表示不传该字段。
  QString openAiEffort;
};

/// 全部标准档位，顺序即 UI 展示顺序（由弱到强）。
const QList<ReasoningLevel> & standardReasoningLevels();

/// 按 id 查档位；未知 id 返回 nullptr。
const ReasoningLevel * findReasoningLevel(const QString & id);

/// 档位 id → 用户标签；未知 id 原样返回（不隐藏未知值，避免旧配置显示为空）。
QString reasoningLevelLabel(const QString & id);

/// 把 id 列表渲染成一行标签文本，用于设置页的只读展示。
QString formatReasoningLevels(const QStringList & ids);

/// Anthropic 要求 budget_tokens 至少 1024，且必须小于 max_tokens
/// （否则请求会被直接拒绝）。这里做一次收敛，保证发出的请求一定合法。
int clampReasoningBudget(int budgetTokens, int maxOutputTokens);

/// 规整用户/配置里请求的档位：
///   * 请求为空 → 取 `supported` 里第一个"非 off"的档位；都没有则返回空
///   * 请求不合法或不被 `supported` 支持 → 回退到第一个可用档位
///   * `supported` 为空 → 返回空（表示该模型不支持思考，不传任何参数）
/// 返回空字符串的含义是"关闭思考"。
QString normalizeReasoningLevel(const QString & requested, const QStringList & supported);

/// 该模型是否应当展示思考档位选择器。
bool modelSupportsReasoning(const ModelInfo & info);

/// 默认档位列表：模型自报为空时使用的保守集合（off + low/medium/high）。
QStringList defaultReasoningLevelIds();

}  // namespace lycode
