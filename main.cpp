#include "mainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    a.setApplicationName("SimpleWeather");
    a.setOrganizationName("WeatherApp");

    // Проверяем наличие папки lang ДО запуска главного окна
    QString appDir = a.applicationDirPath();
    QString langDir = appDir + "/lang";

    qDebug() << "===========================================";
    qDebug() << "Application starting...";
    qDebug() << "Executable directory:" << appDir;
    qDebug() << "Language folder path:" << langDir;
    qDebug() << "Language folder exists?" << QDir(langDir).exists();

    if (!QDir(langDir).exists()) {
        qCritical() << "CRITICAL ERROR: 'lang' folder not found!";
        qCritical() << "Please create folder 'lang' next to the executable";
        qCritical() << "Expected location:" << langDir;

        QMessageBox::critical(nullptr, "Error",
            QString("Language files not found!\n\n"
                   "Please create 'lang' folder with ru.ini and en.ini\n"
                   "in the same directory as the executable:\n\n%1").arg(appDir));
    } else {
        qDebug() << "Language folder found!";

        // Проверяем наличие файлов
        QDir dir(langDir);
        QStringList langFiles = dir.entryList(QStringList() << "*.ini", QDir::Files);
        qDebug() << "Language files found:" << langFiles;

        if (!langFiles.contains("ru.ini")) {
            qWarning() << "WARNING: ru.ini not found in lang folder!";
        }
        if (!langFiles.contains("en.ini")) {
            qWarning() << "WARNING: en.ini not found in lang folder!";
        }
    }
    qDebug() << "===========================================";

    MainWindow w;
    w.show();

    return a.exec();
}
