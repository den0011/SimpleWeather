#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QUrlQuery>
#include <QPixmap>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_settings(new QSettings(this))
    , m_refreshTimer(new QTimer(this))
    , m_searchDebounceTimer(new QTimer(this))
    , m_isDarkTheme(false)
    , m_isCelsius(true)
    , m_completerModel(new QStringListModel(this))
{
    ui->setupUi(this);

    loadSettings();
    applyTheme();
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
        fetchWeather(m_currentCity);
        fetchForecast(m_currentCity);
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
    connect(ui->m_themeButton, &QPushButton::clicked, this, &MainWindow::toggleTheme);
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
        QMessageBox::warning(this, "Ошибка", "Введите название города");
        return;
    }

    QUrl url(GEOCODING_API_URL);
    QUrlQuery query;
    query.addQueryItem("name", city);
    query.addQueryItem("count", "1");
    query.addQueryItem("language", "ru");
    query.addQueryItem("format", "json");
    url.setQuery(query);

    QNetworkRequest request = createRequest(url);
    QNetworkReply *reply = m_networkManager->get(request);

    m_searchReplies.insert(reply);
}

void MainWindow::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
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
        QMessageBox::warning(this, "Ошибка сети",
            "Не удалось найти город: " + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject obj = doc.object();
    QJsonArray results = obj["results"].toArray();

    if (results.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Город не найден. Попробуйте ввести название по-другому.");
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
    QStringList parts = city.split(", ");
    if (parts.isEmpty()) return;

    QUrl geoUrl(GEOCODING_API_URL);
    QUrlQuery geoQuery;
    geoQuery.addQueryItem("name", parts[0]);
    geoQuery.addQueryItem("count", "1");
    geoQuery.addQueryItem("language", "ru");
    geoUrl.setQuery(geoQuery);

    QNetworkRequest request = createRequest(geoUrl);
    QNetworkReply *geoReply = m_networkManager->get(request);

    connect(geoReply, &QNetworkReply::finished, this, [this, geoReply]() {
        geoReply->deleteLater();

        if (geoReply->error() != QNetworkReply::NoError) {
            qDebug() << "Geo error:" << geoReply->errorString();
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(geoReply->readAll());
        QJsonArray results = doc.object()["results"].toArray();

        if (!results.isEmpty()) {
            QJsonObject location = results[0].toObject();
            double lat = location["latitude"].toDouble();
            double lon = location["longitude"].toDouble();

            QUrl url(WEATHER_API_URL);
            QUrlQuery query;
            query.addQueryItem("latitude", QString::number(lat));
            query.addQueryItem("longitude", QString::number(lon));
            query.addQueryItem("current", "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m");
            query.addQueryItem("timezone", "auto");
            url.setQuery(query);

            QNetworkRequest weatherRequest = createRequest(url);
            m_networkManager->get(weatherRequest);
        }
    });
}

void MainWindow::onWeatherFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject obj = doc.object();
    QJsonObject current = obj["current"].toObject();

    WeatherData data;
    data.city = m_currentCity;
    data.temp = current["temperature_2m"].toDouble();
    data.feelsLike = current["apparent_temperature"].toDouble();
    data.humidity = current["relative_humidity_2m"].toInt();
    data.windSpeed = current["wind_speed_10m"].toDouble();

    int weatherCode = current["weather_code"].toInt();
    if (weatherCode == 0) data.description = "Ясно";
    else if (weatherCode <= 3) data.description = "Облачно";
    else if (weatherCode <= 67) data.description = "Дождь";
    else if (weatherCode <= 77) data.description = "Снег";
    else data.description = "Гроза";

    displayWeather(data);
}

void MainWindow::fetchForecast(const QString &city)
{
    QStringList parts = city.split(", ");
    if (parts.isEmpty()) return;

    QUrl geoUrl(GEOCODING_API_URL);
    QUrlQuery geoQuery;
    geoQuery.addQueryItem("name", parts[0]);
    geoQuery.addQueryItem("count", "1");
    geoQuery.addQueryItem("language", "ru");
    geoUrl.setQuery(geoQuery);

    QNetworkRequest request = createRequest(geoUrl);
    QNetworkReply *geoReply = m_networkManager->get(request);

    connect(geoReply, &QNetworkReply::finished, this, [this, geoReply]() {
        geoReply->deleteLater();

        if (geoReply->error() != QNetworkReply::NoError) {
            qDebug() << "Forecast geo error:" << geoReply->errorString();
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(geoReply->readAll());
        QJsonArray results = doc.object()["results"].toArray();

        if (!results.isEmpty()) {
            QJsonObject location = results[0].toObject();
            double lat = location["latitude"].toDouble();
            double lon = location["longitude"].toDouble();

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
        }
    });
}

void MainWindow::onForecastFinished(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
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
        if (code == 0) fd.description = "Ясно";
        else if (code <= 3) fd.description = "Облачно";
        else if (code <= 67) fd.description = "Дождь";
        else if (code <= 77) fd.description = "Снег";
        else fd.description = "Гроза";

        forecast.append(fd);
    }

    displayForecast(forecast);
}

void MainWindow::displayWeather(const WeatherData &data)
{
    ui->m_cityLabel->setText(data.city);
    ui->m_tempLabel->setText(QString::number(convertTemp(data.temp), 'f', 1) + getTempUnit());
    ui->m_descLabel->setText(data.description);
    ui->m_feelsLikeLabel->setText("Ощущается: " + QString::number(convertTemp(data.feelsLike), 'f', 1) + getTempUnit());
    ui->m_humidityLabel->setText("💧 Влажность: " + QString::number(data.humidity) + "%");
    ui->m_windLabel->setText("💨 Ветер: " + QString::number(convertSpeed(data.windSpeed), 'f', 1) + " " + getSpeedUnit());

    QString icon = "☀️";
    if (data.description.contains("Облачно")) icon = "☁️";
    else if (data.description.contains("Дождь")) icon = "🌧️";
    else if (data.description.contains("Снег")) icon = "❄️";
    else if (data.description.contains("Гроза")) icon = "⛈️";

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

        QString icon = "☀️";
        if (fd.description.contains("Облачно")) icon = "☁️";
        else if (fd.description.contains("Дождь")) icon = "🌧️";
        else if (fd.description.contains("Снег")) icon = "❄️";
        else if (fd.description.contains("Гроза")) icon = "⛈️";

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
        QMessageBox::warning(this, "Ошибка", "Сначала выберите город");
        return;
    }

    if (m_favoriteCities.contains(m_currentCity)) {
        QMessageBox::information(this, "Информация", "Город уже в избранном");
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

void MainWindow::toggleTheme()
{
    m_isDarkTheme = !m_isDarkTheme;
    applyTheme();
    saveSettings();
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
    query.addQueryItem("language", "ru");
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
    QString theme;

    if (m_isDarkTheme) {
        ui->m_themeButton->setText("☀️");
        theme = R"(
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
    } else {
        ui->m_themeButton->setText("🌙");
        theme = R"(
            QMainWindow {
                background-color: #f5f5f5;
            }
            QWidget {
                background-color: #f5f5f5;
                color: #333333;
            }
            QLabel {
                color: #333333;
                background-color: transparent;
            }
            QFrame {
                background-color: #ffffff;
                color: #333333;
                border: 1px solid #e0e0e0;
                border-radius: 8px;
            }
            QLineEdit {
                background-color: #ffffff;
                color: #333333;
                border: 1px solid #cccccc;
                border-radius: 4px;
                padding: 5px;
                selection-background-color: #2196F3;
            }
            QLineEdit:focus {
                border: 1px solid #2196F3;
            }
            QComboBox {
                background-color: #ffffff;
                color: #333333;
                border: 1px solid #cccccc;
                border-radius: 4px;
                padding: 5px;
            }
            QComboBox:hover {
                border: 1px solid #2196F3;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox QAbstractItemView {
                background-color: #ffffff;
                color: #333333;
                selection-background-color: #2196F3;
                border: 1px solid #cccccc;
            }
            QPushButton {
                background-color: #2196F3;
                color: #ffffff;
                border: none;
                border-radius: 4px;
                padding: 8px 16px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: #1976D2;
            }
            QPushButton:pressed {
                background-color: #0D47A1;
            }
            QListWidget {
                background-color: #ffffff;
                color: #333333;
                border: 1px solid #e0e0e0;
                border-radius: 4px;
                outline: none;
            }
            QListWidget::item {
                color: #333333;
                padding: 8px;
                border-bottom: 1px solid #f0f0f0;
            }
            QListWidget::item:selected {
                background-color: #2196F3;
                color: #ffffff;
            }
            QListWidget::item:hover {
                background-color: #f5f5f5;
            }
            QScrollArea {
                background-color: #f5f5f5;
                border: none;
            }
            QScrollBar:vertical {
                background-color: #f0f0f0;
                width: 12px;
                border: none;
            }
            QScrollBar::handle:vertical {
                background-color: #c0c0c0;
                border-radius: 6px;
                min-height: 20px;
            }
            QScrollBar::handle:vertical:hover {
                background-color: #a0a0a0;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0px;
            }
            QScrollBar:horizontal {
                background-color: #f0f0f0;
                height: 12px;
                border: none;
            }
            QScrollBar::handle:horizontal {
                background-color: #c0c0c0;
                border-radius: 6px;
                min-width: 20px;
            }
            QScrollBar::handle:horizontal:hover {
                background-color: #a0a0a0;
            }
            QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
                width: 0px;
            }
            QMenuBar {
                background-color: #ffffff;
                color: #333333;
                border-bottom: 1px solid #e0e0e0;
            }
            QMenuBar::item {
                background-color: transparent;
                padding: 4px 8px;
            }
            QMenuBar::item:selected {
                background-color: #f0f0f0;
            }
            QToolBar {
                background-color: #ffffff;
                border: none;
                spacing: 3px;
            }
            QStatusBar {
                background-color: #ffffff;
                color: #333333;
                border-top: 1px solid #e0e0e0;
            }
        )";
    }

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
    m_isDarkTheme = m_settings->value("darkTheme", false).toBool();
    m_isCelsius = m_settings->value("celsius", true).toBool();

    updateFavoritesList();
    ui->m_unitsCombo->setCurrentIndex(m_isCelsius ? 0 : 1);
}

void MainWindow::saveSettings()
{
    m_settings->setValue("favorites", m_favoriteCities);
    m_settings->setValue("lastCity", m_currentCity);
    m_settings->setValue("darkTheme", m_isDarkTheme);
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
    return m_isCelsius ? "м/с" : "миль/ч";
}

QString MainWindow::getWeatherIconUrl(const QString &icon)
{
    return "https://openweathermap.org/img/wn/" + icon + "@2x.png";
}

void MainWindow::downloadWeatherIcon(const QString &iconCode)
{
    // Заглушка
}

void MainWindow::onIconDownloaded(QNetworkReply *reply)
{
    // Заглушка
}
