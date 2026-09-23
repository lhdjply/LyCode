// LyCode — 权限确认对话框
//
// 职责边界（与 PermissionGate 的分工，见 src/agent/PermissionGate.h）：
//   * 本对话框只做「呈现 + 收集裁决」：把工具给出的 PermissionRequest 原样渲染，
//     把用户点击的那个 PermissionOption 的 response 回传给调用方。
//   * **不发明选项、不改写入参、不做权限策略判断**。选项集合完全由
//     request.options 决定，这样"有哪些选择"只有工具一处定义。
//   * 不做持久化：AllowAlways 之类的规则落地由调用方（AgentRuntime / 会话配置）负责。
//
// 交互约定：
//   * Esc       → 若有 Deny 选项则触发它，否则按"用户关闭窗口"处理（Deny）。
//   * Enter     → 触发 request.defaultOption()（第一个允许项，没有则第一个拒绝项）。
//   * 关闭窗口  → response() 为 Deny + reason="用户关闭了确认窗口"。
#pragma once

#include <QDialog>
#include <QString>

#include <functional>

#include "core/Types.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace lycode::ui
{

class PermissionDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit PermissionDialog(const PermissionRequest & request, QWidget * parent = nullptr);
    ~PermissionDialog() override;

    /// 用户选择的结果。exec() 返回 QDialog::Accepted 时有效；
    /// 用户关闭窗口（Rejected）时返回 Deny 裁决。
    PermissionResponse response() const
    {
      return response_;
    }

    /// 便捷入口：无阻塞地弹出并把结果交给回调。
    /// 对话框在回调后被销毁（WA_DeleteOnClose）。
    /// 若 request.options 为空，则立即以 Deny 回调，不弹窗。
    static void present(QWidget * parent, const PermissionRequest & request,
                        std::function<void(const PermissionResponse &)> onResolved);

  protected:
    /// 拦截 Esc：优先命中 Deny 选项，避免"按了 Esc 却什么都没发生"。
    void keyPressEvent(QKeyEvent * event) override;

  private:
    /// 构建整棵控件树（头部 / 入参 / 按钮行）。
    void buildUi();
    void addHeader(QVBoxLayout * root);
    void addInputSection(QVBoxLayout * root);
    void addOptionButtons(QVBoxLayout * root);

    /// 对话框外壳：一级容器圆角 16px + popoverBorder 描边。
    void applyShellStyle();
    /// 主题令牌变化（含字号）后整棵树重刷。本地样式表保存的是构造时的颜色与字号，
    /// 不重刷就会残留旧主题；由 Theme::changed 触发。
    void refreshTokenStyles();

    /// 标题回退链：title → toolName → 固定文案。
    QString resolvedTitle() const;

    /// 查找某个类别的选项；未命中返回 nullptr。返回的是 request_.options 内部指针。
    const PermissionOption * findOption(PermissionOptionKind kind) const;

    /// 采纳某个选项：把 option.response 记为结果并关闭对话框。
    void chooseOption(const PermissionOption & option);

    PermissionRequest request_;
    /// 默认即拒绝：任何非显式选择的退出路径都落在安全侧。
    /// 用逐字段赋值而不是花括号列表初始化——PermissionResponse 有 4 个字段，
    /// 漏字段的聚合初始化会触发 -Wmissing-field-initializers。
    PermissionResponse response_;
    QPushButton * defaultButton_ = nullptr;
};

}  // namespace lycode::ui
