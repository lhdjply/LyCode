// LyCode — 系统提示词组装
//
// 提示词由若干独立片段拼成，顺序固定，便于缓存（前缀稳定 = 命中 provider 的
// prompt cache）与测试。任何片段的内容变化都要先改这里，不允许 Agent 循环里
// 临时拼接字符串。
//
// 片段顺序：
//   1. 身份与总体行为准则
//   2. 环境信息（工作区、平台、日期）
//   3. 工具使用规范（**只写通用规范，不列举工具 schema**）
//   4. 会话模式附加约束（plan / build / edit / yolo）
//   5. 项目说明文件（AGENTS.md，按层级合并）
//
// ⚠ 重要约束（本实现的既定设计）：**工具的参数说明只经 provider 的
// `tools` 字段下发，绝不再镜像进系统提示词**。把 schema 抄进提示词会造成
// 两处定义漂移，并白白占用上下文预算。
#pragma once

#include <QString>
#include <QStringList>

#include "core/Types.h"
#include "tools/Tool.h"

namespace lycode
{

/// 项目说明文件的候选名，按优先级排列。
extern const QStringList kProjectInstructionFileNames;

/// 项目说明文件的默认读取上限（按既定语义：单文件 100KiB）。
constexpr int kProjectInstructionMaxBytes = 100 * 1024;

struct SystemPromptInput {
  Workspace workspace;
  QString cwd;
  SessionMode mode = SessionMode::Build;
  PermissionMode permissionMode = PermissionMode::Default;
  /// 已注册的工具声明。仅用于判断"有哪些能力"，不用于生成 schema 说明。
  QList<ToolSpec> tools;
  /// 应用版本，用于环境信息。
  QString appVersion;
  /// 覆盖"今天日期"，便于测试稳定输出。
  QString todayOverride;
  /// 已知的项目上下文（语言、包管理器、构建文件），由 detectProjectContext 得到。
  QStringList projectLanguages;
  QString projectPackageManager;
  QStringList projectBuildFiles;
  /// 关闭项目说明文件读取（测试用）。
  bool skipProjectInstructions = false;

  /// 非空表示这是子代理运行时：使用子代理身份与说明，
  /// 而不是主代理身份。取值来自 SubagentProfile::id。
  QString subagentType;
  /// 派生子代理时给出的任务描述，用于让子代理确认自己的职责边界。
  QString subagentDescription;

  /// Skills 清单片段（名字 + 描述）。由 SkillLibrary 生成，这里只负责插入——
  /// 让提示词层依赖一个字符串而不是 skills 库，避免 agent/ → skills/ 的耦合。
  /// 为空表示没有可用技能。
  QString skillsSection;
};

class SystemPromptBuilder
{
  public:
    /// 生成完整系统提示词。
    static QString build(const SystemPromptInput & input);

    /// 从用户级目录与工作区逐层向上收集项目说明文件内容。
    ///
    /// 候选来源最多两个：`<数据目录>/AGENTS.md`（用户级）与从 cwd 向上到
    /// 项目根（第一个含 `.git` 的目录）找到的**第一个** `AGENTS.md`（工作区级）。
    /// 用户级在前、工作区级在后，用空行拼接。两者都缺失时返回空字符串。
    static QString loadProjectInstructions(const QString & workspacePath, const QString & cwd,
                                           int maxBytesPerFile = kProjectInstructionMaxBytes);

    /// 环境信息片段（工作区、cwd、平台、日期、项目上下文）。
    static QString environmentSection(const SystemPromptInput & input);

    /// 工具使用通用规范片段。**不列举工具名与参数**，只写跨工具的行为准则。
    static QString toolingNormsSection(const QList<ToolSpec> & tools);

    /// 会话模式约束片段。
    static QString modeSection(SessionMode mode, PermissionMode permissionMode);

    /// 子代理身份片段。取代主代理身份：子代理的职责边界与主代理不同
    /// （无人对话、只看结论、不得再派生子代理）。
    static QString subagentIdentitySection(const QString & subagentType,
                                           const QString & description);

    /// 子代理的模式约束。与 modeSection 分开：主代理的 plan 模式要求
    /// "产出一份计划供用户批准"，而只读子代理要的是"把结论讲清楚"。
    /// 混用会让只读子代理去写计划而不是给调查结果。
    static QString subagentModeSection(SessionMode mode);

    /// 探测项目上下文：语言、包管理器、构建文件。
    static void detectProjectContext(const QString & workingDirectory,
                                     QStringList * languagesOut,
                                     QString * packageManagerOut,
                                     QStringList * buildFilesOut);

    /// 项目根：自 startDirectory 向上找到的第一个含 `.git` 的目录；找不到返回空。
    static QString findProjectRoot(const QString & startDirectory);
};

}  // namespace lycode

