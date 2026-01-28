#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QTimer>
#include <QCompleter>
#include <QStringListModel>

namespace Ui {
class MainWindow;
}

struct WeatherData {
    QString city;
    QString country;
    double temp;
    double feelsLike;
    int humidity;
    double windSpeed;
    QString description;
    QString icon;
    QDateTime dateTime;
    int weatherCode;
};

struct ForecastData {
    QDateTime dateTime;
    double temp;
    double tempMin;
    double tempMax;
    QString description;
    QString icon;
    int weatherCode;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void searchCity();
    void onSearchFinished(QNetworkReply *reply);
    void onSuggestionsFinished(QNetworkReply *reply);
    void onWeatherFinished(QNetworkReply *reply);
    void onForecastFinished(QNetworkReply *reply);
    void addToFavorites();
    void removeFromFavorites();
    void loadFavoriteCity(const QString &city);
    void toggleLanguage();
    void toggleUnits();
    void refreshCurrentCity();
    void updateSearchSuggestions(const QString &text);
    void performSearchSuggestions(const QString &text);
    void onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);

private:
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void fetchWeather(const QString &city);
    void fetchForecast(const QString &city);
    void displayWeather(const WeatherData &data);
    void displayForecast(const QList<ForecastData> &forecast);
    void applyTheme();
    void updateLanguage();
    void updateFavoritesList();
    QString getWeatherIconUrl(const QString &icon);
    double convertTemp(double temp);
    double convertSpeed(double speed);
    QString getTempUnit();
    QString getSpeedUnit();
    QNetworkRequest createRequest(const QUrl &url);
    QString getWeatherDescription(int code);
    QString getWeatherIcon(const QString &description);
    QString getCurrentLanguageCode() const;

    Ui::MainWindow *ui;
    QNetworkAccessManager *m_networkManager;
    QSettings *m_settings;
    QTimer *m_refreshTimer;
    QTimer *m_searchDebounceTimer;

    // Данные
    QString m_currentCity;
    QStringList m_favoriteCities;
    QString m_currentLanguage;  // Теперь храним код языка вместо bool
    bool m_isCelsius;
    QMap<QString, QPixmap> m_iconCache;
    QCompleter *m_completer;
    QStringListModel *m_completerModel;
    QSet<QNetworkReply*> m_searchReplies;

    // Сохраненные данные погоды для перерисовки
    WeatherData m_currentWeatherData;
    QList<ForecastData> m_currentForecastData;
    bool m_hasWeatherData;

    // Константы
    const QString WEATHER_API_URL = "http://api.open-meteo.com/v1/forecast";
    const QString GEOCODING_API_URL = "http://geocoding-api.open-meteo.com/v1/search";
};

#endif // MAINWINDOW_H
