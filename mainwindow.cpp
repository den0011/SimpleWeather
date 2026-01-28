#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "translator.h"
#include <QMessageBox>
#include <QUrlQuery>
#include <QPixmap>
#include <QDateTime>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_settings(new QSettings(this))
    , m_refreshTimer(new QTimer(this))
    , m_searchDebounceTimer(new QTimer(this))
    , m_currentLanguage("ru")
    , m_isCelsius(true)
    , m_completerModel(new QStringListModel(this))
    , m_hasWeatherData(false)
{
    ui->setupUi(this);

    qDebug() << "=== MainWindow initialization ===";

    // КРИТИЧЕСКИ ВАЖНО: загружаем язык ПЕРВЫМ делом, до любых UI операций
    m_currentLanguage = m_settings->value("language", "ru").toString();
    qDebug() << "Loading language:" << m_currentLanguage;

    if (!Translator::instance().loadLanguage(m_currentLanguage)) {
        qWarning() << "Failed to load language" << m_currentLanguage << ", trying 'ru'";
        m_currentLanguage = "ru";
        if (!Translator::instance().loadLanguage("ru")) {
            qCritical() << "CRITICAL: Failed to load fallback language 'ru'!";
            qCritical() << "Make sure 'lang' folder exists next to the executable!";
        }
    }

    // Теперь загружаем остальные настройки
    loadSettings();

    // Применяем тему и обновляем язык UI
    applyTheme();
    updateLanguage();
    setupConnections();

    // Настройка автодополнения
    m_completer = new QCompleter(m_completerModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    ui->m_searchInput->setCompleter(m_completer);

    // Настройка таймера для задержки автодополнения
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(500);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, [this]() {
        QString text = ui->m_searchInput->text().trimmed();
        if (text.length() >= 2) {
            performSearchSuggestions(text);
        }
    });

    // Игнорируем SSL ошибки
    connect(m_networkManager, &QNetworkAccessManager::sslErrors,
            this, &MainWindow::onSslErrors);

    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, [this](QNetworkReply *reply) {
        QString url = reply->url().toString();
        if (url.contains("geocoding-api")) {
            if (m_searchReplies.contains(reply)) {
                m_searchReplies.remove(reply);
                onSearchFinished(reply);
            } else if (url.contains("count=10")) {
                onSuggestionsFinished(reply);
            }
        } else if (url.contains("forecast") && url.contains("current")) {
            onWeatherFinished(reply);
        } else if (url.contains("forecast") && url.contains("daily")) {
            onForecastFinished(reply);
        }
    });

    m_refreshTimer->setInterval(600000); // 10 минут
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshCurrentCity);
    m_refreshTimer->start();

    // Автозагрузка последнего города
    if (!m_currentCity.isEmpty()) {
        qDebug() << "Loading last city:" << m_currentCity;
        QTimer::singleShot(100, this, [this]() {
            fetchWeather(m_currentCity);
            fetchForecast(m_currentCity);
        });
    }
}

MainWindow::~MainWindow()
{
    saveSettings();
    delete ui;
}

void MainWindow::setupConnections()
{
    connect(ui->m_searchButton, &QPushButton::clicked, this, &MainWindow::searchCity);
    connect(ui->m_searchInput, &QLineEdit::returnPressed, this, &MainWindow::searchCity);
    connect(ui->m_favoriteButton, &QPushButton::clicked, this, &MainWindow::addToFavorites);
    connect(ui->m_languageButton, &QPushButton::clicked, this, &MainWindow::toggleLanguage);
    connect(ui->m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshCurrentCity);
    connect(ui->m_unitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::toggleUnits);
    connect(ui->m_favoritesList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *item) {
        loadFavoriteCity(item->text());
    });
    connect(ui->removeFavButton, &QPushButton::clicked, this, &MainWindow::removeFromFavorites);
    connect(ui->m_searchInput, &QLineEdit::textChanged, this, &MainWindow::updateSearchSuggestions);
}

