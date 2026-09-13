#include <Arduino.h>
#include <ESP8266WiFi.h>

const uint32_t SLEEP_SECONDS = 30;

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
    Serial.println(
        "Entro in Deep Sleep...");

    Serial.flush();

    delay(1000);

    ESP.deepSleep(
        (uint64_t)SLEEP_SECONDS * 1000000ULL,
        WAKE_RF_DEFAULT);
}

void loop()
{
}
