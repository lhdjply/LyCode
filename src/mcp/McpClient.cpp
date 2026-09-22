#include "mcp/McpClient.h"

#include "core/Json.h"
#include "core/Logging.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace lycode::mcp {
namespace {

Q_LOGGING_CATEGORY(log, "lycode.mcp")

/// 协议版本。写死一个我们确实实现的版本，而不是声明"最新"——
/// 服务器会按协商结果决定用哪套能力，虚报会导致它发来我们解析不了的东西。
constexpr char kProtocolVersion[] = "2024-11-05";

/// 把 MCP 的 content 数组拼成纯文本。
///
/// 只取 text 类型：image / resource 需要额外的落盘与转发链路，本阶段不做，
/// 但要在文本里**明确说明**被略过了哪些块——静默丢弃会让模型以为工具没返回内容。
QString contentToText(const QJsonArray &content) {
    QStringList chunks;
    int skipped = 0;
    for (const QJsonValue &value : content) {
        const QJsonObject block = value.toObject();
        const QString type = json::str(block, QStringLiteral("type"));
        if (type == QLatin1String("text")) {
            chunks.append(json::str(block, QStringLiteral("text")));
        } else if (type == QLatin1String("resource")) {
            const QJsonObject resource = json::object(block, QStringLiteral("resource"));
            const QString text = json::str(resource, QStringLiteral("text"));
            if (!text.isEmpty()) {
                chunks.append(text);
            } else {
                ++skipped;
            }
        } else if (!type.isEmpty()) {
            ++skipped;
        }
    }
    if (skipped > 0) {
        chunks.append(QStringLiteral("（另有 %1 个非文本内容块未展示：%2）")
                          .arg(skipped)
                          .arg(QStringLiteral("本阶段只处理 text/resource 文本")));
    }
    return chunks.join(QLatin1Char('\n'));
}

}  // namespace

Client::Client(ServerConfig config, QObject *parent)
    : QObject(parent), config_(std::move(config)) {}

Client::~Client() {
    stop();
}

void Client::start() {
    if (process_ != nullptr) {
        return;
    }
    if (!config_.isValid()) {
        setFailed(QStringLiteral("MCP 服务器配置不完整（需要 id 与 command）"));
        return;
    }

    state_ = ServerState::Starting;
    process_ = new QProcess(this);
    process_->setProgram(config_.command);
    process_->setArguments(config_.args);
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    // 叠加用户声明的环境变量。不整体替换环境——服务器通常需要 PATH 等。
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &entry : config_.env) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0) {
            environment.insert(entry.left(equals), entry.mid(equals + 1));
        } else {
            qCWarning(log) << "忽略格式错误的环境变量（应为 KEY=VALUE）; server="
                           << config_.id << "entry=" << entry;
        }
    }
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::readyReadStandardOutput, this, &Client::handleStdout);
    connect(process_, &QProcess::readyReadStandardError, this, &Client::handleStderr);
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) { handleProcessError(); });
    connect(process_, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) { handleFinished(exitCode); });

    qCInfo(log) << "启动 MCP 服务器; id=" << config_.id << "command=" << config_.command
                << "args=" << config_.args.join(QLatin1Char(' '));
    process_->start();

    QTimer::singleShot(kHandshakeTimeoutMs, this, &Client::timeoutHandshake);

    // ── 握手 ────────────────────────────────────────────────────────────────
    QJsonObject clientInfo;
    clientInfo.insert(QStringLiteral("name"), QStringLiteral("lycode"));
    clientInfo.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());

    QJsonObject params;
    params.insert(QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion));
    params.insert(QStringLiteral("capabilities"), QJsonObject{});
    params.insert(QStringLiteral("clientInfo"), clientInfo);

    sendRequest(
        QStringLiteral("initialize"), params,
        [this](const QJsonObject &result, const QString &error) {
            if (!error.isEmpty()) {
                setFailed(QStringLiteral("initialize 失败：%1").arg(error));
                return;
            }
            serverName_ = json::str(json::object(result, QStringLiteral("serverInfo")),
                                    QStringLiteral("name"));
            serverVersion_ = json::str(json::object(result, QStringLiteral("serverInfo")),
                                       QStringLiteral("version"));

            // 协议要求：initialize 之后必须发这个通知，否则不少服务器会
            // 拒绝后续请求（它们用它来确认客户端已经准备好）。
            sendNotification(QStringLiteral("notifications/initialized"));

            sendRequest(QStringLiteral("tools/list"), QJsonObject{},
                        [this](const QJsonObject &listResult, const QString &listError) {
                            if (!listError.isEmpty()) {
                                setFailed(QStringLiteral("tools/list 失败：%1").arg(listError));
                                return;
                            }
                            tools_.clear();
                            for (const QJsonValue &value :
                                 json::array(listResult, QStringLiteral("tools"))) {
                                const QJsonObject item = value.toObject();
                                ToolInfo info;
                                info.name = json::str(item, QStringLiteral("name"));
                                info.description = json::str(item, QStringLiteral("description"));
                                info.inputSchema =
                                    json::object(item, QStringLiteral("inputSchema"));
                                const QJsonObject annotations =
                                    json::object(item, QStringLiteral("annotations"));
                                info.readOnly =
                                    json::boolean(annotations, QStringLiteral("readOnlyHint"));
                                info.destructive =
                                    json::boolean(annotations, QStringLiteral("destructiveHint"));
                                if (!info.name.isEmpty()) {
                                    tools_.append(info);
                                }
                            }
                            handshakeDone_ = true;
                            state_ = ServerState::Ready;
                            qCInfo(log) << "MCP 服务器就绪; id=" << config_.id
                                        << "server=" << serverName_ << serverVersion_
                                        << "工具数=" << tools_.size();
                            emit ready();
                        });
        });
}

