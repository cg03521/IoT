#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <math.h>

// ============================================================================
// CONFIGURAZIONE
// ============================================================================

constexpr char LOCATION_LABEL[] = "Esterno";

constexpr uint32_t SLEEP_SECONDS = 300;
constexpr uint8_t WIFI_CHANNEL = 1;

uint8_t receiverMac[] = {
    0x60, 0x01, 0x94, 0x74, 0x62, 0x05
};

constexpr uint32_t RTC_OFFSET = 65;
constexpr uint32_t RTC_MAGIC_KEY = 0xABCD1234;

// ============================================================================
// LIMITI DI VALIDAZIONE
// ============================================================================

constexpr float MIN_TEMP     = -20.0F;
constexpr float MAX_TEMP     = 60.0F;

constexpr float MIN_HUMIDITY = 0.0F;
constexpr float MAX_HUMIDITY = 100.0F;

constexpr float MIN_PRESSURE = 850.0F;
constexpr float MAX_PRESSURE = 1100.0F;

// Variazione massima ammessa in 5 minuti
constexpr float MAX_TEMP_DELTA     = 3.0F;
constexpr float MAX_HUMIDITY_DELTA = 15.0F;
constexpr float MAX_PRESSURE_DELTA = 2.0F;


// ============================================================================
// STRUTTURE
// ============================================================================

struct SensorData {
    char location[16];

    float temperature;
    float humidity;
    float pressure;
};


// RTC memory
//
// CRC deve essere l'ultimo campo.
//
// La struttura viene sempre azzerata prima di essere utilizzata,
// così anche eventuali byte di padding sono deterministici.
//

struct RTCData {
    uint32_t magic;

    uint32_t bootCount;

    uint8_t errorCount;
    uint8_t previousValid;

    uint16_t reserved;

    float prevTemp;
    float prevHum;
    float prevPress;

    uint32_t sendOK;
    uint32_t sendErrors;
    uint32_t filterErrors;

    char lastResetReason[32];

    uint32_t crc;
};


SensorData sensorData;
RTCData rtcData;

Adafruit_BME280 bme;

uint32_t bootStartMillis = 0;


// ============================================================================
// ESP-NOW CALLBACK
// ============================================================================

volatile bool sendCompleted = false;
volatile uint8_t sendStatus = 1;

void onDataSent(uint8_t *mac_addr, uint8_t status)
{
    sendStatus = status;
    sendCompleted = true;
}


// ============================================================================
// CRC32
// ============================================================================

uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;

    while (length--)
    {
        uint8_t byte = *data++;

        crc ^= byte;

        for (uint8_t i = 0; i < 8; i++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }

    return crc ^ 0xFFFFFFFF;
}


// ============================================================================
// LETTURA RTC
// ============================================================================

bool readRTC()
{
    RTCData temp;

    memset(&temp, 0, sizeof(temp));

    bool ok = ESP.rtcUserMemoryRead(
        RTC_OFFSET,
        reinterpret_cast<uint32_t*>(&temp),
        sizeof(temp)
    );

    if (!ok)
    {
        Serial.println(F("RTC: errore lettura"));
        return false;
    }

    // Controllo MAGIC
    if (temp.magic != RTC_MAGIC_KEY)
    {
        Serial.println(F("RTC: MAGIC non valido"));
        return false;
    }

    // Salva CRC presente
    uint32_t storedCRC = temp.crc;

    // Esclude il CRC dal calcolo
    temp.crc = 0;

    uint32_t calculatedCRC =
        crc32(
            reinterpret_cast<const uint8_t*>(&temp),
            sizeof(temp)
        );

    if (storedCRC != calculatedCRC)
    {
        Serial.println(F("RTC: CRC NON VALIDO"));
        return false;
    }

    rtcData = temp;

    return true;
}


// ============================================================================
// SCRITTURA RTC
// ============================================================================

bool writeRTC()
{
    rtcData.magic = RTC_MAGIC_KEY;

    // CRC escluso dal calcolo
    rtcData.crc = 0;

    rtcData.crc =
        crc32(
            reinterpret_cast<const uint8_t*>(&rtcData),
            sizeof(rtcData)
        );

    bool ok = ESP.rtcUserMemoryWrite(
        RTC_OFFSET,
        reinterpret_cast<uint32_t*>(&rtcData),
        sizeof(rtcData)
    );

    if (!ok)
    {
        Serial.println(F("RTC: errore scrittura"));
        return false;
    }

    return true;
}


