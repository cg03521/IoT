#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <espnow.h>
#include <math.h>

// ============================================================
// CONFIGURAZIONE ACCESS POINT
// ============================================================

constexpr char AP_SSID[] = "ESPNow-Meteo";
constexpr char AP_PASSWORD[] = "Meteo2026";

constexpr uint8_t WIFI_CHANNEL = 1;
constexpr uint16_t WEB_SERVER_PORT = 80;

// Il nuovo sender trasmette ogni 5 minuti.
// Sette minuti consentono un certo margine operativo.
constexpr uint32_t OFFLINE_TIMEOUT_MS = 420000UL;

// ============================================================
// CONFIGURAZIONE STORICO E SENSORI
// ============================================================

constexpr uint16_t HISTORY_SIZE = 100;
constexpr uint8_t MAX_SENSORS = 8;

// Limiti dichiarati per BME280.
// Per i nodi DHT22 pressure deve essere impostata a zero.
constexpr float MIN_VALID_TEMPERATURE = -40.0F;
constexpr float MAX_VALID_TEMPERATURE = 85.0F;

constexpr float MIN_VALID_HUMIDITY = 0.0F;
constexpr float MAX_VALID_HUMIDITY = 100.0F;

constexpr float MIN_VALID_PRESSURE = 300.0F;
constexpr float MAX_VALID_PRESSURE = 1100.0F;

// Intervallo meteorologico molto insolito per una stazione a bassa quota.
// Il dato non viene scartato, ma genera un warning.
constexpr float SUSPICIOUS_PRESSURE_LOW = 850.0F;
constexpr float SUSPICIOUS_PRESSURE_HIGH = 1085.0F;

// ============================================================
// STRUTTURE DATI
//
// IMPORTANTE:
// Questa struttura deve essere IDENTICA in tutti i sender.
// I sender DHT22 devono impostare pressure = 0.0F.
// ============================================================

struct __attribute__((packed)) SensorData
{
    char location[16];
    float temperature;
    float humidity;
    float pressure;
};

struct HistorySample
{
    float temperature;
    float humidity;
    float pressure;
    uint32_t timestampSeconds;
};

struct ForecastResult
{
    bool available;
    float trendHpaPerHour;
    float observedHours;
    uint16_t pressureSamples;
    char title[48];
    char description[180];
};

struct SensorNode
{
    uint8_t mac[6];
    char macStr[18];
    char name[24];

    SensorData currentData;

    bool hasData = false;
    bool hasPressure = false;

    uint32_t packetCount = 0;
    uint32_t invalidPacketCount = 0;
    uint32_t lastUpdateMs = 0;

    bool warningActive = false;
    char warning[120];

    HistorySample historyBuffer[HISTORY_SIZE];
    uint16_t historyStart = 0;
    uint16_t historyCount = 0;
};

SensorNode sensors[MAX_SENSORS];
uint8_t activeSensorCount = 0;

// ============================================================
// SERVER WEB
// ============================================================

ESP8266WebServer server(WEB_SERVER_PORT);

// ============================================================
// DATI RICEVUTI DALLA CALLBACK
// ============================================================

volatile bool pendingPacketAvailable = false;

SensorData pendingData{};
uint8_t pendingSenderMac[6]{};

