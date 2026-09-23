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

namespace lycode::mcp
{
namespace
{

Q_LOGGING_CATEGORY(log, "lycode.mcp")

/// 协议版本。写死一个我们确实实现的版本，而不是声明"最新"——
/// 服务器会按协商结果决定用哪套能力，虚报会导致它发来我们解析不了的东西。
constexpr char kProtocolVersion[] = "2024-11-05";

/// 把 MCP 的 content 数组拼成纯文本。
///
/// 只取 text 类型：image / resource 需要额外的落盘与转发链路，本阶段不做，
/// 但要在文本里**明确说明**被略过了哪些块——静默丢弃会让模型以为工具没返回内容。
QString contentToText(const QJsonArray & content)
{
  QStringList chunks;
  int skipped = 0;
  for(const QJsonValue & value : content) {
    const QJsonObject block = value.toObject();
    const QString type = json::str(block, QStringLiteral("type"));
    if(type == QLatin1String("text")) {
      chunks.append(json::str(block, QStringLiteral("text")));
    }
    else if(type == QLatin1String("resource")) {
      const QJsonObject resource = json::object(block, QStringLiteral("resource"));
      const QString text = json::str(resource, QStringLiteral("text"));
      if(!text.isEmpty()) {
        chunks.append(text);
      }
      else {
        ++skipped;
      }
    }
    else if(!type.isEmpty()) {
      ++skipped;
    }
  }
  if(skipped > 0) {
    chunks.append(QStringLiteral("（另有 %1 个非文本内容块未展示：%2）")
                  .arg(skipped)
                  .arg(QStringLiteral("本阶段只处理 text/resource 文本")));
  }
  return chunks.join(QLatin1Char('\n'));
}

QString processStateToken(QProcess::ProcessState state)
{
  switch(state) {
    case QProcess::NotRunning:
      return QStringLiteral("NotRunning");
    case QProcess::Starting:
      return QStringLiteral("Starting");
    case QProcess::Running:
      return QStringLiteral("Running");
  }
  return QStringLiteral("Unknown");
}

/// 退出码的可读形式。Windows 上异常退出码是 NTSTATUS（负的十进制），
/// 只有十六进制才对得上微软文档，所以两个都给。
QString exitCodeText(int exitCode)
{
  if(exitCode < 0) {
    return QStringLiteral("%1 (0x%2)")
           .arg(exitCode)
           .arg(static_cast<quint32>(exitCode), 8, 16, QLatin1Char('0'));
  }
  return QString::number(exitCode);
}

}  // namespace

Client::Client(ServerConfig config, QObject * parent)
  : QObject(parent), config_(std::move(config)) {}

Client::~Client()
{
  stop();
}

void Client::start()
{
  if(process_ != nullptr) {
    return;
  }
  if(!config_.isValid()) {
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
  for(const QString & entry : config_.env) {
    const qsizetype equals = entry.indexOf(QLatin1Char('='));
    if(equals > 0) {
      environment.insert(entry.left(equals), entry.mid(equals + 1));
    }
    else {
      qCWarning(log) << "忽略格式错误的环境变量（应为 KEY=VALUE）; server="
                     << config_.id << "entry=" << entry;
    }
  }
  process_->setProcessEnvironment(environment);

  connect(process_, &QProcess::readyReadStandardOutput, this, &Client::handleStdout);
  connect(process_, &QProcess::readyReadStandardError, this, &Client::handleStderr);
  connect(process_, &QProcess::errorOccurred, this,
  [this](QProcess::ProcessError) {
    handleProcessError();
  });
  connect(process_, &QProcess::finished, this,
  [this](int exitCode, QProcess::ExitStatus) {
    handleFinished(exitCode);
  });

  qCInfo(log) << "启动 MCP 服务器; id=" << config_.id << "command=" << config_.command
              << "args=" << config_.args.join(QLatin1Char(' '))
              << "请求协议版本=" << QString::fromLatin1(kProtocolVersion);
  process_->start();
  handshakeTimer_.start();

  QTimer::singleShot(kHandshakeTimeoutMs, this, &Client::timeoutHandshake);

  // ── 握手 ────────────────────────────────────────────────────────────────
  QJsonObject clientInfo;
  clientInfo.insert(QStringLiteral("name"), QStringLiteral("lycode"));
  clientInfo.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());

  QJsonObject params;
  params.insert(QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion));
  params.insert(QStringLiteral("capabilities"), QJsonObject{});
  params.insert(QStringLiteral("clientInfo"), clientInfo);

