# ESP-NOW_WiFi_Gateway
A Gateway between ESP-NOW sensors and your WiFi/MQTT/Pushover......etc

Currently Still awaiting the hardware to test but this is what it should look like
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
