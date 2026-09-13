#include <Arduino.h>
#include <ESP8266WiFi.h>

void printChipInfo()
{
    Serial.println();
    Serial.println("================================");
    Serial.println("ESP8266 DIAGNOSTIC");
    Serial.println("================================");

    Serial.printf(
        "Chip ID       : %06X\n",
        ESP.getChipId());

    Serial.printf(
        "CPU Freq      : %u MHz\n",
        ESP.getCpuFreqMHz());

    Serial.printf(
        "Core Version  : %s\n",
        ESP.getCoreVersion().c_str());

    Serial.printf(
        "SDK Version   : %s\n",
        ESP.getSdkVersion());

    Serial.printf(
        "Flash Chip ID : %08X\n",
        ESP.getFlashChipId());

    Serial.printf(
        "Flash Real    : %u KB\n",
        ESP.getFlashChipRealSize() / 1024);

    Serial.printf(
        "Flash IDE     : %u KB\n",
        ESP.getFlashChipSize() / 1024);

    Serial.printf(
        "Flash Speed   : %u Hz\n",
        ESP.getFlashChipSpeed());

    Serial.printf(
        "Free Heap     : %u bytes\n",
        ESP.getFreeHeap());

    Serial.printf(
        "Sketch Size   : %u bytes\n",
        ESP.getSketchSize());

    Serial.printf(
        "Free Sketch   : %u bytes\n",
        ESP.getFreeSketchSpace());

    Serial.println();

    Serial.print("MAC Address   : ");
    Serial.println(WiFi.macAddress());

    Serial.print("Hostname      : ");
    Serial.println(WiFi.hostname());

    Serial.println();

    Serial.print("Reset Reason  : ");
    Serial.println(ESP.getResetReason());

    Serial.println("================================");
}

void setup()
{
    Serial.begin(115200);

    delay(2000);

    pinMode(LED_BUILTIN, OUTPUT);

    WiFi.mode(WIFI_STA);

    printChipInfo();
}

void loop()
{
    digitalWrite(LED_BUILTIN, LOW);
    delay(250);

    digitalWrite(LED_BUILTIN, HIGH);
    delay(750);
}