  qCDebug(log) << "握手 → 第 1/3 步 发送 initialize; server=" << config_.id;
  sendRequest(
    QStringLiteral("initialize"), params,
  [this](const QJsonObject & result, const QString & error) {
    if(!error.isEmpty()) {
      setFailed(QStringLiteral("initialize 失败：%1").arg(error));
      return;
    }
    serverName_ = json::str(json::object(result, QStringLiteral("serverInfo")),
                            QStringLiteral("name"));
    serverVersion_ = json::str(json::object(result, QStringLiteral("serverInfo")),
                               QStringLiteral("version"));
    const QString negotiated = json::str(result, QStringLiteral("protocolVersion"));
    qCInfo(log) << "握手 ← initialize 成功; server=" << config_.id
                << "serverName=" << serverName_ << "serverVersion=" << serverVersion_
                << "协商协议版本=" << negotiated
                << "耗时=" << handshakeTimer_.elapsed() << "ms";
    // 版本不一致不一定就是错（服务器可能向后兼容旧版本），但必须留痕：
    // 真出现解析不了的怪象时，这是第一个要看的线索。
    if(!negotiated.isEmpty() && negotiated != QLatin1String(kProtocolVersion)) {
      qCWarning(log) << "协商的协议版本与请求的不一致; server=" << config_.id
                     << "请求=" << QString::fromLatin1(kProtocolVersion)
                     << "服务器=" << negotiated;
    }

    // 协议要求：initialize 之后必须发这个通知，否则不少服务器会
    // 拒绝后续请求（它们用它来确认客户端已经准备好）。
    qCDebug(log) << "握手 → 第 2/3 步 发送 notifications/initialized; server="
                 << config_.id;
    sendNotification(QStringLiteral("notifications/initialized"));

    qCDebug(log) << "握手 → 第 3/3 步 发送 tools/list; server=" << config_.id;
    sendRequest(QStringLiteral("tools/list"), QJsonObject{},
    [this](const QJsonObject & listResult, const QString & listError) {
      if(!listError.isEmpty()) {
        setFailed(QStringLiteral("tools/list 失败：%1").arg(listError));
        return;
      }
      tools_.clear();
      for(const QJsonValue & value :
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
        if(!info.name.isEmpty()) {
          tools_.append(info);
        }
      }
      handshakeDone_ = true;
      state_ = ServerState::Ready;
      qCInfo(log) << "握手 ← tools/list 成功; server=" << config_.id
                  << "工具数=" << tools_.size()
                  << "握手总耗时=" << handshakeTimer_.elapsed() << "ms";
      qCInfo(log) << "MCP 服务器就绪; id=" << config_.id
                  << "server=" << serverName_ << serverVersion_
                  << "工具数=" << tools_.size();
      emit ready();
    });
  });
}

void Client::setFailed(const QString & reason)
{
  lastError_ = reason;
  state_ = ServerState::Failed;
  qCWarning(log) << "MCP 服务器失败; id=" << config_.id << "reason=" << reason;
  emit failed(reason);
}

void Client::timeoutHandshake()
{
  if(handshakeDone_ || state_ == ServerState::Failed) {
    return;
  }
  // 超时原因要尽量具体：子进程是死了、还是活着但不说话、还是说了但格式不对。
  // 这些都能从我们自己记录的状态里判定，不必让用户去猜。
  QString reason = QStringLiteral("握手超时（%1 ms）").arg(kHandshakeTimeoutMs);
  if(process_ != nullptr) {
    reason += QStringLiteral("；子进程状态=%1").arg(processStateToken(process_->state()));
  }
  reason += QStringLiteral("；stdout 收到 %1 字节（非 JSON 行 %2 行）")
            .arg(stdoutBytes_)
            .arg(nonJsonStdoutLines_);
  if(!stderrTail_.trimmed().isEmpty()) {
    reason += QStringLiteral("；stderr: %1").arg(stderrTail_.trimmed().left(300));
  }
  setFailed(reason);
}

void Client::appendStderr(const QString & text)
{
  const QString trimmed = text.trimmed();
  if(trimmed.isEmpty()) {
    return;
  }
  if(!stderrTail_.isEmpty()) {
    stderrTail_ += QLatin1Char('\n');
  }
  stderrTail_ += trimmed;
  // 只留尾部：服务器可能疯狂打日志，诊断不需要全文。
  constexpr qsizetype kTailMax = 4000;
  if(stderrTail_.size() > kTailMax) {
    stderrTail_ = stderrTail_.right(kTailMax);
  }
}

