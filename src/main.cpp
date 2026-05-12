// Milestone 4 — PPS-disciplined NTP server + MQTT status publish.
//
// Builds on milestones 1-3:
//   - NMEA via SoftwareSerial on D2, PPS via interrupt on D5 (M1).
//   - Wi-Fi via WiFiManager + UDP/123 listener (M2).
//   - PPS-disciplined NTP time + holdover (M3).
//   - Retained MQTT status to shack/esp8266-ntp/status every 30 s (M4),
//     with LWT `{"event":"offline"}` so the broker maintains a marker
//     when the ESP drops. Topic field names mirror the Pi NTP server's
//     `shack/gpsntp/chrony` topic where they apply (host, ts, stratum,
//     ref_id, leap, root_delay_s, root_dispersion_s, fix_mode,
//     sat_used) plus ESP-specific (pps_count, pps_interval_us,
//     pps_sync, ntp_requests, rssi_dbm, uptime_s, free_heap).
//
// In M3 (still in M4): the NTP responder uses real PPS-disciplined GPS time.
// On each PPS edge the ISR records micros() as the on-time reference.
// When the next $GxRMC sentence arrives (within ~500 ms of that edge),
// we know the Unix epoch for that PPS edge. NTP requests are then
// answered with timestamps computed as
//   current_unix = unix_seconds_at_pps + (micros() - micros_at_pps)/1e6
// and converted to NTP (epoch 1900-01-01, +2208988800 s offset).
//
// Holdover: if no fresh PPS+RMC pair is seen for >5 s the server
// reverts to stratum 16 + refid "INIT". Clients silently fall over
// to other servers — the right behaviour per RFC 5905 §3.5.
//
// Wiring (unchanged from M1):
//   D2 (GPIO4)  <- L89 TX    (SoftwareSerial RX, 9600 baud — *not* the
//                             NodeMCU RX header pin, which is GPIO3 /
//                             hardware UART RX and shares the USB-serial)
//   D5 (GPIO14) <- L89 PPS   (direct on Quectel L89 pin 6, native
//                             active-high → RISING edge marks on-time)
//   3V3, GND    <- L89 power and common ground

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <TinyGPS++.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

constexpr uint8_t  GPS_RX_PIN        = D2;
constexpr uint8_t  PPS_PIN           = D5;
constexpr uint32_t GPS_BAUD          = 9600;
constexpr uint32_t LOG_INTERVAL_MS   = 1000;
constexpr uint32_t WINDOW_LENGTH_MS  = 60000;

constexpr uint16_t NTP_PORT          = 123;
constexpr size_t   NTP_PACKET_SIZE   = 48;

// NTP epoch (1900-01-01) is 2208988800 seconds before Unix epoch (1970-01-01).
constexpr uint32_t NTP_UNIX_OFFSET   = 2208988800UL;

// Sync state thresholds:
// We sync only when *all four* gates pass: location fresh, RMC freshly
// parsed, PPS edge advanced, and PPS-to-RMC latency in a plausible band.
// The latency band is the off-by-one fix: a too-small sincePps means a
// *newer* PPS edge has fired between the RMC's emission and our parse,
// so the latest edge belongs to the *next* second, not the one this
// RMC describes; a too-large sincePps means the pairing is unreliable.
constexpr uint32_t SYNC_PPS_LATENCY_MIN_US = 100000UL;  // RMC arrives ≥ 100 ms after its PPS
constexpr uint32_t SYNC_PPS_LATENCY_MAX_US = 800000UL;  // ...and ≤ 800 ms
constexpr uint32_t SYNC_RMC_MAX_AGE_MS     = 200UL;     // act only on freshly parsed RMC
constexpr uint32_t SYNC_HOLDOVER_US        = 5000000UL; // 5 s of holdover before falling back
constexpr uint32_t SYNC_FIX_MAX_AGE_MS     = 3000UL;    // location data must be < 3 s old

