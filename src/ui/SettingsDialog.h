// LyCode — 设置对话框
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
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;
class QWidget;

namespace lycode::ui {

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
    void settingsPreviewChanged(const lycode::ui::AppSettings &settings);

private:
    // ── 页面构建 ────────────────────────────────────────────────────────────
    QWidget *buildAppearancePage();
    QWidget *buildProviderPage();
    QWidget *buildSessionPage();
    /// MCP 服务器与 Skills 目录。两者都是"外部能力接入"，放在同一页。
    QWidget *buildIntegrationsPage();

    /// 用当前表格内容重建 MCP 表格。
    void refreshMcpTable();
    /// 编辑第 row 行（row < 0 表示新增）。
    void editMcpServer(int row);
    /// 用当前目录列表重建 Skills 目录列表，并预览能扫出多少技能。
    void refreshSkillDirectories();
    void addSkillDirectory();
    void removeSelectedSkillDirectory();
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

    // ── Provider 页：模型能力覆盖 ───────────────────────────────────────────
    /// 按「模型列表」输入框的内容重建表格行；行集合不变时不重建（保留编辑态）。
    /// 每行的值从 settings_.modelOverrides 回填，因此增删行不会丢已有编辑值。
    /// 写表格的表头文字。构造与每次 reload 都要调用——QTableWidget::clear()
    /// 会连表头一起清空。
    void applyCapabilityHeaderLabels();
    void reloadModelCapabilityTable();
    /// 创建一个模型行（模型 / 上下文窗口 / 最大输出 / 思考档位 / 默认档位）。
    void addCapabilityRow(int row, const QString &modelId, const ModelOptionOverride &override);
    /// 读该行控件 → 写回 settings_.modelOverrides → 刷新非法态与确定按钮。
    void applyCapabilityRow(int row);
    /// 「思考档位」文本变化：重建该行「默认档位」下拉（保留仍合法的选择）后写回。
    void onCapabilityReasoningTextChanged(int row);
    /// 用该行「思考档位」里解析出的合法 id 重建下拉；preferredDefault 仍合法则保留。
    void rebuildDefaultReasoningCombo(int row, const QString &preferredDefault);
    /// 解析该行「思考档位」文本：返回合法 id（去重保序），非法 id 追加到 unknownOut。
    QStringList validReasoningIdsForRow(int row, QStringList *unknownOut) const;
    /// 汇总所有行的非法档位提示（destructive 文案），无错误时隐藏。
    void refreshCapabilityValidation();
    /// 是否存在非法档位 id；用于禁用「确定」。
    bool hasCapabilityErrors() const;
    /// 取该行对应的模型 id（存在第 0 列的 UserRole 里）。
    QString capabilityModelIdAt(int row) const;
    /// 表格行高与整体高度：随行数与字号令牌变化，超出上限由表格自身滚动。
    void applyCapabilityTableHeight();
    /// accept() 前清理「模型列表里已不存在」的模型的覆盖键（只动可枚举的 Provider）。
    void pruneStaleModelOverrides();

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

    // Provider · 模型能力
    QGroupBox *modelCapabilityGroup_ = nullptr;
    QTableWidget *modelCapabilityTable_ = nullptr;
    QLabel *modelCapabilityEmptyHint_ = nullptr;
    QLabel *modelCapabilityErrorLabel_ = nullptr;
    /// 表格当前展示的模型 id 顺序（含所属 Provider id）：模型列表变化时用它判断是否重建。
    QStringList capabilityModelIds_;
    QString capabilityProviderId_;

    // 会话
    QComboBox *sessionModeCombo_ = nullptr;
    QCheckBox *persistSessionsCheck_ = nullptr;
    QCheckBox *titleGenerationCheck_ = nullptr;

    QTableWidget *mcpTable_ = nullptr;
    QListWidget *skillDirList_ = nullptr;
    QLabel *skillPreview_ = nullptr;
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

}  // namespace lycode::ui
