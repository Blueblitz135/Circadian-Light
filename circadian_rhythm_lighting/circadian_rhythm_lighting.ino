#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>


// Network Details (use your own WiFi's SSID and Password) 
const char* WIFI_SSID     = "PLACEHOLDER"; // rewrite this field
const char* WIFI_PASSWORD = "PLACEHOLDER"; // rewrite this field

// Pins and temperature values
#define WARM_LED_PIN 25   // DAC1
#define COOL_LED_PIN 26   // DAC2
#define WARM_CCT 2500.0f
#define COOL_CCT 6000.0f
uint8_t currentDacCh1 = 0; // GPIO 25 (2500K)
uint8_t currentDacCh2 = 0; // GPIO 26 (6000K)
int manualOverrideMins = -1; // -1 = no override
bool powerOn = true;

// Current slider values (0–255)
uint8_t currentSliderCh1 = 0;
uint8_t currentSliderCh2 = 0;

// Last slider values from the web UI (0–255)
uint8_t manualDAC1 = 0;
uint8_t manualDAC2 = 0;
bool slider1Set = false;
bool slider2Set = false;

// Modes
enum ModeType {
  MODE_MANUAL,
  MODE_MANUAL_OVERRIDE,
  MODE_AUTO_NATURAL,
  MODE_AUTO_DAYLIGHT,
  MODE_AUTO_NIGHT,
  MODE_AUTO_DIM
};
ModeType currentMode = MODE_MANUAL;

// mDNS name: http://esp32-dac.local/
const char* MDNS_NAME = "esp32-dac";


//------------------Circadian Rhyhtm Lighting Algorithm------------------- 
// Linear interpolation helper
float linear(float a, float b, float t) {
  return a + (b - a) * t;
}

// Function for converting hours + minutes into a float between 0 and 24 exclusive
float getTimeFloat(int hour, int minute) {
  return hour + (minute / 60.0f);
}

// Function for getting correct target temperatures based on the time of day
float getTargetCCT(int hour, int minute) {
  float time = getTimeFloat(hour, minute);

  if (time >= 9 && time < 16)  return linear(5564, 4228, (time - 9) / 7.0);
  else if (time >= 16 && time < 20) return linear(4228, 2661, (time - 16) / 4.0);
  else if (time >= 6 && time < 9) return linear(2661, 5564, (time - 6) / 3.0);
  else return 2661;
}

// Function for displaying target temperature based on mode
float computeCurrentTargetCCT() {
  int hour, minute;
  if (manualOverrideMins >= 0) {
    hour = manualOverrideMins / 60;
    minute = manualOverrideMins % 60;
  } else {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return -1;
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
  }

  switch (currentMode) {
    case MODE_AUTO_DAYLIGHT:    return 5000;
    case MODE_AUTO_NIGHT:       return 2700;
    case MODE_AUTO_DIM:         return getTargetCCT(hour, minute);
    case MODE_MANUAL_OVERRIDE:  return getTargetCCT(hour, minute);
    case MODE_AUTO_NATURAL:     return getTargetCCT(hour, minute);
    case MODE_MANUAL:           return -1; // not applicable
    default:                    return -1;
  }
}

