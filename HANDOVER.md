# ESP8266 GPS NTP — Handover

## Status

**Milestones 1–3 done 2026-05-12.** `src/main.cpp` is a working
PPS-disciplined NTP server. A small `TimeSync` state machine pairs
each PPS edge with the Unix-seconds value parsed from its
`$GxRMC` sentence; NTP timestamps are then computed as
`unix_seconds_at_pps + (micros() - micros_at_pps_edge)`, packed
onto the NTP epoch (+2,208,988,800 s offset).

- **Synced** (fresh fix + PPS+RMC pair within 900 ms): replies
  advertise stratum 1, refid `GPS`, LI 0, precision −20 (~1 µs
  local), root dispersion ~1 ms.
- **Holdover** (no fresh PPS+RMC for >5 s, or fix lost): replies
  drop to stratum 16, refid `INIT`, LI 3. Clients silently fall
  back to other servers per RFC 5905 §3.5.

Verified to date:
- M1 — NMEA + PPS counter, 60/60 PPS per minute, ±10 µs interval
  jitter (ESP crystal vs GPS atomic).
- M2 — Mac `sntp 192.168.1.38` round-trips 3–7 ms, server correctly
  refused as unsynchronised (deliberate placeholder stage).
- M3 — code path live; field verification against the production Pi
  via `chronyc add server 192.168.1.38 iburst` is the next user-facing
  step.

Next: M4 — MQTT status publish under `shack/esp8266-ntp/` so the
Node-RED dashboard can render the ESP alongside the production Pi
NTP server. M5 is the cross-server offset measurement write-up.

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

**Outcome: PPS is accessible at two viable tap points; using the
native L89 pin 6 directly (active-high, RISING edge).**

The story in order, because both tap points come up in the code/
comments and the reasoning behind each matters:

1. **The 1 Hz blue LED on the 7Semi L89 breakout** confirmed PPS is
   alive on the PCB. The LED only lights when the module has a 3D
   fix, so it doubles as a coarse "fix acquired" indicator.
2. **First tap: LED-driver transistor collector.** Tracing the LED
   showed it is driven by an NPN transistor with a current-limit
   resistor returning to the +3.3 V rail. The collector node is
   reachable. At this point the signal is **inverted** vs. the chip:
   it idles +3.3 V and pulses to ~0 V for 500 ms each second, so the
   *falling* edge is the on-time boundary. We verified this worked
   (firmware on `FALLING` produced clean 1 Hz interrupts).
3. **Second tap (the one in use): direct on Quectel L89 module
   pin 6.** This is the chip's native 1 PPS output — active-high,
   idles 0 V, pulses to +3.3 V for 500 ms, **rising** edge marks the
   on-time second (per Quectel L89 Hardware Design rev 1.1, 3.9 ns
   RMS accuracy). No intermediate transistor delay, no inversion.
   Firmware uses `attachInterrupt(D5, isr, RISING)`.

Why two tap points are documented:

- If you ever lose access to module pin 6 (heat-damaged pad, rework
  trouble), the collector-side tap is still a valid fallback — just
  flip the ISR back to `FALLING` and accept ~sub-µs of buffering
  delay through the transistor.
- The two are functionally interchangeable for ms-class accuracy.
  The direct tap is preferred because the timing reference is the
  *leading* edge, not a delayed/inverted edge.

Other notes:

- Multimeter readings were useless for distinguishing polarity here
  (50% duty cycle averages to ~1.65 V regardless). Topology was the
  reliable signal — confirmed at the breadboard with the working
  firmware afterwards.
- The 7Semi product page and Arduino library do not document PPS
  exposure — finding it required PCB inspection.
- Fallback if neither tap had been reachable: swap to a GY-NEO8MV2
  (u-blox NEO-M8N, same module the sibling Pi project uses) which
  has a labelled PPS pin. Not needed.

## Bring-up lessons (milestone 1)

Captured because they cost real bench time and aren't obvious from
the code:

- **L89 TX must go to `D2` (GPIO4)**, *not* the NodeMCU header pin
  labelled `RX`. The `RX` header pin is GPIO3 — the hardware UART RX
  the USB-serial chip uses. Hooking the GPS there silently fights
  for the same path the monitor reads from; you see no `[nmea]`
  bytes in the SoftwareSerial buffer and you see NMEA fragments
  leaking *into* your debug output as garbage.
