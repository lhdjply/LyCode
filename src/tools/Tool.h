// ZCode Qt — 工具系统契约
//
// 工具是 Agent 与真实世界交互的唯一通道。设计核心（照搬 npm 版最值得复用的
// 一处设计）：**策略完全由声明式 metadata 驱动，执行器与权限判定从不按工具名
// 猜测行为**。
//
// 一份 ToolMetadata 同时决定三件事：
//   1. 调度并发分组  —— destructive/concurrency/readOnly/sideEffectScope
//   2. 权限判定      —— readOnly/riskLevel/needsApproval/alwaysAsk/sideEffectScope
//   3. turn 终止     —— stopTurnOnSuccess
// 新增工具只需要声明 metadata，不需要在调度器或权限服务里加分支。
//
// 执行默认异步：Bash / Grep / Glob 都可能跑很久，绝不能在 GUI 线程阻塞。
// 纯内存工具（TodoWrite 等）可以立即调用 done()。
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

#include "core/Types.h"
#include "model/ModelProvider.h"

namespace zcode {

/// 工具副作用范围。
///
/// 改写工作区的集合是 {Workspace, Git, System}；`Session` 与 `UserInteraction`
/// 只影响会话内部状态，不"碰外部世界"。这个区分是权限判定的基础。
enum class SideEffectScope {
    None,             ///< 无副作用（纯计算）
    Workspace,        ///< 读写工作区文件
    Git,              ///< 改动 git 状态
    Network,          ///< 发起网络请求
    System,           ///< 执行系统命令 / 进程
    Session,          ///< 仅改动会话状态
    UserInteraction,  ///< 需要用户参与（提问、计划审批）
};

QString toToken(SideEffectScope scope);
SideEffectScope sideEffectScopeFromToken(const QString &value);
/// 该范围是否属于"改写工作区"的写入集。
bool sideEffectScopeWritesWorkspace(SideEffectScope scope);
/// 该范围是否触碰会话之外的世界。
bool sideEffectScopeTouchesOutsideWorld(SideEffectScope scope);

/// 工具声明式元数据。字段语义与 npm 版 ToolMetadata 对齐。
struct ToolMetadata {
    QString name;
    QString description;
    /// 写给模型的额外使用提示，会并入工具声明描述。
    QString modelInstructions;

    /// 是否允许在 plan（只读）模式下使用。
    bool allowedInPlanMode = false;
    /// 是否只读（不产生任何持久化影响）。
    bool readOnly = false;
    /// 是否具备破坏性（用于并发分组与风险评估）。
    bool destructive = false;
    /// 并发策略。**三态**，不是 bool。
    ///
    /// npm 里 `concurrentSafe` 是 `boolean | undefined`：显式 true、显式 false、
    /// 未声明是三件不同的事。用 bool 会把"显式声明必须串行"和"未声明"混为一谈——
    /// 于是显式声明串行的只读工具会被"只读且无副作用 ⇒ 可并发"的豁免分支
    /// 错误地放行。这个缺陷在写测试时被真实暴露出来。
    enum class Concurrency {
        Unspecified,  ///< 未声明：按只读与副作用范围推断
        Safe,         ///< 显式声明可与其他工具并发
        Serial,       ///< 显式声明必须独占执行
    };
    Concurrency concurrency = Concurrency::Unspecified;
    /// 是否需要用户交互（提问、计划审批）。
    bool requiresUserInteraction = false;

    /// 单次执行超时（毫秒）。0 表示使用执行器默认值。
    int timeoutMs = 0;
    /// 输出字节上限，超出按 resultBudget 策略截断或落盘。
    int maxOutputBytes = 256 * 1024;

    SideEffectScope sideEffectScope = SideEffectScope::None;
    RiskLevel riskLevel = RiskLevel::Low;

    /// 是否需要用户批准（即使 yolo 模式也要经过 alwaysAsk 链）。
    bool needsApproval = false;
    /// 是否无条件询问（可穿透 yolo 与 plan 的直通规则）。
    bool alwaysAsk = false;

    /// 是否对模型可见。false 表示仅作为别名/内部工具存在。
    bool providerVisible = true;
    /// 成功后是否终止本轮 turn（如 submit_result）。
    bool stopTurnOnSuccess = false;
    /// 本会话重复调用是否无额外收益（如 TodoRead），供 UI 提示。
    bool idempotent = true;
};

/// 由 metadata 推导粗粒度权限类别，用于 UI 风险呈现与默认策略。
PermissionKind permissionKindFor(const ToolMetadata &metadata);

/// 工具执行结果。
struct ToolResult {
    bool ok = false;
    /// 面向模型与用户的展示文本。会被截断到展示预算之内。
    QString output;
    /// 失败原因；ok == false 时应填写。
    QString error;
    /// 错误码，便于程序化处理（与 npm 的 toolCallRow.error.code 对齐）。
    QString errorCode;
    /// 结构化补充：cwd、exitCode、durationMs、path、diff 等。
    QJsonObject metadata;

    /// 成功后是否终止本轮 turn。由工具按 metadata.stopTurnOnSuccess 或
    /// 动态条件（如 ExitPlanMode 被拒）设置。
    bool stopTurnAfterResult = false;

    static ToolResult success(const QString &output, const QJsonObject &metadata = {});
    static ToolResult failure(const QString &error, const QString &errorCode = {},
                              const QJsonObject &metadata = {});
};

/// 会话 todo 存储。由 AgentRuntime 持有并注入，工具只通过它读写，
/// 保证 todo 的唯一所有者是会话而不是工具实例。
class TodoStore;

/// 执行上下文。由 Agent 主循环构造，工具只读。
struct ToolContext {
    Id sessionId;
    Id turnId;
    Workspace workspace;
    /// 命令与相对路径的解析基准目录。
    QString workingDirectory;

