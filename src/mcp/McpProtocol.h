// LyCode — MCP 协议类型
//
// MCP（Model Context Protocol）的 stdio 传输：客户端把服务器作为子进程拉起，
// 双方用**换行分隔的 JSON-RPC 2.0** 通信（不是 LSP 的 Content-Length 头）。
//
// 握手顺序是协议规定的，不能省：
//   1. → initialize（带 protocolVersion / capabilities / clientInfo）
//   2. ← result（服务器自己的能力与 serverInfo）
//   3. → notifications/initialized   ← 缺这一步很多服务器会拒绝后续请求
//   4. → tools/list
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace lycode::mcp {

/// 一个 MCP 服务器的配置。对应 settings.json 里 mcpServers 的一项。
struct ServerConfig {
    /// 用户起的名字。用于日志、工具名前缀，也是配置里的唯一键。
    QString id;
    /// 可执行文件（`npx`、`python3`、绝对路径…）。
    QString command;
    QStringList args;
    /// 追加的环境变量，形如 `KEY=VALUE`。会叠加在继承的环境之上。
    QStringList env;
    bool enabled = true;

    bool isValid() const { return !id.trimmed().isEmpty() && !command.trimmed().isEmpty(); }
};

/// 服务器报告的一个工具。
struct ToolInfo {
    /// 服务器端的原始名字。
    QString name;
    QString description;
    /// 入参 JSON Schema，原样透传给模型。
    QJsonObject inputSchema;

    /// MCP 的 annotations 提示。服务器可以声明工具是否只读。
    ///
    /// ⚠ 这是**服务器自述**，不是可信信息：恶意/写错的服务器可以把一个删库
    /// 工具标成只读。所以只用它来"放宽到免确认"，而绝不用它来"收紧"——
    /// 没声明就按最保守的来（需要确认）。
    bool readOnly = false;
    bool destructive = false;
};

/// 服务器连接状态，用于界面展示与日志。
enum class ServerState {
    Disabled,
    Starting,
    Ready,
    Failed,
};

QString serverStateToken(ServerState state);

}  // namespace lycode::mcp
