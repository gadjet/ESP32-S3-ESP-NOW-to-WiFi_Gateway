/**
 * ============================================================================
 * ESP32-S3 ESP-NOW to MQTT Gateway - U10 WiFi & MQTT Gateway Firmware
 * ============================================================================
 * Version 0.1 First Working Version
 * ============================================================================
 * Target Board: Seeed Studio XIAO ESP32S3
 * Role: U10 - WiFi, MQTT Client, Web Server, RGB LED & I2C Display Controller
 *
 * Hardware Connections (from Schematic & PCB):
 *   RGB Status LED (LED1 - Common Anode to VCC, Active LOW):
 *     - Red:   D0 (GPIO1) via R3 (680Ω) -> LOW = ON, HIGH = OFF
 *     - Green: D1 (GPIO2) via R5 (680Ω) -> LOW = ON, HIGH = OFF
 *     - Blue:  D2 (GPIO3) via R4 (680Ω) -> LOW = ON, HIGH = OFF
 *
 *   I2C Connector (H1 - Optional SSD1306 Display):
 *     - SDA:   D4 (GPIO5) with R1 (4.7kΩ pull-up to VCC)
 *     - SCL:   D5 (GPIO6) with R2 (4.7kΩ pull-up to VCC)
 *     - GND:   GND (Pin 1)
 *     - VCC:   +3.3V (Pin 2)
 *
 *   Capacitive Touch Input:
 *     - Touch: D3 (GPIO4 / TOUCH4) connected to copper tape inside 2mm cover
 *
 *   Inter-ESP Bus (connected to U1):
 *     - D6 (GPIO43): Configured as U10 Hardware UART RX (receives from U1 TX on
 * D6)
 *     - D7 (GPIO44): Configured as U10 Hardware UART TX (transmits to U1 RX on
 * D7)
 *
 *   USB Type-C:
 *     - USB1+ & USB1- connected when USB-C cable is oriented towards U10.
 *     - Board LED3 (KT-0603R) illuminates to indicate U10 is connected to USB
 * host.
 *
 *   Power:
 *     - 5V & GND common system power rail
 * ============================================================================
 */

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

// ----------------------------------------------------------------------------
// Optional OLED Display Support (Adafruit SSD1306)
// Automatically detects if an SSD1306 display is plugged into header H1.
// If not fitted, execution continues gracefully without blocking.
// ----------------------------------------------------------------------------
#define ENABLE_OLED_SUPPORT 1

#if ENABLE_OLED_SUPPORT
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_TIMEOUT_MS                                                        \
  30000UL // Display stays ON for 30 seconds after boot or touch
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledFound = false;
bool oledAwake = true;
unsigned long oledWakeTime = 0;
uint8_t oledAddress = 0x3C;
#endif

// ----------------------------------------------------------------------------
// Pin Definitions (Direct from Schematic & PCB)
// ----------------------------------------------------------------------------
// RGB LED Pins (Common Anode to VCC: LOW = ON, HIGH = OFF)
#define LED_PIN_RED D0   // GPIO1 (Red cathode via R3 680Ω)
#define LED_PIN_GREEN D1 // GPIO2 (Green cathode via R5 680Ω)
#define LED_PIN_BLUE D2  // GPIO3 (Blue cathode via R4 680Ω)

// Capacitive Touch Pin (Connected to copper tape inside 2mm PLA cover)
#define TOUCH_WAKE_PIN D3 // GPIO4 (ESP32-S3 TOUCH4)

// I2C Pins (Connector H1 on schematic)
#define I2C_SDA_PIN D4 // GPIO5 (R1 4.7kΩ pull-up to VCC)
#define I2C_SCL_PIN D5 // GPIO6 (R2 4.7kΩ pull-up to VCC)

// Inter-ESP Bus Pins (Hardware UART1 connected to U1)
#define BUS_RX_PIN D6 // GPIO43 (receives from U1 TX on D6)
#define BUS_TX_PIN D7 // GPIO44 (transmits to U1 RX on D7)

#define BUS_BAUD_RATE 115200
#define SERIAL_DEBUG_BAUD 115200

// ----------------------------------------------------------------------------
// Configuration Settings
// Defaults can be updated via the Web Configuration page (/config)
// ----------------------------------------------------------------------------
struct GatewayConfig {
  char wifi_ssid[32];
  char wifi_pass[64];
  char mqtt_server[64];
  int mqtt_port;
  char mqtt_user[64];
  char mqtt_pass[64];
  char mqtt_client_id[32];
  char mqtt_topic_prefix[64];
  bool oled_timeout_enabled;
};

GatewayConfig config;
Preferences preferences;

// Default fallback values
const char DEFAULT_WIFI_SSID[] = "Gadjets3";
const char DEFAULT_WIFI_PASS[] = "M@ttscbu02";
const char DEFAULT_MQTT_HOST[] = "rattlesnake.rmq.cloudamqp.com";
const int DEFAULT_MQTT_PORT = 1883;
const char DEFAULT_MQTT_USER[] = "zrplvfgu:zrplvfgu";
const char DEFAULT_MQTT_PASS[] = "Ky-4d6ALGPLtLxYaX4n50v4UtiaoCRbi";
const char DEFAULT_TOPIC_PRE[] = "espnow/gateway";

// ----------------------------------------------------------------------------
// Objects & State Variables
// ----------------------------------------------------------------------------
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);
HardwareSerial busSerial(1); // Hardware UART1 for inter-ESP bus

// LED State Tracking
enum LedColor {
  LED_OFF,
  LED_RED,
  LED_GREEN,
  LED_BLUE,
  LED_YELLOW,
  LED_CYAN,
  LED_MAGENTA,
  LED_WHITE
};

LedColor currentBaseColor = LED_RED;
LedColor pulseColor = LED_OFF;
unsigned long pulseEndTime = 0;

// Inter-ESP (U1) Communication & Telemetry State
bool u1Online = false;
unsigned long lastU1Heartbeat = 0;
String u1MacAddress = "Unknown";
int u1Channel = 1;
int lastEspNowRssi = 0;
bool hasLastEspNowRssi = false;

// Packet History Rolling Buffer
#define MAX_LOG_ENTRIES 15
struct MessageLog {
  unsigned long timestamp;
  String sourceMac;
  int rssi;
  String payload;
  String mqttTopic;
  bool success;
};

MessageLog messageHistory[MAX_LOG_ENTRIES];
int messageHistoryCount = 0;
int messageHistoryHead = 0;

// Gateway Statistics
unsigned long statsRxPackets = 0;
unsigned long statsTxPackets = 0;
unsigned long statsMqttPublished = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastWifiReconnectAttempt = 0;
unsigned long lastWifiRoamCheck = 0;
unsigned long lastU1Ping = 0;

// WiFi RSSI Trend History (1-minute intervals, 60 samples = 1 hour)
#define RSSI_HISTORY_SIZE 60
int8_t rssiHistory[RSSI_HISTORY_SIZE];
int rssiHistoryCount = 0;
int rssiHistoryHead = 0;
unsigned long lastRssiSample = 0;

// ----------------------------------------------------------------------------
// RGB LED Control (Active LOW for Common Anode: LOW = ON, HIGH = OFF)
// ----------------------------------------------------------------------------
void setRgbRaw(bool r, bool g, bool b) {
  digitalWrite(LED_PIN_RED, r ? LOW : HIGH);
  digitalWrite(LED_PIN_GREEN, g ? LOW : HIGH);
  digitalWrite(LED_PIN_BLUE, b ? LOW : HIGH);
}

void setRgbColor(LedColor color) {
  switch (color) {
  case LED_RED:
    setRgbRaw(true, false, false);
    break;
  case LED_GREEN:
    setRgbRaw(false, true, false);
    break;
  case LED_BLUE:
    setRgbRaw(false, false, true);
    break;
  case LED_YELLOW:
    setRgbRaw(true, true, false);
    break;
  case LED_CYAN:
    setRgbRaw(false, true, true);
    break;
  case LED_MAGENTA:
    setRgbRaw(true, false, true);
    break;
  case LED_WHITE:
    setRgbRaw(true, true, true);
    break;
  case LED_OFF:
  default:
    setRgbRaw(false, false, false);
    break;
  }
}

