#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <espnow.h>

// ============================================================
// CONFIGURAZIONE ACCESS POINT
// ============================================================

constexpr char AP_SSID[] = "ESPNow-Meteo";
constexpr char AP_PASSWORD[] = "Meteo2026";

constexpr uint8_t WIFI_CHANNEL = 1;
constexpr uint16_t WEB_SERVER_PORT = 80;

// Il trasmettitore invia ogni 5 secondi.
// Dopo 15 secondi senza pacchetti viene indicato come non aggiornato.
constexpr uint32_t OFFLINE_TIMEOUT_MS = 15000;

// ============================================================
// CONFIGURAZIONE STORICO
// ============================================================

constexpr uint16_t HISTORY_SIZE = 100;

struct SensorData
{
    float temperature;
    float humidity;
};

struct HistorySample
{
    float temperature;
    float humidity;
    uint32_t timestampSeconds;
};

// Buffer circolare di 100 campioni.
HistorySample historyBuffer[HISTORY_SIZE];

uint16_t historyStart = 0;
uint16_t historyCount = 0;

// ============================================================
// SERVER WEB
// ============================================================

ESP8266WebServer server(WEB_SERVER_PORT);

// ============================================================
// DATI RICEVUTI
// ============================================================

// La callback copia qui il nuovo pacchetto.
// Il pacchetto sarà elaborato nel loop principale.
volatile bool pendingPacketAvailable = false;

SensorData pendingData {};
uint8_t pendingSenderMac[6] = {};

// Ultimi dati elaborati.
SensorData receivedData {};

char senderMacString[18] = "--";

bool hasReceivedData = false;

uint32_t packetCounter = 0;
uint32_t lastUpdateMs = 0;

// ============================================================
// PAGINA WEB
//
// Memorizzata in Flash con PROGMEM per non occupare inutilmente RAM.
// I grafici usano Canvas, quindi non richiedono Internet.
// ============================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">

    <meta
        name="viewport"
        content="width=device-width, initial-scale=1.0">

    <title>ESP-NOW Meteo</title>

    <style>
        * {
            box-sizing: border-box;
        }

        body {
            margin: 0;
            padding: 20px;
            font-family: Arial, Helvetica, sans-serif;
            color: #17212b;
            background: #eef3f8;
        }

        .container {
            max-width: 900px;
            margin: 0 auto;
        }

        h1 {
            margin-bottom: 5px;
            text-align: center;
            color: #075985;
        }

        .subtitle {
            margin-top: 0;
            text-align: center;
            color: #64748b;
        }

        #status {
            width: fit-content;
            margin: 16px auto;
            padding: 9px 18px;
            border-radius: 20px;
            font-weight: bold;
            background: #e2e8f0;
        }

        .status-ok {
            color: #166534;
            background: #dcfce7 !important;
        }

        .status-warning {
            color: #9a3412;
            background: #ffedd5 !important;
        }

        .cards {
            display: grid;
            grid-template-columns:
                repeat(auto-fit, minmax(230px, 1fr));
            gap: 16px;
            margin-bottom: 20px;
        }

        .card {
            padding: 22px;
            text-align: center;
            background: white;
            border-radius: 16px;
            box-shadow: 0 4px 14px rgba(0, 0, 0, 0.08);
        }

        .label {
            color: #64748b;
            font-size: 17px;
        }

        .value {
            margin-top: 8px;
            color: #075985;
            font-size: 38px;
            font-weight: bold;
        }

        .chart-card {
            margin-bottom: 20px;
            padding: 18px;
            background: white;
            border-radius: 16px;
            box-shadow: 0 4px 14px rgba(0, 0, 0, 0.08);
        }

        .chart-card h2 {
            margin-top: 0;
            margin-bottom: 10px;
            color: #334155;
            font-size: 20px;
        }

        canvas {
            display: block;
            width: 100%;
            height: 240px;
            border-radius: 8px;
            background: #ffffff;
        }

        .details {
            padding: 18px;
            line-height: 1.9;
            background: white;
            border-radius: 16px;
            box-shadow: 0 4px 14px rgba(0, 0, 0, 0.08);
        }

        .details strong {
            color: #334155;
        }

        .footer {
            margin-top: 18px;
            color: #64748b;
            font-size: 13px;
            text-align: center;
        }

        @media (max-width: 600px) {
            body {
                padding: 12px;
            }

            canvas {
                height: 200px;
            }

            .value {
                font-size: 32px;
            }
        }
    </style>
