// LyCode — 内置模型目录
//
// 读 assets/model_list.txt：按提供商分组列出模型、协议、base_url、可用思考档位
// 与上下文窗口。设置页用它让用户**从列表里选**，而不是逐个手打模型 id。
//
// 文件格式（块之间空行可有可无）：
//
//   1.DeepSeek
//   model:deepseek-flash,deepseek-v4-pro
//   base_url_type:Anthropic
//   base_url:https://api.deepseek.com/anthropic
//   thinking:off,low,medium,high,max
//   maxcontextwindow:1000000,1000000
//
// 两个字段的"对齐对象"不同，这是最容易读错的地方：
//   * thinking        —— **整个提供商共用**一组档位（样例里 5 个档位配 2 个模型）
//   * maxcontextwindow —— **逐个模型**对齐 model 的顺序；个数不足时后者取 0（用内置默认）
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/Types.h"

namespace lycode::model
{

/// 目录里的一个模型。
struct CatalogModel {
  QString id;
  /// 该模型可用的思考档位 id。来自提供商的 thinking 字段。
  QStringList reasoningLevels;
  /// 上下文窗口；0 表示"未指定，用内置默认"。
  int contextWindow = 0;
};

/// 目录里的一个提供商。
struct CatalogProvider {
  QString name;
  ProviderKind kind = ProviderKind::OpenAICompatible;
  QString baseUrl;
  QList<CatalogModel> models;
};

class ModelCatalog
{
  public:
    /// 仓库内随二进制一起分发的目录文件（编译进资源）。
    static QString bundledResourcePath();
    /// 用户可覆盖的目录文件：`<数据目录>/model_list.txt`。
    /// 放一份在这里就能改模型清单而不必重新编译。
    static QString overridePath();

    /// 载入目录：优先用磁盘上的覆盖文件，否则用内嵌的。
    /// 解析失败时返回空列表并填 `errorOut`。
    static QList<CatalogProvider> load(QString * errorOut = nullptr);

    /// 解析目录文本。**空白与空行都会被忽略**，`#` 开头的行视为注释。
    /// 不认识的键会被跳过（便于以后加字段而不破坏旧版本）。
    static QList<CatalogProvider> parse(const QString & text, QString * errorOut = nullptr);

    /// 该模型在目录里的能力覆盖。找不到时返回空的 override（`isEmpty()` 为真）。
    /// `providerId` 用调用方自己的 provider id（目录里的名字只是展示名）。
    static ModelOptionOverride overrideFor(const CatalogProvider & provider,
                                           const QString & modelId,
                                           const QString & providerId);
};

}  // namespace lycode::model