void pulseRgbColor(LedColor pColor, unsigned long durationMs = 80) {
  pulseColor = pColor;
  pulseEndTime = millis() + durationMs;
  setRgbColor(pulseColor);
}

void updateLedState() {
  if (pulseEndTime > 0 && millis() < pulseEndTime) {
    return; // Active pulse running
  }
  pulseEndTime = 0;

  // Determine base status color based on connection states
  if (WiFi.status() != WL_CONNECTED) {
    currentBaseColor = LED_RED; // Solid Red: Waiting for WiFi
  } else if (!mqttClient.connected()) {
    currentBaseColor =
        LED_YELLOW; // Solid Yellow: WiFi connected, connecting to MQTT
  } else {
    currentBaseColor =
        LED_GREEN; // Solid Green: Fully connected and operational
  }

  setRgbColor(currentBaseColor);
}

// ----------------------------------------------------------------------------
// Persistent Configuration Management (Preferences / NVS)
// ----------------------------------------------------------------------------
void loadConfig() {
  preferences.begin("gateway", false);

  String ssid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
  String pass = preferences.getString("pass", DEFAULT_WIFI_PASS);
  String host = preferences.getString("host", DEFAULT_MQTT_HOST);
  int port = preferences.getInt("port", DEFAULT_MQTT_PORT);
  String user = preferences.getString("user", DEFAULT_MQTT_USER);
  String mpass = preferences.getString("mpass", DEFAULT_MQTT_PASS);
  String cid = preferences.getString("cid", "");
  String pre = preferences.getString("pre", DEFAULT_TOPIC_PRE);
  config.oled_timeout_enabled = preferences.getBool("oled_to", true);

  strncpy(config.wifi_ssid, ssid.c_str(), sizeof(config.wifi_ssid));
  strncpy(config.wifi_pass, pass.c_str(), sizeof(config.wifi_pass));
  strncpy(config.mqtt_server, host.c_str(), sizeof(config.mqtt_server));
  config.mqtt_port = port;
  strncpy(config.mqtt_user, user.c_str(), sizeof(config.mqtt_user));
  strncpy(config.mqtt_pass, mpass.c_str(), sizeof(config.mqtt_pass));
  strncpy(config.mqtt_topic_prefix, pre.c_str(),
          sizeof(config.mqtt_topic_prefix));

  if (cid.length() > 0) {
    strncpy(config.mqtt_client_id, cid.c_str(), sizeof(config.mqtt_client_id));
  } else {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(config.mqtt_client_id, sizeof(config.mqtt_client_id),
             "ESP32S3_GW_%02X%02X", mac[4], mac[5]);
  }

  preferences.end();
  Serial.printf("[CONFIG] Loaded settings: SSID='%s', MQTT Host='%s:%d', Topic "
                "Prefix='%s'\n",
                config.wifi_ssid, config.mqtt_server, config.mqtt_port,
                config.mqtt_topic_prefix);
}

void saveConfig() {
  preferences.begin("gateway", false);
  preferences.putString("ssid", config.wifi_ssid);
  preferences.putString("pass", config.wifi_pass);
  preferences.putString("host", config.mqtt_server);
  preferences.putInt("port", config.mqtt_port);
  preferences.putString("user", config.mqtt_user);
  preferences.putString("mpass", config.mqtt_pass);
  preferences.putString("cid", config.mqtt_client_id);
  preferences.putString("pre", config.mqtt_topic_prefix);
  preferences.putBool("oled_to", config.oled_timeout_enabled);
  preferences.end();
  Serial.println(F("[CONFIG] Settings saved to Flash NVS."));
}

// ----------------------------------------------------------------------------
// Packet Logging Helper
// ----------------------------------------------------------------------------
void logMessage(const String &mac, int rssi, const String &payload,
                const String &topic, bool success) {
  int nextIdx;
  if (messageHistoryCount < MAX_LOG_ENTRIES) {
    nextIdx = messageHistoryCount;
    messageHistoryCount++;
  } else {
    nextIdx = messageHistoryHead;
    messageHistoryHead = (messageHistoryHead + 1) % MAX_LOG_ENTRIES;
  }

  messageHistory[nextIdx].timestamp = millis();
  messageHistory[nextIdx].sourceMac = mac;
  messageHistory[nextIdx].rssi = rssi;
  messageHistory[nextIdx].payload = payload;
  messageHistory[nextIdx].mqttTopic = topic;
  messageHistory[nextIdx].success = success;
}

// ----------------------------------------------------------------------------
// I2C & SSD1306 OLED Management (Optional Hardware)
// ----------------------------------------------------------------------------
void initI2CAndDisplay() {
#if ENABLE_OLED_SUPPORT
  Serial.println(F("[I2C] Scanning I2C bus on SDA=D4, SCL=D5..."));
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);

  // Scan bus for OLED display addresses 0x3C or 0x3D
  byte error;
  for (uint8_t address = 0x3C; address <= 0x3D; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      oledAddress = address;
      oledFound = true;
      Serial.printf("[I2C] SSD1306 OLED detected at address 0x%02X!\n",
                    oledAddress);
      break;
    }
  }

  if (oledFound) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddress)) {
      Serial.println(F("[OLED] Allocation failed, disabling OLED."));
      oledFound = false;
    } else {
      display.clearDisplay();
      display.setTextWrap(false);
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println(F("ESP32-S3 Gateway"));
      display.println(F("====================="));
      display.println(F("U10: WiFi & MQTT"));
      display.println(F("U1 : ESP-NOW Receiver"));
      display.println(F("Connecting to WiFi..."));
      display.display();
      oledAwake = true;
      oledWakeTime = millis();
      delay(1200);
    }
  } else {
    Serial.println(F("[I2C] No OLED display found on connector H1 (optional, "
                     "continuing without display)."));
  }
#endif
}

// ----------------------------------------------------------------------------
// Capacitive Touch Wake & 30s OLED Sleep Manager (Pin D3 / GPIO4)
// ----------------------------------------------------------------------------
uint32_t touchBaseline = 0;
uint32_t touchThresholdDelta = 1500;
uint32_t lastTouchReading = 0;
unsigned long lastTouchSample = 0;

void initTouchSensor() {
  // Warm up ESP32-S3 touch FSM (initial readings right after touch_pad_init
  // can be ~280k transients before settling to ~22k steady-state)
  for (int i = 0; i < 40; i++) {
    touchRead(TOUCH_WAKE_PIN);
    delay(15);
  }

  // Take 16 settled readings to establish untouched baseline through 2mm PLA
  uint64_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += touchRead(TOUCH_WAKE_PIN);
    delay(15);
  }
  touchBaseline = (uint32_t)(sum / 16);

  // On ESP32-S3, touchRead() increases when capacitance increases (finger
  // approaches copper tape). A ~3% delta (minimum 300 counts) detects touch
  // through 2mm of PLA reliably.
  touchThresholdDelta = max(300UL, (unsigned long)(touchBaseline / 33));
  lastTouchReading = touchBaseline;
  Serial.printf(
      "[TOUCH] D3 (GPIO4) calibrated: baseline=%lu, trigger delta=+%lu\n",
      (unsigned long)touchBaseline, (unsigned long)touchThresholdDelta);
}

void setOledPower(bool wakeUp) {
#if ENABLE_OLED_SUPPORT
  if (!oledFound)
    return;
  if (wakeUp) {
    oledWakeTime = millis();
    if (!oledAwake) {
      oledAwake = true;
      display.ssd1306_command(SSD1306_DISPLAYON);
      Serial.println(F("[OLED] Waking display ON for 30 seconds."));
    }
  } else {
    if (oledAwake) {
      oledAwake = false;
      display.ssd1306_command(SSD1306_DISPLAYOFF);
      Serial.println(F("[OLED] 30s timeout elapsed -> Display switched OFF "
                       "(touch D3 to wake)."));
    }
  }
#endif
}