- **L89 PPS pulse, not the LED bias, is the timing source.** When
  the module loses fix, the PPS line goes inactive even if the
  module is otherwise transmitting NMEA; that's expected and the
  firmware should fall back to stratum 16 in that state (TBD in
  milestone 3).
- **Spurious PPS counts on a floating D5** look like wire-touch
  transients separated by many seconds, with `lvl=0` always. If you
  see that pattern, the PPS jumper isn't actually seated — re-plug
  it before touching anything else.

## Wiring sketch (assuming PPS is accessible)

| L89 pin | NodeMCU pin | Notes |
|---------|-------------|-------|
| VCC (3.3 V) | 3V3 | Power |
| GND | GND | Common ground |
| TX | D2 (GPIO4) via SoftwareSerial | Leaves USB serial free for debug |
| PPS | D5 (GPIO14) — `attachInterrupt`-capable | The whole game lives here. Tap is direct on Quectel L89 pin 6 (active-high native) → use `RISING` edge. Collector-side LED-driver tap is a documented fallback (then `FALLING`). |

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
   hardware UART0 with TX/RX swap. *Lean: SoftwareSerial.*
   → **Decided in M1:** SoftwareSerial on D2 (GPIO4). The hardware
   UART stays for `Serial.print` debug. Confirmed working.
2. **Stratum claim.** Honest: stratum 2 (Wi-Fi adds jitter). Conv-
   entional: stratum 1 (source is GPS). *Lean: stratum 1.*
   → **Decided in M3:** stratum 1 when synced, stratum 16 when not.
   LAN-only, never public — clients decide trust. Wi-Fi jitter is
   accounted for in the dispersion field, not stratum.
3. **Holdover behaviour on fix loss.** Stop / drift / report
   stratum 16. *Lean: stratum 16.*
   → **Decided in M3:** stratum 16 after 5 s without a fresh
   PPS+RMC pair (`SYNC_HOLDOVER_US = 5_000_000`). RFC 5905 §3.5
   conforming; clients silently fall back to other servers.
4. **Time-keeping unit.** `millis()` vs `micros()`.
   → **Decided in M3:** `micros()` for PPS edge capture and NTP
   timestamp interpolation (32-bit wraps every 71 min, handled via
   uint32_t subtraction). `millis()` only for the per-second log
   pacing where 1 ms granularity is fine.
5. **OTA updates.** Add `ArduinoOTA` from the start, or keep wired
   firmware flashing? *Lean: add OTA later — keep v0.1 minimal.*
   → Still open; deferred past M5.
6. **MQTT status broadcast?** *Lean: out of scope for v0.1, add later.*
   → To be done in M4. Topic prefix `shack/esp8266-ntp/`, broker
   and field conventions per "Shack conventions" above.

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

## Milestone roadmap

1. ~~**M1: NMEA + PPS counter.** Breadboard the wiring per the
   sketch, flash `src/main.cpp`, verify exactly 60 PPS interrupts
   per 60 s window with `fix=Y` and parsed UTC.~~ **Done
   2026-05-12.**
2. ~~**M2: Wi-Fi + placeholder NTP responder.** Add WiFiManager so the
   ESP joins the shack Wi-Fi without compile-time secrets. Open a
   UDP listener on port 123, reply to NTPv4 requests with timestamps
   derived from `millis()` only (no GPS yet).~~ **Done 2026-05-12.**
   Mac `sntp 192.168.1.38` round-tripped in 3–7 ms and correctly
   refused the server as unsynchronised (stratum 16 / refid `INIT`).
3. ~~**M3: GPS-disciplined NTP responder.** Stitch the PPS+NMEA→Unix
   epoch logic into the NTP response. Stratum 1 when fixed, stratum
   16 when not. Holdover behaviour: respond stratum 16 immediately
   on fix loss.~~ **Done 2026-05-12.** `TimeSync` state machine pairs
   each PPS edge with its RMC time within a 900 ms window; replies
   advertise stratum 1 / refid `GPS` when fresh, fall back to
   stratum 16 / refid `INIT` after 5 s of stale sync.
4. **M4: MQTT status broadcast.** Publish to `shack/esp8266-ntp/`
   under the conventions in "Shack conventions". LWT for offline
   detection. Mirror Pi field names where they overlap.
5. **M5: Measurement.** Discipline another machine against this
   server, log offsets vs the Pi NTP server for several hours,
   write up findings in the repo README.

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
