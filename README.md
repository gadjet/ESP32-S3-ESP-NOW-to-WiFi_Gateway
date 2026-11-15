# ESP-NOW_WiFi_Gateway
A Gateway between ESP-NOW sensors and your WiFi/MQTT/Pushover......etc

This is a first for me, this project has been sponsored by PCBWay, they have provided the PCBs populated with all the SMD components apart from the Xiao ESP32 S3s.
For PCBs, PCBA, 3D printing and CNC machining visit www.PCBWay.com.

## <mark>### This hardware design is solely done by me without AI assistance but the software is written by Gemini AI based on my instructions ###</mark>


<img width="1840" height="1183" alt="image" src="https://github.com/user-attachments/assets/d3dd373d-776d-4292-bb3b-342299cdb658" />

This design is the next iteration on from a similar design that used two WEMOS D1 Minis. One improvement over the D1 Mini design is that the Xiao ESP32 S3s use external antennas which means I can add a little more separation between the antennas redcucing the interference between the ESP Radios, the D1 Minis worked OK but the ESP-NOW range was not great with the PCB antenna even with the other ESP radio switched off.  I'm hoping for a better ESP-NOW range with an external antenna design.

There are locations for 2 Xaio ESP32 S3s the one on the right will be the ESP-NOW receiver, the one on the left will be connected to the WiFi and receive data from the right hand EPS and transfer it over to MQTT or any other configured service.
The left ESP32 can also be connected to an optional external screen via the I2C connector and has a discrete RGB LED for status display.

Programming the two ESP32s can be done by connecting to the single USBC connector on the PCB which has the two USB data lines, USB1 and USB2 connected to each ESP32 respectively, this means that each ESP32 can be programmed seperately depending on the orientaion of the USB connector, there is an LED under each ESP32 to indicate which ESP32 is connected to the flasher, this uses the CC1/CC2 connections.  One drawback to this approach is that you must use a USB C to USB C connection cable which seperates the USB data connections, a USB A to C cable will not work because it shorts out the two USB busses. 

<img width="2300" height="1200" alt="image" src="https://github.com/user-attachments/assets/104252e2-552b-4ced-a7f1-4e8f55dfe06c" />

To help identify which ESP32 is connected to the USB port each ESP32 has an LED next to its own USB Connector that will illuminate to show which ESP32 is connected.
## Cable detection circuit diagram
<img width="901" height="695" alt="image" src="https://github.com/user-attachments/assets/7eed681c-b6e2-4c65-8595-4f1c3a8aa3d1" />

The circuit uses the two 5.1K pulldown resistors CC1 and CC2, by connecting to these resistors the circuit detects which input is connected to the Vconn pin of the USB Cable, when connected to the Vconn pin the resistor will have a voltage across it that will be greater than the 200mV level set on the non + input of the OPAMP triggering the output low and illuminating the LED.  The other OPAMP will se 0V on the + input and the output will be high and the LED will remain off.

# AI Generated Project Description
## Dual XIAO ESP32-S3 ESP-NOW to MQTT Gateway

An industrial-grade, dual-microcontroller bridge between low-power **ESP-NOW** sensor nodes and an **MQTT broker** (e.g. Home Assistant, Mosquitto, CloudAMQP). Built around two **Seeed Studio XIAO ESP32-S3** modules on a custom PCB.

---

## 1. System Architecture

```
[ Remote ESP-NOW Sensor Nodes ] (Battery powered, deep-sleep, ultra-fast burst)
             │  ESP-NOW (2.4 GHz RF - Channel 1 default)
             ▼
┌────────────────────────────┐
│   Xiao ESP32-S3 (U1)       │  - Dedicated ESP-NOW RX/TX
│   ESP-NOW Receiver Node    │  - Continuous listening (zero packet loss)
└─────────────┬──────────────┘  - Hardware FIFO frame packaging
              │
              │ Inter-ESP Hardware UART (D6 & D7 @ 115200 baud)
              ▼
┌────────────────────────────┐
│   Xiao ESP32-S3 (U10)      │  - WiFi Station (Auto-reconnect)
│   WiFi & MQTT Gateway      │  - MQTT Client (Publish & Subscribe)
└──────┬──────────────┬──────┘  - Web Dashboard & REST API
       │              │         - Common-Anode RGB Status LED (LED1)
       │              └────────► Optional I2C OLED Display (SSD1306 on H1)
       ▼
[ Local MQTT Broker / Home Assistant ]
```

