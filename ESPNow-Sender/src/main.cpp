#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <DHT.h>

// ============================================================
// CONFIGURAZIONE NODO ED HARDWARE
// ============================================================

constexpr char LOCATION_LABEL[] = "Esterno";
#define DHTPIN D4
#define DHTTYPE DHT22

// Intervallo di invio: 60 secondi (in millisecondi)
constexpr uint32_t SEND_INTERVAL_MS = 60000; 
constexpr uint8_t WIFI_CHANNEL = 1;

uint8_t receiverMac[] = { 0x60, 0x01, 0x94, 0x74, 0x62, 0x05 };

struct __attribute__((packed)) SensorData {
    char location[16];
    float temperature;
    float humidity;
};

SensorData sensorData;
DHT dht(DHTPIN, DHTTYPE);

volatile bool sendCompleted = false;
volatile uint8_t sendStatus = 1;
uint32_t lastSendTime = 0;

void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendStatus = status;
    sendCompleted = true;
}

void readAndSendData() {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(t) || isnan(h)) {
        Serial.println(F("Errore di lettura dal sensore DHT22!"));
        return;
    }

    Serial.printf("Lettura [%s]: Temp = %.1f °C, Umidita = %.1f %%\n", LOCATION_LABEL, t, h);

    memset(&sensorData, 0, sizeof(sensorData));
    strncpy(sensorData.location, LOCATION_LABEL, sizeof(sensorData.location) - 1);
    sensorData.temperature = t;
    sensorData.humidity = h;

    sendCompleted = false;
    uint8_t result = esp_now_send(receiverMac, (uint8_t *)&sensorData, sizeof(sensorData));

    if (result != 0) {
        Serial.printf("Errore invocazione esp_now_send: %d\n", result);
    } else {
        uint32_t startWait = millis();
        while (!sendCompleted && (millis() - startWait < 250)) {
            delay(1);
        }

        if (sendCompleted) {
            Serial.print(F("Stato invio ESP-NOW: "));
            Serial.println(sendStatus == 0 ? F("OK") : F("ERRORE (No ACK dal Ricevitore)"));
        } else {
            Serial.println(F("Stato invio ESP-NOW: TIMEOUT"));
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println(F("--- Wemos D1 Mini: Sender ESP-NOW (Continuous Mode) ---"));

    dht.begin();

    // Attesa necessaria per la stabilizzazione del DHT22 all'accensione
    delay(2000);

    // Inizializzazione Wi-Fi in Station Mode permanente
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    wifi_set_channel(WIFI_CHANNEL);
    delay(100);

    // Inizializzazione ESP-NOW
    if (esp_now_init() != 0) {
        Serial.println(F("Errore critico durante l'inizializzazione di ESP-NOW!"));
        return;
    }

    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);

    if (esp_now_add_peer(receiverMac, ESP_NOW_ROLE_SLAVE, WIFI_CHANNEL, NULL, 0) != 0) {
        Serial.println(F("Errore registrazione peer!"));
    }

    // Primo invio immediato al boot
    readAndSendData();
    lastSendTime = millis();
}

void loop() {
    // Esecuzione temporizzata ogni 60 secondi
    if (millis() - lastSendTime >= SEND_INTERVAL_MS) {
        lastSendTime = millis();
        readAndSendData();
    }
}