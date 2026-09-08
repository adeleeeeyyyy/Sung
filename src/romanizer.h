#pragma once
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class Romanizer {
public:
  static bool containsNonLatin(const QString &text);
  static QString romanizeText(const QString &text);
  static QVariantList romanizeLines(const QVariantList &lines);
};
