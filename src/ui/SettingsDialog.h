// ZCode Qt — 设置对话框
//
// 职责边界：
//   * 只负责「收集 + 校验」AppSettings，**不做持久化**（不调用 AppConfig::save）：
//     保存由调用方负责，这样"谁写盘"只有一个入口（见 AppConfig.h 的说明）。
//   * 实时预览：外观页改动会立刻调用 Theme::instance().setMode / setUiFontSize /
//     setCodeFontSize 并发出 settingsPreviewChanged，供主窗口同步重刷样式表；
//     取消（reject）时回滚到构造时记录的主题状态，因此预览不会残留。
//
// 安全约定（硬性）：
//   * API Key 输入框默认遮蔽；输入框留空表示"不修改"，保留原 key，
//     不会因为用户没重输就把已配置的 key 清掉。
//   * 任何日志都不打印 API Key，只记"是否已配置"。
#pragma once

#include <QDialog>
#include <QString>

#include "core/Types.h"
#include "ui/AppConfig.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QVBoxLayout;
class QWidget;

namespace zcode::ui {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings &settings, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    /// exec() 返回 Accepted 后取回编辑结果（整份替换语义）。
    AppSettings settings() const { return settings_; }

    /// 确定：先校验，不通过则保持打开。
    void accept() override;
    /// 取消：把构造时记录的主题/字号恢复回去（实时预览必须能回滚）。
    void reject() override;

signals:
    /// 设置变化时发出，供调用方实时预览主题（在 accept 之前）。
    void settingsPreviewChanged(const zcode::ui::AppSettings &settings);

private:
    // ── 页面构建 ────────────────────────────────────────────────────────────
    QWidget *buildAppearancePage();
    QWidget *buildProviderPage();
    QWidget *buildSessionPage();
    void buildButtonBox(QVBoxLayout *root);

    // ── 外观页 ──────────────────────────────────────────────────────────────
    void applyAppearancePreview();

    // ── Provider 页 ─────────────────────────────────────────────────────────
    void reloadProviderList(int selectRow);
    void loadProviderForm(int row);
    void applyProviderForm();
    void refreshProviderRow(int row);
    void updateProviderState();
    void syncBaseUrlPlaceholder();
    void addProvider();
    void removeProvider();
    void moveProvider(int delta);

    // ── 会话页 ──────────────────────────────────────────────────────────────
    void reloadRecentWorkspaces();
    void clearRecentWorkspaces();

    /// 保存前校验（覆盖全部 provider）；失败时选中出问题的一项供用户直接修正。
    bool validateAllProviders();
    /// 记日志：只记可审计的配置项，**绝不包含 apiKey**。
    void logAcceptedSettings();

    // ── 令牌样式 ────────────────────────────────────────────────────────────
    void applyShellStyle();
    /// 主题令牌变化后重刷本地样式：设置页自己会实时预览主题，必须跟着变。
    void refreshTokenStyles();

    AppSettings settings_;
    /// 构造时记录的主题状态：reject() 时回滚到这三个值。
    ThemeMode originalMode_ = ThemeMode::System;
    int originalUiFontSize_ = 14;
    int originalCodeFontSize_ = 14;

    // 外观
    QComboBox *themeCombo_ = nullptr;
    QSpinBox *uiFontSpin_ = nullptr;
    QSpinBox *codeFontSpin_ = nullptr;
    QComboBox *languageCombo_ = nullptr;

    // Provider
    QTabWidget *tabs_ = nullptr;
    QListWidget *providerList_ = nullptr;
    QWidget *providerForm_ = nullptr;
    QLineEdit *providerNameEdit_ = nullptr;
    QComboBox *providerKindCombo_ = nullptr;
    QLineEdit *providerBaseUrlEdit_ = nullptr;
    QLineEdit *providerApiKeyEdit_ = nullptr;
    QCheckBox *providerShowKeyCheck_ = nullptr;
    QPlainTextEdit *providerModelsEdit_ = nullptr;
    QCheckBox *providerEnabledCheck_ = nullptr;
    QLabel *providerAvailabilityLabel_ = nullptr;
    QLabel *providerReasonLabel_ = nullptr;
    QLabel *providerNameErrorLabel_ = nullptr;
    QLabel *providerBaseUrlErrorLabel_ = nullptr;
    QLabel *providerEmptyHintLabel_ = nullptr;
    QLabel *providerModelsHintLabel_ = nullptr;
    QLabel *providerAccountCaptionLabel_ = nullptr;
    QPushButton *removeProviderButton_ = nullptr;
    QPushButton *moveUpButton_ = nullptr;
    QPushButton *moveDownButton_ = nullptr;

    // 会话
    QComboBox *sessionModeCombo_ = nullptr;
    QCheckBox *persistSessionsCheck_ = nullptr;
    QListWidget *recentList_ = nullptr;
    QLabel *recentEmptyLabel_ = nullptr;
    QPushButton *clearRecentButton_ = nullptr;

    QPushButton *okButton_ = nullptr;

    /// 当前正在编辑的 provider 下标；-1 表示列表为空或无选中。
    int currentProvider_ = -1;
    /// 程序化回填表单期间置位：避免把"加载"当成"用户编辑"回写。
    /// 用 save/restore 而不是直接置位，防止嵌套调用提前清掉。
    bool syncing_ = false;
};

}  // namespace zcode::ui