void MainWindow::searchCity()
{
    QString city = ui->m_searchInput->text().trimmed();
    if (city.isEmpty()) {
        QMessageBox::warning(this, TR("Search/error_title"), TR("Search/error_empty"));
        return;
    }

    QUrl url(GEOCODING_API_URL);
    QUrlQuery query;
    query.addQueryItem("name", city);
    query.addQueryItem("count", "1");
    query.addQueryItem("language", getCurrentLanguageCode());
    query.addQueryItem("format", "json");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply *reply = m_networkManager->get(request);

    m_searchReplies.insert(reply);
}

void MainWindow::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    Q_UNUSED(errors)
    reply->ignoreSslErrors();
}

QNetworkRequest MainWindow::createRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "SimpleWeather/1.0");

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(10000);
#endif

    return request;
}

void MainWindow::onSearchFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QMessageBox::warning(this, TR("Search/network_error"),
                           TR("Search/failed_to_find") + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject obj = doc.object();
    QJsonArray results = obj["results"].toArray();

    if (results.isEmpty()) {
        QMessageBox::warning(this, TR("Search/error_title"), TR("Search/city_not_found"));
        return;
    }

    QJsonObject city = results[0].toObject();
    QString cityName = city["name"].toString();
    QString country = city["country"].toString();
    m_currentCity = cityName + ", " + country;

    fetchWeather(m_currentCity);
    fetchForecast(m_currentCity);
}

void MainWindow::fetchWeather(const QString &city)
{
    qDebug() << "Fetching weather for:" << city;

    QStringList parts = city.split(", ");
    if (parts.isEmpty()) return;

    QUrl geoUrl(GEOCODING_API_URL);
    QUrlQuery geoQuery;
    geoQuery.addQueryItem("name", parts[0]);
    geoQuery.addQueryItem("count", "1");
    geoQuery.addQueryItem("language", getCurrentLanguageCode());
    geoQuery.addQueryItem("format", "json");
    geoUrl.setQuery(geoQuery);

    qDebug() << "Geocoding URL:" << geoUrl.toString();

    QNetworkRequest request = createRequest(geoUrl);
    QNetworkReply *geoReply = m_networkManager->get(request);

    connect(geoReply, &QNetworkReply::finished, this, [this, geoReply]() {
        geoReply->deleteLater();

        if (geoReply->error() != QNetworkReply::NoError) {
            qDebug() << "Geo error:" << geoReply->errorString();
            return;
        }

        QByteArray geoData = geoReply->readAll();
        qDebug() << "Geocoding response:" << geoData;

        QJsonDocument doc = QJsonDocument::fromJson(geoData);
        QJsonObject obj = doc.object();
        QJsonArray results = obj["results"].toArray();

        if (!results.isEmpty()) {
            QJsonObject location = results[0].toObject();
            double lat = location["latitude"].toDouble();
            double lon = location["longitude"].toDouble();

            qDebug() << "Got coordinates:" << lat << lon;

            QUrl url(WEATHER_API_URL);
            QUrlQuery query;
            query.addQueryItem("latitude", QString::number(lat));
            query.addQueryItem("longitude", QString::number(lon));
            query.addQueryItem("current", "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m");
            query.addQueryItem("timezone", "auto");
            url.setQuery(query);

            QNetworkRequest weatherRequest = createRequest(url);
            m_networkManager->get(weatherRequest);
        } else {
            qDebug() << "No geocoding results found";
        }
    });
}

void MainWindow::onWeatherFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Weather error:" << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    qDebug() << "Weather response received, size:" << responseData.size();

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    QJsonObject obj = doc.object();
    QJsonObject current = obj["current"].toObject();

    if (current.isEmpty()) {
        qDebug() << "Current weather data is empty!";
        return;
    }

    WeatherData data;
    data.city = m_currentCity;
    data.temp = current["temperature_2m"].toDouble();
    data.feelsLike = current["apparent_temperature"].toDouble();
    data.humidity = current["relative_humidity_2m"].toInt();
    data.windSpeed = current["wind_speed_10m"].toDouble();

    int weatherCode = current["weather_code"].toInt();
    data.weatherCode = weatherCode;
    data.description = getWeatherDescription(weatherCode);

    qDebug() << "Weather data:" << data.city << data.temp << data.description;

    m_currentWeatherData = data;
    m_hasWeatherData = true;

    displayWeather(data);
}