void updateOledDisplay() {
#if ENABLE_OLED_SUPPORT
  if (!oledFound || !oledAwake)
    return;

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Header Bar (max 21 chars @ 6px = 126px)
  display.setCursor(0, 0);
  display.print(F("GW:"));
  if (WiFi.status() == WL_CONNECTED) {
    display.print(WiFi.localIP());
  } else {
    display.print(F("Connecting..."));
  }

  // Draw separator line
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Status Line 1: WiFi (x=0..66) & MQTT (x=72..126) - no overlap
  display.setCursor(0, 12);
  if (WiFi.status() == WL_CONNECTED) {
    display.print(F("WiFi:"));
    display.print(WiFi.RSSI());
    display.print(F("dBm"));
  } else {
    display.print(F("WiFi:WAIT"));
  }

  display.setCursor(72, 12);
  display.print(F("MQTT:"));
  display.print(mqttClient.connected() ? F("OK") : F("WAIT"));

  // Status Line 2: Inter-ESP U1 Link (x=0..60) & Channel (x=90..120) - no
  // overlap
  display.setCursor(0, 22);
  display.print(F("U1:"));
  display.print(u1Online ? F("ONLINE") : F("OFFLINE"));

  display.setCursor(90, 22);
  display.print(F("Ch:"));
  display.print(u1Channel);

  // Status Line 3: Packet Counters
  display.setCursor(0, 32);
  display.print(F("RX:"));
  display.print(statsRxPackets);
  display.print(F(" TX:"));
  display.print(statsTxPackets);
  display.print(F(" PUB:"));
  display.print(statsMqttPublished);

  // Draw separator line
  display.drawLine(0, 41, 127, 41, SSD1306_WHITE);

  // Status Line 4 & 5: Last Received Packet (max 21 chars per line)
  display.setCursor(0, 44);
  if (messageHistoryCount > 0) {
    int latestIdx =
        (messageHistoryHead + messageHistoryCount - 1) % MAX_LOG_ENTRIES;
    String shortMac = messageHistory[latestIdx].sourceMac;
    if (shortMac.length() >= 8) {
      shortMac = shortMac.substring(shortMac.length() - 8);
    }
    display.print(F("Last: "));
    display.print(shortMac);

    display.setCursor(0, 54);
    String p = messageHistory[latestIdx].payload;
    if (p.length() > 21)
      p = p.substring(0, 18) + "...";
    display.print(p);
  } else {
    display.print(F("Last: None yet"));
    display.setCursor(0, 54);
    display.print(F("Listening ESP-NOW..."));
  }

  display.display();
#endif
}

void handleTouchAndOledTimeout() {
#if ENABLE_OLED_SUPPORT
  if (!oledFound)
    return;

  // Poll capacitive touch sensor on D3 every 50ms
  if (millis() - lastTouchSample >= 50) {
    lastTouchSample = millis();
    uint32_t reading = touchRead(TOUCH_WAKE_PIN);
    lastTouchReading = reading;

    // Check if capacitance increased beyond threshold (touch through 2mm PLA
    // cover)
    if (reading > (touchBaseline + touchThresholdDelta)) {
      bool wasSleeping = !oledAwake;
      if (wasSleeping) {
        Serial.printf(
            "[TOUCH] D3 triggered! reading=%lu, baseline=%lu, delta=+%lu\n",
            (unsigned long)reading, (unsigned long)touchBaseline,
            (unsigned long)(reading - touchBaseline));
      }
      setOledPower(
          true); // Wakes OLED or extends the 30s timer if already awake
      if (wasSleeping) {
        updateOledDisplay();
      }
    } else if (reading > 0 &&
               reading < touchBaseline + (touchThresholdDelta / 3)) {
      // Slowly track ambient temperature/humidity drift when untouched
      touchBaseline = (touchBaseline * 127 + reading) / 128;
    }
  }

  // Turn off OLED after 30 seconds of inactivity (when screen timeout is
  // enabled)
  if (config.oled_timeout_enabled) {
    if (oledAwake && (millis() - oledWakeTime >= OLED_TIMEOUT_MS)) {
      setOledPower(false);
    }
  } else if (!oledAwake) {
    setOledPower(true);
    updateOledDisplay();
  }
#endif
}

// ----------------------------------------------------------------------------
// MQTT Message Handler (Downlink / Inbound from Home Assistant / Broker)
// ----------------------------------------------------------------------------
void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  char messageBuf[512];
  unsigned int copyLen =
      (length < sizeof(messageBuf) - 1) ? length : (sizeof(messageBuf) - 1);
  memcpy(messageBuf, payload, copyLen);
  messageBuf[copyLen] = '\0';

  String topicStr = String(topic);
  String payloadStr = String(messageBuf);

  Serial.printf("[MQTT RECV] Topic: %s | Payload: %s\n", topic, messageBuf);

  // Determine target MAC for outbound transmission
  String targetMac = "FF:FF:FF:FF:FF:FF"; // Default broadcast
  String sendPrefix = String(config.mqtt_topic_prefix) + "/send/";
  if (topicStr.startsWith(sendPrefix)) {
    targetMac = topicStr.substring(sendPrefix.length());
    targetMac.replace("_", ":");
  }

  // Construct command JSON to send to U1 over hardware UART
  String cmdJson = "{\"cmd\":\"send\",\"target\":\"";
  cmdJson += targetMac;
  cmdJson += "\",\"data\":";

  if (payloadStr.startsWith("{") && payloadStr.endsWith("}")) {
    cmdJson += payloadStr;
  } else {
    cmdJson += "\"";
    for (size_t i = 0; i < payloadStr.length(); i++) {
      char c = payloadStr[i];
      if (c == '"')
        cmdJson += "\\\"";
      else if (c == '\\')
        cmdJson += "\\\\";
      else
        cmdJson += c;
    }
    cmdJson += "\"";
  }
  cmdJson += "}\n";

  busSerial.print(cmdJson);
  statsTxPackets++;
  pulseRgbColor(LED_CYAN, 80);
}

// ----------------------------------------------------------------------------
// MQTT Reconnection Logic
// ----------------------------------------------------------------------------
void reconnectMqtt() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (mqttClient.connected())
    return;

  if (millis() - lastMqttReconnectAttempt < 5000)
    return;
  lastMqttReconnectAttempt = millis();

  Serial.print(F("[MQTT] Attempting connection to "));
  Serial.print(config.mqtt_server);
  Serial.print(F(":"));
  Serial.println(config.mqtt_port);

  String lwtTopic = String(config.mqtt_topic_prefix) + "/status";

  bool connected = false;
  if (strlen(config.mqtt_user) > 0) {
    connected = mqttClient.connect(config.mqtt_client_id, config.mqtt_user,
                                   config.mqtt_pass, lwtTopic.c_str(), 1, true,
                                   "offline");
  } else {
    connected = mqttClient.connect(config.mqtt_client_id, lwtTopic.c_str(), 1,
                                   true, "offline");
  }

  if (connected) {
    Serial.println(F("[MQTT] Connected successfully!"));
    mqttClient.publish(lwtTopic.c_str(), "online", true);

    // Subscribe to outbound transmission topics
    String txTopic = String(config.mqtt_topic_prefix) + "/tx";
    String sendWildcard = String(config.mqtt_topic_prefix) + "/send/+";
    mqttClient.subscribe(txTopic.c_str());
    mqttClient.subscribe(sendWildcard.c_str());
    Serial.printf("[MQTT] Subscribed to '%s' and '%s'\n", txTopic.c_str(),
                  sendWildcard.c_str());
  } else {
    Serial.printf("[MQTT] Connection failed, rc=%d. Retrying in 5s...\n",
                  mqttClient.state());
  }
}

