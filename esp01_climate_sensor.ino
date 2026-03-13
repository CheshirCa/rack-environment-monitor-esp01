/*
 *   Reads temperature and humidity from an AHT20 sensor and atmospheric
 *   pressure from a BMP280 sensor over I2C. Serves the data over WiFi as
 *   a small web server with multiple output formats. Logs readings to flash
 *   memory (LittleFS) and lets you configure everything through the serial port.
 *
 * Hardware:
 *   - ESP-01 (ESP8266, 1 MB flash) with CH340 USB-serial adapter
 *   - Combined BMP280 + AHT20 breakout board
 *   - I2C connected via a TRRS 3.5 mm jack:
 *       Tip    (T)  -> SDA -> GPIO0
 *       Ring 1 (R1) -> SCL -> GPIO2
 *       Ring 2 (R2) -> 3.3 V power
 *       Sleeve (S)  -> GND
 *
 * Arduino IDE board settings (Tools menu):
 *   Board      : Generic ESP8266 Module
 *   Flash Size : 1MB (FS:512KB, OTA:~246KB)  <-- important, must match
 *
 * Required libraries (install via Library Manager):
 *   - Adafruit BMP280 Library
 *   - Adafruit AHTX0
 *   - Adafruit Unified Sensor
 *   LittleFS and NTP are part of the ESP8266 Arduino core, no extra install needed.
 *
 * First-time filesystem setup:
 *   After setting Flash Size above, run:
 *   Tools -> ESP8266 LittleFS Data Upload
 *   Then upload the sketch as usual.
 */

#include <ESP8266WiFi.h>       // WiFi stack for ESP8266
#include <ESP8266WebServer.h>  // simple HTTP server
#include <Wire.h>              // I2C communication
#include <Adafruit_BMP280.h>   // BMP280 pressure sensor driver
#include <Adafruit_AHTX0.h>    // AHT20 temperature/humidity sensor driver
#include <EEPROM.h>            // persistent settings storage in flash
#include <LittleFS.h>          // filesystem for CSV log files
#include <time.h>              // POSIX time functions (used after NTP sync)
#include <WiFiUdp.h>

// ============================================================
//  Constants
// ============================================================

#define FW_VERSION         "1.2"
#define EEPROM_SIZE        352         // bytes reserved in flash for settings
#define EEPROM_MAGIC       0xAE        // changed from 0xAD — forces re-init after DNS field was added
#define SERIAL_BAUD        115200
#define WIFI_TIMEOUT_MS    15000       // give up connecting after 15 s
#define RECONNECT_MS       30000       // try to reconnect every 30 s when offline
#define READ_INTERVAL_MS   10000       // poll sensors every 10 s
#define LOG_INTERVAL_MS    300000UL    // write a log entry every 5 minutes
#define NTP_SYNC_INTERVAL  3600000UL   // re-sync time with NTP every hour
#define LOG_MAX_DAYS       31          // keep at most 31 daily CSV files

// SNMP agent (SNMPv1 / SNMPv2c, GET only)
// Uses Private Enterprise Number 99999 as a placeholder.
// Register your own for free at: https://www.iana.org/form/pen
#define SNMP_PORT         161          // standard SNMP UDP port
#define SNMP_BUF_SZ       256          // shared RX/TX packet buffer; max response fits easily

// OIDs exposed:
//   1.3.6.1.2.1.1.1.0          sysDescr    OctetString  (sensor name + FW version)
//   1.3.6.1.2.1.1.3.0          sysUpTime   TimeTicks    (centiseconds since boot)
//   1.3.6.1.4.1.99999.1.1.0   temperature Integer32    (×10, e.g. 225 = 22.5 °C)
//   1.3.6.1.4.1.99999.1.2.0   humidity    Gauge32      (×10, e.g. 456 = 45.6 %)
//   1.3.6.1.4.1.99999.1.3.0   pressure    Gauge32      (×10, e.g. 10132 = 1013.2 hPa)

// I2C pin assignments (ESP-01 only has GPIO0 and GPIO2 available)
#define I2C_SDA            0
#define I2C_SCL            2

// BMP280 I2C address: 0x76 when SDO pin is LOW, 0x77 when SDO is HIGH.
// Most cheap combo boards have SDO tied HIGH, so try 0x77 if 0x76 fails.
#define BMP280_ADDR        0x77

// Sensor output sanity limits — readings outside these are flagged as errors
#define TEMP_MIN          -40.0f
#define TEMP_MAX           85.0f
#define HUM_MIN             0.0f
#define HUM_MAX           100.0f
#define PRES_MIN          870.0f    // roughly sea-level minus ~1200 m altitude
#define PRES_MAX         1085.0f    // roughly sea-level maximum

// Stub (simulation) mode: uncomment the line below to run without real sensors.
// Useful for testing the web interface and logging before hardware is ready.
// #define STUB_MODE
#define STUB_TEMP_BASE    22.0f
#define STUB_HUM_BASE     45.0f
#define STUB_PRES_BASE  1013.0f

// ============================================================
//  Settings (EEPROM)
//  Everything the user can configure is stored in this struct.
//  It is read from EEPROM at boot and written back on save.
// ============================================================

struct Settings {
  uint8_t  magic;           // must equal EEPROM_MAGIC; otherwise defaults are applied
  char     ssid[32];        // WiFi network name
  char     password[64];    // WiFi password
  char     ip[16];          // static IP; leave empty to use DHCP
  char     gateway[16];     // default gateway for static IP
  char     subnet[16];      // subnet mask for static IP
  char     dns[16];         // primary DNS server; leave empty to use gateway as DNS
  char     sensorName[64];  // friendly name shown on the web page and in API responses
  char     token[32];       // optional bearer token protecting /status; empty = no auth
  char     ntpServer[64];   // NTP server hostname
  int8_t   utcOffset;       // timezone offset in whole hours, -12 to +14
  bool     loggingEnabled;  // whether CSV logging to LittleFS is active
  char     snmpCommunity[24]; // SNMPv1/v2c community string; default "public"
};

Settings cfg;  // global instance; loaded from EEPROM in setup()

// ============================================================
//  Globals
// ============================================================

ESP8266WebServer server(80);  // HTTP server on port 80
Adafruit_BMP280  bmp;         // BMP280 driver object
Adafruit_AHTX0   aht;         // AHT20 driver object

// All sensor readings and their validity flags live in this struct
struct SensorData {
  float temperature, humidity, pressure;
  bool  tempValid, humValid, presValid;  // true = value is within sane range
  bool  bmpOk, ahtOk;                   // true = sensor was found on I2C bus
};
SensorData sens = {0,0,0,false,false,false,false,false};

// Timestamps for non-blocking periodic tasks (millis() counters)
unsigned long lastReadMs    = 0;
unsigned long lastReconnMs  = 0;
unsigned long lastLogMs     = 0;
unsigned long lastNtpSyncMs = 0;
bool          ntpSynced     = false;  // becomes true after first successful NTP response

// Last logged integer values; used to skip writing if nothing changed
int prevTempInt = -9999;
int prevHumInt  = -9999;
int prevPresInt = -9999;

// Stub mode simulation state; slightly drifting fake values
float stubTemp = STUB_TEMP_BASE;
float stubHum  = STUB_HUM_BASE;
float stubPres = STUB_PRES_BASE;

// SNMP globals
static WiFiUDP snmpUdp;
static uint8_t snmpBuf[SNMP_BUF_SZ];   // shared RX / TX buffer

// ============================================================
//  PROGMEM HTML
//  Storing string literals in PROGMEM keeps them in flash
//  instead of RAM. The ESP-01 only has 80 KB of heap, so
//  this matters a lot. FPSTR() is used later to copy them
//  into a String when building the HTTP response.
// ============================================================