void MainWindow::fetchForecast(const QString &city)
{
    qDebug() << "Fetching forecast for:" << city;

    QStringList parts = city.split(", ");
    if (parts.isEmpty()) return;

    QUrl geoUrl(GEOCODING_API_URL);
    QUrlQuery geoQuery;
    geoQuery.addQueryItem("name", parts[0]);
    geoQuery.addQueryItem("count", "1");
    geoQuery.addQueryItem("language", getCurrentLanguageCode());
    geoQuery.addQueryItem("format", "json");
    geoUrl.setQuery(geoQuery);

    QNetworkRequest request = createRequest(geoUrl);
    QNetworkReply *geoReply = m_networkManager->get(request);

    connect(geoReply, &QNetworkReply::finished, this, [this, geoReply]() {
        geoReply->deleteLater();

        if (geoReply->error() != QNetworkReply::NoError) {
            qDebug() << "Forecast geo error:" << geoReply->errorString();
            return;
        }

        QByteArray geoData = geoReply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(geoData);
        QJsonObject obj = doc.object();
        QJsonArray results = obj["results"].toArray();

        if (!results.isEmpty()) {
            QJsonObject location = results[0].toObject();
            double lat = location["latitude"].toDouble();
            double lon = location["longitude"].toDouble();

            qDebug() << "Got forecast coordinates:" << lat << lon;

            QUrl url(WEATHER_API_URL);
            QUrlQuery query;
            query.addQueryItem("latitude", QString::number(lat));
            query.addQueryItem("longitude", QString::number(lon));
            query.addQueryItem("daily", "temperature_2m_max,temperature_2m_min,weather_code");
            query.addQueryItem("timezone", "auto");
            query.addQueryItem("forecast_days", "5");
            url.setQuery(query);

            QNetworkRequest forecastRequest = createRequest(url);
            m_networkManager->get(forecastRequest);
        } else {
            qDebug() << "No forecast geocoding results found";
        }
    });
}

void MainWindow::onForecastFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Forecast error:" << reply->errorString();
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject obj = doc.object();
    QJsonObject daily = obj["daily"].toObject();

    QJsonArray times = daily["time"].toArray();
    QJsonArray tempMax = daily["temperature_2m_max"].toArray();
    QJsonArray tempMin = daily["temperature_2m_min"].toArray();
    QJsonArray weatherCodes = daily["weather_code"].toArray();

    QList<ForecastData> forecast;
    for (int i = 0; i < times.size(); ++i) {
        ForecastData fd;
        fd.dateTime = QDateTime::fromString(times[i].toString(), Qt::ISODate);
        fd.tempMax = tempMax[i].toDouble();
        fd.tempMin = tempMin[i].toDouble();

        int code = weatherCodes[i].toInt();
        fd.weatherCode = code;
        fd.description = getWeatherDescription(code);

        forecast.append(fd);
    }

    m_currentForecastData = forecast;
    displayForecast(forecast);
}

void MainWindow::displayWeather(const WeatherData &data)
{
    ui->m_cityLabel->setText(data.city);
    ui->m_tempLabel->setText(QString::number(convertTemp(data.temp), 'f', 1) + getTempUnit());
    ui->m_descLabel->setText(data.description);

    ui->m_feelsLikeLabel->setText(TR("Weather/feels_like") +
                                  QString::number(convertTemp(data.feelsLike), 'f', 1) + getTempUnit());

    ui->m_humidityLabel->setText("💧 " + TR("Weather/humidity") + QString::number(data.humidity) + "%");

    ui->m_windLabel->setText("💨 " + TR("Weather/wind") +
                            QString::number(convertSpeed(data.windSpeed), 'f', 1) + " " + getSpeedUnit());

    QString icon = getWeatherIcon(data.description);
    ui->m_iconLabel->setText(icon);
}

