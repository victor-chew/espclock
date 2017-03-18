#include <stdint.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include "FS.h"
#include <EEPROM.h>
#include <ArduinoJson.h>

// Application Javascript to be injected into WiFiManager's config page
#define QUOTE(...) #__VA_ARGS__
const char* jscript = QUOTE(
  <script>
    document.addEventListener('DOMContentLoaded', get_location, false);
    function get_location() {
      var loc = document.getElementById("loc");
      if (navigator.geolocation) {
          navigator.geolocation.getCurrentPosition(function(pos) {
            loc.value = pos.coords.latitude + "," + pos.coords.longitude;
          });
      }
    }
  </script>
);

const unsigned int NTP_LOCAL_PORT = 2390;
const char* NTP_SERVER_NAME = "time.nist.gov";
const int NTP_PACKET_SIZE = 48;
const int NTP_CHECK_INTERVAL = 90; // How often to ping NTP server (seconds)
const int CLOCK_DIFF_RANGE = 5; // If diff between clock and NTP time <= value, we consider them synchronized
const String GOOGLE_API_URL = "https://maps.googleapis.com/maps/api/timezone/json?location=[loc]&timestamp=[ts]";
const char* GOOGLE_API_CERT = "AD:B8:13:99:64:F5:75:F5:78:5C:FA:43:19:EA:8F:AF:2B:AE:54:FE";

os_timer_t secTimer;
WiFiUDP ntpPort;
byte packetBuffer[ NTP_PACKET_SIZE];
bool shouldSaveConfig = false, tickOccurred = false, ntpReplied = false, pulseAndWait = false;
int clockHH = 0, clockMM = 0, clockSS = 0, ntpHH = 0, ntpMM = 0, ntpSS = 0, ntpCount = 0, ntpTime = 0, tickPin = D1;
char loc[64] = "", clocktime[7];

void debug(const char *format, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  Serial.println(buf);
}

// Called when WifiManager 
void saveConfigCallback () {
  shouldSaveConfig = true;
}

// Read config parameters from the SPIFFS filesystem
// Returns false if config file cannot be loaded
bool loadConfig() {
  if (SPIFFS.exists("/config.json")) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      size_t size = configFile.size();
      std::unique_ptr<char[]> buf(new char[size]);
      configFile.readBytes(buf.get(), size);
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.parseObject(buf.get());
      if (json.success()) {
        debug("Loaded config file");
        if (json.containsKey("loc")) strcpy(loc, json["loc"]);
        debug("loc = %s", loc);
        return true;
      }
    }
  }
  debug("Failed to load config file");
  return false;
}

// Write config parameters to the SPIFFS filesystem
void saveConfig() {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["loc"] = loc;
    debug("Saving config file...");
    debug("loc = %s", loc);
    File configFile = SPIFFS.open("/config.json", "w");
    if (configFile) {
      json.printTo(configFile);
      configFile.close();
    } else {
      debug("Failed to open config file for writing");
    }
}

// Read clock time from EEPROM
void readClockTime() {
  
  EEPROM.begin(3);
  clockHH = (int)EEPROM.read(0);
  clockMM = (int)EEPROM.read(1);
  clockSS = (int)EEPROM.read(2);
  snprintf(clocktime, sizeof(clocktime), "%02d%02d%02d", clockHH, clockMM, clockSS);

  debug("readClockTime: %02d:%02d:%02d", clockHH, clockMM, clockSS);
}

// Write clock time to EEPROM
void writeClockTime() {

  debug("clockTime: %02d:%02d:%02d; ntpTime: %02d:%02d:%02d", clockHH, clockMM, clockSS, ntpHH, ntpMM, ntpSS);
  
  EEPROM.begin(3);
  EEPROM.write(0, (byte)clockHH);
  EEPROM.write(1, (byte)clockMM);
  EEPROM.write(2, (byte)clockSS);
  EEPROM.end();
}

// Increment given clock time by 1s, taking care of rollover i.e. 11:59:59 => 01:00:00
void incClockTime(int& hh, int &mm, int &ss) {
  ss = (ss + 1) % 60;
  if (ss == 0) {
    if (++mm > 59) {
      mm = 0;
      if (++hh > 12) {
        hh = 1;
      }
    }
  }
}

