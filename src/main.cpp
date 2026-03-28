/*
 Basic rtl_433_ESP example for OOK/ASK Devices
 https://github.com/NorthernMan54/rtl_433_ESP

*/
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <rtl_433_ESP.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <Preferences.h>
#include "radioDataInterface.h"
#include "secrets.h"

#ifndef RF_MODULE_FREQUENCY
#  define RF_MODULE_FREQUENCY 433.92
#endif

#define JSON_MSG_BUFFER 512

Preferences preferences;
char messageBuffer[JSON_MSG_BUFFER];

rtl_433_ESP rf; // use -1 to disable transmitter

int count = 0;

//bool wifiConnected = false;

AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;

int lastPublishedId = -1;
unsigned long lastPublishTime = 0;
unsigned long lastStatusReport = 0;
const unsigned long STATUS_REPORT_INTERVAL = 300000; // 5 minutes in milliseconds

struct SystemSettings 
  {
// Network
  char ssidPrimary[33]=WIFI_SSID_PRIMARY;
  char passPrimary[64]=WIFI_PASS_PRIMARY;
  char ssidBackup[33]=WIFI_SSID_BACKUP;
  char passBackup[64]=WIFI_PASS_BACKUP;

  // MQTT
  char mqttHost[64]=MQTT_HOST;
  uint16_t mqttPort=MQTT_PORT;
  
  // Radio/System
  unsigned long wifiCheckInterval=30000; // Default to 30 seconds
  int appliedRSSIThreshold=9;
  };
SystemSettings settings; // Create with default values

void enterLightSleep() 
  {
  Log.notice(F("Entering Light Sleep... Waiting for GDO2 signal." CR));
  
  // 1. Configure GDO2 (GPIO 4) as the wakeup source
  // Logic level 1 means wake up when the pin goes HIGH (signal detected)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)RF_MODULE_GDO2, 1); 

  // 2. Stop the MQTT client and WiFi from background processing if needed
  // (In Light Sleep, WiFi association is maintained automatically by the hardware)

  // 3. Enter Light Sleep
  esp_light_sleep_start();

  // --- The processor pauses here until GDO2 triggers ---

  Log.notice(F("Resuming radio processing." CR));
  }

void sendSettings()
  {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  
  // Network
  doc["ssid_primary"] = settings.ssidPrimary;
  doc["pass_primary"] = settings.passPrimary;
  doc["ssid_backup"] = settings.ssidBackup;
  doc["pass_backup"] = settings.passBackup;
  
  // MQTT
  doc["mqtt_host"] = settings.mqttHost;
  doc["mqtt_port"] = settings.mqttPort;
  
  // System
  doc["interval"] = settings.wifiCheckInterval;
  doc["min_rssi"] = settings.appliedRSSIThreshold;

  String settingsTopic = String(PUBLISH_TOPIC) + "settings";
  char buffer[1024]; // Slightly larger buffer for the full string set
  serializeJson(doc, buffer);
  
  mqttClient.publish(settingsTopic.c_str(), 0, false, buffer);
  Log.notice(F("OK: Settings report published." CR));
  }

void sendStatus()
  {
  if (!mqttClient.connected()) return;

  JsonDocument statusDoc;
  statusDoc["active_ssid"] = WiFi.SSID();
  statusDoc["ip"] = WiFi.localIP().toString();
  statusDoc["interval"] = settings.wifiCheckInterval;
  
  // Pull the current threshold directly from the library object
  statusDoc["applied_rssi_threshold_delta"] = rf.rssiThresholdDelta;
  
  statusDoc["uptime_sec"] = millis() / 1000;
  statusDoc["heap"] = ESP.getFreeHeap();

  String statusTopic = String(PUBLISH_TOPIC) + "status";
  char statusBuffer[512];
  serializeJson(statusDoc, statusBuffer);
  mqttClient.publish(statusTopic.c_str(), 0, false, statusBuffer);
  Log.notice(F("OK: Status report published." CR));
  }

void applyPreferences()
  {
  // Apply the RSSI threshold from settings to the OOK receiver
  rf.setRSSIThreshold(settings.appliedRSSIThreshold);
  Log.notice(F("Applied RSSI Threshold: %d" CR), settings.appliedRSSIThreshold);
  
  // Future settings can be applied here as well (e.g., frequency offset, debug mode)
  }