// ============================================================================
// VALIDAZIONE SINGOLO VALORE
//
// Restituisce:
// true  = valore corrente utilizzabile
// false = valore corrente da sostituire con precedente
// ============================================================================

bool validateValue(
    float current,
    float minVal,
    float maxVal,
    float maxDelta,
    float previous,
    bool previousValid,
    const char* name
)
{
    // ------------------------------------------------------------
    // NaN / range assoluto
    // ------------------------------------------------------------

    if (isnan(current) ||
        current < minVal ||
        current > maxVal)
    {
        Serial.printf(
            "%s NON VALIDO: %.2f\n",
            name,
            current
        );

        return false;
    }


    // ------------------------------------------------------------
    // Delta rispetto alla precedente
    // ------------------------------------------------------------

    if (previousValid)
    {
        float delta = fabs(current - previous);

        if (delta > maxDelta)
        {
            Serial.printf(
                "%s SPIKE: prec=%.2f attuale=%.2f delta=%.2f max=%.2f\n",
                name,
                previous,
                current,
                delta,
                maxDelta
            );

            return false;
        }
    }

    return true;
}


// ============================================================================
// DEEP SLEEP
// ============================================================================

void goToSleep()
{
    uint32_t elapsed = millis() - bootStartMillis;

    Serial.printf(
        "Tempo attivo: %lu ms\n",
        (unsigned long)elapsed
    );

    Serial.printf(
        "Deep Sleep %lu s...\n",
        (unsigned long)SLEEP_SECONDS
    );

    Serial.flush();

    ESP.deepSleep(
        (uint64_t)SLEEP_SECONDS * 1000000ULL,
        WAKE_RF_DEFAULT
    );
}


// ============================================================================
// LETTURA BME280 + VALIDAZIONE + INVIO
// ============================================================================