// Function for updating the correct temperature of the LEDs in automatic modes
void updateAutoLights() {
  if (!powerOn) {
    currentDacCh1 = 0;
    currentDacCh2 = 0;
    dacWrite(WARM_LED_PIN, 0);
    dacWrite(COOL_LED_PIN, 0);
    return;
  }
  int hour, minute;

  if (manualOverrideMins >= 0) {
    hour   = manualOverrideMins / 60;
    minute = manualOverrideMins % 60;
  } else {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;
    hour   = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
  }

  float targetCCT;
  uint8_t brightness = 80;

  switch (currentMode) {
    case MODE_MANUAL_OVERRIDE:
    case MODE_AUTO_NATURAL:
      targetCCT = getTargetCCT(hour, minute);
      break;

    case MODE_AUTO_DAYLIGHT:
      targetCCT = 5000;    // bright neutral/cool
      brightness = 80;
      break;

    case MODE_AUTO_NIGHT:
      targetCCT = 2700;    // warm
      brightness = 40;     // dim
      break;

    case MODE_AUTO_DIM:
      targetCCT = getTargetCCT(hour, minute);
      brightness = 30;     // always dim
      break;

    default:
      targetCCT = getTargetCCT(hour, minute);
      break;
  }

  // clamp CCT to LED range
  if (targetCCT < WARM_CCT) targetCCT = WARM_CCT;
  if (targetCCT > COOL_CCT) targetCCT = COOL_CCT;

  // Mix warm/cool LEDs based on CCT
  float t = (targetCCT - WARM_CCT) / (COOL_CCT - WARM_CCT); // 0→all warm, 1→all cool
  float warmFrac = 1.0f - t;
  float coolFrac = t;

  uint8_t warmDAC = (uint8_t)(brightness * warmFrac);
  uint8_t coolDAC = (uint8_t)(brightness * coolFrac);

  uint8_t warmOut = sliderToDac(warmDAC);
  uint8_t coolOut = sliderToDac(coolDAC);

  // If a slider was explicitly set to 0, force that channel fully off even in auto modes.
  if (slider1Set && manualDAC1 == 0) warmOut = 0;
  if (slider2Set && manualDAC2 == 0) coolOut = 0;

  currentDacCh1 = warmOut;
  currentDacCh2 = coolOut;

  dacWrite(WARM_LED_PIN, warmOut);
  dacWrite(COOL_LED_PIN, coolOut);
}



//------------Time Configuration Setup----------------

// Toronto timezone (auto-DST)
static const char* TZ_TORONTO = "EST5EDT,M3.2.0/2,M11.1.0/2";

// call this in setup() after Wi-Fi STA connects (or anytime you get internet)
bool initTimeNTP(unsigned long wait_ms = 10000) {
  setenv("TZ", TZ_TORONTO, 1);
  tzset();

  // Use ESP32 helper that sets TZ and NTP in one go
  configTzTime(TZ_TORONTO, "pool.ntp.org", "time.nist.gov", "ca.pool.ntp.org");

  // Wait for time to sync
  struct tm tmnow;
  unsigned long t0 = millis();
  while (millis() - t0 < wait_ms) {
    if (getLocalTime(&tmnow, 500)) {
      Serial.printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
        tmnow.tm_year + 1900, tmnow.tm_mon + 1, tmnow.tm_mday,
        tmnow.tm_hour, tmnow.tm_min, tmnow.tm_sec);
      return true;
    }
  }
  Serial.println("NTP sync timed out.");
  return false;
}

// String formatter for the time (Ex: "Mon, Nov 10, 2025 06:42:03 PM EST")
String formatTime() {
  struct tm tmnow;
  if (!getLocalTime(&tmnow, 200)) return "Time: (not set)";
  char buf[48];
  strftime(buf, sizeof(buf), "Time: %a, %b %d, %Y %I:%M:%S %p %Z", &tmnow);
  return String(buf);
}



//---------------------WI-FI Functionality---------------------
// // Connect to wi-fi
void connectWiFi() {
  WiFi.mode(WIFI_STA);
   
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("\nConnecting");

  while(WiFi.status() != WL_CONNECTED){
      Serial.print(".");
      delay(100);
  }
  Serial.println("\nConnected to the WiFi network");
  Serial.print("Local ESP32 IP: ");
  Serial.println(WiFi.localIP());
}



// --------- Digital to Analog Converter Write Helpers ---------
void writeDAC(uint8_t ch, uint8_t val) {
  if (ch == 1) dacWrite(WARM_LED_PIN, val);
  else if (ch == 2) dacWrite(COOL_LED_PIN, val);
}

// Map slider 0–255 to DAC 0–255 with a 0.5 V "floor" when > 0
uint8_t sliderToDac(uint8_t sliderVal) {
  // 0 -> 0 V, 1–255 -> 0.5–3.3 V
  if (sliderVal == 0) return 0;

  const float minV = 0.5f;
  const float maxV = 3.3f;
  float frac = sliderVal / 255.0f;
  float v = minV + (maxV - minV) * frac;
  uint8_t dac = (uint8_t) round((v / 3.3f) * 255.0f);
  if (dac > 255) {
    dac = 255;
  }
  return dac;
}



