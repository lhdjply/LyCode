#include "mcp/McpManager.h"

#include "core/Logging.h"
#include "mcp/McpClient.h"
#include "tools/Tool.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QLoggingCategory>

namespace lycode::mcp {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.mcp")

/// 一个远端 MCP 工具在本地的适配器。
///
/// 它不做任何协议处理，只负责：把本地 Tool 契约翻译成一次 MCP 调用，
/// 以及**决定权限元数据**。后者是这里唯一需要动脑的地方：
/// 远端工具对我们是不透明的，默认必须要求确认。
class McpTool : public Tool {
public:
    McpTool(Client *client, QString serverId, QString toolName, ToolInfo info)
        : client_(client),
          serverId_(std::move(serverId)),
          toolName_(std::move(toolName)),
          info_(std::move(info)) {}

    ToolMetadata metadata() const override {
        ToolMetadata meta;
        meta.name = Manager::qualifiedToolName(serverId_, toolName_);
        meta.description = info_.description.isEmpty()
                               ? QStringLiteral("MCP 工具（来自服务器 %1）").arg(serverId_)
                               : info_.description;
        // 告诉模型"这是远端工具"，它会据此更谨慎地使用。
        meta.modelInstructions =
            QStringLiteral("此工具由外部 MCP 服务器 \"%1\" 提供。").arg(serverId_);

        // ⚠ 权限元数据的默认值是**保守**的，而且只允许服务器声明"放宽"：
        //   * readOnlyHint=true 时才免确认——这是服务器自述，只在放宽方向采信；
        //   * 没声明就按 System 处理，即每次都要用户确认。
        // 反过来（信任服务器的 destructiveHint=false 就放行）等于把权限交给了
        // 一个我们并不审计的第三方进程。
        meta.readOnly = info_.readOnly && !info_.destructive;
        meta.destructive = info_.destructive;
        meta.sideEffectScope = meta.readOnly ? SideEffectScope::None : SideEffectScope::System;
        meta.riskLevel = meta.readOnly ? RiskLevel::Low : RiskLevel::Medium;
        // 远端工具我们无法判断是否会并发写同一份状态，一律独占执行。
        meta.concurrency = meta.readOnly ? ToolMetadata::Concurrency::Safe
                                         : ToolMetadata::Concurrency::Serial;
        meta.needsApproval = !meta.readOnly;
        meta.maxOutputBytes = 256 * 1024;
        return meta;
    }

    QJsonObject inputSchema() const override {
        // 原样透传服务器的 schema。这里刻意不做修补：改动 schema 会让
        // 模型看到的入参与服务器实际期望的不一致，报错还很难定位。
        return info_.inputSchema;
    }

    QString title(const QJsonObject &input) const override {
        Q_UNUSED(input)
        return QStringLiteral("%1: %2").arg(serverId_, toolName_);
    }

    QString permissionDescription(const QJsonObject &input) const override {
        Q_UNUSED(input)
        return QStringLiteral("调用外部 MCP 服务器 \"%1\" 的工具 \"%2\"。"
                              "该工具的行为不受本应用审计。")
            .arg(serverId_, toolName_);
    }

    void execute(const QJsonObject &input, const ToolContext &, ToolCallback done) override {
        if (done == nullptr) {
            qCCritical(log) << "MCP 工具缺少回调，调用被丢弃; tool=" << toolName_;
            return;
        }
        if (client_ == nullptr) {
            done(ToolResult::failure(QStringLiteral("MCP 服务器连接已失效。"),
                                     QStringLiteral("mcp_disconnected")));
            return;
        }

        client_->callTool(toolName_, input,
                          [done, this](bool ok, const QString &text, const QJsonObject &raw) {
                              ToolResult result = ok
                                                      ? ToolResult::success(text)
                                                      : ToolResult::failure(
                                                            text, QStringLiteral("mcp_error"));
                              result.metadata.insert(QStringLiteral("mcpServer"), serverId_);
                              result.metadata.insert(QStringLiteral("mcpTool"), toolName_);
                              if (!raw.isEmpty()) {
                                  result.metadata.insert(QStringLiteral("mcpResult"), raw);
                              }
                              done(result);
                          });
    }

private:
    Client *client_ = nullptr;
    QString serverId_;
    QString toolName_;
    ToolInfo info_;
};

}  // namespace