// ----------------------------------------------------------------------------
// Process Messages Received from U1 across Inter-ESP Bus
// ----------------------------------------------------------------------------
void processU1Message(const String &line) {
  // Check for Heartbeat telemetry:
  // {"type":"status","mac":"...","uptime":...,"channel":...}
  if (line.indexOf("\"type\":\"status\"") >= 0) {
    u1Online = true;
    lastU1Heartbeat = millis();

    int macIdx = line.indexOf("\"mac\":\"");
    if (macIdx >= 0) {
      u1MacAddress = line.substring(macIdx + 7, macIdx + 24);
    }
    int chIdx = line.indexOf("\"channel\":");
    if (chIdx >= 0) {
      u1Channel = line.substring(chIdx + 10).toInt();
    }
    Serial.printf("[U1 HB] Online! MAC=%s, Ch=%d\n", u1MacAddress.c_str(),
                  u1Channel);
    return;
  }

  // Check for Outbound Transmission ACK: {"type":"send_ack",...}
  if (line.indexOf("\"type\":\"send_ack\"") >= 0) {
    Serial.print(F("[U1 ACK] "));
    Serial.println(line);
    return;
  }

  // Incoming ESP-NOW packet: {"mac":"...","rssi":...,"len":...,"data":...}
  int macIdx = line.indexOf("\"mac\":\"");
  if (macIdx < 0)
    return;

  String srcMac = line.substring(macIdx + 7, macIdx + 24);
  String cleanMac = srcMac;
  cleanMac.replace(":", "_");

  int pktRssi = 0;
  int rssiIdx = line.indexOf("\"rssi\":");
  if (rssiIdx >= 0) {
    pktRssi = line.substring(rssiIdx + 7).toInt();
    lastEspNowRssi = pktRssi;
    hasLastEspNowRssi = true;
  }

  int dataIdx = line.indexOf("\"data\":");
  if (dataIdx < 0)
    return;

  String payload = line.substring(dataIdx + 7);
  payload.trim();
  if (payload.endsWith("}")) {
    payload = payload.substring(0, payload.length() - 1);
  }
  payload.trim();
  if (payload.startsWith("\"") && payload.endsWith("\"")) {
    payload = payload.substring(1, payload.length() - 1);
  }

  statsRxPackets++;
  u1Online = true;
  lastU1Heartbeat = millis();

  // Blue pulse for packet reception
  pulseRgbColor(LED_BLUE, 70);

  // Publish to MQTT: <prefix>/rx/<CLEAN_MAC>
  String topic = String(config.mqtt_topic_prefix) + "/rx/" + cleanMac;
  bool pubSuccess = false;
  if (mqttClient.connected()) {
    pubSuccess = mqttClient.publish(topic.c_str(), payload.c_str());
    if (pubSuccess)
      statsMqttPublished++;
  }

  logMessage(srcMac, pktRssi, payload, topic, pubSuccess);
  Serial.printf("[GATEWAY RX] %s (%d dBm) -> MQTT '%s' [%s]\n", srcMac.c_str(),
                pktRssi, topic.c_str(), pubSuccess ? "PUB OK" : "PUB FAILED");
}

