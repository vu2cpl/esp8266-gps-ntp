# ESP8266 GPS NTP — Handover

## Status

**Not started.** This document is the design brief / handover written
*before* any code is committed, so the next session (or another
contributor) can start from a clear shared understanding instead of
re-deriving the design.

## Why this project exists

The VU2CPL shack already runs a stratum-1 GPS-disciplined NTP server
on a Raspberry Pi 3B with a u-blox NEO-M8N — sub-µs locally, 1–4 ms
over the LAN. That solves the time-server *problem* completely. See
the sibling repo **`pi-gps-ntp-server`** at
<https://github.com/vu2cpl/pi-gps-ntp-server>.

This project is a deliberate detour. The goal is to build a
microcontroller-class NTP server to **learn the embedded-firmware
side** of GPS-disciplined timekeeping: PPS interrupts, NMEA parsing,
NTP packet format, holdover, leap-second handling, Wi-Fi UDP timing
jitter, the lot. The output will be useful enough for casual LAN
clients (ham logging timestamps, shack PCs) but will not match the
Pi setup on any axis that matters for production. That's fine — it's
not trying to.

**Explicitly not a production replacement.** The Pi stays in service.

## Who the user is

- Manoj (VU2CPL), ham radio operator, Mac-based shack.
- Already runs `pi-gps-ntp-server` (operational since 2026-05-09;
  swapped from QLG1 to dedicated NEO-M8N on 2026-05-11).
- Comfortable with Linux, chrony, gpsd. New to ESP8266 firmware.
- Other shack projects: SkimServer Mac, DXCluster Aggregator,
  LP-700 power-meter app, Node-RED RPi Fleet dashboard.

## Historical context: the rejected ESP32 path

The Pi project's `HANDOVER.md` "Decision #3" rejected an ESP32-based
NTP server in favour of the Pi. The reasons were sound and still apply:

- chrony + gpsd + kernel PPS is what real public stratum-1 servers
  run; hobby firmware re-implements leap seconds, holdover, multi-source
  comparison, and stratum honesty — all already correct in chrony.
- Linux timestamps NTP packets at NIC level; ESP userspace timestamps
  in lwIP. Client-visible accuracy ends up worse.
- Wi-Fi adds 100s of µs to ms of jitter; the Pi has wired Ethernet.
- Debug: SSH + `chronyc` vs serial console + logic analyzer.

The empty `~/projects/ESP32 GPS NTP/` directory is the ghost of that
abandoned attempt. **This project is not re-litigating that decision** —
it accepts all of it. The goal is learning, not better timekeeping.

## Hardware

| Item | Detail |
|------|--------|
| NodeMCU dev board (ESP8266MOD) | Tensilica L106 80 MHz, ~80 KB usable RAM, Wi-Fi only, USB-serial debug |
| 7Semi L89 GPS breakout | Quectel L89 IRNSS-capable multi-GNSS, 3.3 V native |
| 3.3 V-only signal levels | Both ends are 3.3 V CMOS — no level shifting needed |

## PPS verification — RESOLVED 2026-05-12

**Outcome: PPS is accessible, tap point identified, polarity inverted.**

Findings:

- The 7Semi L89 breakout carries a blue indicator LED that blinks at
  1 Hz once the module has a 3D fix — confirmed PPS is alive on the
  PCB.
- Tracing the LED: it is driven by an NPN transistor with a current-
  limit resistor returning to the +3.3 V rail. The accessible tap
  point is on the **collector** side of that transistor, not the
  base. Tapping at the base would be cleaner but the base trace has
  no exposed pad.
- This means the signal at the tap point is **inverted** vs. the
  chip's native PPS:
  - Native L89 PPS (per Quectel L89 Hardware Design rev 1.1):
    active-high, idles 0 V, pulses to +3.3 V for 500 ms each second,
    rising edge = on-time second boundary.
  - At our tap point: idles +3.3 V, pulses to ~0 V for 500 ms,
    **falling edge = on-time second boundary**.
- Multimeter readings were inconclusive (50% duty cycle averages to
  ~1.65 V regardless of polarity). The polarity is established from
  the circuit topology, which is the reliable signal here.
- Added switching delay through the transistor is sub-µs — negligible
  for this project's ms-class accuracy ceiling. The transistor also
  buffers the GPS module's drive pin from the ESP, which is a small
  bonus.

**Implication for firmware:** `attachInterrupt(D5, isr, FALLING)`,
not `RISING`. Documented at point of use in the sketch.

For background (original verification plan, kept because the
reasoning is still useful):

- Quectel L89 silicon: 1 PPS on module pin 6, 3.9 ns RMS accuracy,
  3.3 V CMOS, rising-edge, 500 ms pulse width.
- The 7Semi product page and Arduino library do not document PPS
  exposure — finding it required PCB inspection.