// ============================================================
// PAGINA HTML
//
// Il browser richiede separatamente:
//   /api/sensors
//   /api/history?id=N
//
// Questo evita di costruire un unico JSON molto grande,
// riducendo l'uso e la frammentazione della heap ESP8266.
// ============================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">
    <meta
        name="viewport"
        content="width=device-width, initial-scale=1.0">

    <title>Stazione Meteo ESP-NOW</title>

    <style>
        * {
            box-sizing: border-box;
        }

        body {
            margin: 0;
            padding: 18px;
            font-family: -apple-system, BlinkMacSystemFont,
                         "Segoe UI", Roboto, Arial, sans-serif;
            color: #1e293b;
            background: #f1f5f9;
        }

        .container {
            max-width: 1000px;
            margin: 0 auto;
        }

        header {
            text-align: center;
            margin-bottom: 18px;
        }

        h1 {
            margin: 0 0 5px;
            color: #0f172a;
        }

        .subtitle {
            margin: 0;
            color: #64748b;
        }

        .tabs {
            display: flex;
            gap: 8px;
            overflow-x: auto;
            padding-bottom: 5px;
            margin-bottom: 16px;
        }

        .tab-btn,
        .forecast-btn {
            border: 0;
            padding: 10px 16px;
            border-radius: 12px;
            font-weight: 600;
            cursor: pointer;
        }

        .tab-btn {
            color: #475569;
            background: #e2e8f0;
            white-space: nowrap;
        }

        .tab-btn.active {
            color: white;
            background: #2563eb;
        }

        .forecast-btn {
            margin: 0 0 18px;
            color: white;
            background: #0369a1;
        }

        .status {
            display: inline-block;
            padding: 7px 14px;
            margin-bottom: 14px;
            border-radius: 20px;
            font-weight: 600;
            font-size: 13px;
        }

        .ok {
            color: #15803d;
            background: #dcfce7;
        }

        .offline {
            color: #b91c1c;
            background: #fee2e2;
        }

        .waiting {
            color: #c2410c;
            background: #ffedd5;
        }

        .warning {
            display: none;
            padding: 12px 15px;
            margin-bottom: 16px;
            border-radius: 12px;
            color: #92400e;
            background: #fef3c7;
            border: 1px solid #f59e0b;
        }

        .cards {
            display: grid;
            grid-template-columns:
                repeat(auto-fit, minmax(190px, 1fr));
            gap: 14px;
            margin-bottom: 18px;
        }

        .card,
        .panel {
            padding: 18px;
            border: 1px solid #e2e8f0;
            border-radius: 16px;
            background: white;
            box-shadow: 0 1px 3px rgba(0, 0, 0, .05);
        }

        .card-title {
            color: #64748b;
            font-size: 14px;
        }

        .card-value {
            margin-top: 7px;
            color: #0f172a;
            font-size: 31px;
            font-weight: 700;
        }

        .panel {
            margin-bottom: 18px;
        }

        .panel h2 {
            margin: 0 0 12px;
            font-size: 18px;
        }

        .forecast {
            display: none;
            border-left: 5px solid #0369a1;
        }

        .forecast-title {
            color: #075985;
            font-size: 21px;
            font-weight: 700;
        }

        .forecast-description {
            margin-top: 8px;
            line-height: 1.55;
        }

        canvas {
            display: block;
            width: 100%;
            height: 220px;
        }

        .details {
            line-height: 1.8;
            font-size: 14px;
        }

        @media (max-width: 600px) {
            body {
                padding: 10px;
            }

            .card-value {
                font-size: 27px;
            }
        }
    </style>
</head>

<body>
<div class="container">
    <header>
        <h1>Stazione Meteo ESP-NOW</h1>
        <p class="subtitle">Ricevitore multi-nodo</p>
    </header>

    <div id="tabs" class="tabs"></div>

    <div id="status" class="status waiting">
        In attesa dei dati
    </div>

    <div id="warning" class="warning"></div>

    <div class="cards">
        <div class="card">
            <div class="card-title">Temperatura</div>
            <div id="temp" class="card-value">--.- °C</div>
        </div>

        <div class="card">
            <div class="card-title">Umidità</div>
            <div id="hum" class="card-value">--.- %</div>
        </div>

        <div class="card">
            <div class="card-title">Pressione assoluta</div>
            <div id="pressure" class="card-value">--.- hPa</div>
        </div>
    </div>

    <button
        id="forecastButton"
        class="forecast-btn"
        onclick="showForecast()">
        Calcola previsione
    </button>

    <div id="forecastPanel" class="panel forecast">
        <div id="forecastTitle" class="forecast-title">
            Previsione non disponibile
        </div>

        <div id="forecastDescription"
             class="forecast-description"></div>

        <div class="details">
            <strong>Trend pressione:</strong>
            <span id="forecastTrend">--</span>
            <br>

            <strong>Periodo analizzato:</strong>
            <span id="forecastHours">--</span>
            <br>

            <strong>Campioni barometrici:</strong>
            <span id="forecastSamples">0</span>
        </div>
    </div>

    <div class="panel">
        <h2>Storico temperatura</h2>
        <canvas id="tempChart"></canvas>
    </div>

    <div class="panel">
        <h2>Storico umidità</h2>
        <canvas id="humChart"></canvas>
    </div>

    <div id="pressureChartPanel" class="panel">
        <h2>Storico pressione</h2>
        <canvas id="pressureChart"></canvas>
    </div>

    <div class="panel details">
        <strong>Nodo:</strong>
        <span id="nodeName">--</span>
        <br>

        <strong>MAC:</strong>
        <span id="nodeMac">--</span>
        <br>

        <strong>Pacchetti ricevuti:</strong>
        <span id="packets">0</span>
        <br>

        <strong>Pacchetti non validi:</strong>
        <span id="invalidPackets">0</span>
        <br>

        <strong>Campioni:</strong>
        <span id="samples">0</span> / 100
        <br>

        <strong>Ultimo aggiornamento:</strong>
        <span id="age">--</span>
    </div>