void loadSettings()
  {
  preferences.begin("tower-radio", true);
  
  // Check if the "config" key exists to avoid loading garbage on a brand new chip
  if (preferences.isKey("config"))
    {
    preferences.getBytes("config", &settings, sizeof(SystemSettings));
    Log.notice(F("--> Settings restored from Flash." CR));
    applyPreferences(); // Apply loaded settings to the system (e.g., set RSSI threshold)
    }
  else
    {
    Log.warning(F("--> No Flash settings found. Using defaults." CR));
    }
    
  preferences.end();
  }

void saveSettings()
  {
  preferences.begin("tower-radio", false);
  
  // Save the entire struct as a single blob of bytes
  preferences.putBytes("config", &settings, sizeof(SystemSettings));
  
  preferences.end();
  Log.notice(F("--> All settings saved to Flash." CR));
  }

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) 
  {
  bool changed = false;
  char message[len + 1];
  memcpy(message, payload, len);
  message[len] = '\0';
  String msgString = String(message);

  int splitPos = msgString.indexOf('=');
  String key = (splitPos != -1) ? msgString.substring(0, splitPos) : msgString;
  String val = (splitPos != -1) ? msgString.substring(splitPos + 1) : "";

  // Logic Branch
  if (key == "status") 
    {
    sendStatus();
    }
  else if (key == "settings") 
    {
    sendSettings();
    }
  else if (key == "interval") 
    {
    long newInterval = val.toInt();
    if (newInterval >= 5000) 
      {
      settings.wifiCheckInterval = newInterval;
      changed = true;
      sendStatus(); // Confirm the change by sending back the new state
      }
    }
  else if (key == "min_rssi") 
    {
    int newRSSI = val.toInt();
    // Ensure we are in a sane range for OOK (usually -60 to -100)
    if (newRSSI >= 1 && newRSSI <= 20) 
      {
      rf.setRSSIThreshold(newRSSI);
      settings.appliedRSSIThreshold = newRSSI; // Update our preferences
      Log.notice(F("OK: MIN_RSSI set to %d" CR), newRSSI);
      changed = true;
      sendStatus(); // Report back the new state
      }
    else 
      {
      Log.error(F("ERROR: RSSI must be between -60 and -105" CR));
      }
    }
  else if (key == "ssid_primary") 
    {
    strncpy(settings.ssidPrimary, val.c_str(), sizeof(settings.ssidPrimary) - 1);
    changed = true;
    }
  else if (key == "pass_primary") 
    {
    strncpy(settings.passPrimary, val.c_str(), sizeof(settings.passPrimary) - 1);
    changed = true;
    }
  else if (key == "ssid_backup") 
    {
    strncpy(settings.ssidBackup, val.c_str(), sizeof(settings.ssidBackup) - 1);
    changed = true;
    }
  else if (key == "pass_backup") 
    {
    strncpy(settings.passBackup, val.c_str(), sizeof(settings.passBackup) - 1);
    changed = true;
    }
  else if (key == "mqtt_host") 
    {
    strncpy(settings.mqttHost, val.c_str(), sizeof(settings.mqttHost) - 1);
    settings.mqttHost[sizeof(settings.mqttHost) - 1] = '\0'; // Force null terminator
    changed = true;
    }  
  else if (key == "mqtt_port")
    {
    settings.mqttPort = val.toInt();
    changed = true;
    }  
  else if (key == "restart")
    {
    Log.warning(F("OK: Restarting..." CR));
    delay(500);
    ESP.restart();
    }
  else 
    {
    Log.error(F("ERROR: Unknown key [%s]" CR), key.c_str());
    }
  if (changed) 
    {
    saveSettings(); // Persist any changes to Flash
    sendSettings(); // Send back the full settings report to confirm the new state
    }
  }

void connectToMqtt() 
  {
  Log.notice(F("Connecting to MQTT..." CR));
  mqttClient.connect();
  }