constexpr uint32_t    WIFI_PORTAL_TIMEOUT_S = 300;
constexpr const char* WIFI_AP_NAME          = "vu2cpl-esp8266-ntp-setup";
constexpr const char* WIFI_AP_PASSWORD      = "vu2cpl1234";
constexpr const char* WIFI_HOSTNAME         = "esp8266-ntp";

constexpr const char* MQTT_BROKER             = "192.168.1.169";
constexpr uint16_t    MQTT_PORT               = 1883;
constexpr const char* MQTT_CLIENT_ID          = "esp8266-ntp";
constexpr const char* MQTT_TOPIC_STATUS       = "shack/esp8266-ntp/status";
constexpr const char* MQTT_LWT_OFFLINE        = "{\"event\":\"offline\"}";
constexpr uint16_t    MQTT_KEEPALIVE_S        = 60;
constexpr uint32_t    MQTT_PUBLISH_INTERVAL_MS  = 30000;
constexpr uint32_t    MQTT_RECONNECT_BACKOFF_MS = 5000;
constexpr uint8_t     MQTT_SOCKET_TIMEOUT_S   = 2;  // keep loop() responsive when broker is down

SoftwareSerial gpsSerial(GPS_RX_PIN, -1);
TinyGPSPlus    gps;
WiFiUDP        ntpUdp;
WiFiClient     mqttWifiClient;
PubSubClient   mqttClient(mqttWifiClient);

uint32_t lastMqttPublishMs        = 0;
uint32_t lastMqttConnectAttemptMs = 0;

volatile uint32_t ppsCount          = 0;
volatile uint32_t ppsLastEdgeMicros = 0;
volatile uint32_t ppsPrevEdgeMicros = 0;

struct TimeSync {
  uint32_t unixSecondsAtPps;  // Unix epoch seconds at the synced PPS edge
  uint32_t microsAtPps;       // micros() value when that edge fired
  uint32_t ppsCountAtSync;    // ppsCount we used (so we don't re-sync the same edge)
  bool     valid;
};
TimeSync timeSync = {0, 0, 0, false};

uint32_t ntpRequestsHandled = 0;

void IRAM_ATTR onPpsEdge() {
  uint32_t t = micros();
  ppsPrevEdgeMicros = ppsLastEdgeMicros;
  ppsLastEdgeMicros = t;
  ppsCount++;
}

static inline bool isLeapYear(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// (Y, M, D, h, m, s) UTC → seconds since Unix epoch. Valid for 1970+.
static uint32_t computeUnixSeconds(uint16_t year, uint8_t month, uint8_t day,
                                   uint8_t hour, uint8_t minute, uint8_t second) {
  static const uint16_t daysBeforeMonth[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
  };
  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++) {
    days += isLeapYear(y) ? 366UL : 365UL;
  }
  days += daysBeforeMonth[month - 1];
  if (month > 2 && isLeapYear(year)) days += 1;
  days += day - 1;
  return days * 86400UL + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + second;
}

// True if our (unix, micros) anchor is recent enough to serve real time.
static bool syncIsCurrent() {
  if (!timeSync.valid) return false;
  uint32_t sincePps = micros() - timeSync.microsAtPps;
  return sincePps < SYNC_HOLDOVER_US;
}

// Current best estimate of wall-clock Unix seconds. Returns 0 when not
// synced (used by MQTT to omit a meaningful ts — clients will see 0
// and ignore, mirroring how chrony handles unsynchronised state).
static uint32_t currentUnixSeconds() {
  if (!syncIsCurrent()) return 0;
  uint32_t delta = micros() - timeSync.microsAtPps;
  return timeSync.unixSecondsAtPps + (delta / 1000000UL);
}