// --------- HTTP Handlers ---------
WebServer server(80);
void serveFile(const char* path, const char* type) {
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(404, "text/plain", String("File not found: ") + path); return; }
  server.streamFile(f, type);
  f.close();
}

void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleIndex()     { serveFile("/index.html", "text/html"); }
void handleCSS()       { serveFile("/style.css",  "text/css"); }
void handleJS()        { serveFile("/app.js",     "application/javascript"); }
void handleFavicon()   { server.send(204); }
void handleOptions() { addCORS(); server.send(204); } // preflight

void handleDac() {
  if (!server.hasArg("ch") || !server.hasArg("val")) {
    server.send(400, "text/plain", "Missing ch or val");
    return;
  }

  int ch = server.arg("ch").toInt();
  int val = server.arg("val").toInt();
  if (val < 0) val = 0;
  if (val > 255) val = 255;

  uint8_t sliderVal = (uint8_t)val;
  uint8_t dacVal    = sliderToDac(sliderVal);

  if (ch == 1) {
    currentSliderCh1 = sliderVal;
    manualDAC1 = sliderVal;
    slider1Set = true;
    currentDacCh1 = powerOn ? dacVal : 0;
    if (powerOn) dacWrite(WARM_LED_PIN, dacVal); else dacWrite(WARM_LED_PIN, 0);
  } else if (ch == 2) {
    currentSliderCh2 = sliderVal;
    manualDAC2 = sliderVal;
    slider2Set = true;
    currentDacCh2 = powerOn ? dacVal : 0;
    if (powerOn) dacWrite(COOL_LED_PIN, dacVal); else dacWrite(COOL_LED_PIN, 0);
  }

  server.send(200, "text/plain", "OK");
}

// Function for getting current state
void handleState() {
  char buf[96];
  // mode string optional, useful if you want to display it later
  const char* modeStr =
    currentMode == MODE_MANUAL           ? "manual"  :
    currentMode == MODE_MANUAL_OVERRIDE  ? "manual_override" :
    currentMode == MODE_AUTO_NATURAL     ? "natural" :
    currentMode == MODE_AUTO_DAYLIGHT    ? "daylight":
    currentMode == MODE_AUTO_NIGHT       ? "night"   :
    currentMode == MODE_AUTO_DIM         ? "dim"     : "unknown";

  snprintf(buf, sizeof(buf),
           "{\"ch1\":%u,\"ch2\":%u,\"mode\":\"%s\",\"power\":%s}",
           currentDacCh1, currentDacCh2, modeStr, powerOn ? "true" : "false");

  server.send(200, "application/json", buf);
}

void handleTargetCCT() {
  addCORS();
  float cct = computeCurrentTargetCCT();
  if (cct < 0) { server.send(503, "text/plain", "unavailable"); return; }
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f", cct);
  server.send(200, "text/plain", buf);
}