</head>

<body>
    <div class="container">
        <h1>ESP-NOW Meteo</h1>

        <p class="subtitle">
            Ricevitore WeMos D1 Mini
        </p>

        <div id="status">
            In attesa dei dati
        </div>

        <div class="cards">
            <div class="card">
                <div class="label">
                    Temperatura
                </div>

                <div id="temperature" class="value">
                    --.- °C
                </div>
            </div>

            <div class="card">
                <div class="label">
                    Umidita
                </div>

                <div id="humidity" class="value">
                    --.- %
                </div>
            </div>
        </div>

        <div class="chart-card">
            <h2>
                Storico temperatura
            </h2>

            <canvas id="temperatureChart"></canvas>
        </div>

        <div class="chart-card">
            <h2>
                Storico umidita
            </h2>

            <canvas id="humidityChart"></canvas>
        </div>

        <div class="details">
            <strong>Mittente:</strong>
            <span id="sender">--</span>
            <br>

            <strong>Pacchetti ricevuti:</strong>
            <span id="packets">0</span>
            <br>

            <strong>Campioni memorizzati:</strong>
            <span id="samples">0</span> / 100
            <br>

            <strong>Ultimo aggiornamento:</strong>
            <span id="lastUpdate">--</span>
        </div>

        <div class="footer">
            La pagina si aggiorna automaticamente ogni 2 secondi.
        </div>
    </div>

    <script>
        let historyData = [];

        function resizeCanvas(canvas) {
            const ratio = window.devicePixelRatio || 1;
            const rectangle = canvas.getBoundingClientRect();

            const width =
                Math.max(300, Math.floor(rectangle.width));

            const height =
                Math.max(180, Math.floor(rectangle.height));

            const internalWidth =
                Math.floor(width * ratio);

            const internalHeight =
                Math.floor(height * ratio);

            if (
                canvas.width !== internalWidth ||
                canvas.height !== internalHeight
            ) {
                canvas.width = internalWidth;
                canvas.height = internalHeight;
            }

            const context = canvas.getContext("2d");

            context.setTransform(ratio, 0, 0, ratio, 0, 0);

            return {
                context: context,
                width: width,
                height: height
            };
        }

        function drawChart(
            canvasId,
            values,
            color,
            unit,
            fixedMinimum,
            fixedMaximum
        ) {
            const canvas =
                document.getElementById(canvasId);

            const drawing =
                resizeCanvas(canvas);

            const context =
                drawing.context;

            const width =
                drawing.width;

            const height =
                drawing.height;

            context.clearRect(0, 0, width, height);

            const margin = {
                left: 48,
                right: 16,
                top: 18,
                bottom: 32
            };

            const graphWidth =
                width - margin.left - margin.right;

            const graphHeight =
                height - margin.top - margin.bottom;

            context.fillStyle = "#ffffff";
            context.fillRect(0, 0, width, height);

            if (!values || values.length === 0) {
                context.fillStyle = "#64748b";
                context.font = "14px Arial";
                context.textAlign = "center";

                context.fillText(
                    "In attesa dei campioni",
                    width / 2,
                    height / 2
                );

                return;
            }

            let minimum =
                fixedMinimum !== null
                    ? fixedMinimum
                    : Math.min(...values);

            let maximum =
                fixedMaximum !== null
                    ? fixedMaximum
                    : Math.max(...values);

            if (fixedMinimum === null) {
                minimum -= 1;
            }

            if (fixedMaximum === null) {
                maximum += 1;
            }

            if (maximum <= minimum) {
                maximum = minimum + 1;
            }

            const range =
                maximum - minimum;

            context.strokeStyle = "#e2e8f0";
            context.lineWidth = 1;

            context.fillStyle = "#64748b";
            context.font = "11px Arial";

            for (let line = 0; line <= 4; line++) {
                const y =
                    margin.top +
                    (graphHeight * line / 4);

                context.beginPath();
                context.moveTo(margin.left, y);
                context.lineTo(
                    margin.left + graphWidth,
                    y
                );
                context.stroke();

                const value =
                    maximum -
                    (range * line / 4);

                context.textAlign = "right";

                context.fillText(
                    value.toFixed(1) + unit,
                    margin.left - 6,
                    y + 4
                );
            }

            context.strokeStyle = "#94a3b8";
            context.lineWidth = 1;

            context.beginPath();
            context.moveTo(
                margin.left,
                margin.top
            );
            context.lineTo(
                margin.left,
                margin.top + graphHeight
            );
            context.lineTo(
                margin.left + graphWidth,
                margin.top + graphHeight
            );
            context.stroke();

            context.strokeStyle = color;
            context.lineWidth = 2.5;
            context.lineJoin = "round";
            context.lineCap = "round";

            context.beginPath();

            values.forEach((value, index) => {
                const x =
                    values.length === 1
                        ? margin.left + graphWidth / 2
                        : margin.left +
                          graphWidth *
                          index /
                          (values.length - 1);

                const y =
                    margin.top +
                    graphHeight *
                    (maximum - value) /
                    range;

                if (index === 0) {
                    context.moveTo(x, y);
                } else {
                    context.lineTo(x, y);
                }
            });

            context.stroke();

            if (values.length <= 30) {
                context.fillStyle = color;

                values.forEach((value, index) => {
                    const x =
                        values.length === 1
                            ? margin.left + graphWidth / 2
                            : margin.left +
                              graphWidth *
                              index /
                              (values.length - 1);

                    const y =
                        margin.top +
                        graphHeight *
                        (maximum - value) /
                        range;

                    context.beginPath();
                    context.arc(x, y, 3, 0, Math.PI * 2);
                    context.fill();
                });
            }

            context.fillStyle = "#64748b";
            context.font = "11px Arial";
            context.textAlign = "left";

            context.fillText(
                "Piu vecchio",
                margin.left,
                height - 8
            );

            context.textAlign = "right";

            context.fillText(
                "Piu recente",
                margin.left + graphWidth,
                height - 8
            );
        }

        function redrawCharts() {
            const temperatures =
                historyData.map(
                    sample => sample.temperature
                );

            const humidities =
                historyData.map(
                    sample => sample.humidity
                );

            drawChart(
                "temperatureChart",
                temperatures,
                "#dc2626",
                "°",
                null,
                null
            );

            drawChart(
                "humidityChart",
                humidities,
                "#0284c7",
                "%",
                0,
                100
            );
        }

        async function updateCurrentData() {
            try {
                const response =
                    await fetch(
                        "/api/current",
                        { cache: "no-store" }
                    );

                if (!response.ok) {
                    throw new Error("Errore HTTP");
                }

                const data =
                    await response.json();

                document.getElementById(
                    "packets"
                ).textContent = data.packets;

                document.getElementById(
                    "samples"
                ).textContent = data.samples;

                const status =
                    document.getElementById("status");

                if (data.valid) {
                    document.getElementById(
                        "temperature"
                    ).textContent =
                        data.temperature.toFixed(1) +
                        " °C";

                    document.getElementById(
                        "humidity"
                    ).textContent =
                        data.humidity.toFixed(1) +
                        " %";

                    document.getElementById(
                        "sender"
                    ).textContent =
                        data.sender;

                    document.getElementById(
                        "lastUpdate"
                    ).textContent =
                        data.ageSeconds +
                        " secondi fa";

                    if (data.online) {
                        status.textContent =
                            "Dati ricevuti";

                        status.className =
                            "status-ok";
                    } else {
                        status.textContent =
                            "Trasmettitore non aggiornato";

                        status.className =
                            "status-warning";
                    }
                }
            }
            catch (error) {
                const status =
                    document.getElementById("status");

                status.textContent =
                    "Errore di comunicazione";

                status.className =
                    "status-warning";
            }
        }

        async function updateHistory() {
            try {
                const response =
                    await fetch(
                        "/api/history",
                        { cache: "no-store" }
                    );

                if (!response.ok) {
                    throw new Error("Errore HTTP");
                }

                historyData =
                    await response.json();

                redrawCharts();
            }
            catch (error) {
                console.log(
                    "Errore storico:",
                    error
                );
            }
        }

        async function updatePage() {
            await Promise.all([
                updateCurrentData(),
                updateHistory()
            ]);
        }

        window.addEventListener(
            "resize",
            redrawCharts
        );

        updatePage();

        setInterval(
            updatePage,
            2000
        );
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