- Fallback if it had turned out unreachable: swap to a GY-NEO8MV2
  (u-blox NEO-M8N, same module the sibling Pi project uses) which
  has a labelled PPS pin. Not needed.

## Wiring sketch (assuming PPS is accessible)

| L89 pin | NodeMCU pin | Notes |
|---------|-------------|-------|
| VCC (3.3 V) | 3V3 | Power |
| GND | GND | Common ground |
| TX | D2 (GPIO4) via SoftwareSerial | Leaves USB serial free for debug |
| PPS | D5 (GPIO14) — `attachInterrupt`-capable | The whole game lives here. Inverted at our tap point → use `FALLING` edge. |

UART jitter does not matter for accuracy. PPS edge is the timing
reference; NMEA only labels which integer second the edge belongs to.
SoftwareSerial at 9600 baud is fine and keeps the hardware UART
available for `Serial.print` debug.

Avoid D0 (GPIO16 — no interrupt), D3 (GPIO0 — boot strap), D4 (GPIO2
— boot strap, built-in LED), and D8 (GPIO15 — boot strap pulldown)
for the GPS connections. D5/D6/D7 are all interrupt-capable and free.

## Software architecture

Standard microcontroller NTP server pattern:

```
ISR on PPS edge   : record micros() at edge  → epoch_pps_micros
NMEA parser loop  : consume $GPRMC, extract  → epoch_pps_unix
UDP/123 responder : on NTP request, compute
                    now = epoch_pps_unix
                        + (micros() - epoch_pps_micros) * 1e-6
                    build a 48-byte NTP packet, reply
Stratum / leap    : claim stratum 1 with leap bits from NMEA when in
                    sync; fall back to stratum 16 (unsynced) when no fix
```

Tool choice: **PlatformIO + Arduino framework on ESP8266**.

Libraries:

- `TinyGPSPlus` — NMEA parsing
- `ESP8266WiFi` + `WiFiUDP` — networking (ships with the ESP8266
  Arduino core)
- **No NTP library.** Hand-roll the 48-byte packet from RFC 5905
  Figure 8 — simpler than learning someone else's wrapper.

## Shack conventions (from sibling repos)

Reference projects in `~/projects/`:

- `Pi GPS NTP Server/` — production NTP. Publishes chrony metrics
  via cron every 60 s to retained topic `shack/gpsntp/chrony`.
  Hostname `gpsntp.local`, IP `192.168.1.158`, Ethernet-wired.
- `vu2cpl-as3935-bridge/` — VU2CPL lightning detector. ESP32 +
  PlatformIO, single monolithic `src/main.cpp`, WiFiManager captive
  portal, PubSubClient v2.8 against the shack broker. Closest
  stylistic precedent for *this* firmware.

Conventions to inherit when milestone 2 (Wi-Fi + MQTT) lands:

- **Wi-Fi credentials.** WiFiManager captive portal, *not* a
  compile-time `secrets.h`. Setup AP `vu2cpl-esp8266-ntp-setup`,
  portal password `vu2cpl1234` (mirrors AS3935 bridge). Factory
  reset via long-press of the FLASH/BOOT button at boot.
- **MQTT broker.** `192.168.1.169:1883`, no auth.
- **MQTT library.** `knolleary/PubSubClient@^2.8`.
- **MQTT topics.** `shack/esp8266-ntp/` prefix, parallel to Pi's
  `shack/gpsntp/`. Initial layout:
  - `shack/esp8266-ntp/status` — retained, periodic state JSON.
  - `shack/esp8266-ntp/hb` — retained heartbeat.
  - `shack/esp8266-ntp/cmd` and `.../cmd/ack` — control plane.
  - LWT: `shack/esp8266-ntp/status` with `{"event":"offline"}`
    retained, so Node-RED notices the server going away.
- **Status fields.** Mirror Pi field names where they exist (one
  Node-RED dashboard can render both servers): `host`, `ts`,
  `stratum`, `fix_mode`, `sat_used`, `sat_seen`. Plus ESP-specific:
  `pps_count`, `pps_interval_us`, `rssi_dbm`, `uptime_s`,
  `free_heap`.
- **OTA.** Deferred. AS3935 also skips OTA; USB upload via
  `pio run -t upload` is fine during iteration.
- **File layout.** Monolithic `src/main.cpp`, no `lib/`. AS3935
  precedent: *"a library wrapper would only obscure the contract."*
- **Logging.** `Serial.begin(115200)`; format `[subsystem] message`,
  e.g. `[boot]`, `[gps]`, `[wifi]`, `[mqtt]`, `[ntp]`.
- **README / HANDOVER pattern.** README: Hardware table → Software
  stack → Status → Documentation links. HANDOVER.md captures
  decisions, rationale, operational checks.

Nothing here is binding — if a precedent doesn't fit, document the
deviation in this section so the next reader doesn't have to
re-derive it.

