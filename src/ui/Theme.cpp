// ZCode Qt — 设计令牌与主题实现
//
// 令牌来源（全部来自只读的原版仓库 /home/lhdjply/Desktop/ZCode/ZCode-npm）：
//
//   [styles:default-light]  packages/ui/src/styles.css 的 `@theme { }` 块（第 137-306 行）。
//                           这是原版的「默认浅色」变量集，DESIGN.md 明确说它只是 fallback 基础层。
//   [styles:dark]           packages/ui/src/styles.css 的 `.dark { }` 块（第 308-459 行）。
//   [styles:zai-light]      packages/ui/src/styles.css 的 `.theme-zai-light { }` 块（第 461-604 行）。
//   [styles:zai-dark]       packages/ui/src/styles.css 的 `.theme-zai-dark { }` 块（第 606-750 行）。
//   [scale:neutral-N]       node_modules/tailwindcss/theme.css 第 251-262 行的 Tailwind v4 中性色阶
//                           （oklch -> sRGB 已换算，见下方各处的十六进制值）。
//   [scale:<family>-N]      同一文件里的 red/green/sky/violet/... 色阶。
//   [useTheme]              packages/ui/src/useTheme.ts 第 58-70 行。
//   [DESIGN]                DESIGN.md（537 行，权威约束）。
//
// 主题选择的关键结论（[useTheme] 第 58-70 行 + [DESIGN] 第 42-50 行）：
//   原版对外只有 System / Light / Dark 三个选项，它们实际生效的 CSS 类分别是
//   `theme-zai-dark` / `theme-zai-light` / `theme-zai-dark`：
//       const resolved = resolveTheme(theme);                      // system -> 系统明暗
//       const appliedTheme = theme === "system"
//         ? (resolved === "dark" ? "zai-dark" : "zai-light")
//         : normalizeThemePreference(theme);                       // light->zai-light, dark->zai-dark
//   DESIGN.md 第 50 行也写明：「new UI should be validated against Zai Light and Zai Dark as the
//   active light/dark experiences.」
//   因此本实现的 Light palette == zai-light，Dark palette == zai-dark；System 解析后落到其中一套。
//   默认浅色 / 默认深色两套变量只作对照记录，不作为生效值。
//
// `--color-*` 里大量使用 `color-mix(in oklab, <不透明色> P%, transparent)`。
// 这类表达式在 oklab 的预乘空间里混合等价于「颜色不变、alpha = P%」（已用往返计算验证），
// 所以本文件把它们直接落成「原色 + alpha」，并在注释里保留原始表达式以便核对。
//
// 原版未找到对应令牌的字段，统一标注「原版未找到」，取值依据写在字段旁；这些是本文件里
// 唯一按 DESIGN.md 语义自定的值。

#include "ui/Theme.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QStringList>
#include <QStyleHints>
#include <QtGlobal>

#include <algorithm>