// ============================================================
// GESTIONE BUFFER CIRCOLARE
// ============================================================

void addHistorySample(
    float temperature,
    float humidity)
{
    uint16_t writeIndex;

    if (historyCount < HISTORY_SIZE)
    {
        writeIndex =
            (historyStart + historyCount)
            % HISTORY_SIZE;

        historyCount++;
    }
    else
    {
        writeIndex = historyStart;

        historyStart =
            (historyStart + 1)
            % HISTORY_SIZE;
    }

    historyBuffer[writeIndex].temperature =
        temperature;

    historyBuffer[writeIndex].humidity =
        humidity;

    historyBuffer[writeIndex].timestampSeconds =
        millis() / 1000;
}

// Restituisce l'indice fisico corrispondente
// alla posizione cronologica richiesta.
uint16_t getHistoryIndex(
    uint16_t chronologicalPosition)
{
    return
        (historyStart + chronologicalPosition)
        % HISTORY_SIZE;
}

// ============================================================
// CALLBACK ESP-NOW
//
// La callback resta breve e non usa String, Serial o server web.
// ============================================================

void onDataReceived(
    uint8_t *mac,
    uint8_t *incomingData,
    uint8_t length)
{
    if (
        mac == nullptr ||
        incomingData == nullptr ||
        length != sizeof(SensorData)
    )
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
// ELABORAZIONE PACCHETTO NEL LOOP
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

    uint8_t localSenderMac[6];

    memcpy(
        localSenderMac,
        pendingSenderMac,
        sizeof(localSenderMac));

    pendingPacketAvailable = false;

    interrupts();

    if (
        isnan(localData.temperature) ||
        isnan(localData.humidity)
    )
    {
        Serial.println(
            "Pacchetto ignorato: valori non validi");

        return;
    }

    receivedData = localData;

    formatMacAddress(
        localSenderMac,
        senderMacString,
        sizeof(senderMacString));

    hasReceivedData = true;
    packetCounter++;
    lastUpdateMs = millis();

    addHistorySample(
        receivedData.temperature,
        receivedData.humidity);

    Serial.println(
        "--------------------------------");

    Serial.print("Da: ");
    Serial.println(senderMacString);

    Serial.printf(
        "Temperatura: %.1f C\n",
        receivedData.temperature);

    Serial.printf(
        "Umidita': %.1f %%\n",
        receivedData.humidity);

    Serial.printf(
        "Pacchetti ricevuti: %lu\n",
        static_cast<unsigned long>(
            packetCounter));

    Serial.printf(
        "Campioni nello storico: %u/%u\n",
        historyCount,
        HISTORY_SIZE);
}

