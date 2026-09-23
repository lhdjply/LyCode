// LyCode — 后台任务注册表
//
// `Bash(run_in_background: true)` 让进程在工具返回之后继续跑（典型用例：
// 起一个 dev server、跑一遍耗时测试）。这带来四个必须解决的问题：
//
//   1. **生命周期**：进程必须有人持有。工具调用早已返回，栈上的 QProcess
//      不能承载它——所以注册表接管所有权。
//   2. **输出去哪**：后台任务可能产出几十 MB。全部留在内存里会撑爆，
//      所以输出落盘，内存只保留一个有界的尾部预览。
//   3. **完成通知**：任务是异步结束的，模型不会自己知道。注册表在结束时
//      生成一条 `<task-notification>`，由运行时注入下一轮上下文。
//   4. **跨重启存活**：要活过宿主退出，输出就**不能走管道**——宿主退出后
//      管道读端关闭，子进程再写会拿到 EPIPE。所以分离式后台任务的
//      stdout/stderr 直接重定向到文件，注册表改为轮询文件取预览。
//
// ── 两种后台任务（这是物理约束，不是实现取巧）────────────────────────────
//   detached（分离式）  —— 显式 run_in_background。输出直接写文件，进程
//                          独立会话，宿主退出后仍存活，下次启动由账本恢复。
//   piped（管道式）     —— 前台命令超时后**自动后台化**。它继承前台的管道，
//                          无法在启动后改重定向，因此能在本进程内继续运行，
//                          但不具备跨重启能力。
//
// 与 TodoStore 一样放在 tools/：工具需要它，但 agent/ 的依赖方向不允许
// 工具反向依赖运行时。由 AgentRuntime 创建并注入 ToolContext。
#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QObject>
#include <QString>

#include "core/Types.h"

class QProcess;
class QTimer;

namespace lycode
{

/// 一个后台任务的可读快照。
struct BackgroundTask {
  QString id;
  /// 任务类型。目前只有 `bash`（子代理与工作流将来会各有一种）。
  QString type;
  QString description;
  QString command;
  /// running | completed | failed | killed | lost
  QString status;
  /// 全部输出的落盘文件路径。任务结束后仍然可读。
  QString outputPath;
  /// 内存里保留的输出尾部（有界）。
  QString outputPreview;
  /// 已产生的输出字节数。
  qint64 outputBytes = 0;
  /// 退出码；未结束或非正常退出时为 -1。
  int exitCode = -1;
  TimestampMs startedAtMs = 0;
  TimestampMs endedAtMs = 0;
  /// 是否被 TaskStop 主动终止。
  bool killed = false;
  /// 是否由前台命令超时后自动转入后台（继承前台的管道，不跨重启）。
  bool autoBackgrounded = false;
  /// 是否为分离式任务：输出直接写文件、进程独立于宿主，可跨重启。
  bool detached = false;
  /// 是否为启动时从账本恢复的任务。
  bool recovered = false;
  /// 进程号。0 表示尚未启动。
  int pid = 0;

  bool isRunning() const
  {
    return status == QLatin1String("running");
  }
  qint64 durationMs() const;
};

class BackgroundTaskRegistry : public QObject
{
    Q_OBJECT

  public:
    /// 内存预览的上限。超出的部分只留在落盘文件里。
    /// 8KiB 足够让模型判断"这个任务在干什么/是不是卡住了"，又不至于撑爆上下文。
    static constexpr int kPreviewBytes = 8 * 1024;
    /// 分离式任务的存活轮询间隔。轮询而不是靠信号：进程已经不属于我们，
    /// 没有 finished 信号可等。
    static constexpr int kLivenessPollIntervalMs = 500;

    explicit BackgroundTaskRegistry(QObject * parent = nullptr);
    ~BackgroundTaskRegistry() override;

    /// 派生的入参。
    ///
    /// 两种模式互斥：
    ///   * **管道式**（detached=false）：给出 `process`，注册表接管 QProcess
    ///     所有权并读它的管道。这是前台超时后自动后台化的路径。
    ///   * **分离式**（detached=true）：给出 `pid`，**没有** QProcess。
    ///     进程由 `QProcess::startDetached` 起，输出由 shell 直接重定向到文件。
    ///     不持有 QProcess 是硬要求：`~QProcess` 会杀掉仍在运行的进程，
    ///     那样"活过宿主"就无从谈起。
    struct AdoptRequest {
      QString type;
      QString description;
      QString command;
      /// 管道式任务：进程，注册表接管其所有权（会 setParent 到自己名下）。
      QProcess * process = nullptr;
      /// 分离式任务：已经独立启动的进程号。
      int pid = -1;
      /// 输出文件路径。
      QString outputPath;
      /// 指定 task id；为空则自动生成。
      QString taskId;
      /// 已经产出的输出（自动后台化时把前台已读到的部分带过来，不丢历史）。
      QString initialOutput;
      /// true 表示这是分离式任务：按 pid 跟踪、轮询文件、不接管道。
      ///
      /// 分离式任务**没有 wait() 可取退出码**，所以调用方要让 shell 把 `$?`
      /// 写到一个旁路文件（见 exitCodePathFor）。没有它就只能记 -1，
      /// 那样一个成功退出的后台任务会被报成"失败"。
      bool detached = false;
      /// true 表示这是前台超时后自动转入后台的任务。
      bool autoBackgrounded = false;
      /// 接管一个**已经在运行**的管道式进程。调用方必须先断开自己挂在该
      /// 进程上的读取器，否则两边会抢同一份管道输出。
      bool takeOverRunningProcess = false;
    };

