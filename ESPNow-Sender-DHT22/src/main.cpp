#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <DHT.h>

constexpr char LOCATION_LABEL[] = "Salotto";
constexpr uint32_t SLEEP_SECONDS = 300;   // 5 minuti
constexpr uint8_t WIFI_CHANNEL = 1;

uint8_t receiverMac[] = {0x60, 0x01, 0x94, 0x74, 0x62, 0x05};

struct __attribute__((packed)) SensorData {
    char location[16];
    float temperature;
    float humidity;
    float pressure;
};

SensorData sensorData;

#define DHTPIN D4
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

volatile bool sendCompleted = false;
volatile uint8_t sendStatus = 1;

void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendStatus = status;
    sendCompleted = true;
}

void goToSleep() {
    Serial.printf("Deep Sleep %lu s...\n", (unsigned long)SLEEP_SECONDS);
    Serial.flush();
    ESP.deepSleep((uint64_t)SLEEP_SECONDS * 1000000ULL, WAKE_RF_DEFAULT);
}

bool readAndSendData() {

    float temperature = dht.readTemperature();
    float humidity    = dht.readHumidity();
    float pressure    = 0.0F;

    if (isnan(temperature) || isnan(humidity)) {
        Serial.println(F("Errore lettura DHT22"));
        return false;
    }

    Serial.printf("Temp: %.1f °C | Umid: %.1f %% | Press: %.1f hPa\n",
                  temperature, humidity, pressure);

    memset(&sensorData, 0, sizeof(sensorData));
    strncpy(sensorData.location, LOCATION_LABEL, sizeof(sensorData.location) - 1);

    sensorData.temperature = temperature;
    sensorData.humidity    = humidity;
    sensorData.pressure    = 0.0F;

    sendCompleted = false;

    uint8_t result = esp_now_send(receiverMac, (uint8_t *)&sensorData, sizeof(sensorData));
    if (result != 0) {
        Serial.printf("Errore esp_now_send: %d\n", result);
        return false;
    }

    uint32_t startWait = millis();
    while (!sendCompleted && (millis() - startWait < 500)) {
        delay(1);
    }

    if (sendCompleted) {
        Serial.printf("Invio ESP-NOW: %s\n", sendStatus == 0 ? "OK" : "ERRORE");
        return (sendStatus == 0);
    } else {
        Serial.println(F("Invio ESP-NOW TIMEOUT"));
        return false;
    }
}

void setup() {
    Serial.begin(115200);

    dht.begin();

    // Configurazione Wi-Fi veloce per ESP-NOW
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    wifi_set_channel(WIFI_CHANNEL);

    if (esp_now_init() != 0) {
        Serial.println(F("Errore ESP-NOW"));
        goToSleep();
        return;
    }

    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);
    esp_now_add_peer(receiverMac, ESP_NOW_ROLE_SLAVE, WIFI_CHANNEL, NULL, 0);

    // Esegui la lettura ed invio dati
    readAndSendData();

    // Va in sleep indipendentemente dall'esito dell'invio
    goToSleep();
}

void loop() {
    // Vuoto
}