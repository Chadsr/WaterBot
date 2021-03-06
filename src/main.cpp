#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <string.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Sensor.h>
#include <DHT_U.h>

#include "main.h"

struct SensorReadings {
  uint moisture[NUM_MOISTURE_SENSORS];
  uint avgMoisture;
  float humidity;
  float temperature;
};

TwoWire tw = TwoWire(1); // FOR SDA/SCL of OLED display
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &tw, OLED_RESET); // OLED DISPLAY
WebServer server(API_PORT); // For JSON API
DHT_Unified dht(PIN_DHT, DHT_TYPE);

// The struct we will be keeping an up to date record of our readings in
struct SensorReadings sensorReadings = {0};

// Returns a moisture value between 0 and 100, where 0 is dryest and 100 is fully submersed
void updateMoistureReadings() {
  digitalWrite(PIN_MOISTURE_VCC, HIGH); // Turn the sensors on
  delay(100); // Small delay to make sure the sensor(s) have power

  // Read from the available moisture sensors and 
  for (int i = 0; i < NUM_MOISTURE_SENSORS; i++) {
    int adc = analogRead(PINS_MOISTURE[i]);
    // If we got an erroneous reading, just disregard it
    if (adc >= MAX_MOISTURE_VALUE && adc <= MIN_MOISTURE_VALUE) {
      // Map the analog value to a value between 0 and 100 for easier reading
      sensorReadings.moisture[i] = uint(map(adc, MIN_MOISTURE_VALUE, MAX_MOISTURE_VALUE, 0, 100));
    }
  }

  digitalWrite(PIN_MOISTURE_VCC, LOW); // Turn the sensors off again

  // Get the average value of all moisture sensors
  int avgMoisture = 0;
  for (int i = 0; i < NUM_MOISTURE_SENSORS; i++) {
    avgMoisture += sensorReadings.moisture[i];
  }
  sensorReadings.avgMoisture = (uint) round(avgMoisture / NUM_MOISTURE_SENSORS);
}

void updateTempHumidity() {
  sensors_event_t event;

  dht.temperature().getEvent(&event);
  if (isnan(event.temperature)) {
    sensorReadings.temperature = 0;
  } else {
    sensorReadings.temperature = event.temperature;
  }

  dht.humidity().getEvent(&event);
  if (isnan(event.relative_humidity)) {
    sensorReadings.humidity = 0;
  } else {
    sensorReadings.humidity = event.relative_humidity;
  }
}

void updateSensorReadings() {
  updateMoistureReadings();
  updateTempHumidity();
}

void pumpWater(int seconds) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.printf("Watering for %is...", seconds);
  display.setCursor(0, 10);
  display.printf("Then waiting %is...", WAIT_SECONDS);
  display.display();
  digitalWrite(PIN_PUMP, HIGH); // Trigger the relay
  delay(seconds * 1000); // Wait for the given number of seconds before closing the relay again
  digitalWrite(PIN_PUMP, LOW);
}

void displayReadings() {
  int spacing = int(OLED_HEIGHT / 3);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.printf("%s %i%%", MOISTURE_MSG, sensorReadings.avgMoisture);
  display.setCursor(0, spacing);
  display.printf("%s %.1f%cC", AIR_TEMP_MSG, sensorReadings.temperature, (char)247);
  display.setCursor(0, spacing * 2);
  display.printf("%s %.1f%%", HUMIDITY_MSG, sensorReadings.humidity);
  display.setCursor(0, spacing * 3);  
  display.display();
}

void getReadingsJSON(char *dest, size_t size) {
  StaticJsonDocument<JSON_ARRAY_SIZE(NUM_MOISTURE_SENSORS) + JSON_OBJECT_SIZE(4) + 50> json;

  json.createNestedArray("moisture_values");
  JsonArray moistureValues = json["moisture_values"].as<JsonArray>();

  for (int i = 0; i < NUM_MOISTURE_SENSORS; i++) {
    moistureValues.add(sensorReadings.moisture[i]);
  }

  json["avg_moisture"].set(sensorReadings.avgMoisture);
  json["humidity"].set(sensorReadings.humidity);
  json["temperature"].set(sensorReadings.temperature);

  serializeJson(json, dest, size);
}

void handleNotFound() {
  server.send(404, "text/plain", "Page Not Found");
}

void handleReadings() {
  char buffer[JSON_BUFF_SIZE];
  getReadingsJSON(buffer, JSON_BUFF_SIZE);
  server.send(200, "text/json", String(buffer));
}

void listenerLoop(void * pvParameters) {
  while(true) { // Infinite loopings for the HTTP listener
    server.handleClient();
  }
}

void loop() {
  updateSensorReadings();
  displayReadings();

  // Trigger the pumping process if the average moisture hits our LOW_MOISTURE_TRIGGER value
  // Do nothing if the reading is zero. This likely means no sensor is connected (or it's broken).
  if (sensorReadings.avgMoisture != 0 && sensorReadings.avgMoisture <= LOW_MOISTURE_TRIGGER) {
    while (sensorReadings.avgMoisture < TARGET_MOISTURE) {
      pumpWater(PUMP_SECONDS);
      displayReadings();
      delay(WAIT_SECONDS * 1000);
      updateSensorReadings(); // Read the sensors again
      displayReadings();
    }
  } else {
    delay(WAIT_SECONDS * 1000);
  }
}

void setupWireless() {
  // Prevent power-saving mode
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);

  // Attempt network connection on loop until success
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  bool inverter = true; // Value used to make a simple waiting animation
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("Connecting to:");
    display.setCursor(0, 10);
    display.print(WIFI_SSID);
    display.setCursor(0, 20);

    if (inverter == true) {
      display.print("\\");
    } else {
      display.print("/");
    }

    display.display();
    inverter  = !inverter;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Connected!");
  display.setCursor(0, 10);
  display.print("IP address:");
  display.setCursor(0, 20);
  char buffer[15]; // Buffer large enough to hold an IPv4 address as string
  WiFi.localIP().toString().toCharArray(buffer, 15);
  Serial.println(buffer);
  display.print(buffer);
  display.display();
  delay(5000); // Delay so the IP can be read

  server.onNotFound(handleNotFound);
  server.on(API_ENDPOINT, HTTP_GET, handleReadings); // The http endpoint for GETting values
  server.begin();

  // We are taking advantage of the freeRTOS API function to spin up a separate thread for listening for http clients
  // This way it won't interfere with any of our other functionality
  xTaskCreate(listenerLoop, "HTTP Server Listener", 4096, NULL, 1, NULL);
}

void setup() {
  dht.begin();
  tw.begin(PIN_SDA, PIN_SCL);
  Serial.begin(9600);

  // Setup the pump and make sure it's off
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);

  // Setup the moisture sensor power pin and make sure it's off
  pinMode(PIN_MOISTURE_VCC, OUTPUT);
  digitalWrite(PIN_MOISTURE_VCC, LOW);

  delay(3000); // DM OLED has resistors R3 and R4 swapped, causing reset time of 2.7sec... Here's a hack for stock units
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);// initialize with the I2C addr 0x3C

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1.5);

  setupWireless();
}