// Look for a fresh (PPS edge, RMC date+time) pair and update timeSync.
// Called from loop() after gps.encode(). Idempotent: returns quickly
// when nothing new has arrived.
static void maybeUpdateTimeSync() {
  if (!gps.location.isValid() || gps.location.age() > SYNC_FIX_MAX_AGE_MS) return;
  if (!gps.date.isValid() || !gps.time.isValid()) return;

  // RMC must have been parsed in the last SYNC_RMC_MAX_AGE_MS — otherwise
  // gps.time still holds the *previous* second's value and we'd
  // associate it with a newer PPS edge (1-second offset).
  if (gps.time.age() > SYNC_RMC_MAX_AGE_MS) return;
  if (gps.date.age() > SYNC_RMC_MAX_AGE_MS) return;

  noInterrupts();
  uint32_t edgeCount  = ppsCount;
  uint32_t edgeMicros = ppsLastEdgeMicros;
  interrupts();

  if (edgeCount == 0) return;
  if (edgeCount == timeSync.ppsCountAtSync) return;  // already synced this edge

  // Belt-and-braces second check: the PPS edge we're pairing must be
  // in the expected post-PPS-pre-RMC window. Too-small → next-second's
  // PPS already fired before we parsed this RMC; too-large → RMC is
  // unusually late and we can't safely pair.
  uint32_t sincePps = micros() - edgeMicros;
  if (sincePps < SYNC_PPS_LATENCY_MIN_US) return;
  if (sincePps > SYNC_PPS_LATENCY_MAX_US) return;

  uint32_t unixSec = computeUnixSeconds(
    gps.date.year(), gps.date.month(), gps.date.day(),
    gps.time.hour(), gps.time.minute(), gps.time.second());

  bool wasValid = timeSync.valid;
  timeSync.unixSecondsAtPps = unixSec;
  timeSync.microsAtPps      = edgeMicros;
  timeSync.ppsCountAtSync   = edgeCount;
  timeSync.valid            = true;

  if (!wasValid) {
    Serial.printf_P(PSTR("[ntp] sync acquired: unix=%lu pps=%lu\n"),
                    (unsigned long)unixSec, (unsigned long)edgeCount);
  }
}

// Pack a 64-bit NTP timestamp (seconds + fraction) corresponding to
// micros() value `recordedMicros` into `buf`. When synced: real Unix
// time mapped onto NTP epoch. When not synced: best-effort placeholder
// (ms-since-boot mapped onto NTP epoch 0). Callers pair the latter
// with stratum=16/refid=INIT so clients ignore the values.
static void writeNtpTimestamp(uint8_t *buf, uint32_t recordedMicros) {
  uint32_t ntpSec, frac;
  if (syncIsCurrent()) {
    uint32_t delta    = recordedMicros - timeSync.microsAtPps;
    uint32_t deltaSec = delta / 1000000UL;
    uint32_t deltaUs  = delta % 1000000UL;
    uint32_t unixSec  = timeSync.unixSecondsAtPps + deltaSec;
    ntpSec = unixSec + NTP_UNIX_OFFSET;
    frac   = (uint32_t)(((uint64_t)deltaUs << 32) / 1000000ULL);
  } else {
    uint32_t ms     = recordedMicros / 1000UL;
    ntpSec          = ms / 1000UL;
    uint32_t msPart = ms % 1000UL;
    frac            = (uint32_t)((uint64_t)msPart * 4294967296ULL / 1000ULL);
  }
  buf[0] = (ntpSec >> 24) & 0xff;
  buf[1] = (ntpSec >> 16) & 0xff;
  buf[2] = (ntpSec >>  8) & 0xff;
  buf[3] =  ntpSec        & 0xff;
  buf[4] = (frac   >> 24) & 0xff;
  buf[5] = (frac   >> 16) & 0xff;
  buf[6] = (frac   >>  8) & 0xff;
  buf[7] =  frac          & 0xff;
}

