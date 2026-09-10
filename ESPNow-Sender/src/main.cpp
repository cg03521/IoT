#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <DHT.h>

#define DHTPIN D4
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

// MAC ADDRESS DEL RICEVITORE
// SOSTITUIRE CON IL MAC REALE

uint8_t receiverMac[] =
{
    0xDC,
    0x4F,
    0x22,
    0x0B,
    0x66,
    0x65
};

struct SensorData
{
    float temperature;
    float humidity;
};

SensorData sensorData;

unsigned long lastSend = 0;

void onDataSent(uint8_t *mac_addr, uint8_t status)
{
    Serial.print("Invio: ");

    if (status == 0)
    {
        Serial.println("OK");
    }
    else
    {
        Serial.println("ERRORE");
    }
}

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("ESP-NOW DHT22 Sender");

    dht.begin();

    WiFi.mode(WIFI_STA);

    Serial.print("MAC Sender: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != 0)
    {
        Serial.println("Errore inizializzazione ESP-NOW");

        while (true)
        {
            delay(1000);
        }
    }

    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);

    esp_now_register_send_cb(onDataSent);

    if (esp_now_add_peer(
            receiverMac,
            ESP_NOW_ROLE_SLAVE,
            1,
            NULL,
            0) != 0)
    {
        Serial.println("Errore aggiunta peer");

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println("Pronto");
}

void loop()
{
    if (millis() - lastSend >= 5000)
    {
        lastSend = millis();

        float h = dht.readHumidity();
        float t = dht.readTemperature();

        if (!isnan(t) && !isnan(h))
        {
            sensorData.temperature = t;
            sensorData.humidity = h;

            Serial.printf(
                "T=%.1f°C  H=%.1f%%\n",
                t,
                h);

            uint8_t result = esp_now_send(
                receiverMac,
                (uint8_t *)&sensorData,
                sizeof(sensorData));

            if (result != 0)
            {
                Serial.printf(
                    "Errore invio ESP-NOW: %d\n",
                    result);
            }
        }
        else
        {
            Serial.println("Errore lettura DHT");
        }
    }
}