</div>

<script>
    let sensors = [];
    let selectedIndex = 0;
    let selectedHistory = [];
    let lastHistorySensor = -1;

    function drawChart(canvasId, values, color, unit, fixedMin, fixedMax) {
        const canvas = document.getElementById(canvasId);
        const ratio = window.devicePixelRatio || 1;
        const rect = canvas.getBoundingClientRect();
        const width = Math.max(280, rect.width);
        const height = Math.max(180, rect.height);

        canvas.width = width * ratio;
        canvas.height = height * ratio;

        const ctx = canvas.getContext("2d");
        ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
        ctx.clearRect(0, 0, width, height);

        if (!values.length) {
            ctx.fillStyle = "#94a3b8";
            ctx.font = "14px sans-serif";
            ctx.textAlign = "center";
            ctx.fillText("Nessun dato disponibile", width / 2, height / 2);
            return;
        }

        const margin = {top: 15, right: 12, bottom: 25, left: 49};
        const graphWidth = width - margin.left - margin.right;
        const graphHeight = height - margin.top - margin.bottom;

        let min = fixedMin !== null
            ? fixedMin
            : Math.min(...values) - 0.5;

        let max = fixedMax !== null
            ? fixedMax
            : Math.max(...values) + 0.5;

        if (max <= min) {
            max = min + 1;
        }

        const range = max - min;

        ctx.strokeStyle = "#e2e8f0";
        ctx.fillStyle = "#64748b";
        ctx.font = "11px sans-serif";

        for (let line = 0; line <= 4; line++) {
            const y = margin.top + graphHeight * line / 4;
            const value = max - range * line / 4;

            ctx.beginPath();
            ctx.moveTo(margin.left, y);
            ctx.lineTo(margin.left + graphWidth, y);
            ctx.stroke();

            ctx.textAlign = "right";
            ctx.fillText(
                value.toFixed(1) + unit,
                margin.left - 5,
                y + 4
            );
        }

        ctx.strokeStyle = color;
        ctx.lineWidth = 2.2;
        ctx.beginPath();

        values.forEach((value, index) => {
            const x = values.length === 1
                ? margin.left + graphWidth / 2
                : margin.left +
                  graphWidth * index / (values.length - 1);

            const y = margin.top +
                graphHeight * (max - value) / range;

            if (index === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        });

        ctx.stroke();
    }

    function renderTabs() {
        const tabs = document.getElementById("tabs");
        tabs.innerHTML = "";

        sensors.forEach((sensor, index) => {
            const button = document.createElement("button");

            button.className =
                "tab-btn " +
                (index === selectedIndex ? "active" : "");

            button.textContent = sensor.name;

            button.onclick = async () => {
                selectedIndex = index;
                lastHistorySensor = -1;
                renderTabs();
                await fetchHistory();
                updateDisplay();
            };

            tabs.appendChild(button);
        });
    }

    async function fetchHistory() {
        if (!sensors.length) {
            return;
        }

        if (lastHistorySensor === selectedIndex) {
            return;
        }

        const response = await fetch(
            "/api/history?id=" + selectedIndex,
            {cache: "no-store"}
        );

        if (!response.ok) {
            selectedHistory = [];
            return;
        }

        selectedHistory = await response.json();
        lastHistorySensor = selectedIndex;
    }

    function updateDisplay() {
        if (!sensors.length) {
            return;
        }

        const sensor = sensors[selectedIndex];

        document.getElementById("nodeName").textContent =
            sensor.name;

        document.getElementById("nodeMac").textContent =
            sensor.mac;

        document.getElementById("packets").textContent =
            sensor.packets;

        document.getElementById("invalidPackets").textContent =
            sensor.invalidPackets;

        document.getElementById("samples").textContent =
            sensor.samples;

        document.getElementById("age").textContent =
            sensor.valid
                ? sensor.ageSeconds + " secondi fa"
                : "--";

        document.getElementById("temp").textContent =
            sensor.valid
                ? sensor.temperature.toFixed(1) + " °C"
                : "--.- °C";

        document.getElementById("hum").textContent =
            sensor.valid
                ? sensor.humidity.toFixed(1) + " %"
                : "--.- %";

        document.getElementById("pressure").textContent =
            sensor.hasPressure
                ? sensor.pressure.toFixed(1) + " hPa"
                : "Non disponibile";

        const status = document.getElementById("status");

        if (!sensor.valid) {
            status.textContent = "In attesa dei dati";
            status.className = "status waiting";
        } else if (sensor.online) {
            status.textContent = "Online - Aggiornato";
            status.className = "status ok";
        } else {
            status.textContent = "Offline - Nessun dato recente";
            status.className = "status offline";
        }

        const warning = document.getElementById("warning");

        if (sensor.warning) {
            warning.style.display = "block";
            warning.textContent = "Attenzione: " + sensor.warningText;
        } else {
            warning.style.display = "none";
        }

        document.getElementById("forecastButton").style.display =
            sensor.hasPressure ? "inline-block" : "none";

        document.getElementById("pressureChartPanel").style.display =
            sensor.hasPressure ? "block" : "none";

        const temperatures =
            selectedHistory.map(item => item.temperature);

        const humidities =
            selectedHistory.map(item => item.humidity);

        const pressures =
            selectedHistory
                .filter(item => item.pressure > 0)
                .map(item => item.pressure);

        drawChart(
            "tempChart",
            temperatures,
            "#ef4444",
            "°",
            null,
            null
        );

        drawChart(
            "humChart",
            humidities,
            "#0284c7",
            "%",
            0,
            100
        );

        if (sensor.hasPressure) {
            drawChart(
                "pressureChart",
                pressures,
                "#7c3aed",
                "",
                null,
                null
            );
        }
    }

    function showForecast() {
        if (!sensors.length) {
            return;
        }

        const forecast = sensors[selectedIndex].forecast;
        const panel = document.getElementById("forecastPanel");

        panel.style.display = "block";

        document.getElementById("forecastTitle").textContent =
            forecast.title;

        document.getElementById("forecastDescription").textContent =
            forecast.description;

        document.getElementById("forecastTrend").textContent =
            forecast.available
                ? forecast.trend.toFixed(2) + " hPa/ora"
                : "--";

        document.getElementById("forecastHours").textContent =
            forecast.available
                ? forecast.hours.toFixed(1) + " ore"
                : "--";

        document.getElementById("forecastSamples").textContent =
            forecast.samples;

        panel.scrollIntoView({
            behavior: "smooth",
            block: "nearest"
        });
    }

    async function fetchSensors() {
        try {
            const response = await fetch(
                "/api/sensors",
                {cache: "no-store"}
            );

            if (!response.ok) {
                return;
            }

            const previousLength = sensors.length;
            sensors = await response.json();

            if (selectedIndex >= sensors.length) {
                selectedIndex = 0;
            }

            if (previousLength !== sensors.length) {
                renderTabs();
                lastHistorySensor = -1;
            }

            // Ricarica lo storico se è arrivato un nuovo campione.
            if (
                sensors.length &&
                selectedHistory.length !== sensors[selectedIndex].samples
            ) {
                lastHistorySensor = -1;
            }

            await fetchHistory();
            updateDisplay();
        }
        catch (error) {
            console.error(error);
        }
    }

    window.addEventListener("resize", updateDisplay);

    fetchSensors();
    setInterval(fetchSensors, 5000);
</script>
</body>
</html>
)rawliteral";