bool readAndSendData()
{
    // ========================================================================
    // RTC
    // ========================================================================

    bool rtcValid = readRTC();

    if (!rtcValid)
    {
        Serial.println(
            F("RTC non valida: inizializzazione nuova struttura")
        );

        memset(&rtcData, 0, sizeof(rtcData));

        rtcData.magic = RTC_MAGIC_KEY;
        rtcData.bootCount = 0;
        rtcData.previousValid = 0;
    }


    // ========================================================================
    // BOOT COUNT
    // ========================================================================

    rtcData.bootCount++;

    Serial.printf(
        "Boot count: %lu\n",
        (unsigned long)rtcData.bootCount
    );


    // ========================================================================
    // RESET REASON
    // ========================================================================

    String resetReason = ESP.getResetReason();

    memset(
        rtcData.lastResetReason,
        0,
        sizeof(rtcData.lastResetReason)
    );

    strncpy(
        rtcData.lastResetReason,
        resetReason.c_str(),
        sizeof(rtcData.lastResetReason) - 1
    );

    Serial.printf(
        "Reset reason: %s\n",
        rtcData.lastResetReason
    );


    // ========================================================================
    // BME280 - MISURA FORCED
    // ========================================================================

    if (!bme.takeForcedMeasurement())
    {
        Serial.println(
            F("BME280: errore takeForcedMeasurement")
        );

        writeRTC();

        return false;
    }

    float rawTemp =
        bme.readTemperature();

    float rawHum =
        bme.readHumidity();

    float rawPress =
        bme.readPressure() / 100.0F;


    Serial.printf(
        "RAW -> Temp: %.2f | Hum: %.2f | Press: %.2f\n",
        rawTemp,
        rawHum,
        rawPress
    );


    // ========================================================================
    // VALIDAZIONE
    // ========================================================================

    bool validTemp = validateValue(
        rawTemp,
        MIN_TEMP,
        MAX_TEMP,
        MAX_TEMP_DELTA,
        rtcData.prevTemp,
        rtcData.previousValid,
        "Temperatura"
    );

    bool validHum = validateValue(
        rawHum,
        MIN_HUMIDITY,
        MAX_HUMIDITY,
        MAX_HUMIDITY_DELTA,
        rtcData.prevHum,
        rtcData.previousValid,
        "Umidita"
    );

    bool validPress = validateValue(
        rawPress,
        MIN_PRESSURE,
        MAX_PRESSURE,
        MAX_PRESSURE_DELTA,
        rtcData.prevPress,
        rtcData.previousValid,
        "Pressione"
    );


    // ========================================================================
    // COSTRUZIONE VALORI DA INVIARE
    //
    // Se il valore corrente è valido:
    //     usa quello corrente
    //
    // Se non è valido ma esiste un precedente:
    //     usa il precedente
    //
    // Se non è valido e NON esiste un precedente:
    //     impossibile costruire un pacchetto affidabile
    // ========================================================================

    float sendTemp;
    float sendHum;
    float sendPress;


    // ------------------------------------------------------------------------
    // TEMPERATURA
    // ------------------------------------------------------------------------

    if (validTemp)
    {
        sendTemp = rawTemp;
    }
    else if (rtcData.previousValid)
    {
        sendTemp = rtcData.prevTemp;

        Serial.printf(
            "Temperatura: uso precedente %.2f\n",
            sendTemp
        );
    }
    else
    {
        Serial.println(
            F("Temperatura invalida e nessun valore precedente")
        );

        rtcData.filterErrors++;

        writeRTC();

        return false;
    }


    // ------------------------------------------------------------------------
    // UMIDITA
    // ------------------------------------------------------------------------

    if (validHum)
    {
        sendHum = rawHum;
    }
    else if (rtcData.previousValid)
    {
        sendHum = rtcData.prevHum;

        Serial.printf(
            "Umidita: uso precedente %.2f\n",
            sendHum
        );
    }
    else
    {
        Serial.println(
            F("Umidita invalida e nessun valore precedente")
        );

        rtcData.filterErrors++;

        writeRTC();

        return false;
    }


    // ------------------------------------------------------------------------
    // PRESSIONE
    // ------------------------------------------------------------------------

    if (validPress)
    {
        sendPress = rawPress;
    }
    else if (rtcData.previousValid)
    {
        sendPress = rtcData.prevPress;

        Serial.printf(
            "Pressione: uso precedente %.2f\n",
            sendPress
        );
    }
    else
    {
        Serial.println(
            F("Pressione invalida e nessun valore precedente")
        );

        rtcData.filterErrors++;

        writeRTC();

        return false;
    }


    // ========================================================================
    // AGGIORNAMENTO DEL RIFERIMENTO RTC
    //
    // IMPORTANTE:
    //
    // salviamo solamente valori validati.
    //
    // Se la lettura corrente è uno spike, il precedente rimane.
    // ========================================================================

    if (validTemp)
        rtcData.prevTemp = rawTemp;

    if (validHum)
        rtcData.prevHum = rawHum;

    if (validPress)
        rtcData.prevPress = rawPress;


    rtcData.previousValid = 1;

    rtcData.errorCount = 0;


    // ========================================================================
    // COSTRUZIONE PACCHETTO
    // ========================================================================

    memset(
        &sensorData,
        0,
        sizeof(sensorData)
    );

    strncpy(
        sensorData.location,
        LOCATION_LABEL,
        sizeof(sensorData.location) - 1
    );

    sensorData.temperature = sendTemp;
    sensorData.humidity = sendHum;
    sensorData.pressure = sendPress;


    Serial.printf(
        "DATI DA INVIARE -> Temp: %.2f | Hum: %.2f | Press: %.2f\n",
        sensorData.temperature,
        sensorData.humidity,
        sensorData.pressure
    );


    // ========================================================================
    // SALVA RTC PRIMA DELL'INVIO
    // ========================================================================

    if (!writeRTC())
    {
        Serial.println(
            F("ATTENZIONE: RTC non salvata correttamente")
        );
    }


    // ========================================================================
    // ESP-NOW SEND
    // ========================================================================

    sendCompleted = false;
    sendStatus = 1;


    uint8_t result = esp_now_send(
        receiverMac,
        reinterpret_cast<uint8_t*>(&sensorData),
        sizeof(sensorData)
    );


    if (result != 0)
    {
        Serial.printf(
            "Errore esp_now_send(): %d\n",
            result
        );

        rtcData.sendErrors++;
        writeRTC();

        return false;
    }


    // ========================================================================
    // ATTESA CALLBACK
    // ========================================================================

    uint32_t startWait = millis();

    while (!sendCompleted &&
           (millis() - startWait < 1000))
    {
        delay(1);
    }


    // ========================================================================
    // RISULTATO
    // ========================================================================

    if (sendCompleted)
    {
        if (sendStatus == 0)
        {
            Serial.println(
                F("ESP-NOW: INVIO OK")
            );

            rtcData.sendOK++;

            writeRTC();

            return true;
        }
        else
        {
            Serial.printf(
                "ESP-NOW: INVIO FALLITO, status=%u\n",
                sendStatus
            );

            rtcData.sendErrors++;

            writeRTC();

            return false;
        }
    }


    // ========================================================================
    // TIMEOUT
    // ========================================================================

    Serial.println(
        F("ESP-NOW: TIMEOUT")
    );

    rtcData.sendErrors++;

    writeRTC();

    return false;
}


