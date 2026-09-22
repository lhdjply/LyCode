// LyCode — Skills 测试
//
// 覆盖发现、frontmatter 解析、覆盖优先级，以及"清单只放名字+描述、
// 正文按需加载"这条核心设计——正文如果混进提示词，几十个 skill 就会把
// 上下文塞满，而这正是 skills 机制要解决的问题。
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "skills/SkillLibrary.h"
#include "tools/Tool.h"

using namespace lycode;

namespace {

/// 写一个技能目录：<root>/<name>/SKILL.md
void writeSkill(const QString &root, const QString &name, const QString &content) {
    QDir().mkpath(root + QLatin1Char('/') + name);
    QFile file(root + QLatin1Char('/') + name + QStringLiteral("/SKILL.md"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(content.toUtf8());
    }
}

}  // namespace

class TestSkills : public QObject {
    Q_OBJECT

private slots:
    void discoversDirectoryAndFlatLayouts();
    void parsesFrontmatterAndFallsBackGracefully();
    void projectDirectoryOverridesUserDirectory();
    void promptListsNamesOnlyWhileBodyLoadsOnDemand();
    void skillToolReportsUnknownNameWithAvailableList();
};

void TestSkills::discoversDirectoryAndFlatLayouts() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeSkill(dir.path(), QStringLiteral("pdf"),
               QStringLiteral("---\nname: pdf\ndescription: 处理 PDF\n---\n正文 A"));
    // 扁平布局：<dir>/<name>.md
    QFile flat(dir.filePath(QStringLiteral("excel.md")));
    QVERIFY(flat.open(QIODevice::WriteOnly));
    flat.write("---\nname: excel\ndescription: 处理表格\n---\n正文 B");
    flat.close();
    // 无关文件不该被当成技能
    QFile noise(dir.filePath(QStringLiteral("readme.txt")));
    QVERIFY(noise.open(QIODevice::WriteOnly));
    noise.write("not a skill");
    noise.close();

    skills::Library library;
    library.rescan({dir.path()});
    QCOMPARE(library.skills().size(), 2);
    QVERIFY(library.find(QStringLiteral("pdf")) != nullptr);
    QVERIFY(library.find(QStringLiteral("excel")) != nullptr);
    QVERIFY2(library.find(QStringLiteral("readme")) == nullptr, "非 .md 文件不该被扫描");

    // 不存在的目录直接跳过，不报错——技能目录是可选配置。
    library.rescan({QStringLiteral("/nonexistent/skills/dir")});
    QVERIFY(library.isEmpty());
}

void TestSkills::parsesFrontmatterAndFallsBackGracefully() {
    // ① 正常 frontmatter
    const skills::Skill full = skills::Library::parse(
        QStringLiteral("---\nname: My Skill\ndescription: \"带引号的描述\"\n---\n\n正文内容\n更多"),
        QStringLiteral("fallback"), QStringLiteral("/x/SKILL.md"));
    QCOMPARE(full.id, QStringLiteral("my-skill"));  // id 归一化：小写、空格换连字符
    QCOMPARE(full.name, QStringLiteral("My Skill"));
    QCOMPARE(full.description, QStringLiteral("带引号的描述"));
    QVERIFY(full.body.startsWith(QStringLiteral("正文内容")));
    QVERIFY2(!full.body.contains(QStringLiteral("name:")), "frontmatter 不该进正文");

    // ② 没有 frontmatter：名字取回退值，描述取正文第一行非标题内容
    const skills::Skill bare = skills::Library::parse(
        QStringLiteral("# 标题\n\n这是第一段说明。\n\n剩下是正文"),
        QStringLiteral("fallback-dir"), QStringLiteral("/x/SKILL.md"));
    QCOMPARE(bare.id, QStringLiteral("fallback-dir"));
    QCOMPARE(bare.description, QStringLiteral("这是第一段说明。"));
    QVERIFY(bare.body.contains(QStringLiteral("剩下是正文")));

    // ③ `---` 出现在正文中间时**不该**被当成 frontmatter：
    //    否则一段以分隔线开头的正文会被静默吃掉。
    const skills::Skill mid = skills::Library::parse(
        QStringLiteral("正文开头\n\n---\nname: 不是元数据\n---\n结尾"),
        QStringLiteral("mid"), QStringLiteral("/x/SKILL.md"));
    QVERIFY2(mid.body.contains(QStringLiteral("不是元数据")),
             "正文中间的分隔线不该被当成 frontmatter");

    // ④ 空内容判为无效
    QVERIFY(!skills::Library::parse(QStringLiteral("   \n  "), QStringLiteral("empty"),
                                    QStringLiteral("/x"))
                 .isValid());
}