namespace zcode::ui {
namespace {

// ── 色阶常量 ────────────────────────────────────────────────────────────────
// Tailwind v4 neutral 色阶（node_modules/tailwindcss/theme.css:251-262 的 oklch 值换算成 sRGB）。
// 原版所有 --color-neutral-* 都指向这组值。这里只保留被 zai 主题直接引用的两档
// （zai-light 的 --color-foreground = neutral-800，zai-dark 的 = neutral-300），
// 其余档位的换算结果记录在文件下方的「未使用色阶」注释里。
constexpr const char *kNeutral300 = "#d4d4d4";
constexpr const char *kNeutral800 = "#262626";

// 原版 zai 主题里以字面量写死的品牌/语义色（不跟随 Tailwind 色阶）。
constexpr const char *kZaiInk = "#0d0d0d";        // zai-light 的 brand / terminal-cursor（styles.css:467,502）
constexpr const char *kZaiPaper = "#f8f8f8";      // zai-light 的 background（styles.css:464）
constexpr const char *kZaiInk2 = "#000000";       // zai-light 的 primary（styles.css:545）
constexpr const char *kZaiWhite = "#ffffff";      // zai-light 的 header/panel/card/menu/toast 底色
constexpr const char *kZaiSidebarLight = "#f0f0f0";  // zai-light 的 sidebar/tab（styles.css:485,540）
constexpr const char *kZaiBorderGray = "#e6e6e6";    // zai-light 的 secondary/tag（styles.css:547,583）
constexpr const char *kZaiBlue = "#0b7fff";       // zai-light 的 icon-blue（styles.css:510）
constexpr const char *kZaiGreen = "#1e8a3e";      // zai-light 的 success / diff-added（styles.css:557,566）
constexpr const char *kZaiGreenDeep = "#166b32";  // zai-light 的 interaction-confirmation-foreground（styles.css:552）
constexpr const char *kZaiGreenSoft = "#eaf7ee";  // zai-light 的 interaction-confirmation-surface（styles.css:551）
constexpr const char *kZaiRed = "#e03131";        // zai-light 的 destructive / diff-removed（styles.css:561,568）
constexpr const char *kZaiOrange = "#e07b00";     // zai-light 的 warning（styles.css:563）
constexpr const char *kZaiViolet = "#9e77ed";     // zai-light 的 idle-task（styles.css:559）
constexpr const char *kZaiVioletSurface = "#f5f3ff";  // zai-light 的 idle-task-surface（styles.css:560）
constexpr const char *kZaiBlueSurface = "#ebf4ff";    // zai-light 的 accent / ask-surface（styles.css:476,548）
constexpr const char *kZaiSkyDeep = "#001d3d";    // zai-dark 的 accent / ask-surface（styles.css:621,693）
constexpr const char *kZaiDarkBg = "#161616";     // zai-dark 的 background（styles.css:609）
constexpr const char *kZaiDarkHeader = "#202020"; // zai-dark 的 header/panel（styles.css:628,629）
constexpr const char *kZaiDarkCard = "#2b2b2b";   // zai-dark 的 card/popover/menu/input（styles.css:633,636,688）
constexpr const char *kZaiDarkHover = "#363636";  // zai-dark 的 menu-hover/secondary/tag（styles.css:689,692,729）
constexpr const char *kZaiDarkFg = "#f8f8f8";     // zai-dark 的 tooltip-foreground / bright-white（styles.css:726）
constexpr const char *kZaiDarkBlue = "#80beff";   // zai-dark 的 ask-foreground / bright-blue（styles.css:694,663）
constexpr const char *kZaiDarkGreen = "#46bf72";  // zai-dark 的 success / diff-added（styles.css:702,712）
constexpr const char *kZaiDarkGreenText = "#87d9a4";  // zai-dark 的 confirmation-foreground（styles.css:697）
constexpr const char *kZaiDarkRed = "#ff5c5c";    // zai-dark 的 destructive / diff-removed（styles.css:706,714）
constexpr const char *kZaiDarkOrange = "#ff8a30"; // zai-dark 的 warning（styles.css:709）
constexpr const char *kZaiDarkViolet = "#7b5ce5";      // zai-dark 的 idle-task（styles.css:704）
constexpr const char *kZaiDarkVioletSurface = "#160d38";  // zai-dark 的 idle-task-surface（styles.css:705）

// 下面这组色阶原版里被 --color-* 引用过，但当前 Palette 字段集没有对应语义位置，
// 因此只在此记录换算结果，不定义常量（避免未使用变量告警）：
//   neutral-50  #fafafa  = --color-background（默认浅色 styles.css:158）
//   neutral-100 #f5f5f5  = --color-header/panel/sidebar（styles.css:180-182）
//   neutral-200 #e5e5e5  = --color-foreground（.dark styles.css:408）
//   neutral-500 #737373  = --color-terminal-white（styles.css:212）
//   neutral-700 #404040  = --color-foreground（默认浅色 styles.css:255）
//   neutral-950 #0a0a0a  = --color-primary（默认浅色 styles.css:247）
//   #0066dd              = zai-light 的 --color-interaction-ask-foreground（styles.css:549），
//                          Palette 里没有 ask 专用字段，等待交互统一走 confirmation 绿色
//                          （DESIGN.md 第 127-130 行明确要求等待徽标只用一套绿色）。

QColor make(const char *hex, int alpha = 255) {
    QColor color{QString::fromLatin1(hex)};
    if (alpha < 255) {
        color.setAlpha(alpha);
    }
    return color;
}

}  // namespace

// ── 字体族 ──────────────────────────────────────────────────────────────────

QString sansFamily() {
    // 结果缓存：QFontDatabase::families() 会遍历整个字体库，构造期与每次主题刷新都调用代价过高。
    static const QString cached = [] {
        const QStringList candidates = {
            // Linux 优先：先保证 CJK 字形覆盖，再退到通用西文无衬线。
            QStringLiteral("Noto Sans CJK SC"),
            QStringLiteral("Source Han Sans SC"),
            QStringLiteral("WenQuanYi Micro Hei"),
            QStringLiteral("DejaVu Sans"),
            QStringLiteral("Noto Sans"),
            // 其它平台（桌面版本要跨 macOS / Windows）。
            QStringLiteral("Microsoft YaHei UI"),
            QStringLiteral("PingFang SC"),
            QStringLiteral("Segoe UI"),
            QStringLiteral("Helvetica Neue"),
        };
        const QStringList installed = QFontDatabase::families();
        for (const QString &candidate : candidates) {
            if (installed.contains(candidate, Qt::CaseInsensitive)) {
                return candidate;
            }
        }
        // 兜底：交给 Qt 选系统通用无衬线族。
        return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    }();
    return cached;
}

QString monospaceFamily() {
    static const QString cached = [] {
        const QStringList candidates = {
            QStringLiteral("JetBrains Mono"),
            QStringLiteral("Cascadia Code"),
            QStringLiteral("Fira Code"),
            QStringLiteral("Source Code Pro"),
            // 原版 --font-mono 在通用 monospace 之前显式插入了各平台 CJK 无衬线字体，
            // 免得 Windows 的 Consolas 缺中文字形后落回宋体（styles.css:138-142）。
            QStringLiteral("Noto Sans Mono CJK SC"),
            QStringLiteral("DejaVu Sans Mono"),
            QStringLiteral("monospace"),
        };
        const QStringList installed = QFontDatabase::families();
        for (const QString &candidate : candidates) {
            if (installed.contains(candidate, Qt::CaseInsensitive)) {
                return candidate;
            }
        }
        return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }();
    return cached;
}

// ── 模式 token ──────────────────────────────────────────────────────────────

QString toToken(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:
            return QStringLiteral("light");
        case ThemeMode::Dark:
            return QStringLiteral("dark");
        case ThemeMode::System:
            break;
    }
    return QStringLiteral("system");
}

ThemeMode themeModeFromToken(const QString &value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("light")) {
        return ThemeMode::Light;
    }
    if (normalized == QStringLiteral("dark")) {
        return ThemeMode::Dark;
    }
    return ThemeMode::System;
}

// ── 调色板 ──────────────────────────────────────────────────────────────────