void onMqttConnect(bool sessionPresent) 
  {
  Log.notice(F("Connected to MQTT." CR));

  // Subscribe to the command topic
  String cmdTopic = String(PUBLISH_TOPIC) + "command";
  mqttClient.subscribe(cmdTopic.c_str(), 0);
  
  Log.notice(F("Subscribed to: %s" CR), cmdTopic.c_str());

// this is out of place here:  sendSettings(); // Send settings report on startup
  sendStatus(); // Send an initial status report on startup
  }

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) 
  {
  Log.notice(F("Disconnected from MQTT." CR));
  if (WiFi.isConnected()) 
    {
    xTimerStart(mqttReconnectTimer, 0);
    }
  }

void logJson(JsonDocument jsondata) {
#if defined(ESP8266) || defined(ESP32) || defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
  char JSONmessageBuffer[measureJson(jsondata) + 1];
  serializeJson(jsondata, JSONmessageBuffer, measureJson(jsondata) + 1);
#else
  char JSONmessageBuffer[JSON_MSG_BUFFER];
  serializeJson(jsondata, JSONmessageBuffer, JSON_MSG_BUFFER);
#endif
#if defined(setBitrate) || defined(setFreqDev) || defined(setRxBW)
  Log.setShowLevel(false);
  Log.notice(F("."));
  Log.setShowLevel(true);
#else
  Log.notice(F("Received message : %s" CR), JSONmessageBuffer);
#endif
}

void rtl_433_Callback(char* message) 
  {
  count++;
  if (mqttClient.connected()) 
    {
    JsonDocument jsonDocument;
    DeserializationError error = deserializeJson(jsonDocument, message);

    if (error) 
      {
      Log.error(F("JSON Parse Failed: "));
      Log.error(F("%s" CR), error.f_str());
      }
    else
      {
      logJson(jsonDocument);

      // 2. Extract identifying info
      const char* model = jsonDocument["model"] | "Unknown";
      int id = jsonDocument["id"] | 0;
      
      if (id == lastPublishedId && (millis() - lastPublishTime < 2000))
        {
        Log.warning(F("Duplicate message detected, skipping MQTT publish." CR));
        }
      else
        {
        lastPublishedId = id;
        lastPublishTime = millis();

        // 3. Construct a clean topic: rtl_433/Nolensville/Acurite-6045M/165
        String topic = String(PUBLISH_TOPIC) + String(model) + "/" + String(id);

        // 4. Publish the full JSON payload to that specific topic
        mqttClient.publish(topic.c_str(), 0, false, message);
        }
      }
    }
  }

void connectToWiFi(const char* ssid, const char* password)
  {
  Log.notice(F("Attempting WiFi: %s " CR), ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  // Wait up to 10 seconds (20 * 500ms)
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
    delay(500);
    Serial.print(".");
    attempts++;
    }
  
  if (WiFi.status() == WL_CONNECTED)
    {
    Log.notice(F("SUCCESS!" CR));
    Log.notice(F("Connected to %s | IP: %s" CR), ssid, WiFi.localIP().toString().c_str());
    }
  else
    {
    Log.error(F("Failed to connect." CR));
    }
  }

void setupWiFi()
  {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); 
  delay(100);

  // Try Primary
  connectToWiFi((const char*)settings.ssidPrimary, (const char*)settings.passPrimary);

  // If Primary failed, try Backup
  if (WiFi.status() != WL_CONNECTED)
    {
    connectToWiFi((const char*)settings.ssidBackup, (const char*)settings.passBackup);
    }
  }

void setup() 
  {
  Serial.begin(921600);
  delay(1000);

  loadSettings(); // Populates the preferences

// 1. Establish WiFi first
  setupWiFi();

#ifndef LOG_LEVEL
  #define LOG_LEVEL LOG_LEVEL_SILENT
#endif
  Log.begin(LOG_LEVEL, &Serial);
  Log.notice(F(" " CR));
  Log.notice(F("****** setup ******" CR));

  // Setup MQTT Reconnect Timer
  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage); 
  mqttClient.setServer(settings.mqttHost, settings.mqttPort);

  rf.initReceiver(RF_MODULE_RECEIVER_GPIO, RF_MODULE_FREQUENCY);
  rf.setCallback(rtl_433_Callback, messageBuffer, JSON_MSG_BUFFER);
  rf.enableReceiver();

  // Start the first MQTT connection attempt
  connectToMqtt();

  Log.notice(F("****** setup complete ******" CR));
  rf.getModuleStatus();
  }