QString serverStateToken(ServerState state) {
    switch (state) {
        case ServerState::Disabled:
            return QStringLiteral("disabled");
        case ServerState::Starting:
            return QStringLiteral("starting");
        case ServerState::Ready:
            return QStringLiteral("ready");
        case ServerState::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

QString Manager::toolNamePrefix(const QString &serverId) {
    return QStringLiteral("mcp__") + serverId + QStringLiteral("__");
}

QString Manager::qualifiedToolName(const QString &serverId, const QString &toolName) {
    return toolNamePrefix(serverId) + toolName;
}

Manager::Manager(QObject *parent) : QObject(parent) {}

Manager::~Manager() {
    stopAll();
}

Client *Manager::client(const QString &serverId) const {
    for (Client *candidate : clients_) {
        if (candidate->config().id == serverId) {
            return candidate;
        }
    }
    return nullptr;
}

void Manager::startAll(const QList<ServerConfig> &configs) {
    stopAll();

    for (const ServerConfig &config : configs) {
        if (!config.enabled) {
            qCInfo(log) << "跳过已禁用的 MCP 服务器; id=" << config.id;
            continue;
        }
        if (!config.isValid()) {
            qCWarning(log) << "MCP 服务器配置不完整，已跳过; id=" << config.id;
            emit serverFailed(config.id, QStringLiteral("配置不完整（需要 command）"));
            continue;
        }
        // 同名服务器只保留一个：否则工具名会撞车，注册表里互相覆盖。
        if (client(config.id) != nullptr) {
            qCWarning(log) << "重复的 MCP 服务器 id，已忽略后一个; id=" << config.id;
            continue;
        }

        auto *instance = new Client(config, this);
        clients_.append(instance);

        connect(instance, &Client::ready, this, [this, instance]() {
            // 先注册再发信号：槽函数里可能立刻要去用这些工具。
            emit serverReady(instance->config().id, instance->tools().size());
            emit changed();
        });
        connect(instance, &Client::failed, this,
                [this, instance](const QString &reason) {
                    emit serverFailed(instance->config().id, reason);
                    emit changed();
                });
        connect(instance, &Client::closed, this, [this]() { emit changed(); });

        instance->start();
    }
}

void Manager::stopAll() {
    const QList<Client *> snapshot = clients_;
    clients_.clear();
    registered_.clear();
    for (Client *instance : snapshot) {
        instance->stop();
        instance->deleteLater();
    }
}

int Manager::registerToolsInto(ToolRegistry &registry) {
    int added = 0;
    for (Client *instance : std::as_const(clients_)) {
        if (instance->state() != ServerState::Ready) {
            continue;
        }
        const QString serverId = instance->config().id;
        if (registered_.value(serverId, false)) {
            continue;  // 已经注册过，重复注册会白白覆盖同名工具
        }
        registered_.insert(serverId, true);

        for (const ToolInfo &info : instance->tools()) {
            // 工具名必须过滤：服务器返回的名字会直接进模型可见的声明，
            // 带空格/斜杠的名字在很多 provider 上会被拒。
            QString safeName = info.name;
            safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                             QStringLiteral("_"));
            if (safeName.isEmpty()) {
                qCWarning(log) << "MCP 工具名为空，已跳过; server=" << serverId;
                continue;
            }

            const QString qualified = qualifiedToolName(serverId, safeName);
            if (registry.contains(qualified)) {
                qCWarning(log) << "MCP 工具名与已有工具冲突，已跳过; name=" << qualified;
                continue;
            }
            registry.add(new McpTool(instance, serverId, info.name, info));
            ++added;
        }
    }
    if (added > 0) {
        qCInfo(log) << "已注册 MCP 工具; 数量=" << added;
    }
    return added;
}

bool Manager::isSettling() const {
    return pendingCount() > 0;
}

int Manager::pendingCount() const {
    int count = 0;
    for (const Client *instance : clients_) {
        if (instance->state() == ServerState::Starting) {
            ++count;
        }
    }
    return count;
}

ServerState Manager::state(const QString &serverId) const {
    if (Client *instance = client(serverId)) {
        return instance->state();
    }
    return ServerState::Disabled;
}

QString Manager::lastError(const QString &serverId) const {
    if (Client *instance = client(serverId)) {
        return instance->lastError();
    }
    return {};
}

int Manager::readyCount() const {
    int count = 0;
    for (const Client *instance : clients_) {
        if (instance->state() == ServerState::Ready) {
            ++count;
        }
    }
    return count;
}

int Manager::toolCount(const QString &serverId) const {
    if (Client *instance = client(serverId)) {
        return instance->tools().size();
    }
    return 0;
}

}  // namespace lycode::mcp