// ----------------------------------------------------------------------------
// Web UI Dashboard Handler (Root /)
// ----------------------------------------------------------------------------
void handleRoot() {
  String html = F(R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-S3 ESP-NOW to MQTT Gateway</title>
  <style>
    :root {
      --bg: #0b0f19;
      --card-bg: rgba(22, 30, 49, 0.75);
      --card-border: rgba(255, 255, 255, 0.08);
      --primary: #38bdf8;
      --accent: #818cf8;
      --text: #f1f5f9;
      --text-muted: #94a3b8;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: radial-gradient(circle at 50% 0%, #1e1e38 0%, var(--bg) 75%);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      min-height: 100vh;
      padding: 1.5rem;
    }
    .container { max-width: 1080px; margin: 0 auto; }
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 2rem;
      flex-wrap: wrap;
      gap: 1rem;
      border-bottom: 1px solid var(--card-border);
      padding-bottom: 1rem;
    }
    .title-group { display: flex; align-items: center; gap: 0.75rem; }
    .logo-pulse {
      width: 14px;
      height: 14px;
      border-radius: 50%;
      background: var(--success);
      box-shadow: 0 0 12px var(--success);
      animation: pulse 2s infinite;
    }
    @keyframes pulse {
      0%, 100% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.4; transform: scale(0.85); }
    }
    h1 { font-size: 1.5rem; font-weight: 700; letter-spacing: -0.02em; }
    .badge {
      display: inline-flex;
      align-items: center;
      padding: 0.25rem 0.65rem;
      border-radius: 9999px;
      font-size: 0.75rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }
    .badge-success { background: rgba(16, 185, 129, 0.15); color: var(--success); border: 1px solid rgba(16, 185, 129, 0.3); }
    .badge-warning { background: rgba(245, 158, 11, 0.15); color: var(--warning); border: 1px solid rgba(245, 158, 11, 0.3); }
    .badge-danger { background: rgba(239, 68, 68, 0.15); color: var(--danger); border: 1px solid rgba(239, 68, 68, 0.3); }

    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 1rem; margin-bottom: 1.5rem; }
    .card {
      background: var(--card-bg);
      backdrop-filter: blur(12px);
      border: 1px solid var(--card-border);
      border-radius: 12px;
      padding: 1.25rem;
      box-shadow: 0 8px 30px rgba(0, 0, 0, 0.25);
    }
    .card-title { font-size: 0.825rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 0.5rem; display: flex; justify-content: space-between; align-items: center; }
    .card-val { font-size: 1.35rem; font-weight: 700; margin-bottom: 0.35rem; }
    .card-sub { font-size: 0.8rem; color: var(--text-muted); }

    .panel {
      background: var(--card-bg);
      backdrop-filter: blur(12px);
      border: 1px solid var(--card-border);
      border-radius: 12px;
      padding: 1.25rem;
      margin-bottom: 1.5rem;
    }
    .panel-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem; }
    .panel-title { font-size: 1.1rem; font-weight: 600; }
    
    table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
    th { text-align: left; padding: 0.75rem; color: var(--text-muted); border-bottom: 1px solid var(--card-border); font-weight: 600; }
    td { padding: 0.75rem; border-bottom: 1px solid var(--card-border); word-break: break-all; }
    tr:last-child td { border-bottom: none; }
    .code-box { font-family: monospace; background: rgba(0,0,0,0.3); padding: 0.2rem 0.4rem; border-radius: 4px; font-size: 0.8rem; }

    .btn {
      display: inline-flex;
      align-items: center;
      gap: 0.5rem;
      background: var(--primary);
      color: #04101e;
      border: none;
      padding: 0.5rem 1rem;
      border-radius: 8px;
      font-size: 0.85rem;
      font-weight: 600;
      cursor: pointer;
      text-decoration: none;
      transition: all 0.2s;
    }
    .btn:hover { filter: brightness(1.1); transform: translateY(-1px); }
    .btn-secondary { background: rgba(255, 255, 255, 0.08); color: var(--text); }

    .send-box { display: flex; gap: 0.5rem; flex-wrap: wrap; margin-top: 0.75rem; }
    .send-box input {
      flex: 1;
      min-width: 200px;
      padding: 0.6rem 0.8rem;
      background: rgba(0, 0, 0, 0.35);
      border: 1px solid var(--card-border);
      border-radius: 6px;
      color: var(--text);
      font-size: 0.9rem;
    }
    .send-box input:focus { outline: 1px solid var(--primary); }

    .switch-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-top: 0.75rem;
      padding-top: 0.65rem;
      border-top: 1px solid var(--card-border);
      font-size: 0.8rem;
      color: var(--text-muted);
    }
    .switch {
      position: relative;
      display: inline-block;
      width: 40px;
      height: 22px;
      flex-shrink: 0;
    }
    .switch input { opacity: 0; width: 0; height: 0; }
    .slider {
      position: absolute;
      cursor: pointer;
      inset: 0;
      background: rgba(255, 255, 255, 0.16);
      border-radius: 9999px;
      transition: 0.2s;
    }
    .slider:before {
      position: absolute;
      content: "";
      height: 16px;
      width: 16px;
      left: 3px;
      bottom: 3px;
      background: #f1f5f9;
      border-radius: 50%;
      transition: 0.2s;
    }
    .switch input:checked + .slider { background: var(--primary); }
    .switch input:checked + .slider:before { transform: translateX(18px); background: #04101e; }

    .rssi-trend {
      margin-top: 0.75rem;
      padding-top: 0.6rem;
      border-top: 1px solid var(--card-border);
    }
    .rssi-trend-hdr {
      display: flex;
      justify-content: space-between;
      font-size: 0.72rem;
      color: var(--text-muted);
      margin-bottom: 0.3rem;
    }
    .rssi-svg {
      width: 100%;
      height: 46px;
      display: block;
      background: rgba(0, 0, 0, 0.22);
      border-radius: 6px;
    }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="title-group">
        <div class="logo-pulse"></div>
        <div>
          <h1>ESP32-S3 ESP-NOW to MQTT Gateway</h1>
          <p style="font-size: 0.8rem; color: var(--text-muted);">Dual Seeed Studio XIAO ESP32-S3 Hardware Bridge</p>
        </div>
      </div>
      <div>
        <a href="/config" class="btn btn-secondary">Settings & Setup</a>
      </div>
    </header>

    <div class="grid">
      <!-- WiFi Status -->
      <div class="card">
        <div class="card-title">
          <span>WiFi (U10 Gateway)</span>
          <span id="badge-wifi" class="badge badge-warning">Checking...</span>
        </div>
        <div class="card-val" id="val-wifi-ssid">Connecting...</div>
        <div class="card-sub" id="val-wifi-ip">IP: ---</div>
        <div class="rssi-trend">
          <div class="rssi-trend-hdr">
            <span>RSSI Trend (1m/pt)</span>
            <span id="val-rssi-range">--</span>
          </div>
          <svg id="rssi-chart" class="rssi-svg" viewBox="0 0 240 46" preserveAspectRatio="none">
            <line x1="0" y1="27" x2="240" y2="27" stroke="rgba(245,158,11,0.35)" stroke-width="1" stroke-dasharray="3,3" />
            <polygon id="rssi-area" fill="rgba(56,189,248,0.16)" points="" />
            <polyline id="rssi-line" fill="none" stroke="#38bdf8" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round" points="" />
          </svg>
        </div>
      </div>

      <!-- MQTT Status -->
      <div class="card">
        <div class="card-title">
          <span>MQTT Broker</span>
          <span id="badge-mqtt" class="badge badge-warning">Checking...</span>
        </div>
        <div class="card-val" id="val-mqtt-server">Connecting...</div>
        <div class="card-sub" id="val-mqtt-prefix">Prefix: ---</div>
      </div>

      <!-- Inter-ESP U1 Receiver Status -->
      <div class="card">
        <div class="card-title">
          <span>ESP-NOW Link (U1)</span>
          <span id="badge-u1" class="badge badge-warning">Checking...</span>
        </div>
        <div class="card-val" id="val-u1-status">Waiting...</div>
        <div class="card-sub" id="val-u1-mac">U1 MAC: ---</div>
      </div>

      <!-- Hardware & Display Status -->
      <div class="card">
        <div class="card-title">
          <span>Hardware & OLED</span>
          <span id="badge-oled" class="badge badge-warning">Scanning</span>
        </div>
        <div class="card-val" id="val-led-status">RGB Active</div>
        <div class="card-sub" id="val-oled-status">I2C: D4 (SDA), D5 (SCL)</div>
        <div id="oled-timeout-row" class="switch-row">
          <span id="lbl-oled-timeout">30s Screen Timeout</span>
          <label class="switch">
            <input type="checkbox" id="chk-oled-timeout" onchange="setOledTimeout(this.checked)">
            <span class="slider"></span>
          </label>
        </div>
      </div>
    </div>

    <!-- Packet Counters -->
    <div class="grid" style="grid-template-columns: repeat(3, 1fr);">
      <div class="card">
        <div class="card-title">ESP-NOW Received (U1)</div>
        <div class="card-val" id="cnt-rx" style="color: var(--primary);">0</div>
        <div class="card-sub">Forwarded over serial D6/D7</div>
      </div>
      <div class="card">
        <div class="card-title">MQTT Published</div>
        <div class="card-val" id="cnt-pub" style="color: var(--success);">0</div>
        <div class="card-sub">Delivered to broker topic</div>
      </div>
      <div class="card">
        <div class="card-title">ESP-NOW Transmitted (TX)</div>
        <div class="card-val" id="cnt-tx" style="color: var(--accent);">0</div>
        <div class="card-sub">Outbound downlink commands</div>
      </div>
    </div>

    <!-- Send Command Panel -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">Outbound ESP-NOW Transmitter</div>
        <span style="font-size: 0.8rem; color: var(--text-muted);">Send test packet via U1</span>
      </div>
      <div class="send-box">
        <input type="text" id="tx-target" placeholder="Target MAC (default FF:FF:FF:FF:FF:FF for broadcast)">
        <input type="text" id="tx-data" placeholder="JSON or text payload: {\"cmd\":\"ping\"}">
        <button class="btn" onclick="sendOutbound()">Transmit</button>
      </div>
      <div id="tx-status" style="font-size: 0.8rem; margin-top: 0.5rem; color: var(--text-muted);"></div>
    </div>

    <!-- Recent Packets Table -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">Live Received Packets</div>
        <span style="font-size: 0.8rem; color: var(--text-muted);" id="live-indicator">Polling active (2s)</span>
      </div>
      <table>
        <thead>
          <tr>
            <th style="width: 12%;">Age</th>
            <th style="width: 22%;">Transmitter MAC</th>
            <th style="width: 12%;">RSSI</th>
            <th style="width: 26%;">MQTT Topic</th>
            <th style="width: 28%;">Payload</th>
          </tr>
        </thead>
        <tbody id="messages-tbody">
          <tr><td colspan="5" style="text-align: center; color: var(--text-muted); padding: 2rem;">Awaiting ESP-NOW sensor transmissions...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <script>
    function renderRssiTrend(hist, currentRssi) {
      const pts = (hist && hist.length) ? hist.slice() : [];
      if (typeof currentRssi === 'number' && currentRssi < 0) {
        if (pts.length === 0 || pts[pts.length - 1] !== currentRssi) pts.push(currentRssi);
      }
      const rangeEl = document.getElementById('val-rssi-range');
      const lineEl = document.getElementById('rssi-line');
      const areaEl = document.getElementById('rssi-area');
      if (pts.length === 0) {
        rangeEl.textContent = '--';
        lineEl.setAttribute('points', '');
        areaEl.setAttribute('points', '');
        return;
      }
      const minV = Math.min(...pts);
      const maxV = Math.max(...pts);
      rangeEl.textContent = `${minV}..${maxV} dBm (${pts.length}m)`;
      const w = 240, h = 46, pad = 3;
      const clampY = (v) => {
        const clamped = Math.max(-90, Math.min(-30, v));
        return pad + ((-30 - clamped) / 60) * (h - pad * 2);
      };
      const coords = pts.map((v, i) => {
        const x = pts.length === 1 ? w : (i / (pts.length - 1)) * w;
        return `${x.toFixed(1)},${clampY(v).toFixed(1)}`;
      });
      if (pts.length === 1) coords.unshift(`0.0,${clampY(pts[0]).toFixed(1)}`);
      const polyPts = coords.join(' ');
      lineEl.setAttribute('points', polyPts);
      areaEl.setAttribute('points', `0,${h} ${polyPts} ${w},${h}`);
    }

    async function updateDashboard() {
      try {
        const res = await fetch('/api/status');
        const data = await res.json();

        // WiFi
        const badgeWifi = document.getElementById('badge-wifi');
        if (data.wifi_connected) {
          badgeWifi.className = 'badge badge-success';
          badgeWifi.textContent = 'Connected';
          document.getElementById('val-wifi-ssid').textContent = data.wifi_ssid;
          document.getElementById('val-wifi-ip').textContent = `IP: ${data.wifi_ip} (${data.wifi_rssi} dBm | ${data.wifi_bssid || ''})`;
          renderRssiTrend(data.wifi_rssi_history || [], data.wifi_rssi);
        } else {
          badgeWifi.className = 'badge badge-danger';
          badgeWifi.textContent = 'Disconnected';
          document.getElementById('val-wifi-ssid').textContent = 'Connecting...';
        }

        // MQTT
        const badgeMqtt = document.getElementById('badge-mqtt');
        if (data.mqtt_connected) {
          badgeMqtt.className = 'badge badge-success';
          badgeMqtt.textContent = 'Connected';
          document.getElementById('val-mqtt-server').textContent = `${data.mqtt_server}:${data.mqtt_port}`;
          document.getElementById('val-mqtt-prefix').textContent = `Base Topic: ${data.mqtt_prefix || 'espnow/gateway'}`;
        } else {
          badgeMqtt.className = 'badge badge-danger';
          badgeMqtt.textContent = 'Disconnected';
          document.getElementById('val-mqtt-server').textContent = 'Reconnecting...';
        }

        // Inter-ESP U1 Link
        const badgeU1 = document.getElementById('badge-u1');
        if (data.u1_online) {
          badgeU1.className = 'badge badge-success';
          badgeU1.textContent = 'Online';
          const rssiTxt = (data.u1_last_rssi !== null && data.u1_last_rssi !== undefined) ? ` (${data.u1_last_rssi} dBm)` : '';
          document.getElementById('val-u1-status').textContent = `Channel ${data.u1_channel}${rssiTxt}`;
          document.getElementById('val-u1-mac').textContent = `MAC: ${data.u1_mac}`;
        } else {
          badgeU1.className = 'badge badge-danger';
          badgeU1.textContent = 'No Heartbeat';
          document.getElementById('val-u1-status').textContent = 'Offline';
          document.getElementById('val-u1-mac').textContent = 'Check D6/D7 Serial connection';
        }

        // OLED & LED
        const badgeOled = document.getElementById('badge-oled');
        const timeoutRow = document.getElementById('oled-timeout-row');
        const chkTimeout = document.getElementById('chk-oled-timeout');
        if (data.oled_present) {
          badgeOled.className = data.oled_awake ? 'badge badge-success' : 'badge badge-warning';
          badgeOled.textContent = data.oled_awake ? 'OLED Active' : 'OLED Sleeping';
          const modeTxt = data.oled_timeout_enabled ? (data.oled_awake ? 'ON (30s timer)' : 'Touch D3 to wake') : 'Always ON';
          document.getElementById('val-oled-status').textContent = `SSD1306 (${data.oled_addr}) - ${modeTxt}`;
          timeoutRow.style.display = 'flex';
          chkTimeout.checked = !!data.oled_timeout_enabled;
        } else {
          badgeOled.className = 'badge badge-warning';
          badgeOled.textContent = 'Not Fitted';
          document.getElementById('val-oled-status').textContent = 'Display optional (not detected)';
          timeoutRow.style.display = 'none';
        }
        document.getElementById('val-led-status').textContent = `LED: ${data.led_color}`;

        // Counters
        document.getElementById('cnt-rx').textContent = data.stats_rx;
        document.getElementById('cnt-tx').textContent = data.stats_tx;
        document.getElementById('cnt-pub').textContent = data.stats_published;

        // Messages Table
        const tbody = document.getElementById('messages-tbody');
        if (data.messages && data.messages.length > 0) {
          let html = '';
          const now = data.current_time;
          for (let i = data.messages.length - 1; i >= 0; i--) {
            const m = data.messages[i];
            const ageSec = Math.max(0, Math.round((now - m.ts) / 1000));
            let ageStr = ageSec < 60 ? `${ageSec}s ago` : `${Math.round(ageSec / 60)}m ago`;
            html += `<tr>
              <td><span style="color: var(--text-muted);">${ageStr}</span></td>
              <td><span class="code-box">${m.mac}</span></td>
              <td><span style="color: var(--primary);">${m.rssi} dBm</span></td>
              <td><span class="code-box">${m.topic}</span></td>
              <td><span style="color: var(--success);">${m.payload}</span></td>
            </tr>`;
          }
          tbody.innerHTML = html;
        }
      } catch (e) {
        console.error('Failed to fetch status:', e);
      }
    }

    async function sendOutbound() {
      const target = document.getElementById('tx-target').value.trim() || 'FF:FF:FF:FF:FF:FF';
      const data = document.getElementById('tx-data').value.trim();
      const statusEl = document.getElementById('tx-status');
      if (!data) {
        statusEl.textContent = 'Please enter a message payload.';
        return;
      }
      statusEl.textContent = 'Sending...';
      try {
        const formData = new FormData();
        formData.append('target', target);
        formData.append('data', data);
        const res = await fetch('/api/send', { method: 'POST', body: formData });
        const txt = await res.text();
        statusEl.textContent = txt;
        setTimeout(() => { statusEl.textContent = ''; }, 4000);
      } catch (err) {
        statusEl.textContent = 'Error sending message.';
      }
    }

    async function setOledTimeout(enabled) {
      try {
        await fetch(`/api/oled_timeout?enabled=${enabled ? 'true' : 'false'}`, { method: 'POST' });
        updateDashboard();
      } catch (err) {
        console.error('Failed to update OLED timeout:', err);
      }
    }

    setInterval(updateDashboard, 2000);
    updateDashboard();
  </script>
</body>
</html>)rawliteral");

  server.send(200, "text/html", html);
}

