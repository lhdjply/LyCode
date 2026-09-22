// ZCode Qt — MCP stdio 客户端
//
// 一个实例管一个服务器子进程。职责：
//   * 拉起进程、按行切分 JSON-RPC 消息
//   * 完成 initialize / notifications/initialized / tools/list 握手
//   * 转发 tools/call 并把结果翻译成 ToolResult 可用的形状
//
// 常见坑（都在这份实现里处理了）：
//   * 服务器会把**非 JSON 的日志**打到 stdout（很多 Node 服务器会），
//     按行解析时必须跳过解析失败的行而不是当成致命错误。
//   * JSON-RPC 的**通知**（没有 id）不该被当成响应去找待办请求。
//   * 一个响应可能和请求的顺序不一致；必须按 id 匹配，不能按到达顺序。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

#include "mcp/McpProtocol.h"

class QProcess;

namespace zcode::mcp {

class Client : public QObject {
    Q_OBJECT

public:
    /// 单次请求的超时。服务器卡死时不能让工具调用永远挂着。
    static constexpr int kRequestTimeoutMs = 20000;
    /// 握手（initialize + tools/list）的整体超时。
    static constexpr int kHandshakeTimeoutMs = 30000;

    explicit Client(ServerConfig config, QObject *parent = nullptr);
    ~Client() override;

    const ServerConfig &config() const { return config_; }
    ServerState state() const { return state_; }
    QString lastError() const { return lastError_; }
    QList<ToolInfo> tools() const { return tools_; }
    QString serverName() const { return serverName_; }
    QString serverVersion() const { return serverVersion_; }

    /// 拉起进程并开始握手。结果通过 ready / failed 通知。
    void start();
    /// 终止服务器进程（幂等）。
    void stop();

    /// 调用一个工具。回调**恰好一次**；成功时 `ok` 为真、`text` 为内容。
    using Callback = std::function<void(bool ok, const QString &text,
                                        const QJsonObject &rawResult)>;
    void callTool(const QString &toolName, const QJsonObject &arguments, Callback done);

signals:
    /// 握手完成、工具清单可用。
    void ready();
    /// 启动或握手失败（含超时、进程退出）。
    void failed(const QString &reason);
    /// 进程结束。
    void closed();

private:
    void handleStdout();
    void handleStderr();
    void handleProcessError();
    void handleFinished(int exitCode);
    void dispatch(const QJsonObject &message);
    void sendRequest(const QString &method, const QJsonObject &params,
                     std::function<void(const QJsonObject &result, const QString &error)> done,
                     int timeoutMs = kRequestTimeoutMs);
    void sendNotification(const QString &method, const QJsonObject &params = {});
    void writeMessage(const QJsonObject &message);
    void setFailed(const QString &reason);
    void timeoutHandshake();

    ServerConfig config_;
    ServerState state_ = ServerState::Disabled;
    QString lastError_;
    QString serverName_;
    QString serverVersion_;
    QList<ToolInfo> tools_;

    QProcess *process_ = nullptr;
    /// stdout 的按行缓冲：TCP/管道不保证一次读到一个完整消息。
    QByteArray buffer_;
    int nextId_ = 1;
    /// 待办请求：id → 回调。
    QHash<int, std::function<void(const QJsonObject &, const QString &)>> pending_;
    /// 每个请求的超时定时器，挂在 process_ 下，键为 id。
    QHash<int, QObject *> timeouts_;
    bool handshakeDone_ = false;
};

}  // namespace zcode::mcp