// Move second hand one tick clockwise.
// See: http://www.cibomahto.com/2008/03/controlling-a-clock-with-an-arduino/
void incSecondHand() {
  // Update internal clock time  
  incClockTime(clockHH, clockMM, clockSS);
  writeClockTime();

  // Increment second hand
  tickPin = (tickPin == D1 ? D2: D1);
  digitalWrite(tickPin, HIGH);
  delay(100);
  digitalWrite(tickPin, LOW);
  delay(100);
}

// This will pulse the same pin, which will cause the second hand to vibrate but not advance.
// It lets the user know the clock is waiting for NTP time to catch up.
void pulseSecondHand() {
  
  debug("Stalling: %02d:%02d:%02d; ntpTime: %02d:%02d:%02d", clockHH, clockMM, clockSS, ntpHH, ntpMM, ntpSS);

  digitalWrite(tickPin, HIGH);
  delay(100);
  digitalWrite(tickPin, LOW);
  delay(100);
}

// Triggered by system timer every second
void timerCallback(void *pArg) {
  os_intr_lock();
  tickOccurred = true;
  incClockTime(ntpHH, ntpMM, ntpSS);
  os_intr_unlock();
}

// Get the local time by using Google Timezone API
// Returns true if conversion was successful
bool convertLocalTime() {

  debug("Getting local time...");

  char buf[256];
  String url = GOOGLE_API_URL;
  url.replace("[loc]", loc);
  snprintf(buf, sizeof(buf), "%u", ntpTime);
  url.replace("[ts]", buf);

  debug("HTTP GET: %s", url.c_str());
  HTTPClient http;
  http.begin(url.c_str(), GOOGLE_API_CERT);
  int rc = http.GET();
  debug("HTTP return code: %d", rc);

  if (rc > 0) {
    if (rc == HTTP_CODE_OK) {
      String payload = http.getString();
      debug("HTTP return payload: %s", payload.c_str());
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.parseObject(payload.c_str());
      if (json.success()) {
        const char* rcstatus = json["status"];
        const long rawOffset = json["rawOffset"];
        const long dstOffset = json["dstOffset"];
        if (strcmp(rcstatus, "OK") == 0) {
          ntpTime += rawOffset + dstOffset;
          return true;
        }
      } else {
        debug("JSON parse failed");
      }
    }
  } else {
    debug("HTTP GET failed");
  }
  return false;
}

// Send an NTP request to the time server at the given address
void NTPSend() {
  
  // Prepare NTP packet
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;            // Stratum, or type of clock
  packetBuffer[2] = 6;            // Polling Interval
  packetBuffer[3] = 0xEC;         // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // Send packet
  IPAddress ntpIP;
  WiFi.hostByName(NTP_SERVER_NAME, ntpIP);
  ntpPort.beginPacket(ntpIP, 123);  // NTP requests are to port 123
  ntpPort.write(packetBuffer, NTP_PACKET_SIZE);
  ntpPort.endPacket();

  debug("NTP packet sent");
}

// Receive NTP reply
void NTPReceive() {
  
  const unsigned long seventyYears = 2208988800UL;

  if (ntpPort.parsePacket())
  {
    // Signal that we have received the response from NTP server    
    ntpReplied = true;

    // Read the packet into the buffer
    ntpPort.read(packetBuffer, NTP_PACKET_SIZE);

    // The timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, esxtract the two words:
    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);

    // Combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    ntpTime = (highWord << 16 | lowWord) - seventyYears;

    // Convert to local time
    if (convertLocalTime())
    {
      ntpHH = (ntpTime % 86400L) / 3600;
      ntpMM = (ntpTime % 3600) / 60;
      ntpSS = (ntpTime % 60);
      if (ntpHH == 0) ntpHH = 12;
      else if (ntpHH > 12) ntpHH -= 12;
    }

    debug("ntpTime: %02d:%02d", ntpHH, ntpMM);
  }
}

// Check if clock and NTP time are with permitted range of each other
// Checking for total equality will cause race condition in loop()
bool isTimeSynchronized() {
  int diff = abs(((clockHH * 3600) + (clockMM * 60) + clockSS) - ((ntpHH * 3600) + (ntpMM * 60) + ntpSS));
  return _min(diff, 12*60*60 - diff) <= CLOCK_DIFF_RANGE;
}