// ----------------------------------------------------------------------------
// API Status Endpoint (/api/status)
// ----------------------------------------------------------------------------
void handleApiStatus() {
  String json = "{";
  json += "\"wifi_connected\":" +
          String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"wifi_ssid\":\"" + String(WiFi.SSID()) + "\",";
  json += "\"wifi_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"wifi_bssid\":\"" + WiFi.BSSIDstr() + "\",";
  json += "\"wifi_rssi_history\":[";
  int startIdx = (rssiHistoryCount < RSSI_HISTORY_SIZE) ? 0 : rssiHistoryHead;
  for (int i = 0; i < rssiHistoryCount; i++) {
    int idx = (startIdx + i) % RSSI_HISTORY_SIZE;
    if (i > 0)
      json += ",";
    json += String(rssiHistory[idx]);
  }
  json += "],";

  json += "\"mqtt_connected\":" +
          String(mqttClient.connected() ? "true" : "false") + ",";
  json += "\"mqtt_server\":\"" + String(config.mqtt_server) + "\",";
  json += "\"mqtt_port\":" + String(config.mqtt_port) + ",";
  json += "\"mqtt_prefix\":\"" + String(config.mqtt_topic_prefix) + "\",";

  // Check U1 online timeout (considered offline if no packet or heartbeat for
  // 12s)
  if (millis() - lastU1Heartbeat > 12000) {
    u1Online = false;
  }
  json += "\"u1_online\":" + String(u1Online ? "true" : "false") + ",";
  json += "\"u1_mac\":\"" + u1MacAddress + "\",";
  json += "\"u1_channel\":" + String(u1Channel) + ",";
  json += "\"u1_last_rssi\":" +
          (hasLastEspNowRssi ? String(lastEspNowRssi) : String("null")) + ",";

  json += "\"stats_rx\":" + String(statsRxPackets) + ",";
  json += "\"stats_tx\":" + String(statsTxPackets) + ",";
  json += "\"stats_published\":" + String(statsMqttPublished) + ",";
  json += "\"current_time\":" + String(millis()) + ",";

#if ENABLE_OLED_SUPPORT
  json += "\"oled_present\":" + String(oledFound ? "true" : "false") + ",";
  json +=
      "\"oled_awake\":" + String((oledFound && oledAwake) ? "true" : "false") +
      ",";
  json += "\"oled_timeout_enabled\":" +
          String(config.oled_timeout_enabled ? "true" : "false") + ",";
  char addrBuf[8];
  snprintf(addrBuf, sizeof(addrBuf), "0x%02X", oledAddress);
  json += "\"oled_addr\":\"" + String(addrBuf) + "\",";
#else
  json += "\"oled_present\":false,\"oled_awake\":false,\"oled_timeout_"
          "enabled\":false,\"oled_addr\":\"None\",";
#endif
  json += "\"touch_raw\":" + String(lastTouchReading) + ",";
  json += "\"touch_baseline\":" + String(touchBaseline) + ",";
  json += "\"touch_threshold\":" + String(touchThresholdDelta) + ",";

  String colorStr = "Green (Normal Operation)";
  if (WiFi.status() != WL_CONNECTED)
    colorStr = "Red (Connecting WiFi)";
  else if (!mqttClient.connected())
    colorStr = "Yellow (Connecting MQTT)";
  json += "\"led_color\":\"" + colorStr + "\",";

  // Messages log
  json += "\"messages\":[";
  for (int i = 0; i < messageHistoryCount; i++) {
    int idx = (messageHistoryHead + i) % MAX_LOG_ENTRIES;
    if (i > 0)
      json += ",";
    json += "{";
    json += "\"ts\":" + String(messageHistory[idx].timestamp) + ",";
    json += "\"mac\":\"" + messageHistory[idx].sourceMac + "\",";
    json += "\"rssi\":" + String(messageHistory[idx].rssi) + ",";
    json += "\"topic\":\"" + messageHistory[idx].mqttTopic + "\",";

    String escaped = "";
    for (size_t c = 0; c < messageHistory[idx].payload.length(); c++) {
      char ch = messageHistory[idx].payload[c];
      if (ch == '"')
        escaped += "\\\"";
      else if (ch == '\\')
        escaped += "\\\\";
      else if (ch == '\n')
        escaped += " ";
      else if (ch == '\r')
        continue;
      else
        escaped += ch;
    }
    json += "\"payload\":\"" + escaped + "\",";
    json +=
        "\"success\":" + String(messageHistory[idx].success ? "true" : "false");
    json += "}";
  }
  json += "]";

  json += "}";
  server.send(200, "application/json", json);
}

