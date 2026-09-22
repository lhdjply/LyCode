// ZCode Qt — 应用配置单元测试
//
// 配置是"用户设置能否生效"的唯一通路，而且它是整份替换语义的小文档，
// 一旦某个字段读写不一致，表现是"设置改了没反应"这类很难定位的问题。
// 这里同时覆盖内存往返与真实落盘往返。
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "ui/AppConfig.h"

using namespace zcode;
using namespace zcode::ui;

class TestAppConfig : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void jsonRoundTripKeepsEveryField();
    void emptyOverrideIsRemoved();
    void reasoningLevelRemembersEmptyValue();
    void saveAndLoadFromDisk();

private:
    std::unique_ptr<QTemporaryDir> dataDir_;
};

void TestAppConfig::init() {
    dataDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dataDir_->isValid());
    // 必须在任何 AppConfig 调用之前设置：配置路径按调用时读环境变量。
    qputenv("ZCODE_DATA_BASE_DIR", dataDir_->path().toUtf8());
}

void TestAppConfig::cleanup() {
    qunsetenv("ZCODE_DATA_BASE_DIR");
    dataDir_.reset();
}

void TestAppConfig::jsonRoundTripKeepsEveryField() {
    AppSettings settings;
    settings.themeMode = ThemeMode::Dark;
    settings.uiFontSize = 16;
    settings.codeFontSize = 15;
    settings.language = QStringLiteral("en-US");
    settings.defaultSessionMode = SessionMode::Plan;
    settings.persistSessions = false;

    ProviderConfig provider;
    provider.id = QStringLiteral("p1");
    provider.name = QStringLiteral("Provider One");
    provider.kind = ProviderKind::Anthropic;
    provider.baseUrl = QStringLiteral("https://api.anthropic.com");
    provider.apiKey = QStringLiteral("sk-test");
    provider.models = {QStringLiteral("m1"), QStringLiteral("m2")};
    settings.providers.append(provider);

    settings.rememberModelForWorkspace(QStringLiteral("ws-1"),
                                       ModelSelection{QStringLiteral("p1"),
                                                      QStringLiteral("m1"),
                                                      QStringLiteral("high")});

    ModelOptionOverride override;
    override.contextWindow = 200000;
    override.maxOutputTokens = 32768;
    override.reasoningLevels = {QStringLiteral("off"), QStringLiteral("high")};
    override.defaultReasoningLevel = QStringLiteral("high");
    settings.setModelOverride(QStringLiteral("p1"), QStringLiteral("m1"), override);

    settings.rememberReasoningLevel(QStringLiteral("p1"), QStringLiteral("m2"),
                                    QStringLiteral("low"));

    const AppSettings restored = AppSettings::fromJson(settings.toJson());

    QCOMPARE(restored.themeMode, ThemeMode::Dark);
    QCOMPARE(restored.uiFontSize, 16);
    QCOMPARE(restored.codeFontSize, 15);
    QCOMPARE(restored.language, QStringLiteral("en-US"));
    QCOMPARE(restored.defaultSessionMode, SessionMode::Plan);
    QVERIFY(!restored.persistSessions);

    QCOMPARE(restored.providers.size(), 1);
    QCOMPARE(restored.providers.first().id, QStringLiteral("p1"));
    QCOMPARE(restored.providers.first().kind, ProviderKind::Anthropic);
    QCOMPARE(restored.providers.first().models.size(), 2);

    // 模型能力覆盖必须完整往返——这正是"上下文窗口设置不生效"会出问题的地方。
    const ModelOptionOverride restoredOverride =
        restored.modelOverride(QStringLiteral("p1"), QStringLiteral("m1"));
    QCOMPARE(restoredOverride.contextWindow, 200000);
    QCOMPARE(restoredOverride.maxOutputTokens, 32768);
    QCOMPARE(restoredOverride.reasoningLevels.size(), 2);
    QCOMPARE(restoredOverride.defaultReasoningLevel, QStringLiteral("high"));

    QCOMPARE(restored.reasoningLevelFor(QStringLiteral("p1"), QStringLiteral("m2")),
             QStringLiteral("low"));

    // 工作区最近模型要把思考档位一起带回来。
    const ModelSelection workspaceModel = restored.modelForWorkspace(QStringLiteral("ws-1"));
    QCOMPARE(workspaceModel.providerId, QStringLiteral("p1"));
    QCOMPARE(workspaceModel.reasoningLevel, QStringLiteral("high"));
}

