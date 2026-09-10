#include <Arduino.h>
#include <DHT.h>

#define DHTPIN D4
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("DHT22 TEST");

    dht.begin();

    delay(3000);
}

void loop()
{
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(t) && !isnan(h))
    {
        Serial.printf(
            "T=%.1f°C  H=%.1f%%\n",
            t,
            h
        );
    }
    else
    {
        Serial.println("Errore lettura DHT");
    }

    delay(2000);
}