// ============================================================
// FUNZIONI DI SUPPORTO
// ============================================================

void formatMacAddress(
    const uint8_t *mac,
    char *destination,
    size_t destinationSize)
{
    snprintf(
        destination,
        destinationSize,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);
}

void jsonEscapeAppend(String &destination, const char *source)
{
    while (*source)
    {
        const char character = *source++;

        if (character == '"' || character == '\\')
        {
            destination += '\\';
        }

        if (
            character != '\r' &&
            character != '\n'
        )
        {
            destination += character;
        }
    }
}

int findOrRegisterSensor(
    const uint8_t *mac,
    const char *incomingName)
{
    for (
        uint8_t index = 0;
        index < activeSensorCount;
        index++)
    {
        if (
            memcmp(
                sensors[index].mac,
                mac,
                6) == 0)
        {
            if (
                incomingName != nullptr &&
                incomingName[0] != '\0')
            {
                strncpy(
                    sensors[index].name,
                    incomingName,
                    sizeof(sensors[index].name) - 1);

                sensors[index].name[
                    sizeof(sensors[index].name) - 1
                ] = '\0';
            }

            return index;
        }
    }

    if (activeSensorCount >= MAX_SENSORS)
    {
        return -1;
    }

    const uint8_t index =
        activeSensorCount++;

    memcpy(
        sensors[index].mac,
        mac,
        6);

    formatMacAddress(
        mac,
        sensors[index].macStr,
        sizeof(sensors[index].macStr));

    if (
        incomingName != nullptr &&
        incomingName[0] != '\0')
    {
        strncpy(
            sensors[index].name,
            incomingName,
            sizeof(sensors[index].name) - 1);

        sensors[index].name[
            sizeof(sensors[index].name) - 1
        ] = '\0';
    }
    else
    {
        snprintf(
            sensors[index].name,
            sizeof(sensors[index].name),
            "Sensore %02X%02X",
            mac[4],
            mac[5]);
    }

    return index;
}

