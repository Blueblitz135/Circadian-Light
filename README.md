# CircadianLight

![](circuit_layout.jpeg)

An ESP32-based circadian-rhythm lighting system prototype designed to support hospital patient comfort by dynamically controlling **brightness** and **color temperature** throughout the day. Includes a simple **web dashboard** for real-time control over Wi-Fi.

## What it does
- Adjusts **warm/cool LED channels** to simulate day-to-night lighting changes  
- Lets users control lighting through a **web UI** (sliders/buttons) in real time  
- Exposes lightweight endpoints (e.g., for **mode**, **DAC/brightness**, and **time-based scheduling**)  
- Built to be easy to demo: connect → open dashboard → control lights

## Why this project
Hospital environments can disrupt sleep and circadian rhythms. CircadianLight explores a non-pharmacological approach: **lighting that better matches natural day/night patterns**, aiming to reduce factors that contribute to confusion and poor sleep.

## Hardware/Software used
- **ESP32** (Wi-Fi control + hardware output)
- **Web dashboard**: HTML / CSS / JavaScript
- **LittleFS** (serves the dashboard files)
- Utilized **Digital to Analog Converter (DAC)** pins on ESP32 (for brightness + channel mixing)
- Powered by a **7V power supply**

## Features
- Manual control: brightness + warm/cool balance  
- Preset modes (example: Day / Evening / Night)  
- Auotmatic/Manual time modes  

## Getting started
### 1) Flash the ESP32
1. Download the *circadian_rhythm_lightning* folder and keep the file directory structure the same
2. Open the `circadian_rhythm_lighting.ino` file in Arduino IDE
3. Configure your Wi-Fi credentials (or the project’s config method) located near the top of the code 
4. Upload to your ESP32

### 2) Upload the web files (LittleFS)
- Open command/search bar in Arduino IDE (default shortcut is Ctrl + Shift + P  for Windows/Linux and Cmd + Shift + P for Mac
- Search and click onto "Upload LittleFs to Pico/ESP8266/ESP32"

### 3) Use the dashboard
- Connect to the ESP32 on your network (should automatically be connected after sketch upload if ESP32 is plugged-in to your device)  
- Open the ESP32’s IP address in your browser, or type in "http://circadian.local/" in your browser's search bar  
- You can now control brightness / color temperature throguh the web UI