void MainWindow::displayForecast(const QList<ForecastData> &forecast)
{
    QVBoxLayout *layout = qobject_cast<QVBoxLayout*>(ui->m_forecastFrame->layout());

    // Удаляем старые виджеты (кроме заголовка)
    while (layout->count() > 1) {
        QLayoutItem *item = layout->takeAt(1);
        delete item->widget();
        delete item;
    }

    for (const ForecastData &fd : forecast) {
        QFrame *dayFrame = new QFrame();
        dayFrame->setFrameStyle(QFrame::Box);
        QHBoxLayout *dayLayout = new QHBoxLayout(dayFrame);

        QString dayName = fd.dateTime.toString("ddd, d MMM");
        QLabel *dateLabel = new QLabel(dayName);
        dateLabel->setMinimumWidth(120);
        QFont dateFont = dateLabel->font();
        dateFont.setPointSize(12);
        dateLabel->setFont(dateFont);

        QString icon = getWeatherIcon(fd.description);

        QLabel *iconLabel = new QLabel(icon);
        QFont iconFont = iconLabel->font();
        iconFont.setPointSize(24);
        iconLabel->setFont(iconFont);

        QLabel *descLabel = new QLabel(fd.description);
        descLabel->setMinimumWidth(90);
        QFont descFontForecast = descLabel->font();
        descFontForecast.setPointSize(12);
        descLabel->setFont(descFontForecast);

        QString tempText = QString::number(convertTemp(fd.tempMax), 'f', 0) + getTempUnit() +
                          " / " + QString::number(convertTemp(fd.tempMin), 'f', 0) + getTempUnit();
        QLabel *tempLabel = new QLabel(tempText);
        QFont tempFontForecast = tempLabel->font();
        tempFontForecast.setPointSize(13);
        tempFontForecast.setBold(true);
        tempLabel->setFont(tempFontForecast);

        dayLayout->addWidget(dateLabel);
        dayLayout->addWidget(iconLabel);
        dayLayout->addWidget(descLabel);
        dayLayout->addStretch();
        dayLayout->addWidget(tempLabel);

        layout->addWidget(dayFrame);
    }

    layout->addStretch();
}

void MainWindow::addToFavorites()
{
    if (m_currentCity.isEmpty()) {
        QMessageBox::warning(this, TR("Favorites/info_title"), TR("Favorites/select_first"));
        return;
    }

    if (m_favoriteCities.contains(m_currentCity)) {
        QMessageBox::information(this, TR("Favorites/info_title"), TR("Favorites/already_added"));
        return;
    }

    m_favoriteCities.append(m_currentCity);
    updateFavoritesList();
    saveSettings();
}

void MainWindow::removeFromFavorites()
{
    QListWidgetItem *item = ui->m_favoritesList->currentItem();
    if (!item) return;

    m_favoriteCities.removeAll(item->text());
    updateFavoritesList();
    saveSettings();
}

void MainWindow::loadFavoriteCity(const QString &city)
{
    m_currentCity = city;
    fetchWeather(city);
    fetchForecast(city);
}

void MainWindow::toggleLanguage()
{
    // Переключаем между ru и en
    m_currentLanguage = (m_currentLanguage == "ru") ? "en" : "ru";

    // Загружаем новый язык
    Translator::instance().loadLanguage(m_currentLanguage);

    updateLanguage();

    // Перерисовываем данные с новым языком если они есть
    if (m_hasWeatherData) {
        // Обновляем описания погоды на основе сохраненных кодов
        m_currentWeatherData.description = getWeatherDescription(m_currentWeatherData.weatherCode);
        displayWeather(m_currentWeatherData);

        // Обновляем описания прогноза
        for (int i = 0; i < m_currentForecastData.size(); ++i) {
            m_currentForecastData[i].description = getWeatherDescription(m_currentForecastData[i].weatherCode);
        }
        displayForecast(m_currentForecastData);
    }

    saveSettings();
}