## Decisions yet to be made

1. **UART path.** SoftwareSerial on D1/D2 (USB debug stays free) vs.
   hardware UART0 with TX/RX swap (cleaner timing, no USB debug).
   *Lean: SoftwareSerial — UART jitter doesn't matter; debug matters.*
2. **Stratum claim.** Honest: stratum 2 (Wi-Fi adds jitter to a true
   stratum-1 source). Conventional: stratum 1 (the source is GPS).
   *Lean: stratum 1, since this never goes to public pool — LAN
   clients can decide if they trust it.*
3. **Holdover behaviour on fix loss.** Options: stop responding;
   keep drifting on last-known frequency; respond as stratum 16.
   *Lean: respond as stratum 16 — clients silently fall over to
   other servers, exactly the right behaviour.*
4. **Time-keeping unit.** `millis()` (1 ms tick, easy) vs `micros()`
   (1 µs tick, wraps every 71 minutes, more book-keeping).
   *Lean: `micros()`, handle wrap with `int32_t` subtraction.*
5. **OTA updates.** Add `ArduinoOTA` from the start, or keep wired
   firmware flashing? *Lean: add OTA later — keep v0.1 minimal.*
6. **MQTT status broadcast?** The Pi project publishes chrony
   metrics to the shack MQTT broker so a Node-RED widget and a
   SwiftBar plugin can show live status. The ESP8266 could do the
   same. *Lean: out of scope for v0.1. Add later if useful.*

## Performance expectations

| Metric | Target | (Pi NTP server for comparison) |
|--------|--------|--------------------------------|
| Local clock (PPS-locked) | ~1 ms | sub-µs |
| LAN client offset (Wi-Fi) | 1–10 ms | 1–4 ms |
| Cold-start TTFF | 30 s – 2 min | 30 s |
| Holdover after fix loss | seconds to minutes | hours |
| Leap-second handling | Best-effort | chrony, correct |

Do not expect to match the Pi. The point is to *understand why* the
Pi wins.

## Things explicitly NOT in scope

- Adding this to `pool.ntp.org`. The Wi-Fi-induced jitter is unfair
  to public clients.
- Replacing or supplanting the existing `pi-gps-ntp-server` in the
  VU2CPL shack.
- Sub-microsecond precision. ESP8266 cannot, full stop.
- A USB-Ethernet dongle to bypass Wi-Fi jitter. Defeats the learning
  goal, which is "use what the ESP gives you."
- Production-quality leap-second / GPS-week-rollover handling.
  Get something basic working first.
- Battery backup, GPS antenna mount, weatherproofing, OLED display,
  etc. Park all of that until the firmware works.

## Reference projects to read first

Read for structure and idioms; **don't fork blindly** — each is
different, and none does everything well:

- <https://github.com/Nick-Currawong/NTP-time-server> — ESP8266 +
  GPS, archived but instructive.
- <https://github.com/JoseG/ESP8266_GPS_NTP_Server> — small,
  readable.
- <https://github.com/raspberrypilearning/picow-ntp-server> — Pi
  Pico W cousin, similar architecture.

Also worth a skim:

- RFC 5905 (NTPv4), Figure 8 (packet format) and §7.3 (on-wire
  protocol).
- Quectel `L89 Hardware Design rev 1.1` PDF — pin 6 PPS details.

## Suggested next steps if reopening this project

1. **First, verify the L89 PPS exposure.** Do not buy, solder, or
   build anything else until this question is settled. See
   "Critical open question" above.
2. If PPS *is* available:
   a. Breadboard the wiring per the sketch.
   b. Flash a "hello world" sketch that reads NMEA via
      `TinyGPSPlus` and counts PPS interrupts. Verify exactly
      60 interrupts in 60 seconds. If it drifts, the PPS line is
      noisy and you need a pull-down or shorter wire.
3. Get the ESP8266 onto Wi-Fi, serve a *fake* NTP response from
   `millis()` only (no GPS). Make sure clients (Mac `sntp`,
   another Pi running chrony) can query it.
4. Stitch the PPS+NMEA→Unix epoch logic into the NTP response.
5. Measure: discipline another machine against this server, log
   offsets vs the Pi NTP server for several hours, write up
   findings in the repo README.

## Relationship to `pi-gps-ntp-server`

| | This project | `pi-gps-ntp-server` |
|---|---|---|
| Role | Learning experiment | Production shack NTP |
| Hardware | NodeMCU + L89 | Pi 3B + NEO-M8N |
| Stack | Arduino C++, hand-rolled NTP | chrony + gpsd + kernel PPS |
| Accuracy | ms class | sub-µs class |
| LAN client | Wi-Fi UDP/123 | wired Ethernet UDP/123 |
| Holdover | seconds | hours |

Cross-link in each README so a future reader finds both.