void TestSkills::projectDirectoryOverridesUserDirectory() {
    QTemporaryDir userDir;
    QTemporaryDir projectDir;
    QVERIFY(userDir.isValid() && projectDir.isValid());

    writeSkill(userDir.path(), QStringLiteral("deploy"),
               QStringLiteral("---\nname: deploy\ndescription: 用户级\n---\n用户级正文"));
    writeSkill(projectDir.path(), QStringLiteral("deploy"),
               QStringLiteral("---\nname: deploy\ndescription: 项目级\n---\n项目级正文"));

    skills::Library library;
    // 用户级在前、项目级在后（与 defaultDirectories 的顺序一致）
    library.rescan({userDir.path(), projectDir.path()});

    QCOMPARE(library.skills().size(), 1);
    const skills::Skill *skill = library.find(QStringLiteral("deploy"));
    QVERIFY(skill != nullptr);
    QVERIFY2(skill->description == QStringLiteral("项目级"),
             "项目级应当覆盖同名的用户级（更具体者胜出）");
    QVERIFY(skill->body.contains(QStringLiteral("项目级正文")));
}

void TestSkills::promptListsNamesOnlyWhileBodyLoadsOnDemand() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeSkill(dir.path(), QStringLiteral("pdf"),
               QStringLiteral("---\nname: pdf\ndescription: 处理 PDF\n---\n"
                              "这里是**很长的**正文，包含具体步骤与命令。"));
    skills::Library library;
    library.rescan({dir.path()});

    const QString section = library.promptSection();
    // 清单里要有名字与描述，模型才知道有这个技能。
    QVERIFY(section.contains(QStringLiteral("pdf")));
    QVERIFY(section.contains(QStringLiteral("处理 PDF")));
    // ★ 正文**不能**出现在清单里：几十个 skill 的正文会把上下文塞满，
    //   而这正是 skills 机制要解决的问题。
    QVERIFY2(!section.contains(QStringLiteral("具体步骤与命令")),
             "技能正文不该进系统提示词，只能按需加载");
}

void TestSkills::skillToolReportsUnknownNameWithAvailableList() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeSkill(dir.path(), QStringLiteral("pdf"),
               QStringLiteral("---\nname: pdf\ndescription: 处理 PDF\n---\nPDF 正文"));

    skills::Library library;
    library.rescan({dir.path()});

    ToolRegistry registry = ToolRegistry::createWithBuiltins();
    Tool *tool = registry.find(QStringLiteral("Skill"));
    QVERIFY2(tool != nullptr, "Skill 工具应当注册在内置工具里");
    QVERIFY2(tool->metadata().readOnly, "读本地指令文件不该需要确认");

    ToolContext context;
    context.sessionId = QStringLiteral("session_test");
    context.skills = &library;

    // ① 命中：返回正文
    ToolResult hit;
    tool->execute(QJsonObject{{QStringLiteral("name"), QStringLiteral("pdf")}}, context,
                  [&](ToolResult result) { hit = result; });
    QVERIFY(hit.ok);
    QCOMPARE(hit.output, QStringLiteral("PDF 正文"));
    QCOMPARE(hit.metadata.value(QStringLiteral("skillId")).toString(), QStringLiteral("pdf"));

    // ② 未命中：必须列出可用名字，让模型一次就能纠正
    ToolResult miss;
    tool->execute(QJsonObject{{QStringLiteral("name"), QStringLiteral("nope")}}, context,
                  [&](ToolResult result) { miss = result; });
    QVERIFY(!miss.ok);
    QCOMPARE(miss.errorCode, QStringLiteral("skill_not_found"));
    QVERIFY2(miss.error.contains(QStringLiteral("pdf")), qPrintable(miss.error));

    // ③ 没有注入库时明确失败，而不是假装成功
    ToolContext bare;
    bare.sessionId = QStringLiteral("s");
    ToolResult unavailable;
    tool->execute(QJsonObject{{QStringLiteral("name"), QStringLiteral("pdf")}}, bare,
                  [&](ToolResult result) { unavailable = result; });
    QVERIFY(!unavailable.ok);
    QCOMPARE(unavailable.errorCode, QStringLiteral("skills_unavailable"));
}

QTEST_MAIN(TestSkills)

#include "test_skills.moc"