namespace {

/// Zai Light 调色板。（[useTheme] 把 light 归一化为 zai-light，见文件头说明。）
Palette zaiLight() {
    Palette p;

    // 结构表面
    p.background = make(kZaiPaper);                      // --color-background: #f8f8f8 [styles:zai-light 464]
    p.backgroundAlt = make(kZaiPaper, 179);              // --color-background-alt: color-mix(background 70%, transparent) [466]
    p.header = make(kZaiWhite);                          // --color-header: #ffffff [483]
    p.panel = make(kZaiWhite);                           // --color-panel: #ffffff [484]
    p.sidebar = make(kZaiSidebarLight);                  // --color-sidebar: #f0f0f0 [485]
    p.surface = make(kZaiInk, 8);                        // --color-surface: rgba(13,13,13,0.03) [486]
    p.surfaceHover = make(kZaiInk, 13);                  // --color-surface-hover: rgba(13,13,13,0.05) [487]
    p.card = make(kZaiWhite);                            // --color-card: #ffffff [488]
    p.cardSelected = make(kZaiWhite);                    // --color-card-selected: var(--color-input) = #ffffff [489 -> 495]
    p.popover = make(kZaiWhite);                         // --color-popover: #ffffff [491]
    p.popoverBorder = make(kZaiInk, 26);                 // --color-popover-border: var(--color-border) [494 -> 479]
    p.menu = make(kZaiWhite);                            // --color-menu: #ffffff [543]
    p.menuHover = make(kZaiSidebarLight);                // --color-menu-hover: #f0f0f0 [544]

    // 输入
    p.input = make(kZaiWhite);                           // --color-input: #ffffff [495]
    p.inputFocused = make(kZaiWhite);                    // --color-input-focused: var(--color-input) [496]
    p.inputBorder = make(kZaiInk, 26);                   // --color-input-border: var(--color-border) [497]
    p.inputBorderHover = make(kZaiInk, 38);              // --color-input-border-hover: var(--color-border-hover) [498 -> 480: rgba(13,13,13,0.15)]
    // zai 主题刻意不用品牌蓝做聚焦边框，而是用 border-hover 的 15% 墨色（focus 更「安静」）。
    p.inputBorderFocused = make(kZaiInk, 38);            // --color-input-border-focused: var(--color-border-hover) [499]

    // 边框
    p.border = make(kZaiInk, 26);                        // --color-border: rgba(13,13,13,0.1) [479]
    p.borderHover = make(kZaiInk, 38);                   // --color-border-hover: rgba(13,13,13,0.15) [480]
    p.cardBorder = make(kZaiInk, 26);                    // --color-card-border: var(--color-border) [490]

    // 文本
    p.foreground = make(kNeutral800);                    // --color-foreground: var(--color-neutral-800) = #262626 [553]
    p.foregroundSubtle = make(kNeutral800, 153);         // color-mix(neutral-800 60%, transparent) [554]
    p.foregroundSubtlest = make(kNeutral800, 102);       // color-mix(neutral-800 40%, transparent) [555]
    p.foregroundInverse = make(kZaiWhite);               // --color-foreground-inverse: #ffffff [556]

    // 品牌与交互
    p.brand = make(kZaiInk2);                            // --color-brand: #000000 [467]
    p.accent = make(kZaiBlueSurface);                    // --color-accent: #ebf4ff [476]
    p.iconBlue = make(kZaiBlue);                         // --color-icon-blue: var(--color-terminal-bright-blue) = #0b7fff [510]
    p.primary = make(kZaiInk2);                          // --color-primary: #000000 [545]
    p.primaryForeground = make(kZaiWhite);               // --color-primary-foreground: #ffffff [546]
    p.secondary = make(kZaiBorderGray);                  // --color-secondary: #e6e6e6 [547]
    p.hover = make(kZaiInk, 13);                         // --color-hover: rgba(13,13,13,0.05) [481]
    p.selected = make(kZaiInk, 13);                      // --color-selected: rgba(13,13,13,0.05) [482]

    // 语义反馈
    p.success = make(kZaiGreen);                         // --color-success: #1e8a3e [557]
    p.successForeground = make(kZaiWhite);               // --color-success-foreground: #ffffff [558]
    p.warning = make(kZaiOrange);                        // --color-warning: #e07b00 [563]
    p.warningForeground = make(kZaiWhite);               // --color-warning-foreground: #ffffff [565]
    p.destructive = make(kZaiRed);                       // --color-destructive: #e03131 [561]
    p.destructiveForeground = make(kZaiWhite);           // --color-destructive-foreground: #ffffff [562]
    p.idleTask = make(kZaiViolet);                       // --color-idle-task: #9e77ed [559]
    p.idleTaskSurface = make(kZaiVioletSurface);         // --color-idle-task-surface: #f5f3ff [560]

    // Diff
    p.diffAdded = make(kZaiGreen);                       // --color-diff-added: #1e8a3e [566]
    p.diffAddedForeground = make(kZaiWhite);             // --color-diff-added-foreground: #ffffff [567]
    p.diffRemoved = make(kZaiRed);                       // --color-diff-removed: #e03131 [568]
    p.diffRemovedForeground = make(kZaiWhite);           // --color-diff-removed-foreground: #ffffff [569]

    // 浮层
    p.toast = make(kZaiWhite);                           // --color-toast: #ffffff [578]
    p.tooltip = make(kZaiSidebarLight);                  // --color-tooltip: #f0f0f0 [579]
    p.tooltipForeground = make(kZaiInk);                 // --color-tooltip-foreground: #0d0d0d [580]
    p.tag = make(kZaiBorderGray);                        // --color-tag: #e6e6e6 [583]

    // 等待交互（权限 / 提问 / 计划确认共用同一套绿色）
    p.interactionConfirmationSurface = make(kZaiGreenSoft);    // #eaf7ee [551]
    p.interactionConfirmationForeground = make(kZaiGreenDeep); // #166b32 [552]

    p.isDark = false;
    return p;
}

/// Zai Dark 调色板。
Palette zaiDark() {
    Palette p;

    // 结构表面
    p.background = make(kZaiDarkBg);                     // --color-background: #161616 [styles:zai-dark 609]
    p.backgroundAlt = make(kZaiDarkCard, 153);           // color-mix(background-win-alt #2b2b2b 60%, transparent) [611]
    p.header = make(kZaiDarkHeader);                     // --color-header: #202020 [628]
    p.panel = make(kZaiDarkHeader);                      // --color-panel: #202020 [629]
    p.sidebar = make(kZaiDarkBg);                        // --color-sidebar: #161616 [630]
    p.surface = make(kZaiWhite, 13);                     // rgba(255,255,255,0.05) [631]
    p.surfaceHover = make(kZaiWhite, 26);                // rgba(255,255,255,0.1) [632]
    p.card = make(kZaiDarkCard);                         // --color-card: #2b2b2b [633]
    p.cardSelected = make(kZaiDarkCard);                 // --color-card-selected: var(--color-input) = #2b2b2b [634 -> 640]
    p.popover = make(kZaiDarkCard);                      // --color-popover: #2b2b2b [636]
    p.popoverBorder = make(kZaiWhite, 26);               // --color-popover-border: var(--color-border) [639 -> 624]
    p.menu = make(kZaiDarkCard);                         // --color-menu: #2b2b2b [688]
    p.menuHover = make(kZaiDarkHover);                   // --color-menu-hover: #363636 [689]

    // 输入
    p.input = make(kZaiDarkCard);                        // --color-input: #2b2b2b [640]
    p.inputFocused = make(kZaiDarkCard);                 // --color-input-focused: var(--color-input) [641]
    p.inputBorder = make(kZaiWhite, 26);                 // --color-input-border: var(--color-border) [642]
    p.inputBorderHover = make(kZaiWhite, 38);            // --color-input-border-hover: rgba(255,255,255,0.15) [643 -> 625]
    p.inputBorderFocused = make(kZaiWhite, 38);          // --color-input-border-focused: var(--color-border-hover) [644]

    // 边框
    p.border = make(kZaiWhite, 26);                      // --color-border: rgba(255,255,255,0.1) [624]
    p.borderHover = make(kZaiWhite, 38);                 // --color-border-hover: rgba(255,255,255,0.15) [625]
    p.cardBorder = make(kZaiWhite, 26);                  // --color-card-border: var(--color-border) [635]

    // 文本
    p.foreground = make(kNeutral300);                    // --color-foreground: var(--color-neutral-300) = #d4d4d4 [698]
    p.foregroundSubtle = make(kNeutral300, 153);         // color-mix(neutral-300 60%, transparent) [699]
    p.foregroundSubtlest = make(kNeutral300, 77);        // color-mix(neutral-300 30%, transparent) [700]
    p.foregroundInverse = make(kZaiInk2);                // --color-foreground-inverse: #000000 [701]

    // 品牌与交互
    p.brand = make(kZaiWhite);                           // --color-brand: #ffffff [612]
    p.accent = make(kZaiSkyDeep);                        // --color-accent: #001d3d [621]
    p.iconBlue = make(kZaiDarkBlue);                     // --color-icon-blue: var(--color-terminal-bright-blue) = #80beff [663]
    p.primary = make(kZaiWhite);                         // --color-primary: #ffffff [690]
    p.primaryForeground = make(kZaiInk2);                // --color-primary-foreground: #000000 [691]
    p.secondary = make(kZaiDarkHover);                   // --color-secondary: #363636 [692]
    p.hover = make(kZaiWhite, 13);                       // --color-hover: rgba(255,255,255,0.05) [626]
    p.selected = make(kZaiWhite, 26);                    // --color-selected: rgba(255,255,255,0.1) [627]

    // 语义反馈
    p.success = make(kZaiDarkGreen);                     // --color-success: #46bf72 [702]
    p.successForeground = make(kZaiInk2);                // --color-success-foreground: #000000 [703]
    p.warning = make(kZaiDarkOrange);                    // --color-warning: #ff8a30 [709]
    p.warningForeground = make(kZaiInk2);                // --color-warning-foreground: #000000 [711]
    p.destructive = make(kZaiDarkRed);                   // --color-destructive: #ff5c5c [706]
    // 原版明确修过这里：zai-dark 的 destructive-foreground 固定为白字，不用 inverse 黑
    // （styles.css:707 的注释「与红色危险按钮的固定白字设计冲突」）。
    p.destructiveForeground = make(kZaiWhite);           // #ffffff [708]
    p.idleTask = make(kZaiDarkViolet);                   // --color-idle-task: #7b5ce5 [704]
    p.idleTaskSurface = make(kZaiDarkVioletSurface);     // #160d38 [705]

    // Diff
    p.diffAdded = make(kZaiDarkGreen);                   // --color-diff-added: #46bf72 [712]
    p.diffAddedForeground = make(kZaiInk2);              // #000000 [713]
    p.diffRemoved = make(kZaiDarkRed);                   // --color-diff-removed: #ff5c5c [714]
    p.diffRemovedForeground = make(kZaiInk2);            // #000000 [715]

    // 浮层
    p.toast = make(kZaiDarkCard);                        // --color-toast: #2b2b2b [724]
    p.tooltip = make(kZaiDarkCard);                      // --color-tooltip: #2b2b2b [725]
    p.tooltipForeground = make(kZaiDarkFg);              // --color-tooltip-foreground: #f8f8f8 [726]
    p.tag = make(kZaiDarkHover);                         // --color-tag: #363636 [729]

    // 等待交互
    p.interactionConfirmationSurface = make(kZaiDarkGreen, 41);   // rgba(70,191,114,0.16) [696]
    p.interactionConfirmationForeground = make(kZaiDarkGreenText); // #87d9a4 [697]

    p.isDark = true;
    return p;
}

}  // namespace