QString Client::diagnosticReport() const
{
  QStringList lines;
  lines << QStringLiteral("服务器 id: %1").arg(config_.id);
  lines << QStringLiteral("状态: %1").arg(serverStateToken(state_));
  lines << QStringLiteral("命令: %1 %2")
        .arg(config_.command, config_.args.join(QLatin1Char(' ')));
  if(process_ == nullptr) {
    lines << QStringLiteral("子进程: 未创建");
  }
  else {
    lines << QStringLiteral("子进程状态: %1").arg(processStateToken(process_->state()));
  }
  lines << QStringLiteral("子进程错误: %1")
        .arg(processError_.isEmpty() ? QStringLiteral("(无)") : processError_);
  lines << QStringLiteral("已退出: %1；退出码: %2")
        .arg(exited_ ? QStringLiteral("是") : QStringLiteral("否"),
             exited_ ? exitCodeText(exitCode_) : QStringLiteral("(仍在运行)"));
  lines << QStringLiteral("stdout 收到: %1 字节；非 JSON 行: %2 行")
        .arg(stdoutBytes_)
        .arg(nonJsonStdoutLines_);
  lines << QStringLiteral("记录的错误: %1")
        .arg(lastError_.isEmpty() ? QStringLiteral("(无)") : lastError_);
  lines << QStringLiteral("stderr 尾部: %1")
        .arg(stderrTail_.trimmed().isEmpty() ? QStringLiteral("(空)")
             : stderrTail_.trimmed());
  return lines.join(QLatin1Char('\n'));
}

void Client::stop()
{
  if(process_ == nullptr) {
    return;
  }
  QProcess * process = process_;
  process_ = nullptr;  // 先摘掉指针：下面的信号处理不该再走一遍收尾
  process->disconnect(this);
  if(process->state() != QProcess::NotRunning) {
    process->terminate();
    if(!process->waitForFinished(3000)) {
      process->kill();
      process->waitForFinished(1000);
    }
  }
  process->deleteLater();
  pending_.clear();
  for(QObject * timer : std::as_const(timeouts_)) {
    timer->deleteLater();
  }
  timeouts_.clear();
  if(state_ != ServerState::Failed) {
    state_ = ServerState::Disabled;
  }
}

void Client::handleProcessError()
{
  if(process_ == nullptr) {
    return;
  }
  // 无论哪种错误都记下来：诊断时要看的就是它。
  processError_ = process_->errorString();
  // FailedToStart 是最常见也最需要说清楚的：命令不存在、没有执行权限。
  if(process_->error() == QProcess::FailedToStart) {
    setFailed(QStringLiteral("无法启动“%1”：%2")
              .arg(config_.command, process_->errorString()));
  }
}

void Client::handleFinished(int exitCode)
{
  if(process_ == nullptr) {
    return;
  }
  exitCode_ = exitCode;
  exited_ = true;
  qCInfo(log) << "子进程已结束; server=" << config_.id << "exitCode=" << exitCodeText(exitCode)
              << "握手已完成=" << handshakeDone_;
  // 收尾时把还没读过的 stderr 也并进诊断尾巴，否则最后一段会丢。
  appendStderr(QString::fromUtf8(process_->readAllStandardError()));
  // 握手还没完成就退出，是最容易让人困惑的情况：报错里必须带上 stderr 片段。
  if(!handshakeDone_ && state_ == ServerState::Starting) {
    setFailed(QStringLiteral("进程在握手完成前退出（exitCode=%1）。%2")
              .arg(exitCodeText(exitCode),
                   stderrTail_.trimmed().isEmpty()
                   ? QStringLiteral("（子进程没有输出 stderr）")
                   : stderrTail_.trimmed().left(300)));
  }
  emit closed();
}

void Client::handleStderr()
{
  if(process_ == nullptr) {
    return;
  }
  const QString text =
    QString::fromUtf8(process_->readAllStandardError()).trimmed();
  if(!text.isEmpty()) {
    appendStderr(text);
    // 不当作错误：很多服务器把正常日志写到 stderr。记下来便于排查。
    qCDebug(log) << "MCP stderr; id=" << config_.id << text.left(500);
  }
}

void Client::writeMessage(const QJsonObject & message)
{
  if(process_ == nullptr || process_->state() == QProcess::NotRunning) {
    return;
  }
  // 换行分隔：一个消息一行，末尾必须补 '\n'，否则服务器会一直等下一个字段。
  const QByteArray payload = json::toBytes(message);
  process_->write(payload + '\n');
  // 协议原始数据属于 debug 级（见 core/Logging.h 的分级约定）。
  qCDebug(log) << "→ 发送; server=" << config_.id << payload.left(300);
}

void Client::sendNotification(const QString & method, const QJsonObject & params)
{
  QJsonObject message;
  message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
  message.insert(QStringLiteral("method"), method);
  if(!params.isEmpty()) {
    message.insert(QStringLiteral("params"), params);
  }
  writeMessage(message);
}