void Client::setFailed(const QString &reason) {
    lastError_ = reason;
    state_ = ServerState::Failed;
    qCWarning(log) << "MCP 服务器失败; id=" << config_.id << "reason=" << reason;
    emit failed(reason);
}

void Client::timeoutHandshake() {
    if (handshakeDone_ || state_ == ServerState::Failed) {
        return;
    }
    setFailed(QStringLiteral("握手超时（%1 ms）").arg(kHandshakeTimeoutMs));
}

void Client::stop() {
    if (process_ == nullptr) {
        return;
    }
    QProcess *process = process_;
    process_ = nullptr;  // 先摘掉指针：下面的信号处理不该再走一遍收尾
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(3000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    process->deleteLater();
    pending_.clear();
    for (QObject *timer : std::as_const(timeouts_)) {
        timer->deleteLater();
    }
    timeouts_.clear();
    if (state_ != ServerState::Failed) {
        state_ = ServerState::Disabled;
    }
}

void Client::handleProcessError() {
    if (process_ == nullptr) {
        return;
    }
    // FailedToStart 是最常见也最需要说清楚的：命令不存在、没有执行权限。
    if (process_->error() == QProcess::FailedToStart) {
        setFailed(QStringLiteral("无法启动“%1”：%2")
                      .arg(config_.command, process_->errorString()));
    }
}

void Client::handleFinished(int exitCode) {
    if (process_ == nullptr) {
        return;
    }
    // 握手还没完成就退出，是最容易让人困惑的情况：报错里必须带上 stderr 片段。
    if (!handshakeDone_ && state_ == ServerState::Starting) {
        setFailed(QStringLiteral("进程在握手完成前退出（exitCode=%1）。%2")
                      .arg(exitCode)
                      .arg(QString::fromUtf8(process_->readAllStandardError()).trimmed().left(300)));
    }
    emit closed();
}

void Client::handleStderr() {
    if (process_ == nullptr) {
        return;
    }
    const QString text =
        QString::fromUtf8(process_->readAllStandardError()).trimmed();
    if (!text.isEmpty()) {
        // 不当作错误：很多服务器把正常日志写到 stderr。记下来便于排查。
        qCDebug(log) << "MCP stderr; id=" << config_.id << text.left(500);
    }
}

void Client::writeMessage(const QJsonObject &message) {
    if (process_ == nullptr || process_->state() == QProcess::NotRunning) {
        return;
    }
    // 换行分隔：一个消息一行，末尾必须补 '\n'，否则服务器会一直等下一个字段。
    process_->write(json::toBytes(message) + '\n');
}

void Client::sendNotification(const QString &method, const QJsonObject &params) {
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("method"), method);
    if (!params.isEmpty()) {
        message.insert(QStringLiteral("params"), params);
    }
    writeMessage(message);
}