### Why Dual ESP32-S3 Modules?
Standard single-chip ESP-NOW + WiFi gateways suffer from packet drops, channel conflicts, and latency jitter because WiFi station mode and ESP-NOW share the same radio hardware. Using two dedicated Xiao ESP32-S3 modules connected via hardware UART provides:
* **Zero Packet Drops**: U1 is dedicated to ESP-NOW listening 100% of the time.
* **Independent WiFi Channel Management**: U10 connects to any router channel without disrupting U1.
* **Hardware UART FIFO**: Direct hardware serial communication over pins **D6** and **D7** eliminates SoftwareSerial bit-banging CPU overhead.

---

## 2. Reversible USB-C Connector (Flippable Programming)

The custom PCB features an ingenious USB-C circuit:
* **USB Data Lines (D+/D-)**: Only **one** ESP32-S3 is connected to the USB data lines at a time.
* **Flippable Cable**: Turning the USB-C plug over switches the USB connection between **U10 (WiFi/MQTT)** and **U1 (ESP-NOW)**.
* **Orientation Indicator LEDs**: An onboard `LMV393` comparator monitors the USB-C CC1/CC2 lines:
  * **LED3 (WiFi/MQTT)**: Illuminates when the cable is oriented to program **U10**.
  * **LED2 (ESP-NOW)**: Illuminates when the cable is turned over to program **U1**.

---

## 3. Hardware Pin Mapping

### A. U10 (WiFi, MQTT Gateway, Web Server & Display Controller)
| U10 Pin | ESP32-S3 GPIO | Connected Component | Schematic Net | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **D0** | GPIO1 | LED1 Pin 1 via R3 (680Ω) | `red` | **RGB LED: Red** (Active LOW) |
| **D1** | GPIO2 | LED1 Pin 3 via R5 (680Ω) | `green` | **RGB LED: Green** (Active LOW) |
| **D2** | GPIO3 | LED1 Pin 4 via R4 (680Ω) | `blue` | **RGB LED: Blue** (Active LOW) |
| **D3** | GPIO4 | Test Pad D3# (Copper Tape Pad) | `D3#` | **Capacitive Touch (`TOUCH4`)**: Wakes OLED display for 30s |
| **D4** | GPIO5 | Connector H1 Pin 4 + R1 (4.7kΩ pull-up) | `SDA` | I2C Data (Optional OLED) |
| **D5** | GPIO6 | Connector H1 Pin 3 + R2 (4.7kΩ pull-up) | `SCL` | I2C Clock (Optional OLED) |
| **D6** | GPIO43 | U1 Pin 7 (D6) | `RX` | Hardware UART1 RX from U1 |
| **D7** | GPIO44 | U1 Pin 8 (D7) | `TX` | Hardware UART1 TX to U1 |
| **3V3**| +3.3V | VCC Rail | `VCC` | +3.3V Power |
| **GND**| GND | Ground Rail | `GND` | Common Ground |
| **5V** | +5V | USB1 VBUS Rail | `+5V` | System 5V Input |

### B. U1 (Dedicated ESP-NOW Receiver & Forwarder)
| U1 Pin | ESP32-S3 GPIO | Connected Component | Schematic Net | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **D0** | GPIO1 | Test Pad D0 | `D0` | Breakout / Diagnostic Pin |
| **D1** | GPIO2 | Test Pad D1 | `D1` | Breakout / Diagnostic Pin |
| **D2** | GPIO3 | Test Pad D2 | `D2` | Breakout / Diagnostic Pin |
| **D3** | GPIO4 | Test Pad D3 | `D3` | Breakout / Diagnostic Pin |
| **D4** | GPIO5 | Test Pad D4 | `D4` | Breakout / Diagnostic Pin |
| **D5** | GPIO6 | Test Pad D5 | `D5` | Breakout / Diagnostic Pin |
| **D6** | GPIO43 | U10 Pin 7 (D6) | `TX` | Hardware UART1 TX to U10 |
| **D7** | GPIO44 | U10 Pin 8 (D7) | `RX` | Hardware UART1 RX from U10 |
| **5V** | +5V | USB1 VBUS Rail | `+5V` | System 5V Input |
| **GND**| GND | Ground Rail | `GND` | Common Ground |