void Client::sendRequest(
  const QString & method, const QJsonObject & params,
  std::function<void(const QJsonObject &, const QString &)> done, int timeoutMs)
{
  if(process_ == nullptr) {
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

  auto * timer = new QTimer(this);
  timer->setSingleShot(true);
  connect(timer, &QTimer::timeout, this, [this, id, method]() {
    if(!pending_.contains(id)) {
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

void Client::handleStdout()
{
  if(process_ == nullptr) {
    return;
  }
  const QByteArray chunk = process_->readAllStandardOutput();
  stdoutBytes_ += chunk.size();
  buffer_ += chunk;

  // 按行切分。最后一段可能是不完整的消息，留在缓冲里等下一批数据。
  qsizetype newline = buffer_.indexOf('\n');
  while(newline >= 0) {
    const QByteArray line = buffer_.left(newline).trimmed();
    buffer_.remove(0, newline + 1);
    newline = buffer_.indexOf('\n');

    if(line.isEmpty()) {
      continue;
    }
    qCDebug(log) << "← 收到; server=" << config_.id << line.left(300);
    QString parseError;
    const QJsonObject message = json::parseObject(line, &parseError);
    if(!parseError.isEmpty()) {
      // 服务器把非 JSON 的日志打到 stdout 是常态（Node 服务器尤其）。
      // 跳过即可——当成致命错误会让整个服务器用不了。
      ++nonJsonStdoutLines_;
      qCDebug(log) << "上述行不是 JSON，已跳过; server=" << config_.id
                   << "解析错误=" << parseError;
      continue;
    }
    dispatch(message);
  }
}

void Client::dispatch(const QJsonObject & message)
{
  // 通知（没有 id）不匹配任何待办请求。这里没有需要处理的服务器通知，
  // 但**必须**把它和响应区分开，否则会去 pending_ 里找一个不存在的 key。
  if(!message.contains(QStringLiteral("id")) || message.value(QStringLiteral("id")).isNull()) {
    qCDebug(log) << "  ↳ 是通知（无 id）; server=" << config_.id
                 << "method=" << json::str(message, QStringLiteral("method"));
    return;
  }

  const int id = message.value(QStringLiteral("id")).toInt(-1);
  auto callback = pending_.take(id);
  if(!callback) {
    qCDebug(log) << "  ↳ 响应没有对应请求（可能已超时）; server=" << config_.id
                 << "rpcId=" << id;
    return;
  }
  if(QObject * timer = timeouts_.take(id)) {
    timer->deleteLater();
  }

  if(message.contains(QStringLiteral("error"))) {
    const QJsonObject error = json::object(message, QStringLiteral("error"));
    const int code = json::integer(error, QStringLiteral("code"));
    const QString text = json::str(error, QStringLiteral("message"),
                                   QStringLiteral("未知错误"));
    qCWarning(log) << "  ↳ JSON-RPC 错误响应; server=" << config_.id << "rpcId=" << id
                   << "code=" << code << "message=" << text;
    callback({}, QStringLiteral("%1（code=%2）").arg(text).arg(code));
    return;
  }
  const QJsonObject result = json::object(message, QStringLiteral("result"));
  qCDebug(log) << "  ↳ 是成功响应; server=" << config_.id << "rpcId=" << id
               << "result=" << QString::fromUtf8(json::toBytes(result)).left(300);
  callback(result, QString());
}

void Client::callTool(const QString & toolName, const QJsonObject & arguments, Callback done)
{
  if(state_ != ServerState::Ready) {
    done(false, QStringLiteral("MCP 服务器未就绪：%1").arg(lastError_), {});
    return;
  }

  QJsonObject params;
  params.insert(QStringLiteral("name"), toolName);
  params.insert(QStringLiteral("arguments"), arguments);

  qCInfo(log) << "调用远端工具; server=" << config_.id << "tool=" << toolName;
  sendRequest(
    QStringLiteral("tools/call"), params,
  [this, done, toolName](const QJsonObject & result, const QString & error) {
    if(!error.isEmpty()) {
      qCWarning(log) << "远端工具调用失败; server=" << config_.id
                     << "tool=" << toolName << "原因=" << error;
      done(false, QStringLiteral("MCP 调用失败：%1").arg(error), {});
      return;
    }
    const QString text = contentToText(json::array(result, QStringLiteral("content")));
    // isError 是 MCP 表示"工具自己报告失败"的方式（不是协议错误）。
    const bool isError = json::boolean(result, QStringLiteral("isError"));
    qCDebug(log) << "远端工具返回; server=" << config_.id << "tool=" << toolName
                 << "isError=" << isError << "文本长度=" << text.size();
    done(!isError,
         text.isEmpty() ? QStringLiteral("(MCP 工具没有返回内容)") : text, result);
  });
}

}  // namespace lycode::mcp
