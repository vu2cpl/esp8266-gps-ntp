// Milestone 2 — NMEA + PPS counter, Wi-Fi onboarding, placeholder NTP responder.
//
// Builds on milestone 1 by adding:
//   - WiFiManager captive-portal onboarding (no compile-time secrets).
//     First boot raises AP "vu2cpl-esp8266-ntp-setup" / password
//     "vu2cpl1234"; join from a phone, pick the shack SSID, save.
//   - UDP/123 listener that replies to NTPv3/v4 requests with a well-
//     formed 48-byte packet (RFC 5905 figure 8).
//   - Stratum 16 + reference ID "INIT" on every reply — the standard
//     "not yet synchronised" advert, so clients receive our packet but
//     correctly refuse to use it as a time source. Wiring this up to
//     the GPS PPS / NMEA time path is milestone 3.
//
// Wiring (unchanged from milestone 1):
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

constexpr uint32_t    WIFI_PORTAL_TIMEOUT_S = 300;  // 5 minutes
constexpr const char* WIFI_AP_NAME          = "vu2cpl-esp8266-ntp-setup";
constexpr const char* WIFI_AP_PASSWORD      = "vu2cpl1234";
constexpr const char* WIFI_HOSTNAME         = "esp8266-ntp";

SoftwareSerial gpsSerial(GPS_RX_PIN, -1);
TinyGPSPlus    gps;
WiFiUDP        ntpUdp;

volatile uint32_t ppsCount          = 0;
volatile uint32_t ppsLastEdgeMicros = 0;
volatile uint32_t ppsPrevEdgeMicros = 0;

uint32_t ntpRequestsHandled = 0;

void IRAM_ATTR onPpsEdge() {
  uint32_t t = micros();
  ppsPrevEdgeMicros = ppsLastEdgeMicros;
  ppsLastEdgeMicros = t;
  ppsCount++;
}

// Write a 64-bit NTP timestamp (big-endian: seconds, then fraction) into
// `buf`. For milestone 2 we use "ms since boot" mapped onto NTP epoch 0 —
// clearly the wrong wall clock, but a well-formed timestamp. Milestone 3
// replaces this with PPS-disciplined GPS time.
static void writeNtpTimestampFromMillis(uint8_t *buf, uint32_t ms) {
  uint32_t seconds = ms / 1000;
  uint32_t msPart  = ms % 1000;
  uint32_t frac    = (uint32_t)((uint64_t)msPart * 4294967296ULL / 1000ULL);
  buf[0] = (seconds >> 24) & 0xff;
  buf[1] = (seconds >> 16) & 0xff;
  buf[2] = (seconds >>  8) & 0xff;
  buf[3] =  seconds        & 0xff;
  buf[4] = (frac    >> 24) & 0xff;
  buf[5] = (frac    >> 16) & 0xff;
  buf[6] = (frac    >>  8) & 0xff;
  buf[7] =  frac           & 0xff;
}

static void handleNtpRequest() {
  int packetSize = ntpUdp.parsePacket();
  if (packetSize <= 0) return;

  uint32_t recvMs = millis();
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
    Serial.printf_P(PSTR("[ntp] runt from %s:%u (size=%d) — dropping\n"),
                    clientIp.toString().c_str(), clientPort, packetSize);
    return;
  }

  // Echo the client's protocol version so NTPv3 and NTPv4 both work.
  uint8_t reqVn = (req[0] >> 3) & 0x07;
  if (reqVn < 1 || reqVn > 4) reqVn = 4;

  uint8_t resp[NTP_PACKET_SIZE] = { 0 };

  resp[0]  = (3 << 6) | (reqVn << 3) | 4;  // LI=3 unsync, VN=client, Mode=4 server
  resp[1]  = 16;                            // Stratum 16 = unsynchronised
  resp[2]  = 4;                             // Poll = 2^4 = 16 s
  resp[3]  = (uint8_t)(int8_t)(-10);        // Precision ≈ 2^-10 s ≈ 1 ms
  // resp[4..7]  root delay        = 0
  // resp[8..11] root dispersion   = 0
  resp[12] = 'I';                           // Reference ID "INIT" — not yet synced
  resp[13] = 'N';
  resp[14] = 'I';
  resp[15] = 'T';
  // resp[16..23] reference timestamp = 0 (never synced)

  // Originate timestamp = copy of the client's transmit timestamp.
  memcpy(&resp[24], &req[40], 8);

  // Receive timestamp = when we got the packet.
  writeNtpTimestampFromMillis(&resp[32], recvMs);

  // Transmit timestamp = right before send. Last write for best round-trip
  // accuracy (it's still userspace lwIP — not great — but stamp late anyway).
  writeNtpTimestampFromMillis(&resp[40], millis());

  ntpUdp.beginPacket(clientIp, clientPort);
  ntpUdp.write(resp, NTP_PACKET_SIZE);
  ntpUdp.endPacket();

  ntpRequestsHandled++;
  Serial.printf_P(PSTR("[ntp] req %lu from %s:%u (size=%d) → stratum=16 INIT\n"),
                  ntpRequestsHandled, clientIp.toString().c_str(),
                  clientPort, packetSize);
}

static void connectWifi() {
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(WIFI_HOSTNAME);

  WiFiManager wm;
  wm.setHostname(WIFI_HOSTNAME);
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);

  Serial.printf_P(PSTR("[wifi] autoConnect — portal AP if no saved creds: %s / %s (timeout %lus)\n"),
                  WIFI_AP_NAME, WIFI_AP_PASSWORD,
                  (unsigned long)WIFI_PORTAL_TIMEOUT_S);

  if (!wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASSWORD)) {
    Serial.println(F("[wifi] portal timed out — restarting"));
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
  Serial.println(F("[boot] ESP8266 GPS NTP — milestone 2 (Wi-Fi + placeholder NTP)"));
  Serial.printf_P(PSTR("[boot] GPS RX on D2 (GPIO4) @ %u baud\n"), GPS_BAUD);
  Serial.println(F("[boot] PPS on D5 (GPIO14), RISING edge (direct on L89 pin 6)"));
  Serial.println(F("[boot] NTP replies will be stratum=16 / refid=INIT until milestone 3."));

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

  handleNtpRequest();

  static uint32_t lastLog         = 0;
  static uint32_t lastReportedPps = 0;
  static uint32_t windowStartMs   = 0;
  static uint32_t windowStartPps  = 0;

  uint32_t now = millis();
  if (now - lastLog < LOG_INTERVAL_MS) return;
  lastLog = now;

  noInterrupts();
  uint32_t ppsNow      = ppsCount;
  uint32_t ppsEdge     = ppsLastEdgeMicros;
  uint32_t ppsPrevEdge = ppsPrevEdgeMicros;
  interrupts();

  uint32_t deltaPps  = ppsNow - lastReportedPps;
  int32_t  intervalUs = (int32_t)(ppsEdge - ppsPrevEdge);
  lastReportedPps = ppsNow;

  Serial.printf_P(PSTR("[gps] t=%lus pps=%lu (+%lu) interval=%ldus lvl=%d "),
                  now / 1000, ppsNow, deltaPps, intervalUs,
                  digitalRead(PPS_PIN));

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
                    windowPps, windowPps == 60 ? "PASS" : "CHECK");
    windowStartMs  = now;
    windowStartPps = ppsNow;
  }
}
