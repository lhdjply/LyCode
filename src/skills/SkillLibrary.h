// ZCode Qt — Skills 库
//
// Skill 是一份**可复用的指令包**：一个目录里放 SKILL.md，带 name/description
// 与正文。系统提示词里只列出"名字 + 一句话描述"，模型按需用 Skill 工具把
// 正文取出来——这样几十个 skill 也不会把上下文塞满。
//
// 发现规则：
//   * `<dir>/<name>/SKILL.md`（大小写不敏感，也接受 skill.md）
//   * `<dir>/<name>.md`（扁平文件）
//   * 目录不存在就跳过，不报错——技能目录是可选配置。
//
// 正文里的 frontmatter 是**极简 YAML**，只认 `key: value` 的单行形式。
// 不引入 YAML 解析器：skill 的元数据就是这么简单，为它拉一个依赖不值得，
// 而且一旦支持嵌套结构，用户就会开始写解析器不完整支持的东西。
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace zcode::skills {

/// 一个已发现的 skill。
struct Skill {
    /// 唯一标识（用于工具入参）。取 frontmatter 的 name，缺失时取目录/文件名。
    QString id;
    /// 展示名。
    QString name;
    QString description;
    /// 完整正文（不含 frontmatter）。
    QString body;
    QString path;

    bool isValid() const { return !id.isEmpty() && !body.trimmed().isEmpty(); }
};

class Library {
public:
    /// 默认搜索目录：用户级 + 项目级。
    /// 用户级 `<数据根>/qt/skills`；项目级 `<workspace>/.zcode/skills`。
    static QStringList defaultDirectories(const QString &workspacePath);

    /// 重新扫描。`directories` 里不存在的会被跳过。
    /// 后扫到的同名 skill 不覆盖先扫到的（先扫的是用户级，优先级更高更符合直觉？
    /// 其实相反——这里让**项目级覆盖用户级**，因为项目级更具体）。
    void rescan(const QStringList &directories);
    void clear() { skills_.clear(); }

    const QList<Skill> &skills() const { return skills_; }
    bool isEmpty() const { return skills_.isEmpty(); }
    /// 按 id 查找；找不到返回 nullptr。
    const Skill *find(const QString &id) const;

    /// 系统提示词里的清单片段。没有 skill 时返回空串。
    QString promptSection() const;

    /// 解析一份 SKILL.md 的内容。暴露出来便于单测。
    static Skill parse(const QString &content, const QString &fallbackName,
                       const QString &path);

private:
    QList<Skill> skills_;
};

}  // namespace zcode::skills
