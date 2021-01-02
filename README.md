
#include <BMP280_DEV.h>                           // Include the BMP280_DEV.h library

float temperature, pressure, altitude;            // Create the temperature, pressure and altitude variables
BMP280_DEV bmp280(SDA, SCL);                        // Instantiate (create) a BMP280 object and set-up for I2C operation on pins SDA: A6, SCL: A7

void setup()
{
  Serial.begin(9600);                           // Initialise the serial port
  delay(100);
  bmp280.begin(0x76);                                 // Default initialisation, place the BMP280 into SLEEP_MODE
  bmp280.setTimeStandby(TIME_STANDBY_2000MS);     // Set the standby time to 2 seconds
  bmp280.startNormalConversion();                 // Start BMP280 continuous conversion in NORMAL_MODE
}

void loop()
{
//  if (bmp280.getMeasurements(temperature, pressure, altitude))    // Check if the measurement is complete
//  {
//    Serial.print(temperature);                    // Display the results
//    Serial.print(F(" *C   "));
//    Serial.print(pressure);
//    Serial.println(F(" hPa   "));
//  }
delay(1000);
  bmp280.getCurrentTempPres(temperature, pressure);

  Serial.print(temperature);                    // Display the results
  Serial.print(F(" *C  Current "));
  Serial.print(pressure);
  Serial.println(F(" hPa Current  "));


}
