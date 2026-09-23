// LyCode — 应用配置单元测试
//
// 配置是"用户设置能否生效"的唯一通路，而且它是整份替换语义的小文档，
// 一旦某个字段读写不一致，表现是"设置改了没反应"这类很难定位的问题。
// 这里同时覆盖内存往返与真实落盘往返。
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include <QTableWidget>

#include "ui/AppConfig.h"
#include "ui/SettingsDialog.h"

using namespace lycode;
using namespace lycode::ui;

class TestAppConfig : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();

    void jsonRoundTripKeepsEveryField();
    void emptyOverrideIsRemoved();
    void reasoningLevelRemembersEmptyValue();
    void mcpAndSkillConfigRoundTrip();
    void integrationsPageReflectsAndEditsConfig();
    void saveAndLoadFromDisk();

  private:
    std::unique_ptr<QTemporaryDir> dataDir_;
};

void TestAppConfig::init()
{
  dataDir_ = std::make_unique<QTemporaryDir>();
  QVERIFY(dataDir_->isValid());
  // 必须在任何 AppConfig 调用之前设置：配置路径按调用时读环境变量。
  qputenv("LYCODE_DATA_BASE_DIR", dataDir_->path().toUtf8());
}

void TestAppConfig::cleanup()
{
  qunsetenv("LYCODE_DATA_BASE_DIR");
  dataDir_.reset();
}

void TestAppConfig::jsonRoundTripKeepsEveryField()
{
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

void TestAppConfig::emptyOverrideIsRemoved()
{
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

void TestAppConfig::reasoningLevelRemembersEmptyValue()
{
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

void TestAppConfig::saveAndLoadFromDisk()
{
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

void TestAppConfig::mcpAndSkillConfigRoundTrip()
{
  AppSettings settings;
  settings.mcpServers.append(mcp::ServerConfig{
    QStringLiteral("github"), QStringLiteral("npx"),
    {QStringLiteral("-y"), QStringLiteral("server-github")},
    {QStringLiteral("TOKEN=abc"), QStringLiteral("TRACE=1")}, true});
  settings.mcpServers.append(mcp::ServerConfig{
    QStringLiteral("off"), QStringLiteral("python3"), {QStringLiteral("s.py")}, {}, false});
  settings.skillDirectories = {QStringLiteral("/tmp/skills-a"), QStringLiteral("/tmp/skills-b")};
  QVERIFY(AppConfig::save(settings));

  AppSettings loaded;
  QVERIFY(AppConfig::load(&loaded));

  QCOMPARE(loaded.mcpServers.size(), 2);
  QCOMPARE(loaded.mcpServers.at(0).id, QStringLiteral("github"));
  QCOMPARE(loaded.mcpServers.at(0).command, QStringLiteral("npx"));
  QCOMPARE(loaded.mcpServers.at(0).args.size(), 2);
  QCOMPARE(loaded.mcpServers.at(0).args.at(1), QStringLiteral("server-github"));
  QCOMPARE(loaded.mcpServers.at(0).env.size(), 2);
  QVERIFY(loaded.mcpServers.at(0).enabled);
  QVERIFY2(!loaded.mcpServers.at(1).enabled, "enabled=false 必须被保留");
  QCOMPARE(loaded.skillDirectories, settings.skillDirectories);

  // 配置不完整的条目在读入时被丢弃，而不是留一个永远起不来的服务器。
  const QJsonObject broken = QJsonDocument::fromJson(
                               R"({"mcpServers":[{"id":"no-command"},{"command":"npx"}]})").object();
  AppSettings parsed = AppSettings::fromJson(broken);
  QVERIFY2(parsed.mcpServers.isEmpty(), "缺 id 或 command 的条目不该进配置");

  // 空目录列表的语义是"用默认目录"，必须能原样往返（不能被写成 null 丢失）。
  AppSettings blank;
  QVERIFY(AppConfig::save(blank));
  AppSettings reloaded;
  QVERIFY(AppConfig::load(&reloaded));
  QVERIFY2(reloaded.skillDirectories.isEmpty(),
           "空列表必须保持为空，它表示“用内置默认”而不是“没有配置”");
}

void TestAppConfig::integrationsPageReflectsAndEditsConfig()
{
  AppSettings settings;
  settings.mcpServers.append(mcp::ServerConfig{
    QStringLiteral("github"), QStringLiteral("npx"), {QStringLiteral("-y")}, {}, true});

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  // 造一个真技能，验证设置页的"发现预览"。
  QDir().mkpath(dir.filePath(QStringLiteral("pdf")));
  QFile skill(dir.filePath(QStringLiteral("pdf/SKILL.md")));
  QVERIFY(skill.open(QIODevice::WriteOnly));
  skill.write("---\nname: pdf\ndescription: 处理 PDF\n---\n正文");
  skill.close();
  settings.skillDirectories = {dir.path()};

  SettingsDialog dialog(settings);
  auto * table = dialog.findChild<QTableWidget *>(QStringLiteral("mcpTable"));
  QVERIFY2(table != nullptr, "设置页必须有 MCP 服务器列表");
  QCOMPARE(table->rowCount(), 1);
  QCOMPARE(table->item(0, 0)->text(), QStringLiteral("github"));
  QCOMPARE(table->item(0, 1)->text(), QStringLiteral("npx"));
  QCOMPARE(table->item(0, 3)->text(), QStringLiteral("是"));

  auto * dirList = dialog.findChild<QListWidget *>(QStringLiteral("skillDirList"));
  QVERIFY2(dirList != nullptr, "设置页必须有技能目录列表");
  QCOMPARE(dirList->count(), 1);
  QCOMPARE(dirList->item(0)->text(), dir.path());

  // 预览必须当场告诉用户"这个目录里认到了几个技能"，
  // 而不是等重启后发现一个都没有。
  auto * preview = dialog.findChild<QLabel *>(QStringLiteral("skillPreview"));
  QVERIFY(preview != nullptr);
  QVERIFY2(preview->text().contains(QStringLiteral("pdf")), qPrintable(preview->text()));
  QVERIFY(preview->text().contains(QStringLiteral("1 个技能")));

  // 删除 MCP 服务器后，对话框返回的设置里也要没有它。
  table->setCurrentCell(0, 0);
  auto * removeServer = dialog.findChild<QPushButton *>(QStringLiteral("removeMcpServer"));
  QVERIFY(removeServer != nullptr);
  removeServer->click();
  QCOMPARE(table->rowCount(), 0);
  QVERIFY2(dialog.settings().mcpServers.isEmpty(), "删除必须落到返回的设置里");

  // 移除技能目录同样要落进设置（而不是只改了界面）。
  dirList->setCurrentRow(0);
  auto * removeDir = dialog.findChild<QPushButton *>(QStringLiteral("removeSkillDir"));
  QVERIFY(removeDir != nullptr);
  removeDir->click();
  QVERIFY2(dialog.settings().skillDirectories.isEmpty(),
           "移除目录必须落到返回的设置里");
}

QTEST_MAIN(TestAppConfig)
#include "test_app_config.moc"