### C. RGB Status LED (LED1 - Common Anode, Active LOW)
The RGB LED anodes are connected to **VCC (+3.3V)**. Driving a pin **`LOW`** illuminates the color; driving it **`HIGH`** turns it off.

| State / Condition | LED Color | Pin States |
| :--- | :--- | :--- |
| Connecting to WiFi | **Solid Red** | Red(D0): LOW, Green(D1): HIGH, Blue(D2): HIGH |
| WiFi Connected, Connecting to MQTT | **Solid Yellow** | Red(D0): LOW, Green(D1): LOW, Blue(D2): HIGH |
| WiFi & MQTT Connected (Normal) | **Solid Green** | Red(D0): HIGH, Green(D1): LOW, Blue(D2): HIGH |
| ESP-NOW Packet Received & Forwarded | **Brief Blue Pulse** | Blue(D2): LOW for ~70ms on packet arrival |
| Outbound Message Sent via ESP-NOW | **Brief Cyan Pulse** | Green(D1) + Blue(D2): LOW for ~80ms on transmit |

---

## 4. Optional I2C Display (Connector H1)

Connector H1 is pinned out for standard 4-pin I2C OLED modules (e.g. SSD1306 128x64 or 128x32):
* **Pin 1**: `GND`
* **Pin 2**: `VCC` (+3.3V)
* **Pin 3**: `SCL` (U10 Pin D5)
* **Pin 4**: `SDA` (U10 Pin D4)

> [!NOTE]
> The I2C display is **optional and auto-detected**. At boot, U10 scans the I2C bus for addresses `0x3C` and `0x3D`. If no display is detected, the gateway continues executing normally without delay.

### OLED Power-Save & Capacitive Touch Wake-Up (Pin D3)
To prevent OLED burn-in and save power:
* **Auto-Off after 30 Seconds**: When enabled, the OLED turns on at power-up and automatically enters hardware sleep (`SSD1306_DISPLAYOFF`) 30 seconds after startup or last wake.
* **Web & API Toggle**: The 30-second timeout can be enabled or disabled at any time via the **30s Screen Timeout** slide switch on the Web Dashboard or the `/api/oled_timeout` API endpoint (persisted in Flash NVS).
* **Touch-to-Wake on D3 (`GPIO4` / `TOUCH4`)**: Connecting test pad **D3#** on U10 to a piece of copper tape on the inside of the 2mm 3D-printed PLA cover enables capacitive touch wake-up. Touching the cover over the copper pad wakes the display for 30 seconds (or resets the 30-second timer if already awake).
* **Auto-Calibration**: At startup, U10 automatically calibrates the untouched baseline capacitance of the copper pad through the enclosure and tracks slow ambient temperature/humidity drift.

---

## 5. Project Folder Structure

```
ESP32 S3 Gateway/
├── Gateway_WiFi_MQTT/
│   └── Gateway_WiFi_MQTT.ino       <-- Flash to U10 (WiFi + MQTT + Web Server + LED + OLED)
├── Gateway_ESPNOW_Receiver/
│   └── Gateway_ESPNOW_Receiver.ino <-- Flash to U1 (ESP-NOW Receiver & Forwarder)
├── ESPNOW_Sender_Example/
│   └── ESPNOW_Sender_Example.ino   <-- Test sketch for remote sensor nodes
├── Schematic/
│   ├── SCH_Schematic1_2026-10-30.pdf
│   └── schematic_highres.png
├── PCB/
│   ├── Top.png
│   └── Bottom.png
└── README.md                       <-- Documentation
```

---

## 6. Setup & Flashing Instructions

### Required Libraries
Install the following via the Arduino IDE Library Manager (`Sketch` -> `Include Library` -> `Manage Libraries...`):
* **PubSubClient** by Nick O'Leary
* **Adafruit SSD1306** by Adafruit *(Required only if using OLED)*
* **Adafruit GFX Library** by Adafruit *(Required only if using OLED)*

### Board Settings in Arduino IDE
For both modules, select:
* **Board**: `XIAO_ESP32S3` (under *ESP32 Arduino*)
* **USB CDC On Boot**: `Enabled`
* **Flash Size**: `8MB` (or default)
* **Upload Speed**: `921600`