    /// 会话级权限模式；工具用它判断是否处于只读语义。
    PermissionMode permissionMode = PermissionMode::Default;
    /// Plan/Ask 模式为 true：工具必须拒绝任何副作用，而不是默默执行。
    bool readOnly = false;

    /// 已获批的权限规则集合。
    QList<PermissionRule> grantedRules;

    /// 会话 todo 存储。不得为空——AgentRuntime 必须注入。
    TodoStore *todoStore = nullptr;

    /// 进度回调，可空。用于把长任务的中间输出推给 UI。
    std::function<void(const QString &chunk)> progress;

    /// 取消令牌；工具应在长循环中轮询它。
    std::shared_ptr<std::atomic_bool> cancelled;

    bool isCancelled() const;

    /// 解析工作区内的路径。相对路径按 workingDirectory 解析；
    /// 越界（逃出工作区）时返回空字符串。
    QString resolvePath(const QString &pathOrRelative) const;
    /// 把绝对路径表示成工作区内的相对路径（用于展示）；不在工作区内则原样返回。
    QString displayPath(const QString &absolutePath) const;
};

/// 工具执行完成回调。必须恰好被调用一次。
using ToolCallback = std::function<void(ToolResult)>;

/// 工具抽象。
class Tool {
public:
    virtual ~Tool();

    /// 声明式元数据。name 必须非空且唯一。
    virtual ToolMetadata metadata() const = 0;

    /// JSON Schema 对象，描述入参。
    virtual QJsonObject inputSchema() const = 0;

    /// 权限能力名，用于规则匹配与审计（如 read / edit / bash / webfetch）。
    /// 默认取 metadata().name，需要与工具名解耦的工具（如 Write/Edit 共享 edit）可覆写。
    virtual QString permissionCapability() const;

    /// 该次调用的规则主体：从入参里取一个有代表性的字符串（命令、路径、URL）。
    /// 用于 `allow Bash(npm test:*)` 这类规则的匹配。
    virtual QString ruleSubject(const QJsonObject &input) const;

    /// 一行人类可读摘要，例如 `Bash: pnpm build`。
    virtual QString title(const QJsonObject &input) const;

    /// 生成权限请求的说明文案。默认给出通用描述。
    virtual QString permissionDescription(const QJsonObject &input) const;

    /// 可持久化的"始终允许"规则；返回空列表表示不提供该选项。
    virtual QList<PermissionRule> permissionRules(const QJsonObject &input) const;

    /// 执行。实现必须保证 done 恰好被调用一次，即使失败。
    virtual void execute(const QJsonObject &input, const ToolContext &context,
                         ToolCallback done) = 0;

    /// 入参校验：返回空字符串表示通过，否则是错误说明。
    /// 默认实现检查 inputSchema 的 required 字段是否存在且非 null。
    virtual QString validateInput(const QJsonObject &input) const;

    /// 拼装给模型的声明。
    ToolSpec spec() const;

    /// 并发分组判定（照搬 npm 的 canRunInParallel）。
    bool canRunInParallel() const;

protected:
    /// 读取字符串参数，缺失或类型不符时返回 fallback。
    static QString stringArg(const QJsonObject &input, const QString &key,
                             const QString &fallback = {});
    static bool boolArg(const QJsonObject &input, const QString &key, bool fallback = false);
    static int intArg(const QJsonObject &input, const QString &key, int fallback = 0);
    static QJsonArray arrayArg(const QJsonObject &input, const QString &key);
};

/// 工具注册表：名字（含别名）到实现的映射，并负责生成模型的工具声明列表。
class ToolRegistry {
public:
    ToolRegistry();
    ~ToolRegistry();

    ToolRegistry(const ToolRegistry &) = delete;
    ToolRegistry &operator=(const ToolRegistry &) = delete;

    // 移动构造/赋值：createWithBuiltins() 声明为按值返回，而拷贝构造被删除。
    // 没有移动构造时 `return registry;` 是 ill-formed（NRVO 不是语言保证，
    // 且用户声明的拷贝构造会抑制隐式移动构造），因此这里必须显式声明。
    // 注册表不持有工具所有权，移动只是搬运 entries_/index_。
    ToolRegistry(ToolRegistry &&other) noexcept;
    ToolRegistry &operator=(ToolRegistry &&other) noexcept;

    /// 注册工具；同名覆盖并返回被替换的实现（调用方持有所有权）。
    /// 别名不覆盖已有主名，避免别名悄悄遮蔽真实工具。
    Tool *add(Tool *tool, const QStringList &aliases = {});
    /// 取工具实现（含别名），不存在返回 nullptr。注册表不转移所有权。
    Tool *find(const QString &name) const;
    bool contains(const QString &name) const;

    /// 主名列表（不含别名）。
    QStringList names() const;
    /// 全部可被模型调用的声明（仅 providerVisible）。
    QList<ToolSpec> specs() const;
    /// 按名字过滤出声明列表（用于工具白名单/黑名单）。
    QList<ToolSpec> specsFor(const QStringList &allowedNames) const;
    /// 按黑名单剔除后的声明列表。名字支持 `Name` 与 `Name(pattern)` 两种形式。
    QList<ToolSpec> specsExcluding(const QStringList &disallowedPatterns) const;

    /// 取出全部实现（主名，不含别名）。
    QList<Tool *> tools() const;

    /// 注册全部内置工具。返回注册后的注册表（按值移动）。
    static ToolRegistry createWithBuiltins();

private:
    struct Entry {
        QString name;
        Tool *tool = nullptr;
        QStringList aliases;
    };
    QList<Entry> entries_;
    QHash<QString, Tool *> index_;
};

}  // namespace zcode