bool isTemperatureValid(float value)
{
    return
        isfinite(value) &&
        value >= MIN_VALID_TEMPERATURE &&
        value <= MAX_VALID_TEMPERATURE;
}

bool isHumidityValid(float value)
{
    return
        isfinite(value) &&
        value >= MIN_VALID_HUMIDITY &&
        value <= MAX_VALID_HUMIDITY;
}

bool isPressurePresent(float value)
{
    return
        isfinite(value) &&
        fabs(value) > 0.01F;
}

bool isPressureValid(float value)
{
    if (!isPressurePresent(value))
    {
        return true;
    }

    return
        value >= MIN_VALID_PRESSURE &&
        value <= MAX_VALID_PRESSURE;
}

void setWarning(
    SensorNode &sensor,
    const char *message)
{
    sensor.warningActive = true;

    strncpy(
        sensor.warning,
        message,
        sizeof(sensor.warning) - 1);

    sensor.warning[
        sizeof(sensor.warning) - 1
    ] = '\0';
}

void addHistorySample(
    SensorNode &sensor,
    float temperature,
    float humidity,
    float pressure)
{
    uint16_t writeIndex;

    if (sensor.historyCount < HISTORY_SIZE)
    {
        writeIndex =
            (sensor.historyStart + sensor.historyCount)
            % HISTORY_SIZE;

        sensor.historyCount++;
    }
    else
    {
        writeIndex = sensor.historyStart;

        sensor.historyStart =
            (sensor.historyStart + 1)
            % HISTORY_SIZE;
    }

    HistorySample &sample =
        sensor.historyBuffer[writeIndex];

    sample.temperature = temperature;
    sample.humidity = humidity;
    sample.pressure = pressure;
    sample.timestampSeconds = millis() / 1000UL;
}

// ============================================================
// CALCOLO PREVISIONE
//
// Viene calcolata una regressione lineare:
// pressione = intercetta + pendenza * tempo.
//
// La pendenza viene restituita in hPa/ora.
// È un indicatore sperimentale, non una previsione professionale.
// ============================================================