void TestAppConfig::emptyOverrideIsRemoved() {
    AppSettings settings;

    ModelOptionOverride override;
    override.contextWindow = 64000;
    settings.setModelOverride(QStringLiteral("p1"), QStringLiteral("m1"), override);
    QVERIFY(!settings.modelOverride(QStringLiteral("p1"), QStringLiteral("m1")).isEmpty());

    // 全空等价于"没有覆盖"：必须删除记录，否则配置文件会堆积一堆空对象。
    settings.setModelOverride(QStringLiteral("p1"), QStringLiteral("m1"),
                              ModelOptionOverride{});
    QVERIFY(settings.modelOverride(QStringLiteral("p1"), QStringLiteral("m1")).isEmpty());
    QVERIFY(!settings.toJson().value(QStringLiteral("modelOverrides")).toObject().contains(
        QStringLiteral("p1/m1")));

    // 键的格式必须与运行时查询一致，否则写入与读取会对不上。
    ModelOptionOverride other;
    other.maxOutputTokens = 4096;
    settings.setModelOverride(QStringLiteral("p1"), QStringLiteral("m1"), other);
    QVERIFY(settings.toJson().value(QStringLiteral("modelOverrides")).toObject().contains(
        QStringLiteral("p1/m1")));
}

void TestAppConfig::reasoningLevelRemembersEmptyValue() {
    AppSettings settings;
    settings.rememberReasoningLevel(QStringLiteral("p1"), QStringLiteral("m1"),
                                    QStringLiteral("off"));

    // "关闭"是合法档位，不能被当成"没设置"而丢掉。
    QCOMPARE(settings.reasoningLevelFor(QStringLiteral("p1"), QStringLiteral("m1")),
             QStringLiteral("off"));

    const AppSettings restored = AppSettings::fromJson(settings.toJson());
    QCOMPARE(restored.reasoningLevelFor(QStringLiteral("p1"), QStringLiteral("m1")),
             QStringLiteral("off"));

    // 未记录的模型返回空，由调用方按模型默认值决定。
    QVERIFY(restored.reasoningLevelFor(QStringLiteral("p1"), QStringLiteral("nope")).isEmpty());
}

void TestAppConfig::saveAndLoadFromDisk() {
    AppSettings settings;
    settings.themeMode = ThemeMode::Light;
    settings.uiFontSize = 15;
    settings.lastWorkspace = QStringLiteral("/tmp/some-workspace");

    ModelOptionOverride override;
    override.contextWindow = 100000;
    override.defaultReasoningLevel = QStringLiteral("medium");
    settings.setModelOverride(QStringLiteral("p9"), QStringLiteral("m9"), override);
    settings.rememberReasoningLevel(QStringLiteral("p9"), QStringLiteral("m9"),
                                    QStringLiteral("medium"));

    QString error;
    QVERIFY2(AppConfig::save(settings, &error), qPrintable(error));
    QVERIFY(QFile::exists(AppConfig::configPath()));

    AppSettings loaded;
    QVERIFY2(AppConfig::load(&loaded, &error), qPrintable(error));
    QCOMPARE(loaded.themeMode, ThemeMode::Light);
    QCOMPARE(loaded.uiFontSize, 15);
    QCOMPARE(loaded.lastWorkspace, QStringLiteral("/tmp/some-workspace"));
    QCOMPARE(loaded.modelOverride(QStringLiteral("p9"), QStringLiteral("m9")).contextWindow,
             100000);
    QCOMPARE(loaded.reasoningLevelFor(QStringLiteral("p9"), QStringLiteral("m9")),
             QStringLiteral("medium"));

    // 文件不存在时 load 必须成功并给出默认值——首次启动是正常路径，不是错误。
    QVERIFY(QFile::remove(AppConfig::configPath()));
    AppSettings fresh;
    QVERIFY2(AppConfig::load(&fresh, &error), qPrintable(error));
    QCOMPARE(fresh.uiFontSize, 14);
    QVERIFY(fresh.providers.isEmpty());
}

QTEST_MAIN(TestAppConfig)
#include "test_app_config.moc"
