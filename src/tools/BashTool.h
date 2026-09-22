// ZCode Qt — Bash 工具
//
// 用一个独立进程组跑 shell 命令：超时/取消时对**进程组**发 SIGKILL，
// 避免 `bash -lc "sleep 100 & ..."` 留下孤儿子进程继续占着端口或文件锁。
#pragma once

#include "tools/Tool.h"

namespace zcode {

/// 执行 shell 命令。异步实现（QProcess + 事件循环），绝不阻塞 GUI 线程。
class BashTool : public Tool {
public:
    ToolMetadata metadata() const override;
    QJsonObject inputSchema() const override;
    QString permissionCapability() const override;
    QString title(const QJsonObject &input) const override;
    QString validateInput(const QJsonObject &input) const override;
    void execute(const QJsonObject &input, const ToolContext &context,
                 ToolCallback done) override;
};

}  // namespace zcode
