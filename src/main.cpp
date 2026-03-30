/*
This is a program to receive OOK/ASK signals using a CC1101 module and publish 
them to MQTT in JSON format. Configuration is done via MQTT commands, and 
settings are persisted in flash memory.

Based on rtl_433_ESP example for OOK/ASK Devices
 https://github.com/NorthernMan54/rtl_433_ESP

*/
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <rtl_433_ESP.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
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
  Log.warning(F("OK: Settings report published." CR));
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
  Log.info(F("OK: Status report published." CR));
  }

void applyPreferences()
  {
  // Apply the RSSI threshold from settings to the OOK receiver
  rf.setRSSIThreshold(settings.appliedRSSIThreshold);
  Log.info(F("Applied RSSI Threshold: %d" CR), settings.appliedRSSIThreshold);
  
  // Future settings can be applied here as well (e.g., frequency offset, debug mode)
  }

void loadSettings()
  {
  preferences.begin("tower-radio", true);
  
  // Check if the "config" key exists to avoid loading garbage on a brand new chip
  if (preferences.isKey("config"))
    {
    preferences.getBytes("config", &settings, sizeof(SystemSettings));
    Log.warning(F("--> Settings restored from Flash." CR));
    applyPreferences(); // Apply loaded settings to the system (e.g., set RSSI threshold)
    }
  else
    {
    Log.error(F("--> No Flash settings found. Using defaults." CR));
    }
    
  preferences.end();
  }

void saveSettings()
  {
  preferences.begin("tower-radio", false);
  
  // Save the entire struct as a single blob of bytes
  preferences.putBytes("config", &settings, sizeof(SystemSettings));
  
  preferences.end();
  Log.warning(F("--> All settings saved to Flash." CR));
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
      Log.warning(F("OK: MIN_RSSI set to %d" CR), newRSSI);
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
    Log.warning(F("OK: ssid_primary set to %s" CR), settings.ssidPrimary);
    changed = true;
    }
  else if (key == "pass_primary") 
    {
    strncpy(settings.passPrimary, val.c_str(), sizeof(settings.passPrimary) - 1);
    Log.warning(F("OK: pass_primary set to %s" CR), settings.passPrimary);
    changed = true;
    }
  else if (key == "ssid_backup") 
    {
    strncpy(settings.ssidBackup, val.c_str(), sizeof(settings.ssidBackup) - 1);
    Log.warning(F("OK: ssid_backup set to %s" CR), settings.ssidBackup);
    changed = true;
    }
  else if (key == "pass_backup") 
    {
    strncpy(settings.passBackup, val.c_str(), sizeof(settings.passBackup) - 1);
    Log.warning(F("OK: pass_backup set to %s" CR), settings.passBackup);
    changed = true;
    }
  else if (key == "mqtt_host") 
    {
    strncpy(settings.mqttHost, val.c_str(), sizeof(settings.mqttHost) - 1);
    settings.mqttHost[sizeof(settings.mqttHost) - 1] = '\0'; // Force null terminator
    Log.warning(F("OK: mqtt_host set to %s" CR), settings.mqttHost);
    changed = true;
    }  
  else if (key == "mqtt_port")
    {
    settings.mqttPort = val.toInt();
    Log.warning(F("OK: mqtt_port set to %d" CR), settings.mqttPort);
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
  Log.warning(F("Connecting to MQTT..." CR));
  mqttClient.connect();
  }

void onMqttConnect(bool sessionPresent) 
  {
  Log.warning(F("Connected to MQTT." CR));

  // Subscribe to the command topic
  String cmdTopic = String(PUBLISH_TOPIC) + "command";
  mqttClient.subscribe(cmdTopic.c_str(), 0);
  
  Log.warning(F("Subscribed to: %s" CR), cmdTopic.c_str());

// this is out of place here:  sendSettings(); // Send settings report on startup
  sendStatus(); // Send an initial status report on startup
  }

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) 
  {
  Log.error(F("Disconnected from MQTT." CR));
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
  Log.info(F("."));
  Log.setShowLevel(true);
#else
  Log.info(F("Received message : %s" CR), JSONmessageBuffer);
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
        Log.info(F("Duplicate message detected, skipping MQTT publish." CR));
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
  Log.warning(F("Attempting WiFi: %s " CR), ssid);
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
    Log.warning(F("SUCCESS!" CR));
    Log.warning(F("Connected to %s | IP: %s" CR), ssid, WiFi.localIP().toString().c_str());
    }
  else
    {
    Log.fatal(F("Failed to connect to %s." CR), ssid);
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
  Log.info(F(" " CR));
  Log.info(F("****** setup ******" CR));

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

  Log.info(F("****** setup complete ******" CR));
  rf.getModuleStatus();

  // Set watchdog to 15 seconds. If loop() doesn't finish in 15s, REBOOT.
  esp_task_wdt_init(15, true); 
  esp_task_wdt_add(NULL);      // Add the current (main) task to WDT
  }

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
  esp_task_wdt_reset();        // Tell the watchdog "I'm still alive"

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
      Log.warning(F("WiFi Up, but MQTT Disconnected. Triggering MQTT reconnect..." CR));
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
    Log.info(F(CR "Finished %s: %s, count: %d" CR), TEST, stepPrint, count);
    step += STEP;
    if (step > stepMax) {
      step = stepMin;
    }
    dtostrf(step, 7, 2, stepPrint);
    Log.info(F("Starting %s with %s" CR), TEST, stepPrint);
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
      Log.info(F(CR "Setting  %s: to %s, failed" CR), TEST, stepPrint);
      next = uptime() - 1;
    }
#  endif

    rf.receiveDirect();
    // rf.getModuleStatus();
  }
#endif
}