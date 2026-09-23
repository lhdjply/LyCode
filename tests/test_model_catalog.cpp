// LyCode — 内置模型目录测试
//
// 重点在**两个字段对齐的对象不同**——这是这个格式最容易读错的地方：
//   * thinking         提供商级（一组档位给整块所有模型）
//   * maxcontextwindow 逐模型（按 model 的顺序对齐）
// 以及"字段顺序不该影响结果"（thinking 可能写在 model 之前）。
#include <QCoreApplication>
#include <QFile>
#include <QTest>

#include "model/ModelCatalog.h"

using namespace lycode;
using namespace lycode::model;

class TestModelCatalog : public QObject
{
    Q_OBJECT

  private slots:
    void parsesProvidersAndModels();
    void thinkingIsProviderWideWhileContextWindowIsPerModel();
    void fieldOrderDoesNotMatter();
    void toleratesNoiseAndUnknownKeys();
    void bundledCatalogIsUsable();
};

namespace
{

const char * kSample = R"(1.DeepSeek
model:deepseek-flash,deepseek-v4-pro
base_url_type:Anthropic
base_url:https://api.deepseek.com/anthropic
thinking:off,low,medium,high,max
maxcontextwindow:1000000,2000000

2.Qwen
model:qwen3.8-max,qwen3.8-flash
base_url_type:OpenAI
base_url:https://example.com/v1
thinking:off,high
)";

}  // namespace

void TestModelCatalog::parsesProvidersAndModels() {
  const QList<CatalogProvider> providers = ModelCatalog::parse(QString::fromUtf8(kSample));
  QCOMPARE(providers.size(), 2);

  QCOMPARE(providers.at(0).name, QStringLiteral("DeepSeek"));
  // Anthropic 与 OpenAI 两种协议都要按字面识别。
  QCOMPARE(providers.at(0).kind, ProviderKind::Anthropic);
  QCOMPARE(providers.at(0).baseUrl, QStringLiteral("https://api.deepseek.com/anthropic"));
  QCOMPARE(providers.at(0).models.size(), 2);
  QCOMPARE(providers.at(0).models.at(0).id, QStringLiteral("deepseek-flash"));

  QCOMPARE(providers.at(1).name, QStringLiteral("Qwen"));
  QCOMPARE(providers.at(1).kind, ProviderKind::OpenAICompatible);
  QCOMPARE(providers.at(1).models.size(), 2);
}

void TestModelCatalog::thinkingIsProviderWideWhileContextWindowIsPerModel() {
  const QList<CatalogProvider> providers = ModelCatalog::parse(QString::fromUtf8(kSample));
  const CatalogProvider &deepseek = providers.at(0);

  // thinking 是提供商级：整块两个模型拿到**同一组** 5 个档位。
  QCOMPARE(deepseek.models.at(0).reasoningLevels.size(), 5);
  QCOMPARE(deepseek.models.at(1).reasoningLevels.size(), 5);
  QCOMPARE(deepseek.models.at(0).reasoningLevels,
           deepseek.models.at(1).reasoningLevels);

  // maxcontextwindow 是逐模型：两个模型拿到**各自**的值。
  QCOMPARE(deepseek.models.at(0).contextWindow, 1000000);
  QCOMPARE(deepseek.models.at(1).contextWindow, 2000000);

  // 个数不足时后者取 0（"未指定，用内置默认"），而不是复用前一个值。
  const CatalogProvider &qwen = providers.at(1);
  QCOMPARE(qwen.models.at(0).contextWindow, 0);
  QCOMPARE(qwen.models.at(1).contextWindow, 0);
}

void TestModelCatalog::fieldOrderDoesNotMatter() {
  // thinking 写在 model **之前**：这正是最初实现会漏掉的情况。
  const QList<CatalogProvider> providers = ModelCatalog::parse(
                                             QStringLiteral("1.X\nthinking:off,high\nmodel:a,b\nmaxcontextwindow:100,200\n"));
  QCOMPARE(providers.size(), 1);
  QCOMPARE(providers.at(0).models.size(), 2);
  QCOMPARE(providers.at(0).models.at(0).reasoningLevels,
           QStringList({QStringLiteral("off"), QStringLiteral("high")}));
  QCOMPARE(providers.at(0).models.at(1).reasoningLevels.size(), 2);
  QCOMPARE(providers.at(0).models.at(1).contextWindow, 200);
}

void TestModelCatalog::toleratesNoiseAndUnknownKeys() {
  const QList<CatalogProvider> providers = ModelCatalog::parse(
                                             QStringLiteral("# 注释\n\n"
                                                            "1. A\n"
                                                            "model: m1 , m2 \n"
                                                            "some_future_key: whatever\n"
                                                            "base_url_type: anthropic\n"
                                                            "\n"
                                                            "2. 空块\n"
                                                            "base_url:https://x\n"));
  // 空块（没有 model）应当被跳过，而不是产生一个没有模型的提供商。
  QCOMPARE(providers.size(), 1);
  QCOMPARE(providers.at(0).models.size(), 2);
  // 逗号两侧的空格要被清掉。
  QCOMPARE(providers.at(0).models.at(1).id, QStringLiteral("m2"));
  // 大小写不敏感。
  QCOMPARE(providers.at(0).kind, ProviderKind::Anthropic);

  // 完全解析不出东西时要有错误信息，而不是静默返回空。
  QString error;
  QVERIFY(ModelCatalog::parse(QStringLiteral("不是目录格式"), &error).isEmpty());
  QVERIFY(!error.isEmpty());
}

void TestModelCatalog::bundledCatalogIsUsable() {
  // 内嵌的目录必须真的能读出来——它是"从列表选模型"的数据源，
  // 缺了就会表现为按钮里空空如也。
  QString error;
  const QList<CatalogProvider> providers = ModelCatalog::load(&error);
  QVERIFY2(!providers.isEmpty(), qPrintable(QStringLiteral("载入失败: %1").arg(error)));
  for (const CatalogProvider &provider : providers) {
    QVERIFY2(!provider.name.isEmpty(), "每个提供商都要有名字");
    QVERIFY2(!provider.models.isEmpty(),
             qPrintable(QStringLiteral("%1 没有模型").arg(provider.name)));
    for (const CatalogModel &model : provider.models) {
      QVERIFY2(!model.id.isEmpty(), "模型 id 不能为空");
    }
  }
}

QTEST_MAIN(TestModelCatalog)

#include "test_model_catalog.moc"
