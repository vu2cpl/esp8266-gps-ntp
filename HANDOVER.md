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

## Critical open question — verify BEFORE building anything

**Does the 7Semi L89 breakout expose the 1 PPS pin on a header pad?**

We chased this in the Pi project's L89HA evaluation and got a
strong-but-not-conclusive *no*:

- The Quectel L89 silicon definitely has 1 PPS on module pin 6
  (3.9 ns RMS accuracy, 3.3 V CMOS, rising-edge, 500 ms pulse width;
  see official Quectel L89 Hardware Design rev 1.1).
- The 7Semi product page lists exposed pins as TX, RX, BAT, I2C — no
  PPS.
- The 7Semi Arduino library's wiring table likewise only mentions
  TX/RX/VCC/GND.

If PPS is **not** broken out, this project stalls — you'd be left
with NMEA-only timing (~100 ms class), which isn't worth doing on a
microcontroller. Verification steps before any soldering:

1. Inspect the breakout top *and* bottom for a labelled `PPS` /
   `1PPS` pad. Also any unlabelled hole or via near the module.
2. Look for a small LED (often blue) that flashes at 1 Hz once the
   module has a 3D fix — usually wired to the PPS line.
3. With the module powered, probe module pin 6 with a multimeter
   (DC volts, expect brief 3 V pulses) or scope (clean 1 Hz square wave).
4. Email 7Semi support: *"Is the L89 module's pin 6 (1PPS) routed to
   any pad on this breakout?"*

**If yes →** proceed with the wiring sketch below.
**If no →** swap to a GY-NEO8MV2 (u-blox NEO-M8N — the same module
the sibling Pi project ended up using) which has labelled PPS, and
shelve the L89 for a separate navigation experiment.

## Wiring sketch (assuming PPS is accessible)

| L89 pin | NodeMCU pin | Notes |
|---------|-------------|-------|
| VCC (3.3 V) | 3V3 | Power |
| GND | GND | Common ground |
| TX | D2 (GPIO4) via SoftwareSerial | Leaves USB serial free for debug |
| PPS | D5 (GPIO14) — `attachInterrupt`-capable | The whole game lives here |

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
