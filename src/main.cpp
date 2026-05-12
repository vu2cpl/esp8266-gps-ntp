// Milestone 1 — NMEA + PPS counter (no Wi-Fi, no NTP yet).
//
// Goal: prove the wiring and timing path before adding network code.
//   - SoftwareSerial on D2 reads NMEA from the L89.
//   - PPS interrupt on D5 increments a counter; once the L89 has a 3D
//     fix, expect exactly 60 interrupts per 60 s window.
//   - Every second, print PPS count, edge-to-edge interval, pin level,
//     and the latest parsed UTC / fix state.
//
// Wiring (NodeMCU label → ESP8266 GPIO):
//   D2 (GPIO4)  <- L89 TX    (SoftwareSerial RX, 9600 baud — *not* the
//                             NodeMCU RX header pin, which is GPIO3 /
//                             hardware UART RX and shares the USB-serial)
//   D5 (GPIO14) <- L89 PPS   (direct on Quectel L89 pin 6, native
//                             active-high → RISING edge marks on-time)
//   3V3, GND    <- L89 power and common ground

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <TinyGPS++.h>

constexpr uint8_t  GPS_RX_PIN        = D2;
constexpr uint8_t  PPS_PIN           = D5;
constexpr uint32_t GPS_BAUD          = 9600;
constexpr uint32_t LOG_INTERVAL_MS   = 1000;
constexpr uint32_t WINDOW_LENGTH_MS  = 60000;

SoftwareSerial gpsSerial(GPS_RX_PIN, -1);  // RX only; we don't talk to the GPS yet.
TinyGPSPlus    gps;

volatile uint32_t ppsCount          = 0;
volatile uint32_t ppsLastEdgeMicros = 0;
volatile uint32_t ppsPrevEdgeMicros = 0;

void IRAM_ATTR onPpsEdge() {
  uint32_t t = micros();
  ppsPrevEdgeMicros = ppsLastEdgeMicros;
  ppsLastEdgeMicros = t;
  ppsCount++;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("[boot] ESP8266 GPS NTP — milestone 1 (NMEA + PPS counter)"));
  Serial.printf_P(PSTR("[boot] GPS RX on D2 (GPIO4) @ %u baud\n"), GPS_BAUD);
  Serial.println(F("[boot] PPS on D5 (GPIO14), RISING edge (direct on L89 pin 6)"));
  Serial.println(F("[boot] Once the L89 has a 3D fix, expect 60 PPS per 60 s window."));

  gpsSerial.begin(GPS_BAUD);

  pinMode(PPS_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PPS_PIN), onPpsEdge, RISING);
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

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