    /// 接管一个进程（接管所有权与生命周期）。返回 task id；
    /// 进程无效时返回空字符串，此时进程未被接管。
    QString adopt(const AdoptRequest & request);

    /// 分配一个 task id（供调用方在 start 之前拼出输出路径）。
    QString allocateTaskId() const;
    /// 该 task 的默认输出文件路径；目录不可用时返回空。
    static QString outputPathForTask(const QString & taskId);
    /// 退出码旁路文件路径（`<outputPath>.exit`）。
    /// 分离式任务靠它拿回退出码——`startDetached` 没有 wait()。
    static QString exitCodePathFor(const QString & outputPath);

    QList<BackgroundTask> tasks() const;
    /// 按 id 取快照；不存在时返回的 id 为空。
    BackgroundTask task(const QString & taskId) const;
    bool contains(const QString & taskId) const;
    int runningCount() const;
    int totalCount() const
    {
      return static_cast<int>(entries_.size());
    }

    /// 终止任务。已结束的任务返回 false（幂等边界：UI 可能重复点停止）。
    bool stop(const QString & taskId);
    /// 终止全部未结束的任务。测试与显式清理用。
    void stopAll();

    /// 把仍在运行的任务写进账本并**放弃跟踪**，但**不终止进程**。
    /// 应用退出时调用：这样任务能活过重启，下次启动由 reconcile() 认领。
    void detachAll();
    /// 从账本恢复上次遗留的任务。构造时自动调用一次，也可手动重跑。
    void reconcile();

    /// 进程 start 之后补记 pid 与启动指纹。
    /// adopt() 发生在 start 之前（这样读取器先就位、不丢输出），
    /// 那时 processId() 还是 0，所以需要这一步。
    void recordProcessIdentity(const QString & taskId);

    /// 取出并清空该任务的完成通知文本。
    /// 已经取过或任务仍在运行时返回空。
    QString takeNotification(const QString & taskId);
    /// 是否有待投递的完成通知。
    bool hasPendingNotification() const;

    /// 默认的输出目录：`<数据目录>/tasks`。目录不存在时创建；失败返回空。
    static QString defaultOutputDirectory();
    /// 账本文件路径：`<数据目录>/tasks/ledger.json`。
    static QString ledgerPath();

  signals:
    void taskAdded(const lycode::Id & taskId);
    void taskUpdated(const lycode::Id & taskId);
    /// 任务结束。`ok` 为真表示退出码为 0。
    void taskFinished(const lycode::Id & taskId, bool ok, int exitCode);

  private:
    struct Entry {
      BackgroundTask task;
      QProcess * process = nullptr;
      QFile * logFile = nullptr;
      QByteArray preview;
      /// 待投递的完成通知；被 takeNotification 取走后为空。
      QString notification;
      bool finishedEmitted = false;
      /// 分离式任务：不接管道，靠轮询文件与存活状态。
      bool detached = false;
      /// pid 启动时间指纹，用于防 pid 复用。
      qint64 pidStartTicks = 0;
      /// 已从输出文件读到的偏移量（分离式任务用）。
      qint64 fileReadOffset = 0;
    };

    Entry * findEntry(const QString & taskId);
    const Entry * findEntry(const QString & taskId) const;
    /// 从管道读走可用输出（管道式任务）。
    void readAvailable(Entry & entry);
    /// 从输出文件读增量（分离式任务）。
    void pollDetachedOutput(Entry & entry);
    void handleFinished(Entry & entry, int exitCode, bool killed);
    /// 组装 `<task-notification>` 文本。
    static QString buildNotification(const Entry & entry);
    /// 写账本。
    void persistLedger() const;
    /// 启动存活轮询定时器（有分离式任务时才跑）。
    void ensureLivenessTimer();

    QList<Entry> entries_;
    QTimer * livenessTimer_ = nullptr;
};

/// 进程是否仍存活。`startTicks` 为 launcher 记录的启动时间指纹（可 0）：
/// 非 0 时会在 Linux 上校验 /proc/<pid>/stat，规避 pid 复用造成的误判。
bool processAlive(int pid, qint64 startTicks);
/// 读取进程的启动时间指纹；取不到返回 0。
qint64 processStartTicks(int pid);

}  // namespace lycode
