// LyCode — MCP 服务器管理
//
// 负责按配置拉起全部服务器，并把它们报告的工具注册进 ToolRegistry，
// 使模型看到的 MCP 工具与内置工具**没有任何区别**（同一套声明、权限链、
// 并行调度、结果渲染）。
//
// 工具命名：`mcp__<server>__<tool>`（与 Claude Code 惯例一致）。加前缀有两个
// 理由：避免不同服务器的同名工具互相覆盖；让模型与用户一眼看出这个工具的
// 归属与信任边界（远端工具 vs 本地内置工具）。
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "mcp/McpProtocol.h"

namespace lycode {
class ToolRegistry;
}

namespace lycode::mcp {

class Client;

/// MCP 服务器集合的管理者。
class Manager : public QObject {
    Q_OBJECT

public:
    /// 工具名前缀，也用于从工具名反查服务器。
    static QString toolNamePrefix(const QString &serverId);
    /// 生成注册到工具表里的完整名字。
    static QString qualifiedToolName(const QString &serverId, const QString &toolName);

    explicit Manager(QObject *parent = nullptr);
    ~Manager() override;

    /// 按配置启动全部启用的服务器。**异步**：立即返回，就绪后发 serverReady。
    void startAll(const QList<ServerConfig> &configs);
    /// 停止全部服务器（幂等）。
    void stopAll();

    /// 把已经就绪的服务器所提供的工具注册进 `registry`。
    ///
    /// 必须在对应的 serverReady 之后调用——此时工具清单才拿到。
    /// 已注册过的工具会被跳过（同名覆盖由 ToolRegistry 处理）。
    int registerToolsInto(ToolRegistry &registry);

    /// 是否还有服务器在握手中（既没就绪也没失败）。
    /// 界面据此决定"要不要等 MCP 就绪再发第一次请求"。
    bool isSettling() const;
    /// 尚未就绪也未失败的服务器数量。
    int pendingCount() const;

    ServerState state(const QString &serverId) const;
    QString lastError(const QString &serverId) const;
    /// 多行的失败诊断（状态、子进程、stderr、stdout 计数）。
    /// 只用于日志/测试；查不到该 id 时返回一句说明而不是空串。
    QString diagnostics(const QString &serverId) const;
    int readyCount() const;
    /// 单个服务器的工具数量（未就绪为 0）。
    int toolCount(const QString &serverId) const;

signals:
    /// 某个服务器握手完成、工具清单可用。
    void serverReady(const QString &serverId, int toolCount);
    /// 某个服务器启动或握手失败。
    void serverFailed(const QString &serverId, const QString &reason);
    /// 有服务器就绪或失败（供界面刷新状态）。
    void changed();

private:
    Client *client(const QString &serverId) const;

    QList<Client *> clients_;
    /// 已经注册过工具的服务器，避免重复注册。
    QHash<QString, bool> registered_;
};

}  // namespace lycode::mcp