// ── Theme 单例 ──────────────────────────────────────────────────────────────

Theme::Theme() {
    // 构造时解析一次系统配色。QStyleHints 只在 GUI 线程可用，单例首次使用时
    // QApplication 必然已存在（风格表/字体库同理），这里不需要额外保护。
    if (auto *hints = QGuiApplication::styleHints()) {
        systemDark_ = hints->colorScheme() == Qt::ColorScheme::Dark;
        // colorSchemeChanged 只在 mode_ == System 时影响调色板；refreshFromSystem 内部会判断。
        // 用 AutoConnection：若 styleHints 将来在别的线程发信号，会排队回本对象线程执行。
        connect(hints, &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme) { refreshFromSystem(); });
    }
    resolvePalette();
}

Theme &Theme::instance() {
    // 函数内静态：C++11 起保证线程安全的一次性初始化。
    static Theme theme;
    return theme;
}

void Theme::resolvePalette() {
    // System 走系统明暗；Light/Dark 是显式选择，不理会系统。
    const bool dark = mode_ == ThemeMode::Dark || (mode_ == ThemeMode::System && systemDark_);
    palette_ = dark ? zaiDark() : zaiLight();
}

void Theme::setMode(ThemeMode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    resolvePalette();
    emit changed();
}

void Theme::refreshFromSystem() {
    if (mode_ != ThemeMode::System) {
        // 用户显式选了 Light/Dark：系统配色变化不影响任何令牌，不该发 changed()。
        return;
    }
    if (auto *hints = QGuiApplication::styleHints()) {
        systemDark_ = hints->colorScheme() == Qt::ColorScheme::Dark;
    }
    const bool wasDark = palette_.isDark;
    resolvePalette();
    if (wasDark != palette_.isDark) {
        emit changed();
    }
}

void Theme::setUiFontSize(int pixels) {
    const int clamped = std::clamp(pixels, 10, 24);
    if (clamped == uiFontSize_) {
        return;
    }
    uiFontSize_ = clamped;
    emit changed();
}

void Theme::setCodeFontSize(int pixels) {
    const int clamped = std::clamp(pixels, 8, 32);
    if (clamped == codeFontSize_) {
        return;
    }
    codeFontSize_ = clamped;
    emit changed();
}

int Theme::fontPixelSize(FontRole role) const {
    // 阶梯公式严格照 DESIGN.md 第 190-200 行的表格（对应 styles.css:143-148）。
    switch (role) {
        case FontRole::UiXl:
            return uiFontSize_ + 4;
        case FontRole::UiLg:
            return uiFontSize_ + 2;
        case FontRole::UiBase:
            return uiFontSize_;
        case FontRole::UiCaption:
            return uiFontSize_ - 1;
        case FontRole::UiSm:
            return uiFontSize_ - 2;
        case FontRole::UiXs:
            return uiFontSize_ - 4;
        case FontRole::Mono:
            return codeFontSize_;
        case FontRole::MonoSm:
            return codeFontSize_ - 2;
    }
    return uiFontSize_;
}