void MainWindow::updateLanguage()
{
    // Обновляем кнопку языка
    ui->m_languageButton->setText(m_currentLanguage.toUpper());

    // Обновляем все текстовые элементы интерфейса
    setWindowTitle(TR("General/app_title"));
    ui->m_searchInput->setPlaceholderText(TR("Search/placeholder"));
    ui->m_searchButton->setText(TR("Search/button"));
    ui->m_favoriteButton->setToolTip(TR("Favorites/add_tooltip"));
    ui->m_refreshButton->setToolTip(TR("Controls/refresh_tooltip"));
    ui->m_languageButton->setToolTip(TR("Controls/language_tooltip"));
    ui->m_unitsCombo->setItemText(0, "°C, " + TR("Weather/speed_ms"));
    ui->m_unitsCombo->setItemText(1, "°F, " + TR("Weather/speed_mph"));
    ui->forecastTitle->setText("📅 " + TR("Forecast/title"));
    ui->favoritesTitle->setText("⭐ " + TR("Favorites/title"));
    ui->removeFavButton->setText(TR("Favorites/remove_button"));

    if (m_currentCity.isEmpty()) {
        ui->m_cityLabel->setText(TR("General/select_city"));
    }
}

void MainWindow::toggleUnits()
{
    m_isCelsius = (ui->m_unitsCombo->currentIndex() == 0);

    if (!m_currentCity.isEmpty()) {
        refreshCurrentCity();
    }

    saveSettings();
}

void MainWindow::refreshCurrentCity()
{
    if (!m_currentCity.isEmpty()) {
        fetchWeather(m_currentCity);
        fetchForecast(m_currentCity);
    }
}

void MainWindow::updateSearchSuggestions(const QString &text)
{
    m_searchDebounceTimer->stop();

    if (text.length() >= 2) {
        m_searchDebounceTimer->start();
    }
}

void MainWindow::performSearchSuggestions(const QString &text)
{
    QUrl url(GEOCODING_API_URL);
    QUrlQuery query;
    query.addQueryItem("name", text);
    query.addQueryItem("count", "10");
    query.addQueryItem("language", getCurrentLanguageCode());
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    m_networkManager->get(request);
}

void MainWindow::onSuggestionsFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonArray results = doc.object()["results"].toArray();

    QStringList suggestions;
    for (const QJsonValue &val : results) {
        QJsonObject obj = val.toObject();
        suggestions << obj["name"].toString() + ", " + obj["country"].toString();
    }

    m_completerModel->setStringList(suggestions);
}

void MainWindow::applyTheme()
{
    QString theme = R"(
        QMainWindow {
            background-color: #0d0d0d;
        }
        QWidget {
            background-color: #0d0d0d;
            color: #ffffff;
        }
        QWidget#centralWidget {
            background-color: #0d0d0d;
        }
        QLabel {
            color: #ffffff;
            background-color: transparent;
        }
        QFrame {
            background-color: #1a1a1a;
            color: #ffffff;
            border: 1px solid #2d2d2d;
            border-radius: 8px;
        }
        QFrame#weatherFrame, QFrame#favoritesFrame {
            background-color: #1a1a1a;
            border: 1px solid #2d2d2d;
        }
        QLineEdit {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 5px;
            selection-background-color: #0d7377;
        }
        QLineEdit:focus {
            border: 1px solid #0d7377;
        }
        QComboBox {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 5px;
        }
        QComboBox:hover {
            border: 1px solid #0d7377;
        }
        QComboBox::drop-down {
            border: none;
            width: 20px;
        }
        QComboBox QAbstractItemView {
            background-color: #2d2d2d;
            color: #ffffff;
            selection-background-color: #0d7377;
            border: 1px solid #404040;
        }
        QPushButton {
            background-color: #0d7377;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #14a085;
        }
        QPushButton:pressed {
            background-color: #0a5a5d;
        }
        QListWidget {
            background-color: #1a1a1a;
            color: #ffffff;
            border: 1px solid #2d2d2d;
            border-radius: 4px;
            outline: none;
        }
        QListWidget::item {
            color: #ffffff;
            padding: 8px;
            border-bottom: 1px solid #2d2d2d;
        }
        QListWidget::item:selected {
            background-color: #0d7377;
            color: #ffffff;
        }
        QListWidget::item:hover {
            background-color: #2d2d2d;
        }
        QScrollArea {
            background-color: #0d0d0d;
            border: none;
        }
        QScrollArea > QWidget > QWidget {
            background-color: #0d0d0d;
        }
        QScrollBar:vertical {
            background-color: #1a1a1a;
            width: 12px;
            border: none;
        }
        QScrollBar::handle:vertical {
            background-color: #404040;
            border-radius: 6px;
            min-height: 20px;
        }
        QScrollBar::handle:vertical:hover {
            background-color: #505050;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar:horizontal {
            background-color: #1a1a1a;
            height: 12px;
            border: none;
        }
        QScrollBar::handle:horizontal {
            background-color: #404040;
            border-radius: 6px;
            min-width: 20px;
        }
        QScrollBar::handle:horizontal:hover {
            background-color: #505050;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
        QMenuBar {
            background-color: #1a1a1a;
            color: #ffffff;
            border-bottom: 1px solid #2d2d2d;
        }
        QMenuBar::item {
            background-color: transparent;
            padding: 4px 8px;
        }
        QMenuBar::item:selected {
            background-color: #2d2d2d;
        }
        QToolBar {
            background-color: #1a1a1a;
            border: none;
            spacing: 3px;
        }
        QStatusBar {
            background-color: #1a1a1a;
            color: #ffffff;
            border-top: 1px solid #2d2d2d;
        }
    )";

    setStyleSheet(theme);
}