ForecastResult calculateForecast(
    const SensorNode &sensor)
{
    ForecastResult result{};

    result.available = false;

    strncpy(
        result.title,
        "Dati barometrici insufficienti",
        sizeof(result.title) - 1);

    strncpy(
        result.description,
        "Servono almeno 6 campioni validi e almeno 30 minuti di storico della pressione.",
        sizeof(result.description) - 1);

    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;

    uint16_t validCount = 0;
    uint32_t firstTimestamp = 0;
    uint32_t lastTimestamp = 0;

    for (
        uint16_t position = 0;
        position < sensor.historyCount;
        position++)
    {
        const uint16_t index =
            (sensor.historyStart + position)
            % HISTORY_SIZE;

        const HistorySample &sample =
            sensor.historyBuffer[index];

        if (
            sample.pressure <= 0.0F ||
            !isPressureValid(sample.pressure))
        {
            continue;
        }

        if (validCount == 0)
        {
            firstTimestamp =
                sample.timestampSeconds;
        }

        lastTimestamp =
            sample.timestampSeconds;

        const double hoursFromFirst =
            static_cast<double>(
                sample.timestampSeconds -
                firstTimestamp) / 3600.0;

        const double pressure =
            sample.pressure;

        sumX += hoursFromFirst;
        sumY += pressure;
        sumXY += hoursFromFirst * pressure;
        sumXX += hoursFromFirst * hoursFromFirst;

        validCount++;
    }

    result.pressureSamples = validCount;

    if (
        validCount < 6 ||
        lastTimestamp <= firstTimestamp)
    {
        return result;
    }

    const float observedHours =
        static_cast<float>(
            lastTimestamp -
            firstTimestamp) / 3600.0F;

    result.observedHours =
        observedHours;

    if (observedHours < 0.5F)
    {
        return result;
    }

    const double denominator =
        validCount * sumXX -
        sumX * sumX;

    if (fabs(denominator) < 0.000001)
    {
        return result;
    }

    const float trend =
        static_cast<float>(
            (validCount * sumXY - sumX * sumY)
            / denominator);

    result.available = true;
    result.trendHpaPerHour = trend;

    const float humidity =
        sensor.currentData.humidity;

    if (trend <= -1.0F)
    {
        strncpy(
            result.title,
            "Pressione in rapido calo",
            sizeof(result.title) - 1);

        if (humidity >= 75.0F)
        {
            strncpy(
                result.description,
                "Possibile peggioramento marcato. Il calo rapido della pressione e l'umidita elevata sono compatibili con maggiore instabilita e possibile pioggia.",
                sizeof(result.description) - 1);
        }
        else
        {
            strncpy(
                result.description,
                "Possibile peggioramento. La pressione sta diminuendo rapidamente; osservare l'evoluzione nelle prossime ore.",
                sizeof(result.description) - 1);
        }
    }
    else if (trend <= -0.3F)
    {
        strncpy(
            result.title,
            "Tendenza al peggioramento",
            sizeof(result.title) - 1);

        strncpy(
            result.description,
            "La pressione mostra un calo moderato. Sono possibili aumento della nuvolosita e condizioni piu instabili.",
            sizeof(result.description) - 1);
    }
    else if (trend >= 1.0F)
    {
        strncpy(
            result.title,
            "Pressione in rapido aumento",
            sizeof(result.title) - 1);

        strncpy(
            result.description,
            "Probabile miglioramento delle condizioni. La pressione sta aumentando rapidamente, con tendenza verso maggiore stabilita.",
            sizeof(result.description) - 1);
    }
    else if (trend >= 0.3F)
    {
        strncpy(
            result.title,
            "Tendenza al miglioramento",
            sizeof(result.title) - 1);

        strncpy(
            result.description,
            "La pressione mostra un aumento moderato, compatibile con un graduale miglioramento e maggiore stabilita.",
            sizeof(result.description) - 1);
    }
    else
    {
        strncpy(
            result.title,
            "Pressione sostanzialmente stabile",
            sizeof(result.title) - 1);

        if (humidity >= 85.0F)
        {
            strncpy(
                result.description,
                "Situazione barometrica stabile, ma l'umidita e molto elevata. Sono possibili foschia, nebbia o nuvolosita persistente.",
                sizeof(result.description) - 1);
        }
        else
        {
            strncpy(
                result.description,
                "Non emerge una variazione barometrica significativa. Le condizioni potrebbero rimanere simili nel breve periodo.",
                sizeof(result.description) - 1);
        }
    }

    result.title[
        sizeof(result.title) - 1
    ] = '\0';

    result.description[
        sizeof(result.description) - 1
    ] = '\0';

    return result;
}

// ============================================================
// CALLBACK ESP-NOW
// ============================================================

void onDataReceived(
    uint8_t *mac,
    uint8_t *incomingData,
    uint8_t length)
{
    if (
        mac == nullptr ||
        incomingData == nullptr ||
        length != sizeof(SensorData))
    {
        return;
    }

    memcpy(
        &pendingData,
        incomingData,
        sizeof(pendingData));

    memcpy(
        pendingSenderMac,
        mac,
        sizeof(pendingSenderMac));

    pendingPacketAvailable = true;
}

// ============================================================
// ELABORAZIONE PACCHETTO
// ============================================================