QFont Theme::font(FontRole role) const {
    const bool monospace = role == FontRole::Mono || role == FontRole::MonoSm;
    QFont f{monospace ? monospaceFamily() : sansFamily()};
    f.setPixelSize(fontPixelSize(role));
    // 界面字号变化不应该改变字重层级；字重由控件自己按语义设（DESIGN.md 第 257 行）。
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

QString Theme::css(const QColor &color) {
    if (!color.isValid()) {
        return QStringLiteral("transparent");
    }
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);  // #rrggbb
    }
    // 半透明一律走 rgba()：Qt 的 QCssParser 支持 rgba(r,g,b,a)，且 a 是 0-255 整数。
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString Theme::css(const QColor &color, double alpha) {
    if (!color.isValid()) {
        return QStringLiteral("transparent");
    }
    // alpha 取值 0.0-1.0，钳位后换算成 0-255 整数交给 QSS。
    const double clamped = std::clamp(alpha, 0.0, 1.0);
    QColor copy = color;
    copy.setAlpha(static_cast<int>(clamped * 255.0 + 0.5));
    return css(copy);
}

// ── 全局样式表 ──────────────────────────────────────────────────────────────

QString Theme::styleSheet() const {
    const Palette &p = palette_;

    // 局部别名，让下面的模板只出现令牌名，避免任何硬编码颜色。
    const QString background = css(p.background);
    const QString backgroundAlt = css(p.backgroundAlt);
    const QString header = css(p.header);
    const QString panel = css(p.panel);
    const QString sidebar = css(p.sidebar);
    const QString surface = css(p.surface);
    const QString surfaceHover = css(p.surfaceHover);
    const QString card = css(p.card);
    const QString cardSelected = css(p.cardSelected);
    const QString popover = css(p.popover);
    const QString popoverBorder = css(p.popoverBorder);
    const QString menu = css(p.menu);
    const QString menuHover = css(p.menuHover);

    const QString input = css(p.input);
    const QString inputFocused = css(p.inputFocused);
    const QString inputBorder = css(p.inputBorder);
    const QString inputBorderHover = css(p.inputBorderHover);
    const QString inputBorderFocused = css(p.inputBorderFocused);

    const QString border = css(p.border);
    const QString borderHover = css(p.borderHover);
    const QString cardBorder = css(p.cardBorder);

    const QString foreground = css(p.foreground);
    const QString foregroundSubtle = css(p.foregroundSubtle);
    const QString foregroundSubtlest = css(p.foregroundSubtlest);
    const QString foregroundInverse = css(p.foregroundInverse);

    const QString brand = css(p.brand);
    const QString accent = css(p.accent);
    const QString primary = css(p.primary);
    const QString primaryForeground = css(p.primaryForeground);
    const QString secondary = css(p.secondary);
    const QString hover = css(p.hover);
    const QString selected = css(p.selected);

    const QString success = css(p.success);
    const QString successForeground = css(p.successForeground);
    const QString warning = css(p.warning);
    const QString warningForeground = css(p.warningForeground);
    const QString destructive = css(p.destructive);
    const QString destructiveForeground = css(p.destructiveForeground);
    const QString idleTask = css(p.idleTask);

    const QString diffAdded = css(p.diffAdded);
    const QString diffRemoved = css(p.diffRemoved);

    const QString tooltip = css(p.tooltip);
    const QString tooltipForeground = css(p.tooltipForeground);
    const QString tag = css(p.tag);

    const QString interactionConfirmationSurface = css(p.interactionConfirmationSurface);
    const QString interactionConfirmationForeground = css(p.interactionConfirmationForeground);

    // 分隔线把手：DESIGN.md 第 487 行要求 4px 透明命中区，hover 时显示
    // 2px foreground-subtlest/50 的线。
    const QString splitterHover = css(p.foregroundSubtlest, 0.5);

    const QString uiFamily = sansFamily();
    const QString monoFamily = monospaceFamily();
    const QString uiBase = QString::number(fontPixelSize(FontRole::UiBase));
    const QString uiXl = QString::number(fontPixelSize(FontRole::UiXl));
    const QString uiLg = QString::number(fontPixelSize(FontRole::UiLg));
    const QString uiCaption = QString::number(fontPixelSize(FontRole::UiCaption));
    const QString uiSm = QString::number(fontPixelSize(FontRole::UiSm));
    const QString uiXs = QString::number(fontPixelSize(FontRole::UiXs));

    // 圆角层级照 DESIGN.md 第 287-334 行：一级容器 rounded-xl=12px，嵌套降级 lg=10 / md=8 / sm=6；
    // 菜单与选项浮层 8px（rounded-lg 壳 + rounded-md 选项）；对话框语义用 16px。
    // 间距照 4px 基准：紧凑 8px、标准 12px / 16px。
    //
    // 注意：下面的模板里没有 QSS 注释（/* */）。Qt 的样式表解析器对注释支持不稳定，
    // 且注释会干扰「样式表不得残留占位符」的自检，因此说明性注释全部留在 C++ 这一层。
    static const char *kTemplate = R"QSS(
QWidget {
    background: transparent;
    color: %FOREGROUND%;
    font-family: "%UI_FAMILY%";
    font-size: %UI_BASE%px;
}
QMainWindow, QDialog {
    background: %BACKGROUND%;
    color: %FOREGROUND%;
}
QDialog {
    border: 1px solid %POPOVER_BORDER%;
    border-radius: 16px;
}
QWidget[role="page"] {
    background: %BACKGROUND%;
}
QWidget[role="panel"] {
    background: %PANEL%;
}
QWidget[role="sidebar"] {
    background: %SIDEBAR%;
}
QWidget[role="header"] {
    background: %HEADER%;
    border-bottom: 1px solid %BORDER%;
}
QWidget[role="background-alt"] {
    background: %BACKGROUND_ALT%;
}
QWidget[role="surface"] {
    background: %SURFACE%;
}

/* 附件条：一个淡底圆角的小块，含 36px 缩略图、文件名与移除按钮。
   用背景色而不是边框来区分，避免和输入框的边框抢视觉。 */
#attachmentChip {
    background: %SURFACE%;
    border-radius: 6px;
}

/* 移除按钮要"轻"：它紧挨着缩略图，做成普通按钮会显得比图片本身还重。 */
#attachmentRemove {
    background: transparent;
    border: none;
    color: %FOREGROUND_SUBTLE%;
    font-size: 14px;
    padding: 0;
}

#attachmentRemove:hover {
    color: %DESTRUCTIVE%;
}