---

### Step 1: Flash U1 (ESP-NOW Receiver)
1. Plug the USB-C cable into the gateway board.
2. Verify that **LED2 ("ESP-NOW")** is illuminated. (If LED3 is illuminated instead, flip the USB-C plug over).
3. Open `Gateway_ESPNOW_Receiver/Gateway_ESPNOW_Receiver.ino` in Arduino IDE.
4. Select the COM port corresponding to the board.
5. Click **Upload**.
6. Open Serial Monitor at **115200 baud** to confirm U1 startup and note its MAC address.

### Step 2: Flash U10 (WiFi & MQTT Gateway)
1. Flip the USB-C cable connector over.
2. Verify that **LED3 ("WiFi/MQTT")** is now illuminated.
3. Open `Gateway_WiFi_MQTT/Gateway_WiFi_MQTT.ino` in Arduino IDE.
4. Select the new COM port in the IDE.
5. Click **Upload**.
6. Open Serial Monitor at **115200 baud**.
7. Observe the RGB LED:
   - **Solid Red**: Connecting to WiFi.
   - **Solid Yellow**: WiFi connected, connecting to MQTT.
   - **Solid Green**: Fully connected and operating.

---

## 7. Web Dashboard & Configuration

### Accessing the Web Dashboard
Open your browser and navigate to the gateway's IP address:
```
http://<U10_IP_ADDRESS>/
```
Features:
* **Live Badges**: Real-time connection status for WiFi (including IP, RSSI, and connected AP BSSID), MQTT Broker, Inter-ESP U1 Link (including last ESP-NOW RSSI), and OLED display.
* **1-Hour WiFi RSSI Trend Graph**: Embedded sparkline on the WiFi card plotting up to 60 1-minute RSSI samples (`-90` to `-30` dBm) with a `-65` dBm roaming threshold reference line.
* **Automatic Mesh AP Roaming**: If WiFi RSSI drops below `-65` dBm, U10 periodically scans for a stronger BSSID on the same SSID (`>= 10` dB improvement) and reconnects to it.
* **OLED Timeout Slide Switch**: Toggle the 30-second OLED auto-sleep timeout directly from the OLED status card.
* **Rolling Message Table**: Auto-updating table of received ESP-NOW sensor packets with timestamps, MAC addresses, RSSI, and decoded payloads.
* **Test Transmitter**: Send test unicast or broadcast ESP-NOW messages directly from your browser.

### REST API Endpoints
* `GET /api/status`: Returns live JSON status, `wifi_bssid`, `wifi_rssi_history`, `u1_last_rssi`, `oled_timeout_enabled`, touch calibration values, and recent packets.
* `GET` / `POST /api/oled_timeout?enabled=true|false`: Reads or updates the NVS-persisted OLED 30-second screen timeout setting.
* `POST /api/send`: Dispatches a unicast or broadcast ESP-NOW payload via U1.

### Supported ESP-NOW Payloads (U1 Receiver)
In addition to UTF-8 JSON/text payloads, `Gateway_ESPNOW_Receiver.ino` automatically decodes binary `struct_message` packets (`int id`, `char state[7]`, `int vBatt` — 16 bytes) and forwards them as JSON (`{"id":...,"state":"...","vBatt":...}`).

### Reconfiguring Settings
Navigate to:
```
http://<U10_IP_ADDRESS>/config
```
From here you can adjust WiFi SSID/Password, MQTT broker host, port, credentials, and base topic prefix. All settings are persistently stored in the ESP32-S3 Flash NVS.

---

## 8. MQTT Topics & Payloads

| Direction | Topic | Payload Example | Description |
| :--- | :--- | :--- | :--- |
| **Gateway -> MQTT** | `<prefix>/rx/<CLEAN_MAC>` | `{"temp":22.4,"humidity":55.1}` | Incoming ESP-NOW packet from sensor node |
| **Gateway -> MQTT** | `<prefix>/status` | `online` / `offline` | Gateway LWT availability topic |
| **MQTT -> Gateway** | `<prefix>/tx` | `{"cmd":"restart"}` | Broadcast packet sent to all ESP-NOW nodes |
| **MQTT -> Gateway** | `<prefix>/send/<TARGET_MAC>` | `{"valve":"open"}` | Unicast packet dispatched to specific node |