void setup() {

  debug("ESPCLOCK started");
  
  // Use serial for debugging
  Serial.begin(115200);
  
  // Setup GPIO outputs
  pinMode(D0, INPUT);
  pinMode(D1, OUTPUT);
  pinMode(D2, OUTPUT);
  digitalWrite(D1, LOW);
  digitalWrite(D2, LOW);

  // If D0 is connected to ground, we will start in AP mode for configuration
  bool reset = (digitalRead(D0) == LOW);
  if (reset) debug("Reset request detected");
 
  // Setup SPIFFS and load config values
  if (!SPIFFS.begin()) {
    debug("Failed to mount filesystem");
    debug("ESPCLOCK halted");
    ESP.deepSleep(999999999*999999999U, WAKE_NO_RFCAL);
  }

  // Load saved config values
  if (loadConfig()) readClockTime();
  
  // Setup WiFiManager
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  if (reset) wifiManager.resetSettings();
  wifiManager.setCustomHeadElement(jscript);
  WiFiManagerParameter f_clocktime("clocktime", "Time on clock (12-hr HHMMSS)", clocktime, 7);
  WiFiManagerParameter f_loc("loc", "Location", loc, 63, "type=\"hidden\"");
  wifiManager.addParameter(&f_clocktime);
  wifiManager.addParameter(&f_loc);
  wifiManager.autoConnect("ESPCLOCK");

  // At this point, shouldSaveConfig will tell us whether WiFiManager has connected
  // using previously stored SSID/password, or new ones entered into the config portal
  if (shouldSaveConfig) {
    
    // If new SSID/password, we need to save new config values
    int clocktime = atoi(f_clocktime.getValue());
    clockHH = clocktime / 10000;
    int mmss = clocktime % 10000;
    clockMM = mmss / 100;
    clockSS = mmss % 100;
    while(clockHH > 12) clockHH -= 12;
    if (clockMM > 59) clockMM = 0;    
    if (clockSS > 59) clockSS = 0;
    strncpy(loc, f_loc.getValue(), sizeof(loc) - 1);
    saveConfig();
    writeClockTime();
  }

  // Initialize NTP port
  ntpPort.begin(NTP_LOCAL_PORT);
  ntpHH = clockHH; ntpMM = clockMM; ntpSS = clockSS;

  // Start system timer which triggers every second
  os_timer_setfn(&secTimer, timerCallback, NULL);
  os_timer_arm(&secTimer, 1000, true);
}

void loop() {

  // Send out NTP request every 90s
  if (ntpCount == 0) {
    
    // We have not sent out NTP request yet, let's do it
    NTPSend();
    ntpReplied = false;
    ++ntpCount;
    
  } else if (!ntpReplied && ntpCount % 5 == 0) {
    
    // Check for reply from NTP server every 5s until received
    NTPReceive();
    ++ntpCount;
  }

  // If clock time != NTP time, adjust until they match
  pulseAndWait = false;
  while(!isTimeSynchronized()) {
    // Calculate time difference between clock and NTP time in seconds
    int diff = ((clockHH * 3600) + (clockMM * 60) + clockSS) - ((ntpHH * 3600) + (ntpMM * 60) + ntpSS);
    if (diff < 0) diff = (12*60*60) + diff;

    // If clock time is ahead about 3 mins, pulse second hand and wait for NTP time to catch up
    if (diff <= 3*60) {
      // Continue to pulse while we are outside the diff range
      if (diff > CLOCK_DIFF_RANGE + 1) {
        if (!pulseAndWait) pulseAndWait = true;
      // Once we are at the boundary of the diff range, make a final 1s delay to close the range
      } else {
        delay(1000);
      }
      break;
    // Otherwise fastforward clock to NTP time
    } else {
      debug("Fastforward: %02d%02d%02d => %02d%02d%02d", clockHH, clockMM, clockSS, ntpHH, ntpMM, ntpSS);
      incSecondHand();
    }
  }
  
  // Until normal condition, this section should run every second
  if (tickOccurred) {
    tickOccurred = false;
    ntpCount = (ntpCount + 1) % NTP_CHECK_INTERVAL;
    if (pulseAndWait) pulseSecondHand(); else incSecondHand();
  }  

  delay(250);
}