QPushButton {
    background: %CARD%;
    color: %FOREGROUND%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
    padding: 6px 12px;
    min-height: 26px;
}
QPushButton:hover {
    background: %SURFACE_HOVER%;
    border: 1px solid %BORDER_HOVER%;
}
QPushButton:pressed {
    background: %SELECTED%;
    border: 1px solid %BORDER_HOVER%;
}
QPushButton:disabled {
    background: %SURFACE%;
    color: %FOREGROUND_SUBTLEST%;
    border: 1px solid %BORDER%;
}
QPushButton:focus {
    border: 1px solid %BRAND%;
    outline: none;
}
QPushButton[accent="true"] {
    background: %PRIMARY%;
    color: %PRIMARY_FOREGROUND%;
    border: 1px solid %PRIMARY%;
    font-weight: 500;
}
QPushButton[accent="true"]:hover {
    background: %PRIMARY%;
    border: 1px solid %PRIMARY%;
}
QPushButton[accent="true"]:pressed {
    background: %PRIMARY%;
    border: 1px solid %PRIMARY%;
}
QPushButton[accent="true"]:disabled {
    background: %SURFACE%;
    color: %FOREGROUND_SUBTLEST%;
    border: 1px solid %BORDER%;
}
QPushButton[variant="ghost"] {
    background: transparent;
    border: 1px solid transparent;
}
QPushButton[variant="ghost"]:hover {
    background: %SURFACE_HOVER%;
    border: 1px solid transparent;
}
QPushButton[variant="ghost"]:pressed {
    background: %SELECTED%;
    border: 1px solid transparent;
}
QPushButton[variant="ghost"]:disabled {
    background: transparent;
    color: %FOREGROUND_SUBTLEST%;
    border: 1px solid transparent;
}
QPushButton[variant="secondary"] {
    background: %SECONDARY%;
    color: %FOREGROUND%;
    border: 1px solid %BORDER%;
}
QPushButton[variant="secondary"]:hover {
    border: 1px solid %BORDER_HOVER%;
}
QPushButton[variant="destructive"] {
    background: %DESTRUCTIVE%;
    color: %DESTRUCTIVE_FOREGROUND%;
    border: 1px solid %DESTRUCTIVE%;
    font-weight: 500;
}
QPushButton[variant="destructive"]:hover {
    background: %DESTRUCTIVE%;
    border: 1px solid %DESTRUCTIVE%;
}
QPushButton[variant="link"] {
    background: transparent;
    border: 1px solid transparent;
    color: %BRAND%;
    padding: 2px 4px;
}
QPushButton[variant="link"]:hover {
    background: transparent;
    color: %BRAND%;
}
QToolButton {
    background: transparent;
    color: %FOREGROUND%;
    border: 1px solid transparent;
    border-radius: 8px;
    padding: 4px;
}
QToolButton:hover {
    background: %SURFACE_HOVER%;
}
QToolButton:pressed {
    background: %SELECTED%;
}
QToolButton:disabled {
    color: %FOREGROUND_SUBTLEST%;
}

QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit {
    background: %INPUT%;
    color: %FOREGROUND%;
    border: 1px solid %INPUT_BORDER%;
    border-radius: 8px;
    padding: 6px 8px;
    selection-background-color: %SELECTED%;
    selection-color: %FOREGROUND%;
}
QLineEdit:hover, QTextEdit:hover, QPlainTextEdit:hover, QSpinBox:hover,
QDoubleSpinBox:hover, QDateEdit:hover, QTimeEdit:hover {
    border: 1px solid %INPUT_BORDER_HOVER%;
}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus,
QDoubleSpinBox:focus, QDateEdit:focus, QTimeEdit:focus {
    background: %INPUT_FOCUSED%;
    border: 1px solid %INPUT_BORDER_FOCUSED%;
    outline: none;
}
QLineEdit:disabled, QTextEdit:disabled, QPlainTextEdit:disabled, QSpinBox:disabled,
QDoubleSpinBox:disabled, QDateEdit:disabled, QTimeEdit:disabled {
    background: %SURFACE%;
    color: %FOREGROUND_SUBTLEST%;
    border: 1px solid %BORDER%;
}
QLineEdit::placeholder, QTextEdit::placeholder {
    color: %FOREGROUND_SUBTLEST%;
}
QPlainTextEdit[role="code"], QTextEdit[role="code"] {
    font-family: "%MONO_FAMILY%";
    background: %CARD%;
    border: 1px solid %CARD_BORDER%;
}

QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    background: transparent;
    border: 1px solid transparent;
    width: 14px;
}

QScrollBar:vertical {
    background: transparent;
    width: 6px;
    margin: 0px;
    border: none;
}
QScrollBar:horizontal {
    background: transparent;
    height: 6px;
    margin: 0px;
    border: none;
}
QScrollBar::handle:vertical {
    background: %BORDER%;
    border-radius: 3px;
    min-height: 24px;
}
QScrollBar::handle:horizontal {
    background: %BORDER%;
    border-radius: 3px;
    min-width: 24px;
}
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
    background: %BORDER_HOVER%;
}
QScrollBar::add-line, QScrollBar::sub-line {
    background: transparent;
    border: none;
    width: 0px;
    height: 0px;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}
QScrollArea {
    background: transparent;
    border: none;
}
QAbstractScrollArea::corner {
    background: transparent;
}

QListView, QListWidget, QTreeView, QTableView, QColumnView {
    background: transparent;
    color: %FOREGROUND%;
    border: none;
    outline: none;
    show-decoration-selected: 1;
}
QListView::item, QListWidget::item, QTreeView::item {
    background: transparent;
    border-radius: 8px;
    padding: 4px 8px;
    margin: 1px 4px;
}
QListView::item:hover, QListWidget::item:hover, QTreeView::item:hover {
    background: %HOVER%;
}
QListView::item:selected, QListWidget::item:selected, QTreeView::item:selected {
    background: %SELECTED%;
    color: %FOREGROUND%;
}
QListView::item:disabled, QListWidget::item:disabled, QTreeView::item:disabled {
    color: %FOREGROUND_SUBTLEST%;
}
QTreeView::branch {
    background: transparent;
}
QHeaderView::section {
    background: %SURFACE%;
    color: %FOREGROUND_SUBTLE%;
    border: none;
    border-bottom: 1px solid %BORDER%;
    padding: 4px 8px;
}
QHeaderView::section:hover {
    color: %FOREGROUND%;
}

