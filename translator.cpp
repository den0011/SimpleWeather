#include "translator.h"
#include <QCoreApplication>
#include <QDebug>

Translator& Translator::instance()
{
    static Translator inst;
    return inst;
}

Translator::Translator()
    : m_currentLang("ru")
    , m_translations(nullptr)
{
    // НЕ загружаем язык в конструкторе - это будет сделано из MainWindow
    qDebug() << "Translator instance created";
}

bool Translator::loadLanguage(const QString &langCode)
{
    qDebug() << "========================================";
    qDebug() << "Translator::loadLanguage() called with:" << langCode;

    QString langPath = QCoreApplication::applicationDirPath() + "/lang/" + langCode + ".ini";

    qDebug() << "Full path to language file:" << langPath;
    qDebug() << "Application directory:" << QCoreApplication::applicationDirPath();
    qDebug() << "Current working directory:" << QDir::currentPath();

    // Проверяем существование файла
    QFile file(langPath);
    if (!file.exists()) {
        qCritical() << "ERROR: Language file does NOT exist:" << langPath;

        // Попробуем найти где же файлы
        QDir appDir(QCoreApplication::applicationDirPath());
        qDebug() << "Contents of application directory:";
        QFileInfoList entries = appDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : entries) {
            qDebug() << "  " << (entry.isDir() ? "[DIR]" : "[FILE]") << entry.fileName();
        }

        // Проверяем есть ли папка lang
        if (appDir.exists("lang")) {
            qDebug() << "Found 'lang' directory! Contents:";
            QDir langDir(appDir.filePath("lang"));
            QFileInfoList langEntries = langDir.entryInfoList(QDir::Files);
            for (const QFileInfo &entry : langEntries) {
                qDebug() << "    [FILE]" << entry.fileName();
            }
        } else {
            qCritical() << "ERROR: 'lang' directory NOT FOUND!";
        }

        return false;
    }

    qDebug() << "File exists, attempting to load...";

    if (m_translations) {
        qDebug() << "Deleting previous translations object";
        delete m_translations;
        m_translations = nullptr;
    }

    m_translations = new QSettings(langPath, QSettings::IniFormat);

    // Устанавливаем кодировку UTF-8 для правильного чтения файлов
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    m_translations->setIniCodec("UTF-8");
    qDebug() << "Set INI codec to UTF-8";
#endif

    if (m_translations->status() != QSettings::NoError) {
        qCritical() << "ERROR: Failed to load language file, QSettings status:" << m_translations->status();
        delete m_translations;
        m_translations = nullptr;
        return false;
    }

    // Проверяем, что файл действительно загружен
    QStringList allKeys = m_translations->allKeys();
    qDebug() << "QSettings loaded successfully";
    qDebug() << "Total keys in file:" << allKeys.size();

    if (allKeys.isEmpty()) {
        qCritical() << "ERROR: Language file is empty or has invalid format!";
        qCritical() << "Make sure file is in INI format with [Section] headers";
        delete m_translations;
        m_translations = nullptr;
        return false;
    }

    m_currentLang = langCode;
    qDebug() << "SUCCESS: Language loaded:" << langCode;

    // Выводим ВСЕ ключи для понимания формата
    qDebug() << "ALL KEYS loaded:";
    for (int i = 0; i < allKeys.size(); ++i) {
        QString key = allKeys[i];
        QString value = m_translations->value(key).toString();
        qDebug() << QString("  [%1] \"%2\" = \"%3\"").arg(i).arg(key).arg(value.left(30));
    }

    // Также проверим как читаются ключи через группы
    qDebug() << "\nTesting group-based access:";
    m_translations->beginGroup("General");
    qDebug() << "  General/app_title via group:" << m_translations->value("app_title").toString();
    m_translations->endGroup();

    qDebug() << "\nTesting direct access:";
    qDebug() << "  'General/app_title' directly:" << m_translations->value("General/app_title").toString();

    qDebug() << "========================================";

    return true;
}

QString Translator::tr(const QString &key) const
{
    if (!m_translations) {
        qWarning() << "Translations not loaded! Returning key:" << key;
        return "[NO LANG] " + key;
    }

    // Сначала пробуем читать напрямую (работает для большинства ключей с префиксом)
    QString value = m_translations->value(key).toString();

    // Если не нашли и ключ содержит "/", пробуем только имя ключа без группы
    if (value.isEmpty() && key.contains("/")) {
        QStringList parts = key.split("/");
        if (parts.size() == 2) {
            // Пробуем найти без префикса группы (для app_title, select_city и т.д.)
            value = m_translations->value(parts[1]).toString();
        }
    }

    // Если всё ещё пусто, пробуем через beginGroup/endGroup
    if (value.isEmpty() && key.contains("/")) {
        QStringList parts = key.split("/");
        if (parts.size() == 2) {
            m_translations->beginGroup(parts[0]);
            value = m_translations->value(parts[1]).toString();
            m_translations->endGroup();
        }
    }

    if (value.isEmpty()) {
        qWarning() << "Translation key not found:" << key;
        return key; // Возвращаем ключ как есть
    }

    return value;
}
