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

// Timeout dopo il quale un trasmettitore è considerato offline
constexpr uint32_t OFFLINE_TIMEOUT_MS = 180000;

// ============================================================
// STRUTTURE DATI
// ============================================================

constexpr uint16_t HISTORY_SIZE = 100;
constexpr uint8_t MAX_SENSORS = 8; // Supporto dinamico fino a 8 sensori

// Struttura inviata dal trasmettitore
struct SensorData
{
    char sensorName[16]; // Nome inviato dal trasmettitore (es. "Esterno", "Salotto")
    float temperature;
    float humidity;
};

struct HistorySample
{
    float temperature;
    float humidity;
    uint32_t timestampSeconds;
};

struct SensorNode
{
    uint8_t mac[6];
    char macStr[18];
    char name[24];
    SensorData currentData;
    bool hasData = false;
    uint32_t packetCount = 0;
    uint32_t lastUpdateMs = 0;

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
// DATI RICEVUTI DA CALLBACK
// ============================================================

volatile bool pendingPacketAvailable = false;
SensorData pendingData {};
uint8_t pendingSenderMac[6] = {};

// ============================================================
// PAGINA WEB DEDICATA (PROGMEM)
// ============================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="it">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Stazione Meteo ESP-NOW</title>
    <style>
        * { box-sizing: border-box; }
        body {
            margin: 0;
            padding: 20px;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            color: #1e293b;
            background: #f1f5f9;
        }
        .container { max-width: 960px; margin: 0 auto; }
        header { text-align: center; margin-bottom: 20px; }
        h1 { margin: 0 0 6px 0; color: #0f172a; font-size: 28px; }
        .subtitle { color: #64748b; margin: 0; font-size: 15px; }

        .tabs {
            display: flex;
            gap: 8px;
            margin-bottom: 20px;
            overflow-x: auto;
            padding-bottom: 4px;
        }
        .tab-btn {
            background: #e2e8f0;
            border: none;
            padding: 10px 20px;
            border-radius: 12px;
            font-weight: 600;
            color: #475569;
            cursor: pointer;
            transition: all 0.2s ease;
            white-space: nowrap;
        }
        .tab-btn.active {
            background: #2563eb;
            color: white;
            box-shadow: 0 4px 12px rgba(37, 99, 235, 0.25);
        }

        .status-badge {
            display: inline-block;
            padding: 6px 14px;
            border-radius: 20px;
            font-weight: 600;
            font-size: 13px;
            margin-bottom: 16px;
        }
        .status-ok { background: #dcfce7; color: #15803d; }
        .status-warning { background: #ffedd5; color: #c2410c; }
        .status-offline { background: #fee2e2; color: #b91c1c; }

        .cards {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
            gap: 16px;
            margin-bottom: 20px;
        }
        .card {
            background: white;
            padding: 20px;
            border-radius: 16px;
            box-shadow: 0 1px 3px rgba(0,0,0,0.05);
            border: 1px solid #e2e8f0;
        }
        .card-title { color: #64748b; font-size: 14px; font-weight: 500; }
        .card-value { font-size: 36px; font-weight: 700; margin-top: 8px; color: #0f172a; }

        .chart-card {
            background: white;
            padding: 20px;
            border-radius: 16px;
            box-shadow: 0 1px 3px rgba(0,0,0,0.05);
            border: 1px solid #e2e8f0;
            margin-bottom: 20px;
        }
        .chart-card h2 { margin: 0 0 16px 0; font-size: 18px; color: #334155; }
        canvas { width: 100%; height: 220px; display: block; }

        .details {
            background: white;
            padding: 18px;
            border-radius: 16px;
            border: 1px solid #e2e8f0;
            font-size: 14px;
            line-height: 1.8;
            color: #475569;
        }
        .details strong { color: #0f172a; }

        @media (max-width: 600px) {
            body { padding: 12px; }
            .card-value { font-size: 30px; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>Stazione Meteo ESP-NOW</h1>
            <p class="subtitle">Ricevitore Multi-Nodo</p>
        </header>

        <div id="tabs" class="tabs"></div>

        <div id="statusBadge" class="status-badge status-warning">In attesa dei dati...</div>

        <div class="cards">
            <div class="card">
                <div class="card-title">Temperatura</div>
                <div id="tempVal" class="card-value">--.- °C</div>
            </div>
            <div class="card">
                <div class="card-title">Umidità</div>
                <div id="humVal" class="card-value">--.- %</div>
            </div>
        </div>

        <div class="chart-card">
            <h2>Storico Temperatura</h2>
            <canvas id="tempChart"></canvas>
        </div>

        <div class="chart-card">
            <h2>Storico Umidità</h2>
            <canvas id="humChart"></canvas>
        </div>

        <div class="details">
            <div><strong>Modulo Sorgente:</strong> <span id="sensorName">--</span></div>
            <div><strong>Indirizzo MAC:</strong> <span id="sensorMac">--</span></div>
            <div><strong>Pacchetti Ricevuti:</strong> <span id="packetCount">0</span></div>
            <div><strong>Campioni nello Storico:</strong> <span id="sampleCount">0</span> / 100</div>
            <div><strong>Ultimo Aggiornamento:</strong> <span id="lastUpdate">--</span></div>
        </div>
    </div>

    <script>
        let selectedSensorIndex = 0;
        let sensorsData = [];

        function renderTabs() {
            const container = document.getElementById("tabs");
            container.innerHTML = "";
            sensorsData.forEach((sensor, idx) => {
                const btn = document.createElement("button");
                btn.className = "tab-btn " + (idx === selectedSensorIndex ? "active" : "");
                btn.textContent = sensor.name;
                btn.onclick = () => {
                    selectedSensorIndex = idx;
                    renderTabs();
                    updateDisplay();
                };
                container.appendChild(btn);
            });
        }

        function resizeCanvas(canvas) {
            const ratio = window.devicePixelRatio || 1;
            const rect = canvas.getBoundingClientRect();
            const width = rect.width;
            const height = rect.height;

            if (canvas.width !== width * ratio || canvas.height !== height * ratio) {
                canvas.width = width * ratio;
                canvas.height = height * ratio;
            }
            const ctx = canvas.getContext("2d");
            ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
            return { ctx, width, height };
        }

        function drawChart(canvasId, values, color, unit, minVal, maxVal) {
            const canvas = document.getElementById(canvasId);
            const { ctx, width, height } = resizeCanvas(canvas);
            ctx.clearRect(0, 0, width, height);

            const margin = { top: 20, right: 15, bottom: 25, left: 45 };
            const graphWidth = width - margin.left - margin.right;
            const graphHeight = height - margin.top - margin.bottom;

            if (!values || values.length === 0) {
                ctx.fillStyle = "#94a3b8";
                ctx.font = "14px sans-serif";
                ctx.textAlign = "center";
                ctx.fillText("Nessun dato disponibile per questo sensore", width / 2, height / 2);
                return;
            }

            let min = minVal !== null ? minVal : Math.min(...values) - 0.5;
            let max = maxVal !== null ? maxVal : Math.max(...values) + 0.5;
            if (max <= min) max = min + 1;
            const range = max - min;

            ctx.strokeStyle = "#f1f5f9";
            ctx.lineWidth = 1;
            ctx.fillStyle = "#94a3b8";
            ctx.font = "11px sans-serif";

            for (let i = 0; i <= 4; i++) {
                const y = margin.top + (graphHeight * i / 4);
                ctx.beginPath();
                ctx.moveTo(margin.left, y);
                ctx.lineTo(margin.left + graphWidth, y);
                ctx.stroke();

                const val = max - (range * i / 4);
                ctx.textAlign = "right";
                ctx.fillText(val.toFixed(1) + unit, margin.left - 6, y + 4);
            }

            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.beginPath();

            values.forEach((v, i) => {
                const x = values.length === 1 
                    ? margin.left + graphWidth / 2 
                    : margin.left + (graphWidth * i / (values.length - 1));
                const y = margin.top + graphHeight * (max - v) / range;

                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.stroke();
        }

        function updateDisplay() {
            if (sensorsData.length === 0) return;
            const s = sensorsData[selectedSensorIndex];

            document.getElementById("sensorName").textContent = s.name;
            document.getElementById("sensorMac").textContent = s.mac;
            document.getElementById("packetCount").textContent = s.packets;
            document.getElementById("sampleCount").textContent = s.samples;

            const badge = document.getElementById("statusBadge");
            if (!s.valid) {
                badge.textContent = "Nessun dato ancora ricevuto";
                badge.className = "status-badge status-warning";
                document.getElementById("tempVal").textContent = "--.- °C";
                document.getElementById("humVal").textContent = "--.- %";
                document.getElementById("lastUpdate").textContent = "--";
            } else if (s.online) {
                badge.textContent = "Online - Aggiornato";
                badge.className = "status-badge status-ok";
                document.getElementById("tempVal").textContent = s.temperature.toFixed(1) + " °C";
                document.getElementById("humVal").textContent = s.humidity.toFixed(1) + " %";
                document.getElementById("lastUpdate").textContent = s.ageSeconds + " sec fa";
            } else {
                badge.textContent = "Offline - Segnale perso";
                badge.className = "status-badge status-offline";
                document.getElementById("tempVal").textContent = s.temperature.toFixed(1) + " °C";
                document.getElementById("humVal").textContent = s.humidity.toFixed(1) + " %";
                document.getElementById("lastUpdate").textContent = s.ageSeconds + " sec fa";
            }

            const temps = s.history.map(h => h.temperature);
            const hums = s.history.map(h => h.humidity);

            drawChart("tempChart", temps, "#ef4444", "°", null, null);
            drawChart("humChart", hums, "#0284c7", "%", 0, 100);
        }

        async function fetchData() {
            try {
                const res = await fetch("/api/data", { cache: "no-store" });
                if (!res.ok) return;
                const data = await res.json();
                
                const prevLength = sensorsData.length;
                sensorsData = data;
                
                if (prevLength !== sensorsData.length) {
                    renderTabs();
                }
                updateDisplay();
            } catch (e) {
                console.error("Errore fetch:", e);
            }
        }

        window.addEventListener("resize", updateDisplay);
        fetchData();
        setInterval(fetchData, 2000);
    </script>
</body>
</html>
)rawliteral";

// ============================================================
// FUNZIONI SUPPORTO
// ============================================================

void formatMacAddress(const uint8_t *mac, char *dest, size_t destSize)
{
    snprintf(dest, destSize, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int findOrRegisterSensor(const uint8_t *mac, const char *incomingName)
{
    for (uint8_t i = 0; i < activeSensorCount; i++)
    {
        if (memcmp(sensors[i].mac, mac, 6) == 0)
        {
            // Aggiorna il nome se è stato modificato sul trasmettitore
            if (incomingName && strlen(incomingName) > 0)
            {
                strncpy(sensors[i].name, incomingName, sizeof(sensors[i].name) - 1);
                sensors[i].name[sizeof(sensors[i].name) - 1] = '\0';
            }
            return i;
        }
    }

    if (activeSensorCount >= MAX_SENSORS) return -1;

    uint8_t idx = activeSensorCount++;
    memcpy(sensors[idx].mac, mac, 6);
    formatMacAddress(mac, sensors[idx].macStr, sizeof(sensors[idx].macStr));

    if (incomingName && strlen(incomingName) > 0)
    {
        strncpy(sensors[idx].name, incomingName, sizeof(sensors[idx].name) - 1);
        sensors[idx].name[sizeof(sensors[idx].name) - 1] = '\0';
    }
    else
    {
        snprintf(sensors[idx].name, sizeof(sensors[idx].name), "Sensore %02X%02X", mac[4], mac[5]);
    }

    return idx;
}

void addHistorySample(SensorNode &sensor, float temp, float hum)
{
    uint16_t writeIdx;
    if (sensor.historyCount < HISTORY_SIZE)
    {
        writeIdx = (sensor.historyStart + sensor.historyCount) % HISTORY_SIZE;
        sensor.historyCount++;
    }
    else
    {
        writeIdx = sensor.historyStart;
        sensor.historyStart = (sensor.historyStart + 1) % HISTORY_SIZE;
    }

    sensor.historyBuffer[writeIdx].temperature = temp;
    sensor.historyBuffer[writeIdx].humidity = hum;
    sensor.historyBuffer[writeIdx].timestampSeconds = millis() / 1000;
}

// ============================================================
// CALLBACK ESP-NOW
// ============================================================

void onDataReceived(uint8_t *mac, uint8_t *incomingData, uint8_t length)
{
    if (!mac || !incomingData || length != sizeof(SensorData)) return;

    memcpy(&pendingData, incomingData, sizeof(pendingData));
    memcpy(pendingSenderMac, mac, sizeof(pendingSenderMac));
    pendingPacketAvailable = true;
}

void processPendingPacket()
{
    if (!pendingPacketAvailable) return;

    noInterrupts();
    SensorData localData = pendingData;
    uint8_t localMac[6];
    memcpy(localMac, pendingSenderMac, 6);
    pendingPacketAvailable = false;
    interrupts();

    if (isnan(localData.temperature) || isnan(localData.humidity)) return;

    int idx = findOrRegisterSensor(localMac, localData.sensorName);
    if (idx < 0) return;

    SensorNode &s = sensors[idx];
    s.currentData = localData;
    s.hasData = true;
    s.packetCount++;
    s.lastUpdateMs = millis();

    addHistorySample(s, localData.temperature, localData.humidity);
}

// ============================================================
// ENDPOINT API
// ============================================================

void handleDataApi()
{
    String json;
    json.reserve(2048);
    json += "[";

    uint32_t now = millis();

    for (uint8_t i = 0; i < activeSensorCount; i++)
    {
        if (i > 0) json += ",";
        SensorNode &s = sensors[i];
        uint32_t ageMs = s.hasData ? (now - s.lastUpdateMs) : 0;
        bool online = s.hasData && (ageMs <= OFFLINE_TIMEOUT_MS);

        json += "{";
        json += "\"name\":\"" + String(s.name) + "\",";
        json += "\"mac\":\"" + String(s.macStr) + "\",";
        json += "\"valid\":" + String(s.hasData ? "true" : "false") + ",";
        json += "\"online\":" + String(online ? "true" : "false") + ",";
        json += "\"temperature\":" + String(s.currentData.temperature, 1) + ",";
        json += "\"humidity\":" + String(s.currentData.humidity, 1) + ",";
        json += "\"packets\":" + String(s.packetCount) + ",";
        json += "\"samples\":" + String(s.historyCount) + ",";
        json += "\"ageSeconds\":" + String(ageMs / 1000) + ",";
        json += "\"history\":[";

        for (uint16_t h = 0; h < s.historyCount; h++)
        {
            if (h > 0) json += ",";
            uint16_t idx = (s.historyStart + h) % HISTORY_SIZE;
            json += "{\"temperature\":" + String(s.historyBuffer[idx].temperature, 1) + ",";
            json += "\"humidity\":" + String(s.historyBuffer[idx].humidity, 1) + "}";
        }
        json += "]}";
    }

    json += "]";
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.send(200, "application/json; charset=utf-8", json);
}

void handleRoot()
{
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

// ============================================================
// INIZIALIZZAZIONE & SETUP
// ============================================================

bool startAccessPoint()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();

    uint32_t startMs = millis();
    while (millis() - startMs < 100) { yield(); }

    return WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_CHANNEL);
}

bool startEspNow()
{
    if (esp_now_init() != 0) return false;
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataReceived);
    return true;
}

void setup()
{
    Serial.begin(115200);
    
    uint32_t initStart = millis();
    while (millis() - initStart < 1000) { yield(); }

    Serial.println("\n=== ESP-NOW MULTI-RECEIVER DYNAMIC ===");

    if (!startAccessPoint() || !startEspNow())
    {
        Serial.println("Errore Inizializzazione Hardware!");
        while (true) { yield(); }
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/data", HTTP_GET, handleDataApi);
    server.begin();

    Serial.println("Ricevitore Pronto!");
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