static void handleNtpRequest() {
  int packetSize = ntpUdp.parsePacket();
  if (packetSize <= 0) return;

  uint32_t recvMicros  = micros();
  IPAddress clientIp   = ntpUdp.remoteIP();
  uint16_t  clientPort = ntpUdp.remotePort();

  uint8_t req[NTP_PACKET_SIZE] = { 0 };
  int toRead = packetSize < (int)NTP_PACKET_SIZE ? packetSize : (int)NTP_PACKET_SIZE;
  ntpUdp.read(req, toRead);
  while (ntpUdp.available()) {
    uint8_t scratch[32];
    ntpUdp.read(scratch, sizeof(scratch));
  }

  if (packetSize < (int)NTP_PACKET_SIZE) {
    Serial.printf_P(PSTR("[ntp] runt from %s:%u (size=%d) -- dropping\n"),
                    clientIp.toString().c_str(), clientPort, packetSize);
    return;
  }

  bool synced = syncIsCurrent();
  uint8_t reqVn = (req[0] >> 3) & 0x07;
  if (reqVn < 1 || reqVn > 4) reqVn = 4;

  uint8_t resp[NTP_PACKET_SIZE] = { 0 };
  resp[0] = ((synced ? 0 : 3) << 6) | (reqVn << 3) | 4;  // LI, VN, Mode=4 server
  resp[1] = synced ? 1 : 16;                              // Stratum
  resp[2] = 4;                                            // Poll = 2^4 = 16 s
  resp[3] = synced ? (uint8_t)(int8_t)(-20)               // ~1 us local precision
                   : (uint8_t)(int8_t)(-10);              // ~1 ms placeholder
  // resp[4..7] root delay     = 0 (we are the source)
  // resp[8..11] root dispersion: ~1 ms when synced, 0 otherwise
  if (synced) resp[11] = 0x42;  // 0x42 / 0x10000 ≈ 1 ms in 16.16 fixed-point

  if (synced) {
    resp[12] = 'G'; resp[13] = 'P'; resp[14] = 'S'; resp[15] = ' ';
  } else {
    resp[12] = 'I'; resp[13] = 'N'; resp[14] = 'I'; resp[15] = 'T';
  }

  // Reference timestamp = when we last synced (or zero when unsynced).
  if (synced) {
    writeNtpTimestamp(&resp[16], timeSync.microsAtPps);
  }

  // Originate timestamp = client's transmit field, echoed verbatim.
  memcpy(&resp[24], &req[40], 8);

  // Receive timestamp = when we got the request.
  writeNtpTimestamp(&resp[32], recvMicros);

  // Transmit timestamp — last write before send so the math is honest.
  uint32_t txMicros = micros();
  writeNtpTimestamp(&resp[40], txMicros);

  ntpUdp.beginPacket(clientIp, clientPort);
  ntpUdp.write(resp, NTP_PACKET_SIZE);
  ntpUdp.endPacket();

  ntpRequestsHandled++;
  Serial.printf_P(PSTR("[ntp] req %lu from %s:%u -> stratum=%u refid=%s\n"),
                  (unsigned long)ntpRequestsHandled,
                  clientIp.toString().c_str(), clientPort,
                  synced ? 1 : 16,
                  synced ? "GPS" : "INIT");
}

// --- MQTT status publisher --------------------------------------------------

// Attempt one connection to the shack broker. Returns true on success.
// We connect with LWT so the broker holds an "offline" marker for us
// whenever we drop unexpectedly; the next status publish on
// (re)connect overwrites it.
static bool mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.printf_P(PSTR("[mqtt] connecting to %s:%u as %s\n"),
                  MQTT_BROKER, MQTT_PORT, MQTT_CLIENT_ID);

  bool ok = mqttClient.connect(
    MQTT_CLIENT_ID,
    nullptr, nullptr,         // no auth on the shack broker
    MQTT_TOPIC_STATUS,        // will topic = our status topic
    0,                         // will QoS
    true,                      // will retain
    MQTT_LWT_OFFLINE          // will message
  );

  if (ok) {
    Serial.println(F("[mqtt] connected"));
  } else {
    Serial.printf_P(PSTR("[mqtt] connect failed, state=%d\n"), mqttClient.state());
  }
  return ok;
}