QMenuBar {
    background: %BACKGROUND%;
    color: %FOREGROUND%;
    border-bottom: 1px solid %BORDER%;
}
QMenuBar::item {
    background: transparent;
    border-radius: 6px;
    padding: 4px 8px;
}
QMenuBar::item:selected {
    background: %MENU_HOVER%;
}
QMenu {
    background: %MENU%;
    color: %FOREGROUND%;
    border: 1px solid %POPOVER_BORDER%;
    border-radius: 8px;
    padding: 4px;
}
QMenu::item {
    background: transparent;
    border-radius: 6px;
    padding: 6px 12px;
    margin: 1px 2px;
}
QMenu::item:selected {
    background: %MENU_HOVER%;
}
QMenu::item:disabled {
    color: %FOREGROUND_SUBTLEST%;
}
QMenu::separator {
    height: 1px;
    background: %BORDER%;
    margin: 4px 8px;
}
QMenu::icon {
    padding-left: 4px;
}
QMenu::indicator {
    width: 14px;
    height: 14px;
}

QToolTip {
    background: %TOOLTIP%;
    color: %TOOLTIP_FOREGROUND%;
    border: none;
    border-radius: 6px;
    padding: 4px 8px;
    font-size: %UI_SM%px;
}
QTabWidget::pane {
    background: transparent;
    border: 1px solid %BORDER%;
    border-radius: 10px;
}
QTabBar {
    background: transparent;
    qproperty-drawBase: 0;
}
QTabBar::tab {
    background: transparent;
    color: %FOREGROUND_SUBTLE%;
    border: none;
    border-radius: 8px;
    padding: 5px 12px;
    margin-right: 2px;
}
QTabBar::tab:hover {
    background: %HOVER%;
    color: %FOREGROUND%;
}
QTabBar::tab:selected {
    background: %SELECTED%;
    color: %FOREGROUND%;
}
QTabBar::tab:disabled {
    color: %FOREGROUND_SUBTLEST%;
}
QTabBar::close-button {
    background: transparent;
    border-radius: 4px;
}

QComboBox {
    background: %INPUT%;
    color: %FOREGROUND%;
    border: 1px solid %INPUT_BORDER%;
    border-radius: 8px;
    padding: 6px 8px;
    min-height: 26px;
}
QComboBox:hover {
    border: 1px solid %INPUT_BORDER_HOVER%;
}
QComboBox:focus, QComboBox:on {
    background: %INPUT_FOCUSED%;
    border: 1px solid %INPUT_BORDER_FOCUSED%;
}
QComboBox:disabled {
    background: %SURFACE%;
    color: %FOREGROUND_SUBTLEST%;
    border: 1px solid %BORDER%;
}
QComboBox::drop-down {
    background: transparent;
    border: none;
    width: 18px;
}
QComboBox QAbstractItemView {
    background: %MENU%;
    color: %FOREGROUND%;
    border: 1px solid %POPOVER_BORDER%;
    border-radius: 8px;
    padding: 4px;
    outline: none;
    selection-background-color: %MENU_HOVER%;
    selection-color: %FOREGROUND%;
}
QComboBox QAbstractItemView::item {
    background: transparent;
    border-radius: 6px;
    padding: 6px 8px;
    min-height: 22px;
}
QComboBox QAbstractItemView::item:hover {
    background: %MENU_HOVER%;
}

