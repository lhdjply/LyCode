// ZCode Qt — 测试用假模型网关
//
// 一个极简的 HTTP 服务器，按预定队列返回 SSE 响应体。用途是把
// **真实的** Provider + SSE 解析 + Agent 循环 + UI 一起驱动起来，
// 而不是用假的 ModelProvider 把这几层跳过去（那几层恰恰最容易出错）。
//
// 只实现够用的 HTTP：等到 header 结束、再按 Content-Length 收齐 body，
// 然后回一个 Content-Length 固定的 SSE 响应并关闭连接。
//
// 头文件形式（无 Q_OBJECT）：信号连到普通成员函数不需要 moc，
// 这样测试可以只包含头文件而不引入额外的 moc 目标。
#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

namespace zcode::test {

class FakeGateway : public QObject {
public:
    explicit FakeGateway(QObject *parent = nullptr) : QObject(parent) {
        connect(&server_, &QTcpServer::newConnection, this, &FakeGateway::onConnection);
    }

    bool start() { return server_.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server_.serverPort(); }
    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(server_.serverPort());
    }

    /// 追加一段响应。请求按入队顺序消费；队列空时返回一个空回复。
    void enqueue(const QByteArray &sseBody, int statusCode = 200) {
        responses_.append({statusCode, sseBody});
    }

    int requestCount() const { return requestCount_; }
    QByteArray lastBody() const { return lastBody_; }

    /// 已经发出但未被消费的响应数（用于断言"第 2 轮才发请求"）。
    int queuedCount() const { return static_cast<int>(responses_.size()); }

private:
    struct Response {
        int status = 200;
        QByteArray body;
    };

    void onConnection() {
        while (QTcpSocket *socket = server_.nextPendingConnection()) {
            auto *buffer = new QByteArray;
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                buffer->append(socket->readAll());

                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;  // header 还没收齐
                }
                const QByteArray headers = buffer->left(headerEnd);
                int contentLength = 0;
                for (const QByteArray &line : headers.split('\n')) {
                    const QByteArray trimmed = line.trimmed();
                    if (trimmed.toLower().startsWith("content-length:")) {
                        contentLength = trimmed.mid(15).trimmed().toInt();
                    }
                }
                const int bodyStart = headerEnd + 4;
                if (buffer->size() - bodyStart < contentLength) {
                    return;  // body 还没收齐
                }

                const QByteArray body = buffer->mid(bodyStart, contentLength);
                delete buffer;
                // 断开与 this 的连接，避免后续数据再次进入这个已消费的 lambda
                // （此时 buffer 已释放，再用就是悬垂指针）。
                socket->disconnect(this);

                ++requestCount_;
                lastBody_ = body;

                const Response response = responses_.isEmpty()
                                              ? Response{200, QByteArray()}
                                              : responses_.takeFirst();

                const QByteArray payload = response.body;
                QByteArray out;
                out += "HTTP/1.1 " + QByteArray::number(response.status) + " OK\r\n";
                out += "Content-Type: text/event-stream\r\n";
                out += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
                out += "Connection: close\r\n\r\n";
                out += payload;
                socket->write(out);
                socket->flush();
                socket->disconnectFromHost();
            });
        }
    }

    QTcpServer server_;
    QList<Response> responses_;
    int requestCount_ = 0;
    QByteArray lastBody_;
};

/// 把一段原始文本转义成可以嵌进 JSON 字符串字面量里的形式。
/// 用于把工具入参原样塞进 `arguments` 字段，避免手写 \\\" 转义出错。
inline QByteArray jsonStringEscape(const QByteArray &raw) {
    QByteArray out;
    out.reserve(raw.size() + 8);
    for (const char character : raw) {
        switch (character) {
            case '"':
            case '\\':
                out += '\\';
                out += character;
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += character;
                break;
        }
    }
    return out;
}

/// 纯文本流式响应。
inline QByteArray textResponse(const QByteArray &text) {
    QByteArray body;
    body += "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{\"content\":\"" + text + "\"}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    body += "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20,"
            "\"total_tokens\":120}}\n\n";
    body += "data: [DONE]\n\n";
    return body;
}

/// 纯文本流式响应，并带上缓存用量明细。
///
/// 用于验证"prompt_tokens 含缓存、Usage::inputTokens 约定为不含缓存"这条归一：
/// OpenAI 的 prompt_tokens 必须减掉 cached_tokens 才等于未缓存输入。
inline QByteArray textResponseWithCache(const QByteArray &text, int promptTokens,
                                        int cachedTokens, int completionTokens) {
    QByteArray body;
    body += "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{\"content\":\"" + text + "\"}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    body += "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":" +
            QByteArray::number(promptTokens) + ",\"completion_tokens\":" +
            QByteArray::number(completionTokens) + ",\"total_tokens\":" +
            QByteArray::number(promptTokens + completionTokens) +
            ",\"prompt_tokens_details\":{\"cached_tokens\":" +
            QByteArray::number(cachedTokens) + "}}}\n\n";
    body += "data: [DONE]\n\n";
    return body;
}

/// 带工具调用的流式响应。参数故意分两片下发，以验证增量组装。
///
/// 契约（与 multiToolCallResponse 一致）：传入的是**原始 JSON 片段**，
/// 由本函数负责转义后嵌进 SSE。不要在这里手写 `\\\"`——
/// 两个构造器契约不一致正是踩过坑的地方。
inline QByteArray toolCallResponse(const QString &toolName,
                                   const QByteArray &argumentsFirstHalf,
                                   const QByteArray &argumentsSecondHalf) {
    QByteArray body;
    body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
            "\"function\":{\"name\":\"" +
            toolName.toUtf8() + "\",\"arguments\":\"\"}}]}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":"
            "{\"arguments\":\"" +
            jsonStringEscape(argumentsFirstHalf) + "\"}}]}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":"
            "{\"arguments\":\"" +
            jsonStringEscape(argumentsSecondHalf) + "\"}}]}}]}\n\n";
    body += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n";
    body += "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":10,"
            "\"total_tokens\":60}}\n\n";
    body += "data: [DONE]\n\n";
    return body;
}

/// 一次返回**多个**工具调用。用于验证并行调度。
/// `calls` 每项是 {工具名, 原始入参 JSON}。
inline QByteArray multiToolCallResponse(const QList<QPair<QString, QByteArray>> &calls) {
    QByteArray body;
    body += "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n";
    for (int index = 0; index < calls.size(); ++index) {
        const QByteArray position = QByteArray::number(index);
        const QByteArray callId = "call_" + QByteArray::number(index + 1);
        body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":" + position +
                ",\"id\":\"" + callId + "\",\"function\":{\"name\":\"" +
                calls.at(index).first.toUtf8() + "\",\"arguments\":\"\"}}]}}]}\n\n";
        body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":" + position +
                ",\"function\":{\"arguments\":\"" +
                jsonStringEscape(calls.at(index).second) + "\"}}]}}]}\n\n";
    }
    body += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n";
    body += "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":10,"
            "\"total_tokens\":60}}\n\n";
    body += "data: [DONE]\n\n";
    return body;
}

/// 把字符串转义成可以嵌进 SSE JSON 字符串里的形式（借用 QJsonDocument）。
inline QByteArray jsonEscape(const QString &value) {
    // 外壳前缀 `{"v":"` 是 6 个字符，后缀 `"}` 是 2 个字符。
    return QJsonDocument(QJsonObject{{QStringLiteral("v"), value}})
        .toJson(QJsonDocument::Compact)
        .mid(6)
        .chopped(2);
}

}  // namespace zcode::test