// ============================================================
// PAGINA WEB PRINCIPALE
// ============================================================

void handleRoot()
{
    server.send_P(
        200,
        "text/html; charset=utf-8",
        INDEX_HTML);
}

// ============================================================
// API DATI CORRENTI
// ============================================================

void handleCurrentData()
{
    uint32_t ageMs = 0;

    if (hasReceivedData)
    {
        ageMs =
            millis() - lastUpdateMs;
    }

    const bool senderOnline =
        hasReceivedData &&
        ageMs <= OFFLINE_TIMEOUT_MS;

    String json;
    json.reserve(240);

    json += F("{");

    json += F("\"valid\":");
    json +=
        hasReceivedData
            ? F("true")
            : F("false");

    json += F(",\"online\":");
    json +=
        senderOnline
            ? F("true")
            : F("false");

    json += F(",\"temperature\":");
    json += String(
        receivedData.temperature,
        1);

    json += F(",\"humidity\":");
    json += String(
        receivedData.humidity,
        1);

    json += F(",\"sender\":\"");

    if (hasReceivedData)
    {
        json += senderMacString;
    }
    else
    {
        json += F("--");
    }

    json += F("\"");

    json += F(",\"packets\":");
    json += String(packetCounter);

    json += F(",\"samples\":");
    json += String(historyCount);

    json += F(",\"ageSeconds\":");
    json += String(ageMs / 1000);

    json += F("}");

    server.sendHeader(
        F("Cache-Control"),
        F("no-store"));

    server.send(
        200,
        "application/json; charset=utf-8",
        json);
}