// Build the status JSON and publish it retained. Field names mirror
// the Pi NTP server's `shack/gpsntp/chrony` topic where they apply, so
// Node-RED can render both servers from one schema.
static void mqttPublishStatus() {
  if (!mqttClient.connected()) return;

  bool synced = syncIsCurrent();
  uint32_t ts = currentUnixSeconds();  // 0 when unsynced

  noInterrupts();
  uint32_t ppsNow      = ppsCount;
  uint32_t ppsEdge     = ppsLastEdgeMicros;
  uint32_t ppsPrevEdge = ppsPrevEdgeMicros;
  interrupts();

  int32_t intervalUs = (int32_t)(ppsEdge - ppsPrevEdge);
  bool haveFix = gps.location.isValid() &&
                 gps.location.age() < SYNC_FIX_MAX_AGE_MS;

  char buf[480];
  int n = snprintf(buf, sizeof(buf),
    "{"
    "\"host\":\"%s\","
    "\"ts\":%lu,"
    "\"stratum\":%u,"
    "\"ref_id\":\"%s\","
    "\"leap\":%u,"
    "\"root_delay_s\":0,"
    "\"root_dispersion_s\":0.001,"
    "\"fix_mode\":\"%s\","
    "\"sat_used\":%u,"
    "\"pps_count\":%lu,"
    "\"pps_interval_us\":%ld,"
    "\"pps_sync\":%s,"
    "\"ntp_requests\":%lu,"
    "\"rssi_dbm\":%d,"
    "\"uptime_s\":%lu,"
    "\"free_heap\":%u"
    "}",
    WIFI_HOSTNAME,
    (unsigned long)ts,
    synced ? 1u : 16u,
    synced ? "GPS" : "INIT",
    synced ? 0u : 3u,
    haveFix ? "3D" : "none",
    gps.satellites.value(),
    (unsigned long)ppsNow,
    (long)intervalUs,
    synced ? "true" : "false",
    (unsigned long)ntpRequestsHandled,
    WiFi.RSSI(),
    (unsigned long)(millis() / 1000UL),
    ESP.getFreeHeap()
  );

  if (n < 0 || n >= (int)sizeof(buf)) {
    Serial.println(F("[mqtt] status JSON would overflow buffer, skipping publish"));
    return;
  }

  bool ok = mqttClient.publish(MQTT_TOPIC_STATUS, buf, true);  // retain=true
  if (!ok) {
    Serial.printf_P(PSTR("[mqtt] publish failed, state=%d (msg %d bytes)\n"),
                    mqttClient.state(), n);
  }
}

// Called from loop(). Handles (re)connect throttling and periodic publish.
static void mqttServiceLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();

  if (!mqttClient.connected()) {
    if (now - lastMqttConnectAttemptMs < MQTT_RECONNECT_BACKOFF_MS) return;
    lastMqttConnectAttemptMs = now;
    if (mqttConnect()) {
      // Publish immediately on (re)connect so we overwrite any retained
      // LWT and the dashboard sees us back online without waiting for
      // the next interval.
      mqttPublishStatus();
      lastMqttPublishMs = now;
    }
    return;
  }

  mqttClient.loop();

  if (now - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS) {
    mqttPublishStatus();
    lastMqttPublishMs = now;
  }
}

// --- Wi-Fi onboarding -------------------------------------------------------