const char HTML_MAIN[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>__NAME__</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0;padding:24px}
h1{color:#38bdf8;font-size:1.3em;margin-bottom:4px}
.sub{color:#64748b;font-size:0.82em;margin-bottom:16px}
.cards{display:flex;flex-wrap:wrap;gap:16px;margin:20px 0}
.card{background:#1e293b;border-radius:12px;padding:22px 28px;text-align:center;min-width:130px}
.val{font-size:2.6em;font-weight:700}
.ok{color:#34d399}.err{color:#f87171}
.lbl{font-size:0.8em;color:#94a3b8;margin-top:6px}
.warn{background:#3b1f1f;border:1px solid #f87171;border-radius:8px;
      padding:10px 14px;margin:6px 0;color:#fca5a5;font-size:0.9em}
.stub{background:#1a2a1a;border:1px solid #34d399;border-radius:8px;
      padding:10px 14px;margin:6px 0;color:#6ee7b7;font-size:0.9em}
.links{margin-top:20px;font-size:0.85em}
.links a{color:#38bdf8;text-decoration:none;margin-right:18px}
.links a:hover{text-decoration:underline}
.ts{color:#475569;font-size:0.75em;margin-top:12px}
</style></head><body>
<h1>__NAME__</h1>
<div class="sub">__DATETIME__ &nbsp;|&nbsp; UTC__UTC__</div>
__WARNINGS__
<div class="cards">
  <div class="card"><div class="val __TC__">__TEMP__</div><div class="lbl">&#176;C Temperature</div></div>
  <div class="card"><div class="val __HC__">__HUM__</div><div class="lbl">% Humidity</div></div>
  <div class="card"><div class="val __PC__">__PRES__</div><div class="lbl">hPa Pressure</div></div>
</div>
<div class="links">
  <a href="/logs">&#128196; Logs</a>
  <a href="/status">&#9881; Status</a>
</div>
<div class="ts">Auto-refreshes every 30 s</div>
</body></html>)rawhtml";

const char HTML_STATUS[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Status</title>
<style>
body{font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0;padding:24px}
h1{color:#38bdf8;font-size:1.3em;margin-bottom:20px}
table{border-collapse:collapse;width:100%;max-width:520px}
td{padding:9px 12px;border-bottom:1px solid #1e293b;font-size:0.9em}
td:first-child{color:#94a3b8;width:45%}
.ok{color:#34d399}.err{color:#f87171}
a{color:#38bdf8;text-decoration:none}
p{margin-top:16px;font-size:0.85em}
</style></head><body>
<h1>Device Status</h1>
<table>
<tr><td>Sensor name</td><td>__NAME__</td></tr>
<tr><td>Time (local)</td><td>__TIME__</td></tr>
<tr><td>UTC offset</td><td>UTC__UTC__</td></tr>
<tr><td>NTP server</td><td>__NTP__</td></tr>
<tr><td>NTP synced</td><td class="__SC__">__SS__</td></tr>
<tr><td>IP address</td><td>__IP__</td></tr>
<tr><td>SSID</td><td>__SSID__</td></tr>
<tr><td>MAC address</td><td>__MAC__</td></tr>
<tr><td>RSSI</td><td>__RSSI__ dBm</td></tr>
<tr><td>Uptime</td><td>__UPTIME__</td></tr>
<tr><td>Firmware</td><td>__VER__</td></tr>
<tr><td>AHT20</td><td class="__AC__">__AS__</td></tr>
<tr><td>BMP280</td><td class="__BC__">__BS__</td></tr>
<tr><td>Logging</td><td>__LOG__</td></tr>
<tr><td>FS used / total</td><td>__FSUSED__ / __FSTOTAL__ KB</td></tr>
</table>
<p><a href="/">&#8592; Back</a> &nbsp; <a href="/logs">&#128196; Logs</a></p>
</body></html>)rawhtml";

// ============================================================
//  EEPROM: load / save settings
// ============================================================

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);  // copy raw bytes from flash into the cfg struct

  // If the magic byte doesn't match, this is either a first boot or the
  // EEPROM_MAGIC constant was changed on purpose to force a reset.
  // Either way, fill everything with safe defaults.
  if (cfg.magic != EEPROM_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic          = EEPROM_MAGIC;
    cfg.utcOffset      = 0;
    cfg.loggingEnabled = true;
    strncpy(cfg.sensorName, "Sensor-01",     sizeof(cfg.sensorName) - 1);
    strncpy(cfg.subnet,     "255.255.255.0", sizeof(cfg.subnet)     - 1);
    strncpy(cfg.ntpServer,  "pool.ntp.org",  sizeof(cfg.ntpServer)  - 1);
    saveSettings();
    Serial.println(F("[EEPROM] First boot -- defaults applied. Re-enter your settings."));
  }
  // Upgrade path: community field added after initial release; default it when blank.
  if (cfg.snmpCommunity[0] == '\0')
    strncpy(cfg.snmpCommunity, "public", sizeof(cfg.snmpCommunity) - 1);
}

void saveSettings() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.put(0, cfg);   // copy struct into the EEPROM write buffer
  EEPROM.commit();      // actually write the buffer to flash
}

// ============================================================
//  Time helpers
// ============================================================

// Returns true if the system clock has been set to a plausible year.
// Used to guard against writing log entries with year 1970 (Unix epoch default).
bool timeIsValid() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  return (t->tm_year + 1900 >= 2024);
}

// Returns a date string for use as a log filename, e.g. "2026_02_21"
String getDateString() {          // "2026_02_21"
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d_%02d_%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
  return String(buf);
}

// Returns a human-readable date+time string for the web UI, e.g. "2026-02-21  14:35:00"
String getDateTimeString() {      // "2026-02-21  14:35:00"
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d:%02d",
           t->tm_year+1900, t->tm_mon+1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

// Returns date and time formatted for the first two CSV columns, e.g. "2026-02-21,14:35:00"
String getTimeCSV() {             // "2026-02-21,14:35:00"
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d,%02d:%02d:%02d",
           t->tm_year+1900, t->tm_mon+1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

// Returns the UTC offset as a signed string, e.g. "+3" or "-5"
String getUTCString() {
  int8_t u = cfg.utcOffset;
  return (u >= 0) ? ("+" + String((int)u)) : String((int)u);
}

// ============================================================
//  NTP: network time synchronisation
// ============================================================

// Asks the NTP server for the current time, then waits up to 8 s for a response.
// configTime() sets the ESP8266 system clock; after that, time() works like on Linux.
void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.print(F("[NTP] Syncing: ")); Serial.println(cfg.ntpServer);
  configTime((long)cfg.utcOffset * 3600, 0, cfg.ntpServer);
  unsigned long t = millis();
  while (!timeIsValid() && millis() - t < 8000) { delay(200); yield(); }
  ntpSynced = timeIsValid();
  Serial.println(ntpSynced ? ("[NTP] OK: " + getDateTimeString()) : "[NTP] Sync failed.");
  lastNtpSyncMs = millis();
}

// Called from loop() — re-syncs once per hour without blocking
void tickNTP() {
  if (millis() - lastNtpSyncMs >= NTP_SYNC_INTERVAL) syncNTP();
}

// ============================================================
//  Sensors: reading and validation
// ============================================================

void readSensors() {
  // AHT20 gives us temperature and humidity in a single call
  if (sens.ahtOk) {
    sensors_event_t hEvt, tEvt;
    if (aht.getEvent(&hEvt, &tEvt)) {
      sens.temperature = tEvt.temperature;
      sens.humidity    = hEvt.relative_humidity;
      // Check that the values are physically plausible
      sens.tempValid   = (sens.temperature >= TEMP_MIN && sens.temperature <= TEMP_MAX);
      sens.humValid    = (sens.humidity    >= HUM_MIN  && sens.humidity    <= HUM_MAX);
    } else { sens.tempValid = sens.humValid = false; }
  } else { sens.tempValid = sens.humValid = false; }

  // BMP280 gives us pressure; readPressure() returns Pascals, we want hPa (divide by 100)
  if (sens.bmpOk) {
    float p = bmp.readPressure() / 100.0f;
    sens.pressure  = p;
    sens.presValid = (p >= PRES_MIN && p <= PRES_MAX);
  } else { sens.presValid = false; }

  // Stub mode: only active when STUB_MODE is defined AND both sensors are absent.
  // Produces slowly wandering fake values for UI and log testing.
#ifdef STUB_MODE
  if (!sens.ahtOk && !sens.bmpOk) {
    float t = (float)(millis() % 1000) / 1000.0f;
    float w = (t < 0.5f) ? t : (1.0f - t);
    stubTemp += (w - 0.25f) * 0.04f;
    stubHum  += (w - 0.25f) * 0.06f;
    stubPres += (w - 0.25f) * 0.02f;
    stubTemp = constrain(stubTemp, STUB_TEMP_BASE-2.0f, STUB_TEMP_BASE+2.0f);
    stubHum  = constrain(stubHum,  STUB_HUM_BASE -3.0f, STUB_HUM_BASE +3.0f);
    stubPres = constrain(stubPres, STUB_PRES_BASE-1.0f, STUB_PRES_BASE+1.0f);
    sens.temperature = stubTemp;
    sens.humidity    = stubHum;
    sens.pressure    = stubPres;
    sens.tempValid = sens.humValid = sens.presValid = true;
  }
#endif
}

// Called from loop() — reads sensors on a fixed interval without blocking
void tickSensors() {
  if (millis() - lastReadMs >= READ_INTERVAL_MS) { lastReadMs = millis(); readSensors(); }
}

// ============================================================
//  LittleFS logging: daily CSV files
// ============================================================

// Deletes log files older than LOG_MAX_DAYS to prevent the filesystem from filling up
void deleteOldLogs() {
  if (!timeIsValid()) return;
  time_t cutoff = time(nullptr) - (time_t)LOG_MAX_DAYS * 86400;
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    String n = dir.fileName();
    if (!n.startsWith("log_") || !n.endsWith(".csv") || n.length() < 18) continue;
    // Parse the date embedded in the filename: log_YYYY_MM_DD.csv
    int y = n.substring(4,8).toInt(), m = n.substring(9,11).toInt(), d = n.substring(12,14).toInt();
    if (y < 2024 || m < 1 || m > 12 || d < 1 || d > 31) continue;
    struct tm ft = {}; ft.tm_year = y-1900; ft.tm_mon = m-1; ft.tm_mday = d;
    if (mktime(&ft) < cutoff) {
      LittleFS.remove("/" + n);
      Serial.println("[Log] Deleted old: " + n);
    }
  }
}

// Appends one CSV row to today's log file if any value changed since the last write
void writeLog() {
  if (!cfg.loggingEnabled || !timeIsValid()) return;
  // Skip if all three channels are invalid; nothing useful to write
  if (!sens.tempValid && !sens.humValid && !sens.presValid) return;

  // Round to integers for change detection — avoids spamming entries for sensor noise
  int curTemp = sens.tempValid ? (int)roundf(sens.temperature) : prevTempInt;
  int curHum  = sens.humValid  ? (int)roundf(sens.humidity)    : prevHumInt;
  int curPres = sens.presValid ? (int)roundf(sens.pressure)    : prevPresInt;

  if (curTemp == prevTempInt && curHum == prevHumInt && curPres == prevPresInt) return;
  prevTempInt = curTemp; prevHumInt = curHum; prevPresInt = curPres;

  String filename = "/log_" + getDateString() + ".csv";
  bool   newFile  = !LittleFS.exists(filename);

  File f = LittleFS.open(filename, "a");  // open for appending; creates the file if needed
  if (!f) { Serial.println(F("[Log] ERROR: cannot open log file.")); return; }
  if (newFile) {
    f.println(F("date,time,temp_c,hum_pct,pres_hpa"));  // write CSV header on the first row
    Serial.println("[Log] New file: " + filename);
    deleteOldLogs();  // clean up old files once a day when a new file is created
  }
  f.print(getTimeCSV()); f.print(',');
  f.print(curTemp);      f.print(',');
  f.print(curHum);       f.print(',');
  f.println(curPres);
  f.close();
  Serial.print(F("[Log] "));
  Serial.print(curTemp); Serial.print(F(" C  "));
  Serial.print(curHum);  Serial.print(F("%  "));
  Serial.print(curPres); Serial.println(F(" hPa"));
}

// Called from loop() — writes a log entry on a fixed interval
void tickLog() {
  if (millis() - lastLogMs >= LOG_INTERVAL_MS) { lastLogMs = millis(); writeLog(); }
}

// ============================================================
//  Helpers
// ============================================================

// Formats the device uptime as a human-readable string, e.g. "2d 3h 15m 42s"
String getUptime() {
  unsigned long s = millis()/1000, m = s/60; s%=60;
  unsigned long h = m/60; m%=60;
  unsigned long d = h/24; h%=24;
  String r="";
  if(d) r+=String(d)+"d ";
  if(h) r+=String(h)+"h ";
  if(m) r+=String(m)+"m ";
  return r+String(s)+"s";
}

// Returns true if the request carries a valid token (or no token is configured).
// Sends a 403 response and returns false if the check fails.
bool checkToken() {
  if (strlen(cfg.token) == 0) return true;  // auth disabled, always allow
  if (!server.hasArg("token") || server.arg("token") != String(cfg.token)) {
    server.send(403, F("text/plain"), F("403 Forbidden")); return false;
  }
  return true;
}

// Builds zero or more warning banner strings for the main web page
// (sensor missing, value out of range, NTP not synced, etc.)
String buildWarnings() {
  String w = "";
#ifdef STUB_MODE
  if (!sens.ahtOk && !sens.bmpOk)
    w += F("<div class='stub'>&#9654; STUB MODE — simulated data, no real sensors</div>");
  else {
#endif
  if (!sens.ahtOk) w += F("<div class='warn'>&#9888; AHT20 not found on I2C bus</div>");
  if (!sens.bmpOk) w += F("<div class='warn'>&#9888; BMP280 not found on I2C bus</div>");
#ifdef STUB_MODE
  }
#endif
  if (sens.ahtOk && !sens.tempValid)
    w += "<div class='warn'>&#9888; Temperature out of range: "+String(sens.temperature,1)+" C</div>";
  if (sens.ahtOk && !sens.humValid)
    w += "<div class='warn'>&#9888; Humidity out of range: "+String(sens.humidity,1)+" %</div>";
  if (sens.bmpOk && !sens.presValid)
    w += "<div class='warn'>&#9888; Pressure out of range: "+String(sens.pressure,1)+" hPa</div>";
  if (!ntpSynced)
    w += F("<div class='warn'>&#9888; Time not synced — logs paused until NTP responds</div>");
  return w;
}

// ============================================================
//  HTTP handlers: one function per URL endpoint
// ============================================================

// GET /  —  main dashboard; no authentication required
void handleRoot() {
  String page = FPSTR(HTML_MAIN);
  // Replace __PLACEHOLDER__ tokens with live values before sending the page
  page.replace(F("__NAME__"),     String(cfg.sensorName));
  page.replace(F("__DATETIME__"), ntpSynced ? getDateTimeString() : F("Time not synced"));
  page.replace(F("__UTC__"),      getUTCString());
  page.replace(F("__WARNINGS__"), buildWarnings());
  page.replace(F("__TEMP__"), sens.tempValid ? String((int)roundf(sens.temperature)) : F("ERR"));
  page.replace(F("__HUM__"),  sens.humValid  ? String((int)roundf(sens.humidity))    : F("ERR"));
  page.replace(F("__PRES__"), sens.presValid ? String((int)roundf(sens.pressure))    : F("ERR"));
  page.replace(F("__TC__"), sens.tempValid ? F("ok") : F("err"));  // CSS class for colour
  page.replace(F("__HC__"), sens.humValid  ? F("ok") : F("err"));
  page.replace(F("__PC__"), sens.presValid ? F("ok") : F("err"));
  server.send(200, F("text/html"), page);
}

// GET /status  —  device diagnostics table; requires token if one is configured
void handleStatus() {
  if (!checkToken()) return;
  FSInfo fi; LittleFS.info(fi);
  String page = FPSTR(HTML_STATUS);
  page.replace(F("__NAME__"),    String(cfg.sensorName));
  page.replace(F("__TIME__"),    ntpSynced ? getDateTimeString() : F("not synced"));
  page.replace(F("__UTC__"),     getUTCString());
  page.replace(F("__NTP__"),     String(cfg.ntpServer));
  page.replace(F("__SS__"),      ntpSynced ? F("OK") : F("Not synced"));
  page.replace(F("__SC__"),      ntpSynced ? F("ok") : F("err"));
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  page.replace(F("__IP__"),      wifiOk ? (WiFi.localIP().toString()+(strlen(cfg.ip)?"":"  (DHCP)")) : F("not connected"));
  page.replace(F("__SSID__"),    String(cfg.ssid));
  page.replace(F("__MAC__"),     WiFi.macAddress());
  page.replace(F("__RSSI__"),    wifiOk ? String(WiFi.RSSI()) : F("—"));
  page.replace(F("__UPTIME__"),  getUptime());
  page.replace(F("__VER__"),     F(FW_VERSION));
  page.replace(F("__AS__"),      sens.ahtOk ? F("OK") : F("NOT FOUND"));
  page.replace(F("__BS__"),      sens.bmpOk ? F("OK") : F("NOT FOUND"));
  page.replace(F("__AC__"),      sens.ahtOk ? F("ok") : F("err"));
  page.replace(F("__BC__"),      sens.bmpOk ? F("ok") : F("err"));
  page.replace(F("__LOG__"),     cfg.loggingEnabled ? F("Enabled") : F("Disabled"));
  page.replace(F("__FSUSED__"),  String(fi.usedBytes  / 1024));
  page.replace(F("__FSTOTAL__"), String(fi.totalBytes / 1024));
  server.send(200, F("text/html"), page);
}

// GET /prtg  —  PRTG Network Monitor XML format.
// PRTG polls this URL every minute and creates sensor channels automatically.
void handlePRTG() {
  bool anyData = sens.tempValid || sens.humValid || sens.presValid;
  if (!anyData) {
    server.send(500, F("text/xml"), F("<prtg><e>1</e><text>No sensor data</text></prtg>"));
    return;
  }
  String xml = F("<prtg>\n");
  if (sens.tempValid || sens.ahtOk) {
    xml += F("  <result>\n    <channel>Temperature</channel>\n    <unit>Temperature</unit>\n    <float>1</float>\n");
    xml += "    <value>" + String(sens.temperature,1) + "</value>\n";
    if (!sens.tempValid) xml += F("    <warning>1</warning>\n    <message>Out of valid range</message>\n");
    xml += F("  </result>\n");
    xml += F("  <result>\n    <channel>Humidity</channel>\n    <unit>Percent</unit>\n    <float>1</float>\n");
    xml += "    <value>" + String(sens.humidity,1) + "</value>\n";
    if (!sens.humValid) xml += F("    <warning>1</warning>\n    <message>Out of valid range</message>\n");
    xml += F("  </result>\n");
  }
  if (sens.presValid || sens.bmpOk) {
    xml += F("  <result>\n    <channel>Pressure</channel>\n    <unit>Custom</unit>\n    <customunit>hPa</customunit>\n    <float>1</float>\n");
    xml += "    <value>" + String(sens.pressure,1) + "</value>\n";
    if (!sens.presValid) xml += F("    <warning>1</warning>\n    <message>Out of valid range</message>\n");
    xml += F("  </result>\n");
  }
  xml += F("</prtg>");
  server.send(200, F("text/xml"), xml);
}

// GET /zabbix  —  Zabbix HTTP Agent JSON format.
// The Zabbix template fetches this URL and parses each field with JSONPath preprocessing.
void handleZabbix() {
  bool anyData = sens.tempValid || sens.humValid || sens.presValid;
  if (!anyData) {
    server.send(500, F("application/json"), F("{\"error\":\"No sensor data\"}"));
    return;
  }
  String json = F("{\n");
  json += "  \"temperature\": " + (sens.tempValid ? String(sens.temperature,1) : F("null")) + ",\n";
  json += "  \"humidity\": "    + (sens.humValid  ? String(sens.humidity,1)    : F("null")) + ",\n";
  json += "  \"pressure\": "   + (sens.presValid ? String(sens.pressure,1)   : F("null")) + ",\n";
  json += "  \"sensor_aht20\": "  + String(sens.ahtOk ? F("true") : F("false")) + ",\n";
  json += "  \"sensor_bmp280\": " + String(sens.bmpOk ? F("true") : F("false")) + ",\n";
  json += "  \"all_ok\": " + String((sens.tempValid&&sens.humValid&&sens.presValid)?F("true"):F("false")) + "\n}";
  server.send(200, F("application/json"), json);
}

// GET /logs  —  HTML page listing all CSV log files in the filesystem.
// Uses chunked transfer (sendContent) to avoid buffering the full page in RAM.
void handleLogs() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, F("text/html"), "");

  server.sendContent(F("<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Logs</title><style>"
    "body{font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0;padding:24px}"
    "h1{color:#38bdf8;font-size:1.3em;margin-bottom:16px}"
    "ul{list-style:none;padding:0}"
    "li{padding:8px 0;border-bottom:1px solid #1e293b;font-size:0.95em}"
    "a{color:#38bdf8;text-decoration:none}a:hover{text-decoration:underline}"
    ".sz{color:#475569;font-size:0.8em;margin-left:8px}"
    ".back{margin-top:16px;font-size:0.85em}"
    ".empty{color:#475569}"
    "</style></head><body>"
    "<h1>&#128196; Log Files</h1><ul>"));

  // Collect all matching filenames into an array so we can sort them
  String names[LOG_MAX_DAYS + 2];
  int count = 0;
  Dir dir = LittleFS.openDir("/");
  while (dir.next() && count < LOG_MAX_DAYS + 1) {
    String n = dir.fileName();
    if (n.startsWith("log_") && n.endsWith(".csv")) names[count++] = n;
  }
  // Bubble-sort descending so the newest file appears at the top
  for (int i = 0; i < count-1; i++)
    for (int j = 0; j < count-i-1; j++)
      if (names[j] < names[j+1]) { String t=names[j]; names[j]=names[j+1]; names[j+1]=t; }

  if (count == 0) {
    server.sendContent(F("<li class='empty'>No log files yet.</li>"));
  } else {
    for (int i = 0; i < count; i++) {
      File f = LittleFS.open("/" + names[i], "r");
      size_t sz = f ? f.size() : 0;
      if (f) f.close();
      // Convert filename "log_2026_02_21.csv" to display label "2026-02-21"
      String ds = names[i].substring(4,14); ds.replace("_","-");
      server.sendContent("<li><a href='/logfile?name=" + names[i] + "'>" +
                         ds + ".csv</a><span class='sz'>" + String(sz) + " bytes</span></li>");
    }
  }

  server.sendContent(F("</ul><p class='back'><a href='/'>&#8592; Back</a></p></body></html>"));
  server.sendContent("");  // empty chunk signals end of chunked transfer encoding
}

// GET /logfile?name=log_YYYY_MM_DD.csv  —  download a raw CSV file.
// The name parameter is validated strictly to prevent path traversal attacks.
void handleLogFile() {
  if (!server.hasArg("name")) { server.send(400, F("text/plain"), F("Missing name")); return; }
  String name = server.arg("name");
  if (!name.startsWith("log_") || !name.endsWith(".csv") || name.length() > 24) {
    server.send(400, F("text/plain"), F("Invalid filename")); return;
  }
  String path = "/" + name;
  if (!LittleFS.exists(path)) { server.send(404, F("text/plain"), F("Not found")); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(500, F("text/plain"), F("Cannot open")); return; }
  server.sendHeader(F("Content-Disposition"), "inline; filename=\"" + name + "\"");
  server.streamFile(f, F("text/csv"));  // streams the file directly without copying to RAM
  f.close();
}

// ============================================================
//  Serial menu: configuration via USB / UART terminal
// ============================================================

void printMenu() {
  Serial.println(F("\n=== WiFi Climate Sensor v" FW_VERSION " ==="));
  Serial.println(F(" 1. Show current settings"));
  Serial.println(F(" 2. Set SSID"));
  Serial.println(F(" 3. Set WiFi password"));
  Serial.println(F(" 4. Set static IP  (empty = DHCP)"));
  Serial.println(F(" 5. Set subnet mask"));
  Serial.println(F(" 6. Set gateway"));
  Serial.println(F(" 7. Set DNS server  (empty = use gateway)"));
  Serial.println(F(" 8. Set sensor name / location"));
  Serial.println(F(" 9. Set /status token  (empty = open)"));
  Serial.println(F("10. Set NTP server"));
  Serial.println(F("11. Set UTC offset  (-12..+14)"));
  Serial.println(F("12. Toggle logging  (on/off)"));
  Serial.println(F("13. Force NTP sync now"));
  Serial.println(F("14. Show sensor readings"));
  Serial.println(F("15. List log files on FS"));
  Serial.println(F("16. Hard reset  (wipe all settings + format FS)"));
  Serial.println(F("17. Set SNMP community  (default: public)"));
  Serial.println(F(" 0. Save and reboot"));
  Serial.println(F("Command > (enter number, press Enter):"));
}

void printCurrentSettings() {
  Serial.println(F("\n--- Current Settings ---"));
  Serial.print(F("Sensor name : ")); Serial.println(cfg.sensorName);
  Serial.print(F("SSID        : ")); Serial.println(cfg.ssid);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP          : ")); Serial.print(WiFi.localIP());
    Serial.println(strlen(cfg.ip) ? F("") : F("  (DHCP)"));
    Serial.print(F("Subnet      : ")); Serial.println(WiFi.subnetMask());
    Serial.print(F("Gateway     : ")); Serial.println(WiFi.gatewayIP());
    Serial.print(F("DNS         : ")); Serial.println(WiFi.dnsIP());
    Serial.print(F("RSSI        : ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
  } else {
    Serial.print(F("IP          : ")); Serial.println(strlen(cfg.ip) ? cfg.ip : "DHCP (not connected)");
    Serial.print(F("Subnet      : ")); Serial.println(cfg.subnet);
    Serial.print(F("Gateway     : ")); Serial.println(cfg.gateway);
    Serial.print(F("DNS         : ")); Serial.println(strlen(cfg.dns) ? cfg.dns : "use gateway");
  }
  Serial.print(F("NTP server  : ")); Serial.println(cfg.ntpServer);
  Serial.print(F("UTC offset  : ")); Serial.println((int)cfg.utcOffset);
  Serial.print(F("NTP synced  : ")); Serial.println(ntpSynced ? F("YES") : F("NO"));
  if (ntpSynced) { Serial.print(F("Local time  : ")); Serial.println(getDateTimeString()); }
  Serial.print(F("Logging     : ")); Serial.println(cfg.loggingEnabled ? F("ENABLED") : F("DISABLED"));
  Serial.print(F("SNMP comm   : ")); Serial.println(cfg.snmpCommunity);
  Serial.print(F("Token       : ")); Serial.println(strlen(cfg.token) ? F("[set]") : F("[not set]"));
  Serial.print(F("FW version  : ")); Serial.println(F(FW_VERSION));
  FSInfo fi;
  if (LittleFS.info(fi)) {
    Serial.print(F("FS used     : "));
    Serial.print(fi.usedBytes/1024); Serial.print(F(" KB / "));
    Serial.print(fi.totalBytes/1024); Serial.println(F(" KB"));
  }
}

// Reads one line of text from the serial port with local echo and backspace support.
// Discards leading newline characters left over from a previous Enter keypress.
// Times out after 30 seconds of inactivity.
String readLine() {
  String input = "";

  // Flush any stale \r or \n from the previous command's Enter key (up to 200 ms)
  unsigned long ft = millis();
  while (millis() - ft < 200UL) {
    if (Serial.available()) {
      char c = Serial.peek();
      if (c == '\r' || c == '\n') Serial.read();
      else break;
    }
    yield();
  }

  // Main read loop: collect characters until Enter or timeout
  unsigned long t = millis();
  while (millis() - t < 30000UL) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\r' || c == '\n') break;       // Enter — end of input
      if (c == 127 || c == '\b') {             // Backspace
        if (input.length() > 0) {
          input.remove(input.length() - 1);
        }
      } else if (c >= 32) {                    // any printable character
        Serial.print(c);                       // echo it back so the user sees what they typed
        input += c;
      }
    }
    yield();
  }
  Serial.println();
  lastReconnMs = millis();  // reset reconnect timer so it doesn't fire right after menu use
  return input;
}

// Main serial menu handler. Called every loop iteration.
// Waits for a digit(s) + Enter, then executes the matching command.
void handleSerialMenu() {
  if (!Serial.available()) return;

  // Read digits until Enter or 10-second timeout
  String cmdStr = "";
  unsigned long t = millis();
  while (millis() - t < 10000UL) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\r' || c == '\n') break;   // Enter — done
      if (c == 127 || c == '\b') {          // Backspace
        if (cmdStr.length() > 0) { cmdStr.remove(cmdStr.length()-1); }
      } else if (c >= '0' && c <= '9') {     // only digits make sense here
        cmdStr += c;
      }
    }
    server.handleClient();  // keep serving HTTP requests while waiting for input
    yield();
  }
  Serial.println();
  cmdStr.trim();

  // Nothing typed — just redraw the menu
  if (cmdStr.length() == 0) { printMenu(); return; }

  int cmd = cmdStr.toInt();
  // Flush any trailing \r or \n left over from the Enter keypress
  unsigned long ft = millis();
  while (millis() - ft < 50UL) { if (Serial.available()) Serial.read(); yield(); }
  lastReconnMs = millis();

  String s;
  switch (cmd) {
    case 1:  printCurrentSettings(); break;
    case 2:  Serial.print(F("SSID: ")); readLine().toCharArray(cfg.ssid, sizeof(cfg.ssid)); Serial.println(F("OK")); break;
    case 3:  Serial.print(F("Password: ")); readLine().toCharArray(cfg.password, sizeof(cfg.password)); Serial.println(F("OK")); break;
    case 4:  Serial.print(F("IP (empty=DHCP): ")); readLine().toCharArray(cfg.ip, sizeof(cfg.ip)); Serial.println(F("OK")); break;
    case 5:  Serial.print(F("Subnet: ")); readLine().toCharArray(cfg.subnet, sizeof(cfg.subnet)); Serial.println(F("OK")); break;
    case 6:  Serial.print(F("Gateway: ")); readLine().toCharArray(cfg.gateway, sizeof(cfg.gateway)); Serial.println(F("OK")); break;
    case 7:  Serial.print(F("DNS (empty=use gateway): ")); readLine().toCharArray(cfg.dns, sizeof(cfg.dns)); Serial.println(F("OK")); break;
    case 8:  Serial.print(F("Sensor name: ")); readLine().toCharArray(cfg.sensorName, sizeof(cfg.sensorName)); Serial.println(F("OK")); break;
    case 9:  Serial.print(F("Token (empty=open): ")); readLine().toCharArray(cfg.token, sizeof(cfg.token)); Serial.println(F("OK")); break;
    case 10:
      Serial.print(F("NTP server [pool.ntp.org]: "));
      s = readLine();
      if (s.length() > 0) s.toCharArray(cfg.ntpServer, sizeof(cfg.ntpServer));
      Serial.println(F("OK"));
      break;
    case 11:
      Serial.print(F("UTC offset (-12..+14): "));
      s = readLine();
      if (s.length() > 0) cfg.utcOffset = (int8_t)constrain(s.toInt(), -12, 14);
      Serial.print(F("UTC offset: ")); Serial.println((int)cfg.utcOffset);
      break;
    case 12:
      cfg.loggingEnabled = !cfg.loggingEnabled;
      Serial.print(F("Logging: ")); Serial.println(cfg.loggingEnabled ? F("ENABLED") : F("DISABLED"));
      break;
    case 13:
      syncNTP();
      break;
    case 14:
      // Read sensors and print a quick diagnostic report to the serial monitor
      readSensors();
      Serial.println(F("\n--- Sensors ---"));
      Serial.print(F("AHT20 : ")); Serial.println(sens.ahtOk ? F("found") : F("NOT FOUND"));
      Serial.print(F("BMP280: ")); Serial.println(sens.bmpOk ? F("found") : F("NOT FOUND"));
#ifdef STUB_MODE
      if (!sens.ahtOk && !sens.bmpOk) Serial.println(F("  [STUB MODE -- simulated]"));
#endif
      Serial.print(F("Temp  : ")); Serial.print(sens.temperature, 1);
      Serial.println(sens.tempValid ? F(" C  [OK]") : F(" C  [OUT OF RANGE]"));
      Serial.print(F("Hum   : ")); Serial.print(sens.humidity, 1);
      Serial.println(sens.humValid ? F(" %  [OK]") : F(" %  [OUT OF RANGE]"));
      Serial.print(F("Pres  : ")); Serial.print(sens.pressure, 1);
      Serial.println(sens.presValid ? F(" hPa [OK]") : F(" hPa [OUT OF RANGE]"));
      break;
    case 15: {
      Serial.println(F("\n--- Log files ---"));
      Dir dir = LittleFS.openDir("/");
      bool any = false;
      while (dir.next()) {
        String n = dir.fileName();
        if (!n.startsWith("log_")) continue;
        File f = LittleFS.open("/"+n,"r");
        Serial.print("  "); Serial.print(n);
        if (f) { Serial.print(F("  (")); Serial.print(f.size()); Serial.print(F(" bytes)")); f.close(); }
        Serial.println(); any = true;
      }
      if (!any) Serial.println(F("  (no log files yet)"));
      break;
    }
    case 16: {
      // Hard reset: ask for confirmation before destroying everything
      Serial.println(F("\n*** HARD RESET ***"));
      Serial.println(F("This will erase ALL settings and ALL log files."));
      Serial.print(F("Type YES to confirm, anything else to cancel: "));
      String confirm = readLine();
      if (confirm == "YES") {
        Serial.println(F("Wiping EEPROM..."));
        // Overwrite the entire EEPROM region with zeros; the bad magic byte
        // ensures loadSettings() will re-apply defaults on next boot
        EEPROM.begin(EEPROM_SIZE);
        for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
        EEPROM.commit();
        Serial.println(F("Formatting LittleFS..."));
        if (LittleFS.format()) {
          Serial.println(F("Format OK."));
        } else {
          Serial.println(F("Format FAILED -- reflash the firmware to recover."));
        }
        Serial.println(F("Done. Rebooting in 3 s..."));
        delay(3000);
        ESP.restart();
      } else {
        Serial.println(F("Cancelled."));
      }
      break;
    }
    case 0:
      saveSettings();
      Serial.println(F("Saved. Rebooting..."));
      delay(2000); ESP.restart();
      break;
    case 17:
      Serial.print(F("SNMP community [public]: "));
      { String s = readLine();
        if (s.length() > 0)
          s.toCharArray(cfg.snmpCommunity, sizeof(cfg.snmpCommunity));
        Serial.print(F("SNMP community: ")); Serial.println(cfg.snmpCommunity); }
      break;
    default: break;
  }  // end switch
  lastReconnMs = millis();  // prevent the reconnect timer from firing right after menu use
  printMenu();
}

// ============================================================
//  SNMP minimal agent  (SNMPv1 / SNMPv2c, GET only)
//
//  Implements a hand-rolled BER/ASN.1 encoder-decoder that fits
//  in the ESP-01's tight 80 KB heap with no extra libraries.
//
//  All writes use 2-byte BER length (0x81 0xNN) for SEQUENCE
//  wrappers — valid per BER, accepted by every real SNMP stack.
// ============================================================

// ---- ASN.1 / BER constants -----------------------------------
#define ASN_INT       0x02
#define ASN_OCTSTR    0x04
#define ASN_NULL      0x05
#define ASN_OID       0x06
#define ASN_SEQ       0x30
#define ASN_TIMETICKS 0x43
#define ASN_GAUGE32   0x42
#define SNMP_GET_REQ  0xA0
#define SNMP_GET_RESP 0xA2
// v2c "noSuchObject" exception — returned for unknown OIDs when version == 1 (v2c)
#define SNMP_NOSUCHOBJ 0x80

// ---- Pre-encoded OID data bytes (PROGMEM) --------------------
// First byte 0x2B encodes the mandatory 1.3 prefix (1×40+3=43=0x2B).
// Remaining components are base-128 big-endian with high bit set on all
// non-final bytes.  99999 encodes as 0x86 0x8D 0x1F.

// 1.3.6.1.2.1.1.1.0  sysDescr
static const uint8_t P_SYSDESCR[8]  PROGMEM =
  {0x2B,0x06,0x01,0x02,0x01,0x01,0x01,0x00};
// 1.3.6.1.2.1.1.3.0  sysUpTime
static const uint8_t P_SYSUPTIME[8] PROGMEM =
  {0x2B,0x06,0x01,0x02,0x01,0x01,0x03,0x00};
// 1.3.6.1.4.1.99999.1.1.0  temperature ×10
static const uint8_t P_TEMP[11]     PROGMEM =
  {0x2B,0x06,0x01,0x04,0x01,0x86,0x8D,0x1F,0x01,0x01,0x00};
// 1.3.6.1.4.1.99999.1.2.0  humidity ×10
static const uint8_t P_HUM[11]      PROGMEM =
  {0x2B,0x06,0x01,0x04,0x01,0x86,0x8D,0x1F,0x01,0x02,0x00};
// 1.3.6.1.4.1.99999.1.3.0  pressure ×10
static const uint8_t P_PRES[11]     PROGMEM =
  {0x2B,0x06,0x01,0x04,0x01,0x86,0x8D,0x1F,0x01,0x03,0x00};

// ---- OID index constants -------------------------------------
#define OID_SYSDESCR  0
#define OID_SYSUPTIME 1
#define OID_TEMP      2
#define OID_HUM       3
#define OID_PRES      4
#define OID_UNKNOWN   5

static uint8_t snmpMatchOID(const uint8_t *d, uint8_t n) {
  if (n ==  8 && !memcmp_P(d, P_SYSDESCR,  8)) return OID_SYSDESCR;
  if (n ==  8 && !memcmp_P(d, P_SYSUPTIME, 8)) return OID_SYSUPTIME;
  if (n == 11 && !memcmp_P(d, P_TEMP,     11)) return OID_TEMP;
  if (n == 11 && !memcmp_P(d, P_HUM,      11)) return OID_HUM;
  if (n == 11 && !memcmp_P(d, P_PRES,     11)) return OID_PRES;
  return OID_UNKNOWN;
}

// ---- BER read helpers ----------------------------------------
// rp is the current read offset within snmpBuf.

static uint16_t rp;

static uint8_t  rByte()          { return (rp < SNMP_BUF_SZ) ? snmpBuf[rp++] : 0; }
static void     rSkip(uint16_t n){ rp += n; }

static uint16_t rLen() {
  uint8_t b = rByte();
  if (b < 0x80) return b;          // short form
  uint8_t n = b & 0x7F;            // long form: n bytes follow
  uint16_t l = 0;
  while (n--) l = (l << 8) | rByte();
  return l;
}

// Read n bytes as a signed integer (sign-extended from MSB)
static int32_t rInt32(uint8_t n) {
  int32_t v = (snmpBuf[rp] & 0x80) ? -1L : 0L;
  while (n--) v = (v << 8) | rByte();
  return v;
}

// ---- BER write helpers ---------------------------------------
// wp is the current write offset within snmpBuf.

static uint16_t wp;

static void wByte(uint8_t b) { if (wp < SNMP_BUF_SZ) snmpBuf[wp++] = b; }

// Begin a constructed TLV (SEQUENCE or PDU context tag).
// Writes tag + two-byte length placeholder (0x81 0x00).
// Returns the position of the 0x81 byte; pass it to wSeqEnd() later.
static uint16_t wSeqStart(uint8_t tag) {
  wByte(tag);
  uint16_t pos = wp;
  wByte(0x81); wByte(0x00); // placeholder; patched by wSeqEnd
  return pos;
}

// Patch the length bytes written by wSeqStart.
static void wSeqEnd(uint16_t lenPos) {
  snmpBuf[lenPos + 1] = (uint8_t)(wp - lenPos - 2);
}

// Write a 4-byte INTEGER (handles negative temperatures).
static void wInt32(int32_t v) {
  wByte(ASN_INT); wByte(4);
  wByte((v >> 24) & 0xFF); wByte((v >> 16) & 0xFF);
  wByte((v >>  8) & 0xFF); wByte( v        & 0xFF);
}

// Write a 4-byte unsigned value with a given tag (Gauge32 or TimeTicks).
static void wU32(uint8_t tag, uint32_t v) {
  wByte(tag); wByte(4);
  wByte((v >> 24) & 0xFF); wByte((v >> 16) & 0xFF);
  wByte((v >>  8) & 0xFF); wByte( v        & 0xFF);
}

// Write OID TLV for one of our known OIDs (data from PROGMEM).
static void wOID(uint8_t oidIdx) {
  const uint8_t *pgm; uint8_t len;
  switch (oidIdx) {
    case OID_SYSDESCR:  pgm = P_SYSDESCR;  len =  8; break;
    case OID_SYSUPTIME: pgm = P_SYSUPTIME; len =  8; break;
    case OID_TEMP:      pgm = P_TEMP;      len = 11; break;
    case OID_HUM:       pgm = P_HUM;       len = 11; break;
    case OID_PRES:      pgm = P_PRES;      len = 11; break;
    default: return;
  }
  wByte(ASN_OID); wByte(len);
  for (uint8_t i = 0; i < len; i++) wByte(pgm_read_byte(&pgm[i]));
}

// Write OID TLV by copying raw bytes from snmpBuf (echoing an unknown OID back).
static void wOIDRaw(uint16_t oidDataPos, uint8_t oidDataLen) {
  wByte(ASN_OID); wByte(oidDataLen);
  for (uint8_t i = 0; i < oidDataLen; i++) wByte(snmpBuf[oidDataPos + i]);
}

// ---- Core packet processor -----------------------------------

static void snmpProcess(uint16_t rxLen) {
  rp = 0;

  // ---- Parse the incoming GetRequest ----

  // Outer SEQUENCE
  if (rByte() != ASN_SEQ) return;
  rLen(); // skip outer length

  // Version INTEGER (0 = v1, 1 = v2c)
  if (rByte() != ASN_INT) return;
  int32_t version = rInt32((uint8_t)rLen());
  if (version != 0 && version != 1) return;

  // Community OCTET STRING
  if (rByte() != ASN_OCTSTR) return;
  uint8_t cLen = (uint8_t)rLen();
  if (cLen > 23 || rp + cLen > rxLen) return;
  char rxComm[24];
  memcpy(rxComm, &snmpBuf[rp], cLen); rxComm[cLen] = '\0';
  rSkip(cLen);
  if (strcmp(rxComm, cfg.snmpCommunity) != 0) return; // community mismatch — silent drop

  // PDU type — only GetRequest is supported
  if (rByte() != SNMP_GET_REQ) return;
  rLen(); // skip PDU length

  // Request-ID INTEGER
  if (rByte() != ASN_INT) return;
  int32_t requestId = rInt32((uint8_t)rLen());

  // error-status and error-index (both 0 in a request; skip them)
  if (rByte() != ASN_INT) return; rSkip(rLen());
  if (rByte() != ASN_INT) return; rSkip(rLen());

  // VarBindList SEQUENCE
  if (rByte() != ASN_SEQ) return;
  uint16_t vblLen = rLen();
  uint16_t vblEnd = rp + vblLen;

  // Collect up to 6 requested OIDs.
  // We save: matching index + original data position (for echoing unknown OIDs).
  #define SNMP_MAX_VB 6
  uint8_t  vbIdx[SNMP_MAX_VB];
  uint16_t vbOidPos[SNMP_MAX_VB];  // position of OID data bytes in snmpBuf
  uint8_t  vbOidLen[SNMP_MAX_VB];
  uint8_t  vbCount = 0;

  while (rp < vblEnd && vbCount < SNMP_MAX_VB) {
    if (rByte() != ASN_SEQ) break;
    rLen(); // VarBind length
    if (rByte() != ASN_OID) break;
    uint8_t oidLen = (uint8_t)rLen();
    if (rp + oidLen > vblEnd) break;
    vbIdx[vbCount]    = snmpMatchOID(&snmpBuf[rp], oidLen);
    vbOidPos[vbCount] = rp;
    vbOidLen[vbCount] = oidLen;
    vbCount++;
    rSkip(oidLen);
    // Skip the NULL placeholder in the request (if present)
    if (rp < vblEnd && snmpBuf[rp] == ASN_NULL) { rByte(); rSkip(rLen()); }
  }

  // ---- Build the GetResponse ----
  // We now overwrite snmpBuf from position 0; the parsed request data is no
  // longer needed (we have everything in local variables).

  wp = 0;

  uint16_t outerStart = wSeqStart(ASN_SEQ);

    // Version (echo back what the requester sent)
    wByte(ASN_INT); wByte(1); wByte((uint8_t)version);

    // Community
    uint8_t cl = strlen(cfg.snmpCommunity);
    wByte(ASN_OCTSTR); wByte(cl);
    for (uint8_t i = 0; i < cl; i++) wByte(cfg.snmpCommunity[i]);

    uint16_t pduStart = wSeqStart(SNMP_GET_RESP);

      // Request-ID (fixed 4-byte encoding for simplicity)
      wByte(ASN_INT); wByte(4);
      wByte((requestId >> 24) & 0xFF); wByte((requestId >> 16) & 0xFF);
      wByte((requestId >>  8) & 0xFF); wByte( requestId        & 0xFF);

      // error-status = 0, error-index = 0
      wByte(ASN_INT); wByte(1); wByte(0);
      wByte(ASN_INT); wByte(1); wByte(0);

      uint16_t vblStart = wSeqStart(ASN_SEQ); // VarBindList

        for (uint8_t i = 0; i < vbCount; i++) {
          uint16_t vbStart = wSeqStart(ASN_SEQ); // VarBind

            // Echo the OID back (known OIDs from PROGMEM; unknown ones from the saved rx bytes)
            if (vbIdx[i] != OID_UNKNOWN) wOID(vbIdx[i]);
            else                         wOIDRaw(vbOidPos[i], vbOidLen[i]);

            // Value
            switch (vbIdx[i]) {

              case OID_SYSDESCR: {
                // "SensorName / ESP01-Climate vX.Y"
                String desc = String(cfg.sensorName) +
                              F(" / ESP01-Climate v") + F(FW_VERSION);
                uint8_t dl = min((int)desc.length(), 63);
                wByte(ASN_OCTSTR); wByte(dl);
                for (uint8_t j = 0; j < dl; j++) wByte(desc[j]);
                break;
              }

              case OID_SYSUPTIME:
                // TimeTicks: centiseconds (hundredths of a second) since boot
                wU32(ASN_TIMETICKS, millis() / 10UL);
                break;

              case OID_TEMP:
                if (sens.tempValid)
                  wInt32((int32_t)(sens.temperature * 10.0f +
                         (sens.temperature < 0.0f ? -0.5f : 0.5f)));
                else
                  { wByte(ASN_NULL); wByte(0); }
                break;

              case OID_HUM:
                if (sens.humValid)
                  wU32(ASN_GAUGE32, (uint32_t)(sens.humidity * 10.0f + 0.5f));
                else
                  { wByte(ASN_NULL); wByte(0); }
                break;

              case OID_PRES:
                if (sens.presValid)
                  wU32(ASN_GAUGE32, (uint32_t)(sens.pressure * 10.0f + 0.5f));
                else
                  { wByte(ASN_NULL); wByte(0); }
                break;

              default:
                // Unknown OID: noSuchObject (v2c) or NULL (v1)
                wByte(version == 1 ? SNMP_NOSUCHOBJ : ASN_NULL); wByte(0);
                break;
            }

          wSeqEnd(vbStart); // close VarBind
        }

      wSeqEnd(vblStart);  // close VarBindList
    wSeqEnd(pduStart);    // close GetResponse PDU
  wSeqEnd(outerStart);    // close outer SEQUENCE

  if (wp >= SNMP_BUF_SZ) {
    Serial.println(F("[SNMP] Response overflow, packet dropped"));
    return;
  }

  snmpUdp.beginPacket(snmpUdp.remoteIP(), snmpUdp.remotePort());
  snmpUdp.write(snmpBuf, wp);
  snmpUdp.endPacket();
}

// Called from setup() to open the UDP socket on port 161.
void setupSNMP() {
  snmpUdp.begin(SNMP_PORT);
  Serial.print(F("[SNMP] Agent listening on UDP port "));
  Serial.println(SNMP_PORT);
}

// Called from loop() — checks for an incoming SNMP packet and responds.
void tickSNMP() {
  if (WiFi.status() != WL_CONNECTED) return;
  int n = snmpUdp.parsePacket();
  if (n <= 0 || n > SNMP_BUF_SZ) return;
  int r = snmpUdp.read(snmpBuf, SNMP_BUF_SZ);
  if (r > 0) snmpProcess((uint16_t)r);
}



void connectWiFi() {
  if (strlen(cfg.ssid) == 0) { Serial.println(F("[WiFi] SSID not set.")); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // we handle reconnects ourselves in loop()
  if (strlen(cfg.ip) > 0) {
    // Static IP: parse all three address fields and apply them before connecting
    IPAddress ip, gw, sn, dns;
    bool dnsOk = strlen(cfg.dns) > 0 && dns.fromString(cfg.dns);
    if (ip.fromString(cfg.ip) && gw.fromString(cfg.gateway) && sn.fromString(cfg.subnet)) {
      // If DNS is not set, fall back to using the gateway as DNS (common default)
      WiFi.config(ip, gw, sn, dnsOk ? dns : gw);
    } else Serial.println(F("[WiFi] IP parse error, using DHCP."));
  }
  WiFi.begin(cfg.ssid, cfg.password);
  Serial.print(F("[WiFi] Connecting"));
  unsigned long t = millis();
  // Poll until connected or timeout, printing a dot every 300 ms as a progress indicator
  while (WiFi.status()!=WL_CONNECTED && millis()-t<WIFI_TIMEOUT_MS) { delay(300); Serial.print('.'); }
  if (WiFi.status()==WL_CONNECTED) {
    Serial.println(); Serial.print(F("[WiFi] IP: ")); Serial.println(WiFi.localIP());
  } else { Serial.println(F("\n[WiFi] Failed.")); }
}

// ============================================================
//  Setup: runs once at power-on or after reset
// ============================================================

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  // Print blank lines to push the ROM bootloader noise (74880 baud garbage) off the screen
  for (uint8_t i = 0; i < 12; i++) Serial.println();
  Serial.println(F("[Boot] ESP-01 Climate Sensor v" FW_VERSION));
#ifdef STUB_MODE
  Serial.println(F("[Boot] *** STUB MODE -- simulated sensor data ***"));
#endif

  loadSettings();

  // Mount the LittleFS filesystem from flash memory.
  // If it has never been formatted (first boot) or is corrupted, format it automatically.
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Mount failed -- formatting LittleFS, please wait..."));
    if (LittleFS.format()) {
      Serial.println(F("[FS] Format OK. Mounting..."));
      if (!LittleFS.begin()) {
        Serial.println(F("[FS] FATAL: mount failed after format. Check Flash Size in Tools menu."));
      }
    } else {
      Serial.println(F("[FS] FATAL: format failed. Check Flash Size: must be 1MB (FS:512KB)."));
    }
  }
  {
    FSInfo fi;
    if (LittleFS.info(fi)) {
      Serial.print(F("[FS] OK - "));
      Serial.print(fi.usedBytes/1024); Serial.print(F(" KB used / "));
      Serial.print(fi.totalBytes/1024); Serial.println(F(" KB total"));
    }
  }

  // Start I2C on the two GPIO pins available on ESP-01
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);  // 100 kHz — safer than 400 kHz with long wires or no pull-up resistors

  // Try to initialise both sensors; store the result so the rest of the
  // code knows whether each chip is actually present on the bus
  sens.ahtOk = aht.begin();
  sens.bmpOk = bmp.begin(BMP280_ADDR);
  Serial.print(F("[Sensor] AHT20 : ")); Serial.println(sens.ahtOk ? F("OK") : F("NOT FOUND"));
  Serial.print(F("[Sensor] BMP280: ")); Serial.println(sens.bmpOk ? F("OK") : F("NOT FOUND"));

  // Configure BMP280 for continuous operation with high oversampling and a low-pass filter
  // so the pressure readings are stable despite the inherent sensor noise
  if (sens.bmpOk)
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2, Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,  Adafruit_BMP280::STANDBY_MS_500);

  readSensors();
  connectWiFi();
  syncNTP();

  // Register a handler function for each URL the server will respond to
  server.on(F("/"),        handleRoot);
  server.on(F("/status"),  handleStatus);
  server.on(F("/prtg"),    handlePRTG);
  server.on(F("/zabbix"),  handleZabbix);
  server.on(F("/logs"),    handleLogs);
  server.on(F("/logfile"), handleLogFile);
  server.onNotFound([]() { server.send(404, F("text/plain"), F("404 Not Found")); });
  server.begin();
  Serial.println(F("[HTTP] Server started"));
  setupSNMP();
  printMenu();
}

// ============================================================
//  Loop: runs repeatedly after setup() returns
// ============================================================

void loop() {
  server.handleClient();   // process any pending incoming HTTP requests
  handleSerialMenu();      // check for serial input and run menu commands
  tickSensors();           // read sensors if enough time has passed
  tickLog();               // write a CSV entry if enough time has passed
  tickNTP();               // re-sync time if an hour has passed
  tickSNMP();              // respond to any incoming SNMP GET requests

  // If WiFi dropped, try to reconnect every RECONNECT_MS milliseconds
  if (strlen(cfg.ssid)>0 && WiFi.status()!=WL_CONNECTED) {
    if (millis()-lastReconnMs > RECONNECT_MS) {
      lastReconnMs = millis();
      Serial.println(F("[WiFi] Reconnecting..."));
      WiFi.disconnect(); delay(500);
      connectWiFi();
      if (WiFi.status()==WL_CONNECTED && !ntpSynced) syncNTP();
    }
  }
}