void handlePower() {
  if (!server.hasArg("on")) {
    server.send(400, "text/plain", "Missing on");
    return;
  }
  String s = server.arg("on");
  s.toLowerCase();
  bool newState = (s == "1" || s == "true" || s == "on");
  powerOn = newState;
  if (!powerOn) {
    currentDacCh1 = 0;
    currentDacCh2 = 0;
    dacWrite(WARM_LED_PIN, 0);
    dacWrite(COOL_LED_PIN, 0);
  } else {
    // Restore outputs immediately based on mode/override/slider
    if (currentMode != MODE_MANUAL || manualOverrideMins >= 0) {
      updateAutoLights();
    } else {
      uint8_t d1 = sliderToDac(currentSliderCh1);
      uint8_t d2 = sliderToDac(currentSliderCh2);
      currentDacCh1 = d1;
      currentDacCh2 = d2;
      dacWrite(WARM_LED_PIN, d1);
      dacWrite(COOL_LED_PIN, d2);
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleTimeOverride() {
  if (!server.hasArg("mins")) {
    manualOverrideMins = -1;  // clear override
    if (currentMode == MODE_MANUAL_OVERRIDE) currentMode = MODE_MANUAL;
    server.send(200, "text/plain", "override cleared");
    return;
  }

  String s = server.arg("mins");
  int mins = s.toInt();
  if (mins < 0) mins = 0;
  if (mins > 1439) mins = 1439;

  manualOverrideMins = mins;
  currentMode = MODE_MANUAL_OVERRIDE;
  server.send(200, "text/plain", "ok");
}

void handleMode() {
  if (!server.hasArg("m")) {
    server.send(400, "text/plain", "Missing m");
    return;
  }

  String m = server.arg("m");

  if (m == "manual") {
    currentMode = MODE_MANUAL;
    manualOverrideMins = -1;
  } else if (m == "manual_override") {
    currentMode = MODE_MANUAL_OVERRIDE;
  } else if (m == "natural") {
    currentMode = MODE_AUTO_NATURAL;
  } else if (m == "daylight") {
    currentMode = MODE_AUTO_DAYLIGHT;
  } else if (m == "night") {
    currentMode = MODE_AUTO_NIGHT;
  } else if (m == "dim") {
    currentMode = MODE_AUTO_DIM;
  } else {
    // default: treat unknown as natural auto
    currentMode = MODE_AUTO_NATURAL;
  }

  server.send(200, "text/plain", "OK");
}


// --------- Setup / Loop ---------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nBooting...");

  // Wi-Fi and mDNS
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("circadian")) {                 // hostname: circadian.local
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS started: http://circadian.local");
    } else {
      Serial.println("mDNS start FAILED");
    }
  }
  
  // Initialize time
  bool haveSTA = (WiFi.status() == WL_CONNECTED);
  if (haveSTA) {
    initTimeNTP();  // try to get NTP time if internet is available
  }

  // FS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed! Did you upload /data ?");
  } else {
    Serial.println("LittleFS mounted.");
  }
  
  // HTTP Routes
  server.on("/", handleIndex);
  server.on("/index.html", handleIndex);
  server.on("/style.css", handleCSS);
  server.on("/app.js", handleJS);
  server.on("/favicon.ico", handleFavicon);
  server.on("/dac", HTTP_GET, handleDac);
  server.on("/dac", HTTP_OPTIONS, handleOptions);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/state", HTTP_GET, handleState);
  server.on("/power", HTTP_GET, handlePower);
  server.on("/power", HTTP_OPTIONS, handleOptions);
  server.on("/target_cct", HTTP_GET, handleTargetCCT);
  server.on("/target_cct", HTTP_OPTIONS, handleOptions);
  server.enableCORS(true);  // allow localhost dev if needed

  // Current ESP32 local time (synced via NTP in setup())
  server.on("/time", HTTP_GET, [](){
    addCORS();
    server.send(200, "text/plain", formatTime());
  });

  // Raw epoch seconds for smooth client-side ticking
  server.on("/time_raw", HTTP_GET, [](){
    addCORS();
    time_t now = time(nullptr);
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", (long)now);
    server.send(200, "text/plain", buf);
  });
  server.on("/time_override", HTTP_GET, handleTimeOverride);
  server.begin();
  Serial.println("HTTP server started.");

  // Initialize DAC pin values
  dacWrite(WARM_LED_PIN, manualDAC1);
  dacWrite(COOL_LED_PIN, manualDAC2);
}

void loop() {
  server.handleClient();

  // Re-sync ESP32 local time every hour
  static unsigned long lastNtpResync = 0;
  unsigned long nowMs = millis();
  const unsigned long RESYNC_INTERVAL_MS = 3600000UL; // 1 hour

  if (nowMs - lastNtpResync > RESYNC_INTERVAL_MS && WiFi.status() == WL_CONNECTED) {
    lastNtpResync = nowMs;
    initTimeNTP(2000);  // quick re-sync; don't block too long
  }

  // Determine if automatic light temperature update is needed
  static unsigned long lastAutoUpdate = 0;
  bool autoNeeded = (currentMode != MODE_MANUAL);

  if (autoNeeded && (nowMs - lastAutoUpdate > 1000)) {
    lastAutoUpdate = nowMs;
    updateAutoLights();
  }
}