void MainWindow::updateFavoritesList()
{
    ui->m_favoritesList->clear();
    ui->m_favoritesList->addItems(m_favoriteCities);
}

void MainWindow::loadSettings()
{
    m_favoriteCities = m_settings->value("favorites").toStringList();
    m_currentCity = m_settings->value("lastCity").toString();
    // m_currentLanguage уже загружен в конструкторе
    m_isCelsius = m_settings->value("celsius", true).toBool();

    qDebug() << "Settings loaded:";
    qDebug() << "  Favorites count:" << m_favoriteCities.size();
    qDebug() << "  Last city:" << m_currentCity;
    qDebug() << "  Celsius:" << m_isCelsius;

    updateFavoritesList();
    ui->m_unitsCombo->setCurrentIndex(m_isCelsius ? 0 : 1);
}

void MainWindow::saveSettings()
{
    m_settings->setValue("favorites", m_favoriteCities);
    m_settings->setValue("lastCity", m_currentCity);
    m_settings->setValue("language", m_currentLanguage);
    m_settings->setValue("celsius", m_isCelsius);
}

double MainWindow::convertTemp(double temp)
{
    return m_isCelsius ? temp : (temp * 9.0 / 5.0 + 32.0);
}

double MainWindow::convertSpeed(double speed)
{
    return m_isCelsius ? speed : (speed * 2.237);
}

QString MainWindow::getTempUnit()
{
    return m_isCelsius ? "°C" : "°F";
}

QString MainWindow::getSpeedUnit()
{
    return m_isCelsius ? TR("Weather/speed_ms") : TR("Weather/speed_mph");
}

QString MainWindow::getWeatherDescription(int code)
{
    if (code == 0) return TR("WeatherConditions/clear");
    else if (code <= 3) return TR("WeatherConditions/cloudy");
    else if (code <= 67) return TR("WeatherConditions/rain");
    else if (code <= 77) return TR("WeatherConditions/snow");
    else return TR("WeatherConditions/thunderstorm");
}

QString MainWindow::getWeatherIcon(const QString &description)
{
    QString clear = TR("WeatherConditions/clear");
    QString cloudy = TR("WeatherConditions/cloudy");
    QString rain = TR("WeatherConditions/rain");
    QString snow = TR("WeatherConditions/snow");
    QString thunderstorm = TR("WeatherConditions/thunderstorm");

    if (description == cloudy) return "☁️";
    else if (description == rain) return "🌧️";
    else if (description == snow) return "❄️";
    else if (description == thunderstorm) return "⛈️";
    else return "☀️";
}

QString MainWindow::getCurrentLanguageCode() const
{
    return m_currentLanguage;
}

QString MainWindow::getWeatherIconUrl(const QString &icon)
{
    return "https://openweathermap.org/img/wn/" + icon + "@2x.png";
}
