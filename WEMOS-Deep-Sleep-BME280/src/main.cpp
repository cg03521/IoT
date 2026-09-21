#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include "Adafruit_BME280.h"

const uint32_t SLEEP_SECONDS = 30;

Adafruit_BME280 bme;

void setup()
{
    Serial.begin(115200);

    delay(2000);

    Serial.println();
    Serial.println("==========================");
    Serial.println("WAKE UP");
    Serial.println("==========================");

    Serial.printf(
        "Tempo sleep: %lu secondi\n",
        (unsigned long)SLEEP_SECONDS);

    Serial.printf(
        "Reset reason: %s\n",
        ESP.getResetReason().c_str());

    Serial.printf(
        "Heap libera: %u\n",
        ESP.getFreeHeap());

    Serial.println();

    // Inizializzazione BME280
    bool found = bme.begin(0x76);

    if (!found)
    {
        found = bme.begin(0x77);
    }

    if (!found)
    {
        Serial.println("ERRORE: BME280 non trovato");
    }
    else
    {
        float temperature =
            bme.readTemperature();

        float humidity =
            bme.readHumidity();

        float pressure =
            bme.readPressure() / 100.0F;

        Serial.println("=== BME280 ===");

        Serial.printf(
            "Temperatura : %.1f °C\n",
            temperature);

        Serial.printf(
            "Umidita     : %.1f %%\n",
            humidity);

        Serial.printf(
            "Pressione   : %.1f hPa\n",
            pressure);

        Serial.println();
    }

    Serial.println("Entro in Deep Sleep...");

    Serial.flush();

    delay(1000);

    ESP.deepSleep(
        (uint64_t)SLEEP_SECONDS * 1000000ULL,
        WAKE_RF_DEFAULT);
}

void loop()
{
}
