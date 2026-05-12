// Milestone 3 — PPS-disciplined NTP server.
//
// Combines milestones 1 and 2:
//   - NMEA via SoftwareSerial on D2, PPS via interrupt on D5 (M1).
//   - Wi-Fi via WiFiManager + UDP/123 listener (M2).
//
// New in M3: the NTP responder uses real PPS-disciplined GPS time.
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
constexpr uint32_t SYNC_PPS_WINDOW_US   = 900000UL;   // RMC must arrive < 900 ms after its PPS
constexpr uint32_t SYNC_HOLDOVER_US     = 5000000UL;  // 5 s of holdover before falling back
constexpr uint32_t SYNC_FIX_MAX_AGE_MS  = 3000UL;     // location data must be < 3 s old

constexpr uint32_t    WIFI_PORTAL_TIMEOUT_S = 300;
constexpr const char* WIFI_AP_NAME          = "vu2cpl-esp8266-ntp-setup";
constexpr const char* WIFI_AP_PASSWORD      = "vu2cpl1234";
constexpr const char* WIFI_HOSTNAME         = "esp8266-ntp";

SoftwareSerial gpsSerial(GPS_RX_PIN, -1);
TinyGPSPlus    gps;
WiFiUDP        ntpUdp;

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

// Look for a fresh (PPS edge, RMC date+time) pair and update timeSync.
// Called from loop() after gps.encode(). Idempotent: returns quickly
// when nothing new has arrived.
static void maybeUpdateTimeSync() {
  if (!gps.location.isValid() || gps.location.age() > SYNC_FIX_MAX_AGE_MS) return;
  if (!gps.date.isValid() || !gps.time.isValid()) return;

  noInterrupts();
  uint32_t edgeCount  = ppsCount;
  uint32_t edgeMicros = ppsLastEdgeMicros;
  interrupts();

  if (edgeCount == 0) return;
  if (edgeCount == timeSync.ppsCountAtSync) return;  // already synced this edge

  uint32_t sincePps = micros() - edgeMicros;
  if (sincePps > SYNC_PPS_WINDOW_US) return;  // PPS too old to safely pair with current RMC

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
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  maybeUpdateTimeSync();

  handleNtpRequest();

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
