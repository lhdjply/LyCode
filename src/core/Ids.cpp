#include "core/Ids.h"

#include <QRandomGenerator>

namespace zcode {
namespace {

/// RFC 4648 base32 小写字母表，去掉易混淆的填充符，共 32 个字符 = 5 bit/字符。
constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz234567";

/// 24 个字符 × 5 bit = 120 bit 熵，足以抵抗碰撞与猜测。
constexpr int kIdLength = 24;

}  // namespace

QString newId(const QString &prefix) {
    QString suffix;
    suffix.reserve(kIdLength);
    for (int index = 0; index < kIdLength; ++index) {
        // bounded() 是拒绝采样实现，无取模偏置。
        const quint32 value = QRandomGenerator::system()->bounded(32u);
        suffix.append(QLatin1Char(kAlphabet[value]));
    }
    return prefix + QLatin1Char('_') + suffix;
}

}  // namespace zcode