// unsigned long uptime() {
//   static unsigned long lastUptime = 0;
//   static unsigned long uptimeAdd = 0;
//   unsigned long uptime = millis() / 1000 + uptimeAdd;
//   if (uptime < lastUptime) {
//     uptime += 4294967;
//     uptimeAdd += 4294967;
//   }
//   lastUptime = uptime;
//   return uptime;
// }

// int next = uptime() + 30;

#if defined(setBitrate) || defined(setFreqDev) || defined(setRxBW)

#  ifdef setBitrate
#    define TEST    "setBitrate" // 17.24 was suggested
#    define STEP    2
#    define stepMin 1
#    define stepMax 300
// #    define STEP    1
// #    define stepMin 133
// #    define stepMax 138
#  elif defined(setFreqDev) // 40 kHz was suggested
#    define TEST    "setFrequencyDeviation"
#    define STEP    1
#    define stepMin 5
#    define stepMax 200
#  elif defined(setRxBW)
#    define TEST "setRxBandwidth"

#    ifdef defined(RF_SX1276) || defined(RF_SX1278)
#      define STEP    5
#      define stepMin 5
#      define stepMax 250
#    else
#      define STEP    5
#      define stepMin 58
#      define stepMax 812
// #      define STEP    0.01
// #      define stepMin 202.00
// #      define stepMax 205.00
#    endif
#  endif
float step = stepMin;
#endif

unsigned long lastWifiCheck = 0;
unsigned long lastConnectAttempt = 0;
const unsigned long RECONNECT_DELAY = 10000; // Wait 10 seconds between attempts
unsigned long lastRadioActivity = 0;
const unsigned long SLEEP_DELAY = 30000; // 30 seconds

void loop()
{
  // 1. Give the radio absolute priority
  rf.loop();

  // 2. Periodic Network Health Check (The Gatekeeper)
  if (millis() - lastWifiCheck > settings.wifiCheckInterval)
  {
    lastWifiCheck = millis();
    
    // Check WiFi first
    if (WiFi.status() != WL_CONNECTED)
    {
      Log.error(F("WiFi Down. Triggering reconnect..." CR));
      setupWiFi(); // Calls WiFi.begin() once, then returns
    } 
    // If WiFi is fine, check MQTT
    else if (!mqttClient.connected())
    {
      Log.notice(F("WiFi Up, but MQTT Disconnected. Triggering MQTT reconnect..." CR));
      connectToMqtt(); 
    }
  }

  // 3. Periodic Status Report
  if (millis() - lastStatusReport > STATUS_REPORT_INTERVAL)
  {
    lastStatusReport = millis();
    if (WiFi.status() == WL_CONNECTED && mqttClient.connected())
    {
      sendStatus();
    }
  }
   
#if defined(setBitrate) || defined(setFreqDev) || defined(setRxBW)
  char stepPrint[8];
  if (uptime() > next) {
    next = uptime() + 120; // 60 seconds
    dtostrf(step, 7, 2, stepPrint);
    Log.notice(F(CR "Finished %s: %s, count: %d" CR), TEST, stepPrint, count);
    step += STEP;
    if (step > stepMax) {
      step = stepMin;
    }
    dtostrf(step, 7, 2, stepPrint);
    Log.notice(F("Starting %s with %s" CR), TEST, stepPrint);
    count = 0;

    int16_t state = 0;
#  ifdef setBitrate
    state = rf.setBitRate(step);
    RADIOLIB_STATE(state, TEST);
#  elif defined(setFreqDev)
    state = rf.setFrequencyDeviation(step);
    RADIOLIB_STATE(state, TEST);
#  elif defined(setRxBW)
    state = rf.setRxBandwidth(step);
    if ((state) != RADIOLIB_ERR_NONE) {
      Log.notice(F(CR "Setting  %s: to %s, failed" CR), TEST, stepPrint);
      next = uptime() - 1;
    }
#  endif

    rf.receiveDirect();
    // rf.getModuleStatus();
  }
#endif
}