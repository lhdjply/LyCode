#include "core/Json.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLoggingCategory>

#include <cmath>

namespace zcode::json {
namespace {

Q_LOGGING_CATEGORY(jsonLog, "zcode.json")

/// 统一记录类型不符，便于定位外部数据漂移。
void reportTypeMismatch(const QString &key, const char *expected) {
    qCWarning(jsonLog) << "JSON 字段类型不符，已回退默认值; key=" << key << "expected=" << expected;
}

/// 判断一个 QJsonValue 是否可作为数字使用。
bool isNumeric(const QJsonValue &value) {
    return value.isDouble();
}

}  // namespace

QString str(const QJsonObject &obj, const QString &key, const QString &fallback) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return fallback;
    }
    if (!value.isString()) {
        reportTypeMismatch(key, "string");
        return fallback;
    }
    return value.toString();
}

int integer(const QJsonObject &obj, const QString &key, int fallback) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return fallback;
    }
    if (!isNumeric(value)) {
        reportTypeMismatch(key, "number");
        return fallback;
    }
    return static_cast<int>(value.toDouble());
}

qint64 integer64(const QJsonObject &obj, const QString &key, qint64 fallback) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return fallback;
    }
    if (!isNumeric(value)) {
        reportTypeMismatch(key, "number");
        return fallback;
    }
    return static_cast<qint64>(value.toDouble());
}

double number(const QJsonObject &obj, const QString &key, double fallback) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return fallback;
    }
    if (!isNumeric(value)) {
        reportTypeMismatch(key, "number");
        return fallback;
    }
    return value.toDouble();
}

bool boolean(const QJsonObject &obj, const QString &key, bool fallback) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return fallback;
    }
    if (!value.isBool()) {
        reportTypeMismatch(key, "bool");
        return fallback;
    }
    return value.toBool();
}

QJsonObject object(const QJsonObject &obj, const QString &key) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return {};
    }
    if (!value.isObject()) {
        reportTypeMismatch(key, "object");
        return {};
    }
    return value.toObject();
}

QJsonArray array(const QJsonObject &obj, const QString &key) {
    const QJsonValue value = obj.value(key);
    if (value.isUndefined() || value.isNull()) {
        return {};
    }
    if (!value.isArray()) {
        reportTypeMismatch(key, "array");
        return {};
    }
    return value.toArray();
}

QStringList stringList(const QJsonObject &obj, const QString &key) {
    QStringList result;
    const QJsonArray values = array(obj, key);
    result.reserve(values.size());
    for (const QJsonValue &value : values) {
        if (value.isString()) {
            result.append(value.toString());
        }
    }
    return result;
}

bool has(const QJsonObject &obj, const QString &key) {
    const QJsonValue value = obj.value(key);
    return !value.isUndefined() && !value.isNull();
}

QJsonObject parseObject(const QByteArray &bytes, QString *errorOut) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorOut != nullptr) {
            *errorOut = parseError.errorString();
        }
        qCWarning(jsonLog) << "JSON 解析失败:" << parseError.errorString()
                           << "offset=" << parseError.offset;
        return {};
    }
    if (!document.isObject()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("JSON 根节点不是对象");
        }
        qCWarning(jsonLog) << "JSON 根节点不是对象";
        return {};
    }
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    return document.object();
}

QByteArray toBytes(const QJsonObject &obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray toPrettyBytes(const QJsonObject &obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

QString truncate(const QString &value, int maxChars, const QString &marker) {
    if (maxChars <= 0 || value.size() <= maxChars) {
        return value;
    }
    // QString::size() 返回 qsizetype，必须用同一类型做 max，否则模板推导失败。
    const qsizetype keep = std::max<qsizetype>(0, maxChars - marker.size());
    return value.left(keep) + marker;
}

}  // namespace zcode::json