void processPendingPacket()
{
    if (!pendingPacketAvailable)
    {
        return;
    }

    noInterrupts();

    SensorData localData =
        pendingData;

    uint8_t localMac[6];

    memcpy(
        localMac,
        pendingSenderMac,
        sizeof(localMac));

    pendingPacketAvailable = false;

    interrupts();

    // Garantisce che la label sia sempre terminata correttamente.
    localData.location[
        sizeof(localData.location) - 1
    ] = '\0';

    const int sensorIndex =
        findOrRegisterSensor(
            localMac,
            localData.location);

    if (sensorIndex < 0)
    {
        Serial.println(
            F("ERRORE: numero massimo sensori raggiunto"));

        return;
    }

    SensorNode &sensor =
        sensors[sensorIndex];

    sensor.warningActive = false;
    sensor.warning[0] = '\0';

    const bool temperatureValid =
        isTemperatureValid(
            localData.temperature);

    const bool humidityValid =
        isHumidityValid(
            localData.humidity);

    const bool pressurePresent =
        isPressurePresent(
            localData.pressure);

    const bool pressureValid =
        isPressureValid(
            localData.pressure);

    if (!temperatureValid || !humidityValid)
    {
        sensor.invalidPacketCount++;

        setWarning(
            sensor,
            "Pacchetto scartato: temperatura o umidita fuori scala.");

        Serial.printf(
            "[%s] Pacchetto scartato: T=%.2f, H=%.2f\n",
            sensor.name,
            localData.temperature,
            localData.humidity);

        return;
    }

    if (!pressureValid)
    {
        sensor.invalidPacketCount++;

        setWarning(
            sensor,
            "Pressione fuori dal campo operativo: il valore barometrico e stato ignorato.");

        localData.pressure = 0.0F;
    }
    else if (
        pressurePresent &&
        (
            localData.pressure < SUSPICIOUS_PRESSURE_LOW ||
            localData.pressure > SUSPICIOUS_PRESSURE_HIGH
        ))
    {
        setWarning(
            sensor,
            "Pressione nel campo tecnico del sensore ma molto insolita per una stazione meteorologica a bassa quota.");
    }

    sensor.currentData =
        localData;

    sensor.hasData = true;

    sensor.hasPressure =
        localData.pressure > 0.0F;

    sensor.packetCount++;
    sensor.lastUpdateMs = millis();

    addHistorySample(
        sensor,
        localData.temperature,
        localData.humidity,
        localData.pressure);

    Serial.println(
        F("--------------------------------"));

    Serial.printf(
        "[%s] T=%.1f C, H=%.1f %%",
        sensor.name,
        localData.temperature,
        localData.humidity);

    if (localData.pressure > 0.0F)
    {
        Serial.printf(
            ", P=%.1f hPa",
            localData.pressure);
    }
    else
    {
        Serial.print(
            F(", P=non disponibile"));
    }

    Serial.println();

    if (sensor.warningActive)
    {
        Serial.print(
            F("WARNING: "));

        Serial.println(
            sensor.warning);
    }
}

// ============================================================
// API RIEPILOGO SENSORI
// ============================================================

void handleSensorsApi()
{
    String json;

    json.reserve(
        512 + activeSensorCount * 600);

    json += '[';

    const uint32_t now =
        millis();

    for (
        uint8_t index = 0;
        index < activeSensorCount;
        index++)
    {
        if (index > 0)
        {
            json += ',';
        }

        SensorNode &sensor =
            sensors[index];

        const uint32_t ageMs =
            sensor.hasData
                ? now - sensor.lastUpdateMs
                : 0;

        const bool online =
            sensor.hasData &&
            ageMs <= OFFLINE_TIMEOUT_MS;

        const ForecastResult forecast =
            calculateForecast(sensor);

        json += F("{\"name\":\"");
        jsonEscapeAppend(json, sensor.name);

        json += F("\",\"mac\":\"");
        json += sensor.macStr;

        json += F("\",\"valid\":");
        json += sensor.hasData ? F("true") : F("false");

        json += F(",\"online\":");
        json += online ? F("true") : F("false");

        json += F(",\"temperature\":");
        json += String(
            sensor.currentData.temperature,
            1);

        json += F(",\"humidity\":");
        json += String(
            sensor.currentData.humidity,
            1);

        json += F(",\"pressure\":");
        json += String(
            sensor.currentData.pressure,
            1);

        json += F(",\"hasPressure\":");
        json +=
            sensor.hasPressure
                ? F("true")
                : F("false");

        json += F(",\"packets\":");
        json += String(
            sensor.packetCount);

        json += F(",\"invalidPackets\":");
        json += String(
            sensor.invalidPacketCount);

        json += F(",\"samples\":");
        json += String(
            sensor.historyCount);

        json += F(",\"ageSeconds\":");
        json += String(
            ageMs / 1000UL);

        json += F(",\"warning\":");
        json +=
            sensor.warningActive
                ? F("true")
                : F("false");

        json += F(",\"warningText\":\"");
        jsonEscapeAppend(
            json,
            sensor.warning);

        json += F("\",\"forecast\":{");

        json += F("\"available\":");
        json +=
            forecast.available
                ? F("true")
                : F("false");

        json += F(",\"trend\":");
        json += String(
            forecast.trendHpaPerHour,
            2);

        json += F(",\"hours\":");
        json += String(
            forecast.observedHours,
            1);

        json += F(",\"samples\":");
        json += String(
            forecast.pressureSamples);

        json += F(",\"title\":\"");
        jsonEscapeAppend(
            json,
            forecast.title);

        json += F("\",\"description\":\"");
        jsonEscapeAppend(
            json,
            forecast.description);

        json += F("\"}}");
    }

    json += ']';

    server.sendHeader(
        F("Cache-Control"),
        F("no-store"));

    server.send(
        200,
        "application/json; charset=utf-8",
        json);
}