// ============================================================
// API STORICO DEI 100 CAMPIONI
// ============================================================

void handleHistory()
{
    String json;

    // Riserva memoria per ridurre riallocazioni e frammentazione.
    json.reserve(
        2 + historyCount * 65);

    json += F("[");

    for (
        uint16_t position = 0;
        position < historyCount;
        position++
    )
    {
        if (position > 0)
        {
            json += F(",");
        }

        const uint16_t index =
            getHistoryIndex(position);

        const HistorySample &sample =
            historyBuffer[index];

        json += F("{\"temperature\":");
        json += String(
            sample.temperature,
            1);

        json += F(",\"humidity\":");
        json += String(
            sample.humidity,
            1);

        json += F(",\"time\":");
        json += String(
            sample.timestampSeconds);

        json += F("}");
    }

    json += F("]");

    server.sendHeader(
        F("Cache-Control"),
        F("no-store"));

    server.send(
        200,
        "application/json; charset=utf-8",
        json);
}

// ============================================================
// ROUTE NON TROVATA
// ============================================================

void handleNotFound()
{
    server.send(
        404,
        "text/plain; charset=utf-8",
        "Pagina non trovata");
}

// ============================================================
// AVVIO ACCESS POINT
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

    if (!started)
    {
        return false;
    }

    Serial.println();
    Serial.println("Access Point creato");

    Serial.print("SSID: ");
    Serial.println(AP_SSID);

    Serial.print("Canale: ");
    Serial.println(WIFI_CHANNEL);

    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());

    Serial.print("MAC Station ESP-NOW: ");
    Serial.println(WiFi.macAddress());

    Serial.print("MAC Access Point: ");
    Serial.println(WiFi.softAPmacAddress());

    return true;
}

// ============================================================
// AVVIO ESP-NOW
// ============================================================

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

    Serial.println(
        "ESP-NOW inizializzato");

    Serial.println(
        "Ricevitore pronto");

    return true;
}

// ============================================================
// AVVIO SERVER WEB
// ============================================================

void startWebServer()
{
    server.on(
        "/",
        HTTP_GET,
        handleRoot);

    server.on(
        "/api/current",
        HTTP_GET,
        handleCurrentData);

    server.on(
        "/api/history",
        HTTP_GET,
        handleHistory);

    server.onNotFound(
        handleNotFound);

    server.begin();

    Serial.println(
        "Server web avviato");

    Serial.println(
        "Pagina: http://192.168.4.1");
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
        "================================");

    Serial.println(
        "ESP-NOW WEB RECEIVER");

    Serial.println(
        "Storico: 100 campioni");

    Serial.println(
        "================================");

    if (!startAccessPoint())
    {
        Serial.println(
            "ERRORE: Access Point non creato");

        while (true)
        {
            delay(1000);
        }
    }

    if (!startEspNow())
    {
        Serial.println(
            "ERRORE: inizializzazione ESP-NOW");

        while (true)
        {
            delay(1000);
        }
    }

    startWebServer();

    Serial.println();
    Serial.println(
        "Configurazione completata");
}

// ============================================================
// LOOP PRINCIPALE
// ============================================================

void loop()
{
    processPendingPacket();

    server.handleClient();

    yield();
}