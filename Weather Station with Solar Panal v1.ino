/*
   Weather station with Wemos D1 mini

*/

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>

#include "Wire.h"

#include "DHTesp.h"
DHTesp dht;

ESP8266WiFiMulti WiFiMulti;

const int pinDHT = 2; 

String writeAPIKey = "YOURAPIKEY";
#define THING_SPEAK_ADDRESS "api.thingspeak.com"
#define TIMEOUT  5000 // Timeout for server response.

float temperature = 0;
float humidity = 0;

unsigned int raw = 0;
float volt = 0.0;

WiFiClient client;

void setup()
{
  Serial.begin(115200);
  Serial.setTimeout(2000);

  unsigned long time1 = millis();

  // Wait for serial to initialize.
  while (!Serial) { }

  Serial.println('\n');
  if (connect())
  {
    readSensor();

    readVolt();

    updateThingSpeak();
  }

  Serial.print("Milliseconds running time: ");
  Serial.println(millis() - time1);

  // hibernate
  Serial.println("Going into deep sleep ...");
  //ESP.deepSleep(60e6); // 1 min.
  ESP.deepSleep(300e6); // 5 min.
}

void readVolt()
{
  raw = analogRead(A0);
  delay(100);
  raw = analogRead(A0); // first reading is unreliable
  volt = raw / 1023.0 * 4.2;
  
}

void readSensor()
{
  dht.setup(pinDHT, DHTesp::DHT22);
  delay(dht.getMinimumSamplingPeriod());
  humidity = dht.getHumidity();
  temperature = dht.getTemperature();
}

bool connect()
{
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP("YOUR_SSID", "YOUR_PASSWORD");

  int ctr = 0;
  while (WiFiMulti.run() != WL_CONNECTED)
  {
    delay(100);
    ctr++;
    if (ctr > 70) // 7 seconds
    {
      Serial.println(" ");
      Serial.println("WiFi connection failed!");
      return false;
    }
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WiFi.SSID());

  return true;
}

void updateThingSpeak()
{
  // 1 -> temperature
  // 2 -> humidity
  // 3 -> volt

  if (client.connect(THING_SPEAK_ADDRESS, 80))
  {
    //Serial.println("- succesfully connected");
    String postData = "api_key=" + writeAPIKey;
    postData += "&field1=" + String(temperature);
    postData += "&field2=" + String(humidity);
    postData += "&field3=" + String(volt);

    client.println("POST /update HTTP/1.1");
    client.println("Host: api.thingspeak.com");
    client.println("Connection: close");
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println( "Content-Length: " + String( postData.length() ) );
    client.println();
    client.println( postData );

	// if you want check response
    //String answer = getResponse();

  }
  client.stop();
}

String getResponse() {
  String response;
  long startTime = millis();

  delay( 200 );
  while ( client.available() < 1 && (( millis() - startTime ) < TIMEOUT ) ) {
    delay( 5 );
  }

  if ( client.available() > 0 ) { // Get response from server
    char charIn;
    do {
      charIn = client.read(); // Read a char from the buffer.
      response += charIn;     // Append the char to the string response.
    } while ( client.available() > 0 );
  }
  client.stop();

  return response;
}

void loop() {}