// ----------------------------------------------------------------------------
// API OLED Screen Timeout Switch (/api/oled_timeout)
// Supports GET (query status or ?enabled=true|false|1|0) and POST
// ----------------------------------------------------------------------------
void handleApiOledTimeout() {
  if (server.hasArg("enabled")) {
    String val = server.arg("enabled");
    val.trim();
    val.toLowerCase();

    if (val == "toggle") {
      config.oled_timeout_enabled = !config.oled_timeout_enabled;
    } else if (val == "true" || val == "1" || val == "on") {
      config.oled_timeout_enabled = true;
    } else if (val == "false" || val == "0" || val == "off") {
      config.oled_timeout_enabled = false;
    } else {
      server.send(400, "application/json",
                  "{\"error\":\"Invalid 'enabled' value. Use true/false, 1/0, "
                  "or toggle.\"}");
      return;
    }

    saveConfig();
#if ENABLE_OLED_SUPPORT
    if (oledFound) {
      setOledPower(true); // Wake screen immediately or reset 30s timer
      updateOledDisplay();
    }
#endif
    Serial.printf("[OLED] Screen timeout %s via API.\n",
                  config.oled_timeout_enabled ? "ENABLED (30s)"
                                              : "DISABLED (Always ON)");
  } else if (server.method() == HTTP_POST) {
    config.oled_timeout_enabled = !config.oled_timeout_enabled;
    saveConfig();
#if ENABLE_OLED_SUPPORT
    if (oledFound) {
      setOledPower(true);
      updateOledDisplay();
    }
#endif
    Serial.printf("[OLED] Screen timeout %s via API.\n",
                  config.oled_timeout_enabled ? "ENABLED (30s)"
                                              : "DISABLED (Always ON)");
  }

  String resp = "{";
  resp += "\"oled_timeout_enabled\":" +
          String(config.oled_timeout_enabled ? "true" : "false") + ",";
#if ENABLE_OLED_SUPPORT
  resp +=
      "\"oled_awake\":" + String((oledFound && oledAwake) ? "true" : "false");
#else
  resp += "\"oled_awake\":false";
#endif
  resp += "}";
  server.send(200, "application/json", resp);
}

// ----------------------------------------------------------------------------
// API Send Endpoint (/api/send)
// ----------------------------------------------------------------------------
void handleApiSend() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  String target = server.arg("target");
  String data = server.arg("data");
  if (target.length() == 0)
    target = "FF:FF:FF:FF:FF:FF";

  if (data.length() == 0) {
    server.send(400, "text/plain", "Missing data parameter");
    return;
  }

  // Construct command JSON to send to U1 over hardware UART
  String cmdJson = "{\"cmd\":\"send\",\"target\":\"";
  cmdJson += target;
  cmdJson += "\",\"data\":";

  if (data.startsWith("{") && data.endsWith("}")) {
    cmdJson += data;
  } else {
    cmdJson += "\"";
    for (size_t i = 0; i < data.length(); i++) {
      char c = data[i];
      if (c == '"')
        cmdJson += "\\\"";
      else if (c == '\\')
        cmdJson += "\\\\";
      else
        cmdJson += c;
    }
    cmdJson += "\"";
  }
  cmdJson += "}\n";

  busSerial.print(cmdJson);
  statsTxPackets++;
  pulseRgbColor(LED_CYAN, 80);

  server.send(200, "text/plain",
              "Packet queued for ESP-NOW transmission via U1.");
}

