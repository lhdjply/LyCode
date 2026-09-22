// ZCode Qt — 测试用的假 MCP 服务器
//
// 一个最小但**协议完整**的 stdio MCP 服务器：换行分隔的 JSON-RPC 2.0。
// 用它测客户端，比 mock 掉网络层更有价值——真正被验证的是"两个进程之间
// 按协议对话"这件事，包括握手顺序与消息分帧。
//
// 刻意包含两个边界情况：
//   * 往 stdout 打一行**非 JSON 日志**（Node 服务器常这么干）。
//     客户端必须跳过它而不是当成致命错误——这是最容易让集成"连不上"的原因。
//   * 一条 id 不连续的响应顺序（先回 tools/list 再回一个延迟的通知）。
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>

namespace {

void writeMessage(const QJsonObject &message) {
    // 一个消息一行，末尾必须有 '\n'：客户端按行切分。
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

void writeResult(const QJsonValue &id, const QJsonObject &result) {
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("result"), result);
    writeMessage(message);
}

void writeError(const QJsonValue &id, int code, const QString &text) {
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), text);
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("error"), error);
    writeMessage(message);
}

QJsonObject toolEntry(const QString &name, const QString &description, bool readOnly) {
    QJsonObject properties;
    properties.insert(QStringLiteral("text"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("text")});

    QJsonObject entry;
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("description"), description);
    entry.insert(QStringLiteral("inputSchema"), schema);
    if (readOnly) {
        // 只有"只读"这一个方向的提示会被客户端采信（放宽到免确认）。
        QJsonObject annotations;
        annotations.insert(QStringLiteral("readOnlyHint"), true);
        entry.insert(QStringLiteral("annotations"), annotations);
    }
    return entry;
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);

    bool initialized = false;

    QTextStream input(stdin);
    while (!input.atEnd()) {
        const QString line = input.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError parseError{};
        const QJsonObject message = QJsonDocument::fromJson(line.toUtf8(), &parseError).object();
        if (parseError.error != QJsonParseError::NoError) {
            err << "假服务器收到非 JSON 输入，已忽略\n";
            err.flush();
            continue;
        }

        const QString method = message.value(QStringLiteral("method")).toString();
        const QJsonValue id = message.value(QStringLiteral("id"));
        const bool isNotification = !message.contains(QStringLiteral("id"));

        if (method == QLatin1String("initialize")) {
            // 故意往 stdout 打一行人类日志：真实服务器（尤其 Node 生态）
            // 经常这么干，客户端必须能跳过。
            std::fwrite("fake-mcp-server: starting up\n", 1, 29, stdout);
            std::fflush(stdout);

            QJsonObject serverInfo;
            serverInfo.insert(QStringLiteral("name"), QStringLiteral("fake-mcp"));
            serverInfo.insert(QStringLiteral("version"), QStringLiteral("9.9.9"));

            QJsonObject result;
            result.insert(QStringLiteral("protocolVersion"),
                          QStringLiteral("2024-11-05"));
            result.insert(QStringLiteral("capabilities"),
                          QJsonObject{{QStringLiteral("tools"), QJsonObject{}}});
            result.insert(QStringLiteral("serverInfo"), serverInfo);
            writeResult(id, result);
            continue;
        }

        if (method == QLatin1String("notifications/initialized")) {
            initialized = true;
            continue;  // 通知不回响应
        }

        if (method == QLatin1String("tools/list")) {
            if (!initialized) {
                // 协议规定：没收到 initialized 通知之前不该提供服务。
                // 这里如实拒绝，用来验证客户端确实发了那个通知。
                writeError(id, -32002, QStringLiteral("尚未收到 initialized 通知"));
                continue;
            }
            QJsonObject result;
            result.insert(QStringLiteral("tools"),
                          QJsonArray{
                              toolEntry(QStringLiteral("echo"),
                                        QStringLiteral("回显输入（只读）"), true),
                              toolEntry(QStringLiteral("mutate"),
                                        QStringLiteral("修改状态（需要确认）"), false),
                          });
            writeResult(id, result);
            continue;
        }

        if (method == QLatin1String("tools/call")) {
            const QJsonObject params = message.value(QStringLiteral("params")).toObject();
            const QString name = params.value(QStringLiteral("name")).toString();
            const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();

            if (name == QLatin1String("echo")) {
                QJsonObject block;
                block.insert(QStringLiteral("type"), QStringLiteral("text"));
                block.insert(QStringLiteral("text"),
                             QStringLiteral("echo: ") +
                                 arguments.value(QStringLiteral("text")).toString());
                QJsonObject result;
                result.insert(QStringLiteral("content"), QJsonArray{block});
                result.insert(QStringLiteral("isError"), false);
                writeResult(id, result);
                continue;
            }
            if (name == QLatin1String("mutate")) {
                // 工具自己报告失败：走 isError，而不是 JSON-RPC error。
                QJsonObject block;
                block.insert(QStringLiteral("type"), QStringLiteral("text"));
                block.insert(QStringLiteral("text"), QStringLiteral("拒绝修改"));
                QJsonObject result;
                result.insert(QStringLiteral("content"), QJsonArray{block});
                result.insert(QStringLiteral("isError"), true);
                writeResult(id, result);
                continue;
            }
            writeError(id, -32602, QStringLiteral("未知工具：") + name);
            continue;
        }

        if (method == QLatin1String("tools/call.nonexistent")) {
            writeError(id, -32601, QStringLiteral("未知方法"));
            continue;
        }

        if (!isNotification) {
            writeError(id, -32601, QStringLiteral("未知方法：") + method);
        }
    }

    return 0;
}
