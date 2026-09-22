// ZCode Qt — 统一 diff 生成
//
// 用途：Write / Edit 工具在改动文件后返回结构化补丁，UI 直接渲染，
// 模型也能据此确认"我到底改了什么"。
//
// 实现取舍：
//   * 用经典 LCS + 动态规划，而不是 Myers。文件规模在工具场景下是
//     "单文件、几万行以内"，O(N*M) 的 DP 完全够用，且实现短、易审计。
//     同时加了公共前后缀裁剪与规模上限保护，避免病态输入打爆内存。
//   * 输出格式与 npm 版 structuredPatch 对齐：按 hunk 分组，每行带 type。
//     这样 UI 不需要解析文本 diff，也就不会因为格式细节（"\ No newline"）
//     而出现展示偏差。
//   * 行内容一律以 '\n' 切分，不保留行尾符；渲染端自己决定换行。
#pragma once

#include <QJsonArray>
#include <QString>

namespace zcode {

/// 结构化补丁生成器。
class Diff {
public:
    /// 生成统一 diff 的 hunk 数组。
    /// 每个元素形如：
    ///   {"oldStart":1,"oldLines":3,"newStart":1,"newLines":4,
    ///    "lines":[{"type":"context"|"add"|"remove","text":"..."}]}
    /// 完全相同时返回空数组。`path` 仅用于日志。
    static QJsonArray unified(const QString &oldText, const QString &newText, const QString &path);

    /// 统计新增/删除行数。指针可为 null；hunks 为空数组时置 0。
    static void summary(const QJsonArray &hunks, int *additions, int *deletions);

    /// 把 hunk 数组渲染成文本统一 diff（`--- a/x` / `+++ b/x` / `@@`）。
    /// 供"需要在 output 里给模型看补丁"的场景使用。
    static QString render(const QJsonArray &hunks, const QString &path);

    /// 把文本按行切分（末尾换行不产生额外空行）。
    static QStringList splitLines(const QString &text);
};

}  // namespace zcode