static void connectWifi() {
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(WIFI_HOSTNAME);

  WiFiManager wm;
  wm.setHostname(WIFI_HOSTNAME);
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);

  Serial.printf_P(PSTR("[wifi] autoConnect -- portal AP if no saved creds: %s / %s (timeout %lus)\n"),
                  WIFI_AP_NAME, WIFI_AP_PASSWORD,
                  (unsigned long)WIFI_PORTAL_TIMEOUT_S);

  if (!wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASSWORD)) {
    Serial.println(F("[wifi] portal timed out -- restarting"));
    delay(1000);
    ESP.restart();
  }

  Serial.printf_P(PSTR("[wifi] connected ssid=%s ip=%s rssi=%d host=%s\n"),
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  WIFI_HOSTNAME);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("[boot] ESP8266 GPS NTP -- milestone 3 (PPS-disciplined NTP)"));
  Serial.printf_P(PSTR("[boot] GPS RX on D2 (GPIO4) @ %u baud\n"), GPS_BAUD);
  Serial.println(F("[boot] PPS on D5 (GPIO14), RISING edge (direct on L89 pin 6)"));
  Serial.println(F("[boot] Stratum 1 / refid GPS when synced; stratum 16 / INIT otherwise."));

  gpsSerial.begin(GPS_BAUD);

  pinMode(PPS_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), onPpsEdge, RISING);

  connectWifi();

  ntpUdp.begin(NTP_PORT);
  Serial.printf_P(PSTR("[ntp] listening on udp/%u\n"), NTP_PORT);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_S);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
  Serial.printf_P(PSTR("[mqtt] will publish %s every %lus\n"),
                  MQTT_TOPIC_STATUS,
                  (unsigned long)(MQTT_PUBLISH_INTERVAL_MS / 1000UL));
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  maybeUpdateTimeSync();

  handleNtpRequest();

  mqttServiceLoop();

  static uint32_t lastLog          = 0;
  static uint32_t lastReportedPps  = 0;
  static uint32_t windowStartMs    = 0;
  static uint32_t windowStartPps   = 0;
  static bool     haveLoggedSync   = false;
  static bool     lastSyncState    = false;

  uint32_t now = millis();
  if (now - lastLog < LOG_INTERVAL_MS) return;
  lastLog = now;

  noInterrupts();
  uint32_t ppsNow      = ppsCount;
  uint32_t ppsEdge     = ppsLastEdgeMicros;
  uint32_t ppsPrevEdge = ppsPrevEdgeMicros;
  interrupts();

  uint32_t deltaPps   = ppsNow - lastReportedPps;
  int32_t  intervalUs = (int32_t)(ppsEdge - ppsPrevEdge);
  lastReportedPps = ppsNow;

  bool synced = syncIsCurrent();
  if (haveLoggedSync && synced != lastSyncState && !synced) {
    uint32_t sinceMs = (micros() - timeSync.microsAtPps) / 1000UL;
    Serial.printf_P(PSTR("[ntp] sync lost (last PPS+RMC %lums ago)\n"),
                    (unsigned long)sinceMs);
  }
  lastSyncState  = synced;
  haveLoggedSync = true;

  Serial.printf_P(PSTR("[gps] t=%lus pps=%lu (+%lu) interval=%ldus lvl=%d sync=%s "),
                  now / 1000, (unsigned long)ppsNow, (unsigned long)deltaPps,
                  intervalUs, digitalRead(PPS_PIN),
                  synced ? "Y" : "N");

  if (gps.date.isValid() && gps.time.isValid()) {
    Serial.printf_P(PSTR("UTC %04u-%02u-%02u %02u:%02u:%02u "),
                    gps.date.year(), gps.date.month(), gps.date.day(),
                    gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
    Serial.print(F("UTC ---------- "));
  }

  if (gps.location.isValid()) {
    Serial.printf_P(PSTR("fix=Y sats=%u hdop="), gps.satellites.value());
    Serial.print(gps.hdop.hdop(), 1);
  } else {
    Serial.printf_P(PSTR("fix=N sats=%u"), gps.satellites.value());
  }
  Serial.println();

  if (windowStartMs == 0) {
    windowStartMs  = now;
    windowStartPps = ppsNow;
  } else if (now - windowStartMs >= WINDOW_LENGTH_MS) {
    uint32_t windowPps = ppsNow - windowStartPps;
    Serial.printf_P(PSTR("[gps] window 60s: %lu PPS [%s]\n"),
                    (unsigned long)windowPps,
                    windowPps == 60 ? "PASS" : "CHECK");
    windowStartMs  = now;
    windowStartPps = ppsNow;
  }
}
