#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_MCP9808.h>
#include <Adafruit_GPS.h>
#include <WiFi.h>
#include <HTTPClient.h>

const char* SECRET_SSID = "";
const char* SECRET_PASS = "";
const char* serverUrl = "http://192.168.0.0:4000/data";
const char* authUrl = "http://192.168.0.0:4000/auth"; // URL to get the token

// Initialize sensors
Adafruit_MCP9808 tempsensor = Adafruit_MCP9808();
Adafruit_GPS GPS(&Serial1);

// Heart rate variables
#define heartPin A5
const int numReadings = 20;
int readings[numReadings] = {0};
int readIndex = 0, total = 0, average = 0, lastAverage = 0;
bool rising = false;
unsigned long lastPeakTime = 0;

// Buffer for average/highest BPM (10 values)
const int bufferSize = 10;
int bpmBuffer[bufferSize] = {0};
int bpmIndex = 0, totalBpm = 0, avgBpm = 0;
int highestBpm = 0;

// Separate buffer for lowest BPM (50 values)
const int lowestBufferSize = 50;
int lowestBpmBuffer[lowestBufferSize] = {0};
int lowestBpmIndex = 0;
int lowestBpm = 999;

unsigned long lastSendTime = 0;
String authToken = "";

// HTTP Task to send data
void sendHttpRequestTask(void* pvParameters) {
  String payload = *(String*)pvParameters;
  delete (String*)pvParameters;

  WiFiClient client;
  HTTPClient http;
  http.begin(client, serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + authToken);
  
  int httpCode = http.POST(payload);
  if (httpCode <= 0) {
    Serial.printf("HTTP Error: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("HTTP Code: %d\n", httpCode);
  }
  http.end();
  vTaskDelete(NULL);
}

void sendHttpRequestAsync(String payload) {
  String* payloadCopy = new String(payload);
  xTaskCreatePinnedToCore(sendHttpRequestTask, "HttpTask", 12288, payloadCopy, 1, NULL, 0);
}

// Function to get the token
void getAuthToken() {
  WiFiClient client;
  HTTPClient http;
  
  http.begin(client, "http://192.168.0.0:4000/login"); 
  http.addHeader("Content-Type", "application/json");

  // Prepare the login JSON
  String loginPayload = "{\"name\":\"John Pork\",\"password\":\"123\"}";
  
  int httpCode = http.POST(loginPayload);

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("Received login response: " + payload);
    
    // Extract the token
    int tokenStart = payload.indexOf("\"token\":\"") + 9;
    int tokenEnd = payload.indexOf("\"", tokenStart);
    if (tokenStart != -1 && tokenEnd != -1) {
      authToken = payload.substring(tokenStart, tokenEnd);
      Serial.println("Extracted token: " + authToken);
    } else {
      Serial.println("Token not found in response.");
    }
  } else {
    Serial.printf("Failed to get token. HTTP Error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}


void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  WiFi.begin(SECRET_SSID, SECRET_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");

  if (!tempsensor.begin()) {
    Serial.println("Couldn't find MCP9808 sensor!");
  }

  GPS.begin(9600);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
  GPS.sendCommand(PGCMD_ANTENNA);
  Wire.begin(21, 22);
  Serial.println("Sensors Initialized");

  // Get the authentication token
  getAuthToken();
}

void loop() {
  int heartValue = analogRead(heartPin);
  unsigned long currentMillis = millis();

  // Calculate moving average
  total -= readings[readIndex];
  readings[readIndex] = heartValue;
  total += readings[readIndex];
  readIndex = (readIndex + 1) % numReadings;
  average = total / numReadings;

  if (average > lastAverage) {
    rising = true;
  } else if (average < lastAverage && rising) {
    unsigned long currentTime = millis();
    if (lastPeakTime != 0) {
      unsigned long timeBetweenPeaks = currentTime - lastPeakTime;
      if (timeBetweenPeaks > 300) { 
        int bpm = 60000 / timeBetweenPeaks;

        totalBpm -= bpmBuffer[bpmIndex];
        bpmBuffer[bpmIndex] = bpm;
        totalBpm += bpmBuffer[bpmIndex];
        bpmIndex = (bpmIndex + 1) % bufferSize;
        avgBpm = totalBpm / bufferSize;

        // Update lowest buffer (50 values)
        lowestBpmBuffer[lowestBpmIndex] = bpm;
        lowestBpmIndex = (lowestBpmIndex + 1) % lowestBufferSize;

        highestBpm = 0;
        for (int i = 0; i < bufferSize; i++) {
          if (bpmBuffer[i] > highestBpm) highestBpm = bpmBuffer[i];
        }

        lowestBpm = 999;
        for (int i = 0; i < lowestBufferSize; i++) {
          int currentBpm = lowestBpmBuffer[i];
          if (currentBpm == 0) continue; // Skip zeros
          if (currentBpm < lowestBpm) lowestBpm = currentBpm;
        }
        if (lowestBpm == 999) lowestBpm = 0; // Handle all zeros
      }
    }
    lastPeakTime = currentTime;
    rising = false;
  }
  lastAverage = average;

  if (millis() - lastPeakTime > 3000) {
    avgBpm = 0;
    highestBpm = 0;
    lowestBpm = 999;
    totalBpm = 0;
    memset(bpmBuffer, 0, sizeof(bpmBuffer));
    memset(lowestBpmBuffer, 0, sizeof(lowestBpmBuffer));
    lowestBpmIndex = 0;
  }

  // Read temperature
  float celsius = tempsensor.readTempC();

  // Read GPS data
  while (GPS.available()) {
    char c = GPS.read();
    if (GPS.newNMEAreceived() && !GPS.parse(GPS.lastNMEA())) continue;
  }

  // Send data every 5 seconds
  if (currentMillis - lastSendTime >= 5000) {
    lastSendTime = currentMillis;
    String payload = "{";
    payload += "\"heartRate\":" + String(avgBpm);
    payload += ",\"averageHeartRate\":" + String(avgBpm);
    payload += ",\"highestHeartRate\":" + String(highestBpm);
    payload += ",\"lowestHeartRate\":" + String(lowestBpm);
    payload += ",\"temperature\":" + String(celsius, 2);
    payload += ",\"location\":{\"lat\":" + String(GPS.latitudeDegrees != 0 ? GPS.latitudeDegrees : 53.270962, 6);
    payload += ",\"lng\":" + String(GPS.longitudeDegrees != 0 ? GPS.longitudeDegrees : -9.062691, 6) + "}}";

    sendHttpRequestAsync(payload);
    Serial.println("Sent: " + payload);
  }

  //Serial.print("Heart Rate (BPM): ");
  //Serial.println(avgBpm > 0 ? String(avgBpm) : "Calculating...");
  delay(20);
}