QCheckBox, QRadioButton {
    background: transparent;
    color: %FOREGROUND%;
    spacing: 8px;
    padding: 2px 0px;
}
QCheckBox:disabled, QRadioButton:disabled {
    color: %FOREGROUND_SUBTLEST%;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 14px;
    height: 14px;
    background: %INPUT%;
    border: 1px solid %INPUT_BORDER%;
}
QCheckBox::indicator {
    border-radius: 4px;
}
QRadioButton::indicator {
    border-radius: 7px;
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {
    border: 1px solid %INPUT_BORDER_HOVER%;
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: %PRIMARY%;
    border: 1px solid %PRIMARY%;
}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {
    background: %SURFACE%;
    border: 1px solid %BORDER%;
}

QSplitter {
    background: transparent;
}
QSplitter::handle {
    background: transparent;
    width: 4px;
    height: 4px;
}
QSplitter::handle:hover {
    background: %SPLITTER_HOVER%;
}
QSplitter::handle:pressed {
    background: %SPLITTER_HOVER%;
}

QGroupBox {
    background: transparent;
    color: %FOREGROUND%;
    border: 1px solid %BORDER%;
    border-radius: 10px;
    margin-top: 10px;
    padding: 12px 12px 8px 12px;
    font-weight: 500;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 12px;
    padding: 0px 4px;
    color: %FOREGROUND_SUBTLE%;
}

QLabel {
    background: transparent;
    color: %FOREGROUND%;
}
QLabel[role="h1"] {
    font-size: %UI_XL%px;
    font-weight: 600;
}
QLabel[role="h2"] {
    font-size: %UI_LG%px;
    font-weight: 600;
}
QLabel[role="h3"] {
    font-size: %UI_BASE%px;
    font-weight: 600;
}
QLabel[role="caption"] {
    font-size: %UI_CAPTION%px;
    color: %FOREGROUND_SUBTLE%;
}
QLabel[role="sm"] {
    font-size: %UI_SM%px;
    color: %FOREGROUND_SUBTLE%;
}
QLabel[role="muted"] {
    color: %FOREGROUND_SUBTLE%;
}
QLabel[role="weak"] {
    color: %FOREGROUND_SUBTLEST%;
}
QLabel[role="xs"] {
    font-size: %UI_XS%px;
    color: %FOREGROUND_SUBTLEST%;
}
QLabel[role="mono"] {
    font-family: "%MONO_FAMILY%";
    font-size: %UI_BASE%px;
}
QLabel[role="success"] {
    color: %SUCCESS%;
}
QLabel[role="warning"] {
    color: %WARNING%;
}
QLabel[role="destructive"] {
    color: %DESTRUCTIVE%;
}

QFrame {
    background: transparent;
    border: none;
}
QFrame[role="card"] {
    background: %CARD%;
    border: 1px solid %CARD_BORDER%;
    border-radius: 12px;
}
QFrame[role="card-hover"]:hover {
    background: %CARD_SELECTED%;
}
QFrame[role="panel"] {
    background: %PANEL%;
    border: 1px solid %BORDER%;
    border-radius: 12px;
}
QFrame[role="surface"] {
    background: %SURFACE%;
    border-radius: 10px;
}
QFrame[role="popover"] {
    background: %POPOVER%;
    border: 1px solid %POPOVER_BORDER%;
    border-radius: 12px;
}
QFrame[role="menu"] {
    background: %MENU%;
    border: 1px solid %POPOVER_BORDER%;
    border-radius: 8px;
}
QFrame[role="tooltip"] {
    background: %TOOLTIP%;
    color: %TOOLTIP_FOREGROUND%;
    border-radius: 6px;
}
QFrame[role="toast"] {
    background: %TOAST%;
    border: 1px solid %POPOVER_BORDER%;
    border-radius: 12px;
}
QFrame[role="separator"] {
    background: %BORDER%;
    border: none;
    max-height: 1px;
    min-height: 1px;
}
QFrame[role="separator"][orientation="vertical"] {
    max-width: 1px;
    min-width: 1px;
    max-height: none;
}
QFrame[role="badge"] {
    background: %TAG%;
    border-radius: 6px;
    padding: 1px 6px;
}
QFrame[role="diff-added"] {
    background: %DIFF_ADDED%;
    border: none;
    border-radius: 6px;
}
QFrame[role="diff-removed"] {
    background: %DIFF_REMOVED%;
    border: none;
    border-radius: 6px;
}
QFrame[role="interaction-confirmation"] {
    background: %INTERACTION_CONFIRMATION_SURFACE%;
    border: none;
    border-radius: 10px;
}
QLabel[role="interaction-confirmation"] {
    color: %INTERACTION_CONFIRMATION_FOREGROUND%;
}
QLabel[role="idle-task"] {
    color: %IDLE_TASK%;
}
QFrame[role="accent"] {
    background: %ACCENT%;
    border-radius: 10px;
}

QProgressBar {
    background: %SURFACE%;
    border: none;
    border-radius: 4px;
    height: 6px;
    text-align: center;
    color: %FOREGROUND_SUBTLE%;
}
QProgressBar::chunk {
    background: %BRAND%;
    border-radius: 4px;
}

QSlider::groove:horizontal {
    background: %SURFACE%;
    height: 4px;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: %BRAND%;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: %CARD%;
    border: 1px solid %BORDER_HOVER%;
    width: 12px;
    margin: -5px 0px;
    border-radius: 6px;
}
QSlider::handle:horizontal:hover {
    border: 1px solid %BRAND%;
}

QToolBar {
    background: %BACKGROUND%;
    border: none;
    spacing: 4px;
    padding: 4px;
}
QToolBar::separator {
    background: %BORDER%;
    width: 1px;
    margin: 4px 4px;
}
QStatusBar {
    background: %BACKGROUND%;
    color: %FOREGROUND_SUBTLE%;
    border-top: 1px solid %BORDER%;
}
QStatusBar::item {
    border: none;
}
QDockWidget {
    background: %PANEL%;
    color: %FOREGROUND%;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background: %HEADER%;
    padding: 4px 8px;
    border-bottom: 1px solid %BORDER%;
}
QStackedWidget {
    background: transparent;
}
QSizeGrip {
    background: transparent;
    width: 12px;
    height: 12px;
}
)QSS";

    // 单次顺序替换，避免链式 replace 产生二次替换（例如某令牌值里出现 `%` 之外的标记）。
    QString sheet = QString::fromUtf8(kTemplate);
    const struct {
        const char *token;
        const QString &value;
    } replacements[] = {
        {"%BACKGROUND%", background},
        {"%BACKGROUND_ALT%", backgroundAlt},
        {"%HEADER%", header},
        {"%PANEL%", panel},
        {"%SIDEBAR%", sidebar},
        {"%SURFACE%", surface},
        {"%SURFACE_HOVER%", surfaceHover},
        {"%CARD%", card},
        {"%CARD_SELECTED%", cardSelected},
        {"%POPOVER%", popover},
        {"%POPOVER_BORDER%", popoverBorder},
        {"%MENU%", menu},
        {"%MENU_HOVER%", menuHover},
        {"%INPUT%", input},
        {"%INPUT_FOCUSED%", inputFocused},
        {"%INPUT_BORDER%", inputBorder},
        {"%INPUT_BORDER_HOVER%", inputBorderHover},
        {"%INPUT_BORDER_FOCUSED%", inputBorderFocused},
        {"%BORDER%", border},
        {"%BORDER_HOVER%", borderHover},
        {"%CARD_BORDER%", cardBorder},
        {"%FOREGROUND%", foreground},
        {"%FOREGROUND_SUBTLE%", foregroundSubtle},
        {"%FOREGROUND_SUBTLEST%", foregroundSubtlest},
        {"%FOREGROUND_INVERSE%", foregroundInverse},
        {"%BRAND%", brand},
        {"%ACCENT%", accent},
        {"%PRIMARY%", primary},
        {"%PRIMARY_FOREGROUND%", primaryForeground},
        {"%SECONDARY%", secondary},
        {"%HOVER%", hover},
        {"%SELECTED%", selected},
        {"%SUCCESS%", success},
        {"%SUCCESS_FOREGROUND%", successForeground},
        {"%WARNING%", warning},
        {"%WARNING_FOREGROUND%", warningForeground},
        {"%DESTRUCTIVE%", destructive},
        {"%DESTRUCTIVE_FOREGROUND%", destructiveForeground},
        {"%IDLE_TASK%", idleTask},
        {"%DIFF_ADDED%", diffAdded},
        {"%DIFF_REMOVED%", diffRemoved},
        {"%TOOLTIP%", tooltip},
        {"%TOOLTIP_FOREGROUND%", tooltipForeground},
        {"%TOAST%", css(p.toast)},
        {"%TAG%", tag},
        {"%INTERACTION_CONFIRMATION_SURFACE%", interactionConfirmationSurface},
        {"%INTERACTION_CONFIRMATION_FOREGROUND%", interactionConfirmationForeground},
        {"%SPLITTER_HOVER%", splitterHover},
        {"%UI_FAMILY%", uiFamily},
        {"%MONO_FAMILY%", monoFamily},
        {"%UI_XL%", uiXl},
        {"%UI_LG%", uiLg},
        {"%UI_BASE%", uiBase},
        {"%UI_CAPTION%", uiCaption},
        {"%UI_SM%", uiSm},
        {"%UI_XS%", uiXs},
    };
    for (const auto &item : replacements) {
        sheet.replace(QLatin1String(item.token), item.value);
    }
    return sheet;
}

}  // namespace zcode::ui