// ============================================================
// API STORICO DEL SINGOLO SENSORE
// ============================================================

void handleHistoryApi()
{
    if (!server.hasArg("id"))
    {
        server.send(
            400,
            "application/json",
            "[]");

        return;
    }

    const int sensorIndex =
        server.arg("id").toInt();

    if (
        sensorIndex < 0 ||
        sensorIndex >= activeSensorCount)
    {
        server.send(
            404,
            "application/json",
            "[]");

        return;
    }

    SensorNode &sensor =
        sensors[sensorIndex];

    String json;

    json.reserve(
        2 + sensor.historyCount * 82);

    json += '[';

    for (
        uint16_t position = 0;
        position < sensor.historyCount;
        position++)
    {
        if (position > 0)
        {
            json += ',';
        }

        const uint16_t index =
            (sensor.historyStart + position)
            % HISTORY_SIZE;

        const HistorySample &sample =
            sensor.historyBuffer[index];

        json += F("{\"temperature\":");
        json += String(
            sample.temperature,
            1);

        json += F(",\"humidity\":");
        json += String(
            sample.humidity,
            1);

        json += F(",\"pressure\":");
        json += String(
            sample.pressure,
            1);

        json += F(",\"time\":");
        json += String(
            sample.timestampSeconds);

        json += '}';
    }

    json += ']';

    server.sendHeader(
        F("Cache-Control"),
        F("no-store"));

    server.send(
        200,
        "application/json; charset=utf-8",
        json);
}

void handleRoot()
{
    server.send_P(
        200,
        "text/html; charset=utf-8",
        INDEX_HTML);
}

// ============================================================
// INIZIALIZZAZIONE ACCESS POINT ED ESP-NOW
// ============================================================

bool startAccessPoint()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();

    delay(100);

    const bool started =
        WiFi.softAP(
            AP_SSID,
            AP_PASSWORD,
            WIFI_CHANNEL);

    if (started)
    {
        Serial.print(
            F("Access Point: "));

        Serial.println(
            AP_SSID);

        Serial.print(
            F("IP: "));

        Serial.println(
            WiFi.softAPIP());

        Serial.print(
            F("MAC Station ESP-NOW: "));

        Serial.println(
            WiFi.macAddress());

        Serial.print(
            F("Canale: "));

        Serial.println(
            WIFI_CHANNEL);
    }

    return started;
}

bool startEspNow()
{
    if (esp_now_init() != 0)
    {
        return false;
    }

    esp_now_set_self_role(
        ESP_NOW_ROLE_SLAVE);

    esp_now_register_recv_cb(
        onDataReceived);

    return true;
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println(
        F("================================"));

    Serial.println(
        F("ESP-NOW MULTI-RECEIVER"));

    Serial.println(
        F("Temperatura, umidita, pressione"));

    Serial.println(
        F("================================"));

    if (!startAccessPoint())
    {
        Serial.println(
            F("ERRORE: Access Point non avviato"));

        while (true)
        {
            yield();
        }
    }

    if (!startEspNow())
    {
        Serial.println(
            F("ERRORE: ESP-NOW non inizializzato"));

        while (true)
        {
            yield();
        }
    }

    server.on(
        "/",
        HTTP_GET,
        handleRoot);

    server.on(
        "/api/sensors",
        HTTP_GET,
        handleSensorsApi);

    server.on(
        "/api/history",
        HTTP_GET,
        handleHistoryApi);

    server.begin();

    Serial.println(
        F("Ricevitore pronto"));

    Serial.println(
        F("Dashboard: http://192.168.4.1"));
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    processPendingPacket();
    server.handleClient();
    yield();
}