// ============================================================================
// SETUP
// ============================================================================

void setup()
{
    bootStartMillis = millis();

    Serial.begin(115200);

    delay(50);

    Serial.println();
    Serial.println(F("================================"));
    Serial.println(F("       ESP8266 SENSOR NODE      "));
    Serial.println(F("================================"));
    Serial.println();


    // ========================================================================
    // I2C
    // ========================================================================

    Wire.begin(D2, D1);

    Wire.setClock(100000);


    // ========================================================================
    // BME280
    // ========================================================================

    bool found = bme.begin(0x76);

    if (!found)
    {
        Serial.println(
            F("BME280 0x76 non trovato, provo 0x77")
        );

        found = bme.begin(0x77);
    }


    if (!found)
    {
        Serial.println(
            F("ERRORE: BME280 non trovato")
        );

        goToSleep();
        return;
    }


    Serial.println(
        F("BME280 OK")
    );


    // ========================================================================
    // BME280 CONFIGURAZIONE
    // ========================================================================

    bme.setSampling(
        Adafruit_BME280::MODE_FORCED,

        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1,

        Adafruit_BME280::FILTER_OFF
    );


    // ========================================================================
    // WIFI / ESP-NOW
    // ========================================================================

    WiFi.persistent(false);

    WiFi.mode(WIFI_STA);

    WiFi.disconnect();

    wifi_set_channel(WIFI_CHANNEL);


    Serial.printf(
        "WiFi channel: %u\n",
        WIFI_CHANNEL
    );

    Serial.print(
        "MAC Sender: "
    );

    Serial.println(
        WiFi.macAddress()
    );


    // ========================================================================
    // ESP-NOW INIT
    // ========================================================================

    if (esp_now_init() != 0)
    {
        Serial.println(
            F("ERRORE: esp_now_init()")
        );

        goToSleep();
        return;
    }


    Serial.println(
        F("ESP-NOW init OK")
    );


    // ========================================================================
    // ESP-NOW ROLE
    // ========================================================================

    esp_now_set_self_role(
        ESP_NOW_ROLE_CONTROLLER
    );


    // ========================================================================
    // CALLBACK
    // ========================================================================

    esp_now_register_send_cb(
        onDataSent
    );


    // ========================================================================
    // ADD PEER
    // ========================================================================

    uint8_t peerResult = esp_now_add_peer(
        receiverMac,
        ESP_NOW_ROLE_SLAVE,
        WIFI_CHANNEL,
        NULL,
        0
    );


    if (peerResult != 0)
    {
        Serial.printf(
            "ERRORE esp_now_add_peer(): %u\n",
            peerResult
        );

        goToSleep();
        return;
    }


    Serial.println(
        F("ESP-NOW peer OK")
    );


    // ========================================================================
    // LETTURA + INVIO
    // ========================================================================

    bool sendResult = readAndSendData();


    if (sendResult)
    {
        Serial.println(
            F("CICLO COMPLETATO: INVIO OK")
        );
    }
    else
    {
        Serial.println(
            F("CICLO COMPLETATO: INVIO FALLITO")
        );
    }


    // ========================================================================
    // DEEP SLEEP
    // ========================================================================

    goToSleep();
}


// ============================================================================
// LOOP
// ============================================================================

void loop()
{
    // Non utilizzato: il nodo lavora per un singolo ciclo
    // e torna in deep sleep.
}