void Client::sendRequest(
    const QString &method, const QJsonObject &params,
    std::function<void(const QJsonObject &, const QString &)> done, int timeoutMs) {
    if (process_ == nullptr) {
        done({}, QStringLiteral("服务器未启动"));
        return;
    }
    const int id = nextId_++;

    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("method"), method);
    message.insert(QStringLiteral("params"), params);

    pending_.insert(id, std::move(done));

    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id, method]() {
        if (!pending_.contains(id)) {
            return;
        }
        auto callback = pending_.take(id);
        timeouts_.remove(id);
        qCWarning(log) << "MCP 请求超时; id=" << config_.id << "method=" << method;
        callback({}, QStringLiteral("请求超时（%1 ms）").arg(kRequestTimeoutMs));
    });
    timeouts_.insert(id, timer);
    timer->start(timeoutMs);

    writeMessage(message);
}

void Client::handleStdout() {
    if (process_ == nullptr) {
        return;
    }
    buffer_ += process_->readAllStandardOutput();

    // 按行切分。最后一段可能是不完整的消息，留在缓冲里等下一批数据。
    qsizetype newline = buffer_.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        newline = buffer_.indexOf('\n');

        if (line.isEmpty()) {
            continue;
        }
        QString parseError;
        const QJsonObject message = json::parseObject(line, &parseError);
        if (!parseError.isEmpty()) {
            // 服务器把非 JSON 的日志打到 stdout 是常态（Node 服务器尤其）。
            // 跳过即可——当成致命错误会让整个服务器用不了。
            qCDebug(log) << "MCP stdout 非 JSON 行，已跳过; id=" << config_.id
                         << line.left(200);
            continue;
        }
        dispatch(message);
    }
}

void Client::dispatch(const QJsonObject &message) {
    // 通知（没有 id）不匹配任何待办请求。这里没有需要处理的服务器通知，
    // 但**必须**把它和响应区分开，否则会去 pending_ 里找一个不存在的 key。
    if (!message.contains(QStringLiteral("id")) || message.value(QStringLiteral("id")).isNull()) {
        qCDebug(log) << "MCP 通知; id=" << config_.id
                     << json::str(message, QStringLiteral("method"));
        return;
    }

    const int id = message.value(QStringLiteral("id")).toInt(-1);
    auto callback = pending_.take(id);
    if (!callback) {
        qCDebug(log) << "MCP 响应没有对应请求（可能已超时）; id=" << config_.id << "rpcId=" << id;
        return;
    }
    if (QObject *timer = timeouts_.take(id)) {
        timer->deleteLater();
    }

    if (message.contains(QStringLiteral("error"))) {
        const QJsonObject error = json::object(message, QStringLiteral("error"));
        const QString text = json::str(error, QStringLiteral("message"),
                                       QStringLiteral("未知错误"));
        callback({}, QStringLiteral("%1（code=%2）")
                          .arg(text)
                          .arg(json::integer(error, QStringLiteral("code"))));
        return;
    }
    callback(json::object(message, QStringLiteral("result")), QString());
}

void Client::callTool(const QString &toolName, const QJsonObject &arguments, Callback done) {
    if (state_ != ServerState::Ready) {
        done(false, QStringLiteral("MCP 服务器未就绪：%1").arg(lastError_), {});
        return;
    }

    QJsonObject params;
    params.insert(QStringLiteral("name"), toolName);
    params.insert(QStringLiteral("arguments"), arguments);

    sendRequest(
        QStringLiteral("tools/call"), params,
        [done](const QJsonObject &result, const QString &error) {
            if (!error.isEmpty()) {
                done(false, QStringLiteral("MCP 调用失败：%1").arg(error), {});
                return;
            }
            const QString text = contentToText(json::array(result, QStringLiteral("content")));
            // isError 是 MCP 表示"工具自己报告失败"的方式（不是协议错误）。
            const bool isError = json::boolean(result, QStringLiteral("isError"));
            done(!isError,
                 text.isEmpty() ? QStringLiteral("(MCP 工具没有返回内容)") : text, result);
        });
}

}  // namespace lycode::mcp
