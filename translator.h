#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <QString>
#include <QSettings>
#include <QMap>
#include <QFile>
#include <QDir>

class Translator
{
public:
    static Translator& instance();

    bool loadLanguage(const QString &langCode);
    QString tr(const QString &key) const;
    QString currentLanguage() const { return m_currentLang; }

private:
    Translator();
    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    QString m_currentLang;
    QSettings *m_translations;
};

// Удобный макрос для переводов
#define TR(key) Translator::instance().tr(key)

#endif // TRANSLATOR_H
