/**
 * ============================================================================
 * ESP32-S3 ESP-NOW to MQTT Gateway - U1 ESP-NOW Receiver Firmware
 * ============================================================================
 * Version 0.1 First Working Version
 * ============================================================================
 * Target Board: Seeed Studio XIAO ESP32S3
 * Role: U1 - Dedicated ESP-NOW Receiver and Forwarder
 *
 * Hardware Connections (from Schematic & PCB):
 *   Inter-ESP Bus (connected to U10):
 *     - D6 (GPIO43): Configured as U1 Hardware UART TX (connects to U10 RX on
 * D6)
 *     - D7 (GPIO44): Configured as U1 Hardware UART RX (connects to U10 TX on
 * D7) USB Type-C:
 *     - USB2+ & USB2- connected when USB-C cable is oriented towards U1.
 *     - Board LED2 (KT-0603R) illuminates to indicate U1 is connected to USB
 * host. Test Points:
 *     - D0, D1, D2, D3, D4, D5 (breakout header / test pads)
 *   Power:
 *     - 5V & GND common rail
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ----------------------------------------------------------------------------
// Pin Definitions for Inter-ESP Bus (Hardware UART)
// ----------------------------------------------------------------------------
// PCB net "TX" connects U10 D7 (TX) <-> U1 D6; net "RX" connects U10 D6 (RX)
// <-> U1 D7. Therefore U1 receives on D6 (from U10 TX on D7) and transmits on
// D7 (to U10 RX on D6).
#define BUS_RX_PIN D6 // Connects to U10 TX (D7) via PCB net "TX"
#define BUS_TX_PIN D7 // Connects to U10 RX (D6) via PCB net "RX"

// ----------------------------------------------------------------------------
// Configuration Constants
// ----------------------------------------------------------------------------
#define BUS_BAUD_RATE 115200
#define SERIAL_DEBUG_BAUD 115200
#define ESPNOW_CHANNEL                                                         \
  1 // Default channel (1-13). Should match transmitter nodes
#define HEARTBEAT_INTERVAL 5000 // Send heartbeat packet to U10 every 5 seconds

// ----------------------------------------------------------------------------
// Objects & Globals
// ----------------------------------------------------------------------------
HardwareSerial busSerial(1); // Hardware UART1 for inter-ESP communication

unsigned long lastHeartbeat = 0;
unsigned long packetCount = 0;
unsigned long lastPacketTime = 0;
uint8_t currentChannel = ESPNOW_CHANNEL;

// Broadcast MAC address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Garage Door Sensor binary message structure (must match sender)
typedef struct struct_message {
  int id;
  char state[7];
  int vBatt;
} struct_message;

// Forward declaration
void sendHeartbeat();

// ----------------------------------------------------------------------------
// Helper: Decode Garage Door struct_message into JSON
// ----------------------------------------------------------------------------
bool tryDecodeGarageStruct(const uint8_t *incomingData, int len,
                           String &outJson) {
  if (len != sizeof(struct_message)) {
    return false;
  }

  struct_message msg;
  memcpy(&msg, incomingData, sizeof(struct_message));

  // Verify state is null-terminated and contains printable ASCII
  size_t stateLen = strnlen(msg.state, sizeof(msg.state));
  if (stateLen >= sizeof(msg.state)) {
    return false;
  }
  for (size_t i = 0; i < stateLen; i++) {
    char c = msg.state[i];
    if (c < 32 || c > 126) {
      return false;
    }
  }

  String escapedState = "";
  for (size_t i = 0; i < stateLen; i++) {
    char c = msg.state[i];
    if (c == '"')
      escapedState += "\\\"";
    else if (c == '\\')
      escapedState += "\\\\";
    else
      escapedState += c;
  }

  outJson = "{\"id\":";
  outJson += String(msg.id);
  outJson += ",\"state\":\"";
  outJson += escapedState;
  outJson += "\",\"vBatt\":";
  outJson += String(msg.vBatt);
  outJson += "}";
  return true;
}

// ----------------------------------------------------------------------------
// Helper: Format MAC Address
// ----------------------------------------------------------------------------
void formatMac(const uint8_t *mac, char *out, size_t maxLen) {
  snprintf(out, maxLen, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

// ----------------------------------------------------------------------------
// ESP-NOW Receive Callback (Supports ESP32 Arduino Core 2.x and 3.x)
// ----------------------------------------------------------------------------
#if defined(ESP_ARDUINO_VERSION) &&                                            \
    (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
void onDataRecv(const esp_now_recv_info_t *recv_info,
                const uint8_t *incomingData, int len) {
  const uint8_t *mac = recv_info->src_addr;
  int rssi = (recv_info->rx_ctrl != nullptr) ? recv_info->rx_ctrl->rssi : 0;
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  int rssi = 0;
#endif

  packetCount++;
  lastPacketTime = millis();

  char macStr[18];
  formatMac(mac, macStr, sizeof(macStr));

  // Determine if payload is valid printable text / JSON
  bool isPrintable = true;
  for (int i = 0; i < len; i++) {
    char c = incomingData[i];
    if (c == 0 && i == len - 1)
      break; // Trailing null is fine
    if (c < 32 && c != '\r' && c != '\n' && c != '\t') {
      isPrintable = false;
      break;
    }
  }

  // Construct JSON packet to forward to U10 over the inter-ESP bus
  String jsonPacket = "{\"mac\":\"";
  jsonPacket += macStr;
  jsonPacket += "\",\"rssi\":";
  jsonPacket += String(rssi);
  jsonPacket += ",\"len\":";
  jsonPacket += String(len);
  jsonPacket += ",\"data\":";

  if (isPrintable) {
    String rawPayload = "";
    for (int i = 0; i < len; i++) {
      if (incomingData[i] != 0) {
        rawPayload += (char)incomingData[i];
      }
    }
    rawPayload.trim();

    // If payload is already valid JSON, embed directly
    if (rawPayload.startsWith("{") && rawPayload.endsWith("}")) {
      jsonPacket += rawPayload;
    } else {
      // Escape for JSON string
      jsonPacket += "\"";
      for (size_t i = 0; i < rawPayload.length(); i++) {
        char c = rawPayload[i];
        if (c == '"')
          jsonPacket += "\\\"";
        else if (c == '\\')
          jsonPacket += "\\\\";
        else if (c == '\n')
          jsonPacket += "\\n";
        else if (c == '\r')
          jsonPacket += "\\r";
        else
          jsonPacket += c;
      }
      jsonPacket += "\"";
    }
  } else {
    String decodedStructJson;
    if (tryDecodeGarageStruct(incomingData, len, decodedStructJson)) {
      jsonPacket += decodedStructJson;
    } else {
      // Hex string for other binary payloads
      jsonPacket += "\"0x";
      for (int i = 0; i < len; i++) {
        char hexBuf[3];
        snprintf(hexBuf, sizeof(hexBuf), "%02X", incomingData[i]);
        jsonPacket += hexBuf;
      }
      jsonPacket += "\"";
    }
  }

  jsonPacket += "}\n";

  // Forward packet to U10 over Inter-ESP hardware serial bus
  busSerial.print(jsonPacket);

  // Print debug log to local USB serial (if connected)
  Serial.print(F("[U1 ESPNOW RECV] From: "));
  Serial.print(macStr);
  Serial.print(F(" ("));
  Serial.print(len);
  Serial.println(F(" bytes) -> Forwarded to U10"));
}

// ----------------------------------------------------------------------------
// ESP-NOW Send Callback (for bidirectional transmission)
// ----------------------------------------------------------------------------
#if defined(ESP_ARDUINO_VERSION) &&                                            \
    (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  Serial.print(F("[U1 ESPNOW SENT] Status: "));
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? F("Success (ACK)")
                                                : F("Delivery Failed"));

  // Send delivery ACK back to U10
  String ackPacket = "{\"type\":\"send_ack\",\"status\":";
  ackPacket += (status == ESP_NOW_SEND_SUCCESS ? "\"ok\"" : "\"fail\"");
  ackPacket += "}\n";
  busSerial.print(ackPacket);
}
#else
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  char macStr[18];
  formatMac(mac_addr, macStr, sizeof(macStr));

  Serial.print(F("[U1 ESPNOW SENT] To: "));
  Serial.print(macStr);
  Serial.print(F(" Status: "));
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? F("Success (ACK)")
                                                : F("Delivery Failed"));

  // Send delivery ACK back to U10
  String ackPacket = "{\"type\":\"send_ack\",\"target\":\"";
  ackPacket += macStr;
  ackPacket += "\",\"status\":";
  ackPacket += (status == ESP_NOW_SEND_SUCCESS ? "\"ok\"" : "\"fail\"");
  ackPacket += "}\n";
  busSerial.print(ackPacket);
}
#endif

// ----------------------------------------------------------------------------
// Helper: Ensure Peer Exists Before Transmitting
// ----------------------------------------------------------------------------
bool ensurePeer(const uint8_t *peerMac) {
  if (esp_now_is_peer_exist(peerMac)) {
    return true;
  }
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = currentChannel;
  peerInfo.encrypt = false;
  return (esp_now_add_peer(&peerInfo) == ESP_OK);
}

// ----------------------------------------------------------------------------
// Handle Incoming Commands from U10
// ----------------------------------------------------------------------------
void processU10Command(const String &cmdLine) {
  Serial.print(F("[U10 BUS RECV] "));
  Serial.println(cmdLine);

  if (cmdLine.indexOf("\"cmd\":\"ping\"") >= 0) {
    sendHeartbeat();
  } else if (cmdLine.indexOf("\"cmd\":\"send\"") >= 0) {
    uint8_t targetMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int targetIdx = cmdLine.indexOf("\"target\":\"");
    if (targetIdx >= 0) {
      String macPart = cmdLine.substring(targetIdx + 10, targetIdx + 27);
      if (macPart.length() == 17) {
        unsigned int m[6];
        if (sscanf(macPart.c_str(), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2],
                   &m[3], &m[4], &m[5]) == 6) {
          for (int i = 0; i < 6; i++)
            targetMac[i] = (uint8_t)m[i];
        }
      }
    }

    int dataIdx = cmdLine.indexOf("\"data\":");
    if (dataIdx >= 0) {
      String payload = cmdLine.substring(dataIdx + 7);
      payload.trim();
      if (payload.endsWith("}"))
        payload = payload.substring(0, payload.length() - 1);
      payload.trim();
      if (payload.startsWith("\"") && payload.endsWith("\"")) {
        payload = payload.substring(1, payload.length() - 1);
      }

      ensurePeer(targetMac);
      esp_err_t res = esp_now_send(targetMac, (const uint8_t *)payload.c_str(),
                                   payload.length());
      if (res != ESP_OK) {
        Serial.print(F("[U1 ERROR] esp_now_send error code: "));
        Serial.println(res);
      }
    }
  } else if (cmdLine.indexOf("\"cmd\":\"channel\"") >= 0) {
    int chIdx = cmdLine.indexOf("\"ch\":");
    if (chIdx >= 0) {
      int newCh = cmdLine.substring(chIdx + 5).toInt();
      if (newCh >= 1 && newCh <= 13) {
        currentChannel = newCh;
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
        Serial.print(F("Switched to WiFi/ESP-NOW Channel: "));
        Serial.println(currentChannel);
      }
    }
  }
}

// ----------------------------------------------------------------------------
// Send Heartbeat Status to U10
// ----------------------------------------------------------------------------
void sendHeartbeat() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  formatMac(mac, macStr, sizeof(macStr));

  String hb = "{\"type\":\"status\",\"mac\":\"";
  hb += macStr;
  hb += "\",\"uptime\":";
  hb += String(millis() / 1000);
  hb += ",\"channel\":";
  hb += String(currentChannel);
  hb += "}\n";

  busSerial.print(hb);
  Serial.print(F("[U1 HB] Sent to U10: "));
  Serial.print(hb);
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
  // Initialize USB Serial for diagnostics / programming
  Serial.begin(SERIAL_DEBUG_BAUD);
  delay(100);

  Serial.println(F("\n\n========================================"));
  Serial.println(F(" Xiao ESP32-S3 Gateway: U1 Receiver"));
  Serial.println(F(" Dedicated ESP-NOW Receiver Node"));
  Serial.println(F("========================================"));

  // Detach D6/D7 (GPIO43/GPIO44) from default UART0 IOMUX before attaching
  // UART1
  pinMode(BUS_RX_PIN, INPUT);
  pinMode(BUS_TX_PIN, OUTPUT);

  // Initialize Inter-ESP Bus (Hardware UART1 on D7 RX, D6 TX)
  busSerial.begin(BUS_BAUD_RATE, SERIAL_8N1, BUS_RX_PIN, BUS_TX_PIN);
  Serial.print(F("Inter-ESP Hardware UART initialized (RX=D7, TX=D6 at "));
  Serial.print(BUS_BAUD_RATE);
  Serial.println(F(" baud)"));

  // Set WiFi to Station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);

  // Set channel
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.print(F("U1 MAC Address: "));
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
  Serial.print(F("ESP-NOW Channel: "));
  Serial.println(currentChannel);

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ERROR: ESP-NOW Initialization Failed!"));
    while (true) {
      delay(1000);
    }
  }

  Serial.println(F("ESP-NOW Initialized successfully."));

  // Register ESP-NOW callbacks
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  // Register broadcast peer
  ensurePeer(broadcastAddress);

  // Send initial heartbeat to U10
  sendHeartbeat();
  Serial.println(F("U1 ready and listening for ESP-NOW transmissions...\n"));
}

// ----------------------------------------------------------------------------
// Loop
// ----------------------------------------------------------------------------
void loop() {
  // Read incoming commands from U10 across the inter-ESP hardware serial bus
  static String inputBuffer = "";
  while (busSerial.available()) {
    char c = (char)busSerial.read();
    if (c == '\n') {
      inputBuffer.trim();
      if (inputBuffer.length() > 0) {
        processU10Command(inputBuffer);
      }
      inputBuffer = "";
    } else if (c != '\r') {
      if (inputBuffer.length() < 512) {
        inputBuffer += c;
      }
    }
  }

  // Periodic heartbeat transmission to U10
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }
}