// ----------------------------------------------------------------------------
// Web Configuration Page (/config)
// ----------------------------------------------------------------------------
void handleConfig() {
  if (server.method() == HTTP_POST) {
    strncpy(config.wifi_ssid, server.arg("ssid").c_str(),
            sizeof(config.wifi_ssid));
    if (server.arg("pass").length() > 0) {
      strncpy(config.wifi_pass, server.arg("pass").c_str(),
              sizeof(config.wifi_pass));
    }
    strncpy(config.mqtt_server, server.arg("mqtt_host").c_str(),
            sizeof(config.mqtt_server));
    config.mqtt_port = server.arg("mqtt_port").toInt();
    strncpy(config.mqtt_user, server.arg("mqtt_user").c_str(),
            sizeof(config.mqtt_user));
    if (server.arg("mqtt_pass").length() > 0) {
      strncpy(config.mqtt_pass, server.arg("mqtt_pass").c_str(),
              sizeof(config.mqtt_pass));
    }
    strncpy(config.mqtt_client_id, server.arg("mqtt_cid").c_str(),
            sizeof(config.mqtt_client_id));
    strncpy(config.mqtt_topic_prefix, server.arg("mqtt_pre").c_str(),
            sizeof(config.mqtt_topic_prefix));

    saveConfig();

    String successHtml =
        F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta "
          "http-equiv='refresh' "
          "content='4;url=/"
          "'><style>body{background:#0b0f19;color:#fff;font-family:sans-serif;"
          "text-align:center;padding:5rem;}</style></head><body><h2>Settings "
          "Saved!</h2><p>Rebooting gateway to apply "
          "settings...</p></body></html>");
    server.send(200, "text/html", successHtml);
    delay(1000);
    ESP.restart();
    return;
  }

  String html = F(R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Gateway Settings</title>
  <style>
    body { background: #0b0f19; color: #f1f5f9; font-family: sans-serif; padding: 2rem; max-width: 600px; margin: 0 auto; }
    h1 { font-size: 1.4rem; margin-bottom: 1.5rem; }
    .form-group { margin-bottom: 1.25rem; }
    label { display: block; font-size: 0.85rem; color: #94a3b8; margin-bottom: 0.35rem; }
    input { width: 100%; padding: 0.6rem; background: #161e31; border: 1px solid rgba(255,255,255,0.1); color: #fff; border-radius: 6px; box-sizing: border-box; }
    .btn { background: #38bdf8; color: #04101e; border: none; padding: 0.75rem 1.5rem; font-weight: bold; border-radius: 6px; cursor: pointer; }
    .btn-cancel { background: transparent; color: #94a3b8; text-decoration: none; margin-left: 1rem; }
  </style>
</head>
<body>
  <h1>Gateway Configuration</h1>
  <form method="POST">
    <div class="form-group">
      <label>WiFi SSID</label>
      <input type="text" name="ssid" value=")rawliteral");
  html += String(config.wifi_ssid);
  html += F(R"rawliteral(" required>
    </div>
    <div class="form-group">
      <label>WiFi Password (leave blank to keep current)</label>
      <input type="password" name="pass" placeholder="********">
    </div>
    <div class="form-group">
      <label>MQTT Broker IP / Hostname</label>
      <input type="text" name="mqtt_host" value=")rawliteral");
  html += String(config.mqtt_server);
  html += F(R"rawliteral(" required>
    </div>
    <div class="form-group">
      <label>MQTT Port</label>
      <input type="number" name="mqtt_port" value=")rawliteral");
  html += String(config.mqtt_port);
  html += F(R"rawliteral(" required>
    </div>
    <div class="form-group">
      <label>MQTT Client ID</label>
      <input type="text" name="mqtt_cid" value=")rawliteral");
  html += String(config.mqtt_client_id);
  html += F(R"rawliteral(" required>
    </div>
    <div class="form-group">
      <label>MQTT Username (Optional)</label>
      <input type="text" name="mqtt_user" value=")rawliteral");
  html += String(config.mqtt_user);
  html += F(R"rawliteral(">
    </div>
    <div class="form-group">
      <label>MQTT Password (Optional)</label>
      <input type="password" name="mqtt_pass" placeholder="********">
    </div>
    <div class="form-group">
      <label>MQTT Base Topic Prefix</label>
      <input type="text" name="mqtt_pre" value=")rawliteral");
  html += String(config.mqtt_topic_prefix);
  html += F(R"rawliteral(" required>
    </div>
    <div style="margin-top: 2rem;">
      <button type="submit" class="btn">Save & Restart</button>
      <a href="/" class="btn-cancel">Cancel</a>
    </div>
  </form>
</body>
</html>)rawliteral");

  server.send(200, "text/html", html);
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
  // Initialize RGB LED pins (Active LOW: set HIGH initially to turn OFF)
  pinMode(LED_PIN_RED, OUTPUT);
  pinMode(LED_PIN_GREEN, OUTPUT);
  pinMode(LED_PIN_BLUE, OUTPUT);
  setRgbColor(LED_OFF);

  // Initialize USB Serial for diagnostics
  Serial.begin(SERIAL_DEBUG_BAUD);
  delay(100);

  Serial.println(F("\n\n========================================"));
  Serial.println(F(" Xiao ESP32-S3 Gateway: U10 Gateway"));
  Serial.println(F(" WiFi + MQTT + Web Server + OLED Controller"));
  Serial.println(F("========================================"));

  // Detach D6/D7 (GPIO43/GPIO44) from default UART0 IOMUX before attaching
  // UART1
  pinMode(BUS_RX_PIN, INPUT);
  pinMode(BUS_TX_PIN, OUTPUT);

  // Initialize Hardware UART1 for Inter-ESP Bus (RX=D6, TX=D7)
  busSerial.begin(BUS_BAUD_RATE, SERIAL_8N1, BUS_RX_PIN, BUS_TX_PIN);
  Serial.print(F("Inter-ESP Hardware UART initialized (RX=D6, TX=D7 at "));
  Serial.print(BUS_BAUD_RATE);
  Serial.println(F(" baud)"));

  // Solid Red: Connecting to WiFi
  setRgbColor(LED_RED);

  // Load configuration from Flash NVS
  loadConfig();

  // Initialize I2C and optional OLED display
  initI2CAndDisplay();

  // Connect to WiFi (clear stale WiFi NVS state, disable modem sleep, pick
  // strongest AP)
  Serial.print(F("[WiFi] Connecting to "));
  Serial.println(config.wifi_ssid);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  int nNets = WiFi.scanNetworks();
  for (int i = 0; i < nNets; i++) {
    if (WiFi.SSID(i) == String(config.wifi_ssid)) {
      Serial.printf("[WiFi SCAN] Found '%s' Ch:%d RSSI:%d dBm BSSID:%s\n",
                    WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                    WiFi.BSSIDstr(i).c_str());
    }
  }
  WiFi.scanDelete();
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("[WiFi] Disconnected (reason=%d), will auto-reconnect...\n",
                    info.wifi_sta_disconnected.reason);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.print(F("[WiFi] Got IP: "));
      Serial.println(WiFi.localIP());
    }
  });
  WiFi.begin(config.wifi_ssid, config.wifi_pass);

  unsigned long startWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWifi < 15000) {
    delay(250);
    Serial.print(F("."));
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] Connected! IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[WiFi] Connection timed out! Continuing to start "
                     "WebServer for setup."));
  }

  // Setup Web Server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/send", HTTP_POST, handleApiSend);
  server.on("/api/oled_timeout", handleApiOledTimeout);
  server.on("/config", handleConfig);
  server.begin();
  Serial.println(F("[HTTP] Web Server started on port 80"));

  // Configure MQTT Client
  mqttClient.setServer(config.mqtt_server, config.mqtt_port);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(1024);

  // Ping U1 for initial status
  busSerial.print("{\"cmd\":\"ping\"}\n");

  // Calibrate capacitive touch pad on D3 (GPIO4) after WiFi radio is active
  initTouchSensor();

#if ENABLE_OLED_SUPPORT
  // Start the 30-second OLED display timer from when boot completes
  oledWakeTime = millis();
  oledAwake = true;
#endif

  updateLedState();
  updateOledDisplay();
  Serial.println(F("Gateway initialization complete!\n"));
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void checkStrongerWifiAp() {
  int currentRssi = WiFi.RSSI();
  uint8_t *currentBssid = WiFi.BSSID();
  if (!currentBssid)
    return;

  Serial.printf(
      "[WiFi] Weak RSSI (%d dBm on %s) -> scanning for stronger AP...\n",
      currentRssi, WiFi.BSSIDstr().c_str());

  int n = WiFi.scanNetworks(false, false, false, 300);
  int bestIdx = -1;
  int bestRssi = currentRssi;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == String(config.wifi_ssid)) {
      int rssi = WiFi.RSSI(i);
      if (rssi > bestRssi) {
        bestRssi = rssi;
        bestIdx = i;
      }
    }
  }

  if (bestIdx >= 0 && (bestRssi - currentRssi) >= 10) {
    uint8_t *bestBssid = WiFi.BSSID(bestIdx);
    int32_t bestCh = WiFi.channel(bestIdx);
    if (bestBssid && memcmp(bestBssid, currentBssid, 6) != 0) {
      Serial.printf(
          "[WiFi] Roaming to stronger AP %s (Ch %d, %d dBm -> %d dBm)\n",
          WiFi.BSSIDstr(bestIdx).c_str(), bestCh, currentRssi, bestRssi);
      uint8_t targetBssid[6];
      memcpy(targetBssid, bestBssid, 6);
      WiFi.scanDelete();
      WiFi.disconnect(false);
      WiFi.begin(config.wifi_ssid, config.wifi_pass, bestCh, targetBssid);
      return;
    }
  }

  WiFi.scanDelete();
}

void loop() {
  // Handle HTTP client requests
  server.handleClient();

  // Maintain WiFi and MQTT connections
  if (WiFi.status() == WL_CONNECTED) {
    if (rssiHistoryCount == 0 || millis() - lastRssiSample >= 60000) {
      lastRssiSample = millis();
      rssiHistory[rssiHistoryHead] = (int8_t)WiFi.RSSI();
      rssiHistoryHead = (rssiHistoryHead + 1) % RSSI_HISTORY_SIZE;
      if (rssiHistoryCount < RSSI_HISTORY_SIZE)
        rssiHistoryCount++;
    }
    if (WiFi.RSSI() < -65 && millis() - lastWifiRoamCheck >= 300000) {
      lastWifiRoamCheck = millis();
      checkStrongerWifiAp();
    }
    if (!mqttClient.connected()) {
      reconnectMqtt();
    } else {
      mqttClient.loop();
    }
  } else if (millis() - lastWifiReconnectAttempt >= 30000) {
    lastWifiReconnectAttempt = millis();
    Serial.println(
        F("[WiFi] Still disconnected -> restarting WiFi connection..."));
    WiFi.disconnect(false);
    WiFi.begin(config.wifi_ssid, config.wifi_pass);
  }

  // Read incoming packets from U1 over the inter-ESP bus
  static String busBuffer = "";
  while (busSerial.available()) {
    char c = (char)busSerial.read();
    if (c == '\n') {
      busBuffer.trim();
      if (busBuffer.length() > 0) {
        processU1Message(busBuffer);
      }
      busBuffer = "";
    } else if (c != '\r') {
      if (busBuffer.length() < 1024) {
        busBuffer += c;
      }
    }
  }

  // Periodic ping to U1 every 8 seconds if no heartbeat received
  if (millis() - lastU1Ping >= 8000) {
    lastU1Ping = millis();
    busSerial.print("{\"cmd\":\"ping\"}\n");
  }

  // Update LED states (handles pulses and baseline status)
  updateLedState();

  // Check capacitive touch sensor on D3 and manage 30s OLED sleep timeout
  handleTouchAndOledTimeout();

  // Update OLED display every 500ms (only writes over I2C when awake)
  if (millis() - lastDisplayUpdate >= 500) {
    lastDisplayUpdate = millis();
    updateOledDisplay();
  }
}
