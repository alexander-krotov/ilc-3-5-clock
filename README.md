# ILC-3-5-clock
Digital clock with soviet-made ILC-3-5 (ИЛЦ-3-5/7 in Russian) VFD indicator.
  
Clock is driven by ESP32 C3 Super mini, MAX6921 is used to drive VFD. Indicator I got is Soviet-made IV-28 produced in December 1988.

![clock text](https://github.com/alexander-krotov/IV28-clock/blob/main/clock.jpg?raw=true)

Clock video: https://youtu.be/NhdLMzuFE0I

## Setup instructions
Easy setup instructions are very similar to: https://github.com/alexander-krotov/apollo-clock/blob/main/setup.md

## Software

Clock firmware is derived from a previous clock formware I did for IV-28 clock https://github.com/alexander-krotov/IV28-clock
Also it provides controls for Si4703 RDS module.

Firmware uses NetworkManager for WiFi configuration, Web UI is based on GyverPortal library.

If connected to WiFi, the clock uses NTP to get the time.

References to software components:
- WiFi Manager https://github.com/tzapu/WiFiManager
- GyverPortal https://github.com/GyverLibs/GyverPortal

## Hardware

Hardware schematics:

It is exacly the same as https://github.com/alexander-krotov/IV28-clock
The only difference is R1 resisor. The indicator sinks 100mA cathode current and R1 should be 22R 1W (PCB design allows that).

https://oshwlab.com/alexander.krotov/iv-18-clock_copy_copy

Clock is powered from USB 5v power supply, it could be provided via ESP32 module connector. It eats about XXXmA, peaking to XXXmA.

ILC-3-5 VFD display needs 24v power supply. It is provided by a charge pump implemented in hardware.

DS3231 is used as a backup Real-Time clock (RTC).

Clock has a connector H5 for Si4703 rado module, which could be used as a time provider, but it did not work for me.

Driver pins to VFD connection table:

MAX6921 IO | MAX6921 Pin | VFD Pin | VFD Pin purpose
--- | --- | --- | --- 
10  |  12 | 33 | А segment
7   |  15 | 32 | Б segment
5   |  11 | 31 | В segment
11  |  16 | 30 | Г segment
6   |  17 | 29 | E segment
12  |  10 | 28 | 1 digit
7   |  18 | 25 | 2 digit
13  |   9 | 22 | separator dots
8   |  18 | 19 | 3 digit
17  |   5 | 15 | 4 segment
5   |  21 | 11 | weekday digit
18  |   4 | 7  | 5 digit
4   |  22 | 4  | Ж segment
19  |   3 | 3  | Д segment
3   |  23 | 2  | U6, U3, U2, U1, K
X   |  X  | 34 | Cathode
X   |  X  | 1  | Cathode


References to components:
- ILС-3-5 datasheet https://www.quartz1.com/price/techdata/%D0%98%D0%9B%D0%A63-5-7%20%D1%82%D0%B8%D0%BF1.pdf 
- ESP32 C3 super mini https://dl.artronshop.co.th/ESP32-C3%20SuperMini%20datasheet.pdf
- MAX6921 VFD driver https://www.analog.com/media/en/technical-documentation/data-sheets/max6921-max6931.pdf
