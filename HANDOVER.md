# ESP8266 GPS NTP — Handover

## Status

**Milestones 1–4 done 2026-05-12 → 13.** `src/main.cpp` is a working
PPS-disciplined NTP server with MQTT status broadcast. A small
`TimeSync` state machine pairs each PPS edge with the Unix-seconds
value parsed from its `$GxRMC` sentence; NTP timestamps are computed
as `unix_seconds_at_pps + (micros() - micros_at_pps_edge)`, packed
onto the NTP epoch (+2,208,988,800 s offset). A separate MQTT
service loop publishes a retained JSON status to
`shack/esp8266-ntp/status` every 30 s.

- **Synced** (fresh fix + PPS+RMC pair, latency in [100 ms, 800 ms]):
  NTP replies advertise stratum 1, refid `GPS`, LI 0, precision −20
  (~1 µs local), root dispersion ~1 ms. MQTT status has
  `pps_sync=true`, `ref_id="GPS"`, `stratum=1`, real `ts`.
- **Holdover** (no fresh PPS+RMC for >5 s, or fix lost): NTP replies
  drop to stratum 16, refid `INIT`, LI 3 — clients silently fall back
  to other servers per RFC 5905 §3.5. MQTT status has
  `pps_sync=false`, `ref_id="INIT"`, `stratum=16`, `ts=0`.
- **Wi-Fi loss / power off:** broker holds LWT
  `{"event":"offline"}` retained on the status topic.

Verified end-to-end:
- M1 — NMEA + PPS counter, 60/60 PPS per minute, ±10 µs interval
  jitter (ESP crystal vs GPS atomic).
- M2 — Mac `sntp 192.168.1.38` round-trips 3–7 ms, server correctly
  refused as unsynchronised (deliberate placeholder stage).
- M3 — verified against the Mac on 2026-05-13. `sntp 192.168.1.38`
  reports `−0.0015 s ± 0.004 s` against Apple's reference NTP — well
  inside the handover's 1–10 ms LAN Wi-Fi target and matching the 1 ms
  root-dispersion we advertise. Off-by-one timing race found and fixed
  in the first attempt (see "Bring-up lessons (milestone 3)" below).
- M4 — `mosquitto_sub -h 192.168.1.169 -t "shack/esp8266-ntp/#" -v`
  on the Mac sees a retained JSON immediately on subscribe with all
  16 fields populated correctly: `stratum=1`, `ref_id="GPS"`,
  `pps_sync=true`, `pps_interval_us≈1000005`, `fix_mode="3D"`,
  `sat_used=22`, `rssi_dbm=-62`, sane `uptime_s` and `free_heap`.

Field-deployable now: pin a DHCP reservation for `192.168.1.38`,
plug into a window-side USB charger, point LAN NTP clients at it.
The production Pi `gpsntp.local` remains primary; this is the
parallel/redundant learning path.

Next: M5 — long-soak cross-server offset measurement against the Pi
NTP server, plotted, written up in the README.

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

## Known gotchas

**Dual CP2102 ports collide on the same `/dev/cu.usbserial-0001`.**
Most cheap NodeMCU and ESP32 dev boards in the shack ship with
Silicon Labs CP2102 chips, all carrying the default factory serial
number `0001`. macOS (and Linux) hand `/dev/cu.usbserial-0001` to
whichever board enumerated first, so a hard-coded `upload_port` in
`platformio.ini` silently flashes the wrong board whenever both
this NodeMCU and the `vu2cpl-as3935-bridge` ESP32 are connected.

Reprogramming the CP2102 EEPROM with a unique serial number was
investigated on 2026-05-12 using `cp210x-cfg` (DiUS/cp210x-cfg)
from both macOS (Sequoia, Apple Silicon) and the Pi (Linux). On
both hosts, the vendor-OUT control transfer returns success with no
libusb error, but the chip's EEPROM is unchanged on the next
enumeration (`iSerial` stays `0001`). The chip's markings confirm
it's a genuine Silicon Labs CP2102 (not a CH9102X clone), so the
write isn't being rejected at the protocol layer — the EEPROM has
been **factory-locked** by whoever assembled the dev board, and
that lock is permanent. No software path forward.

Workaround in place: `flash.sh` and `monitor.sh` at the repo root
enumerate the visible USB-serial devices and use bash's `select` to
prompt for the right one when more than one is present. The same
two scripts live in `vu2cpl-as3935-bridge`; both repos' `platformio.ini`
intentionally leaves `upload_port` and `monitor_port` unset so the
wrappers are the single source of truth.

```sh
./flash.sh       # build + upload, prompts when >1 port present
./monitor.sh     # serial monitor, same prompt
```

The rule "ESP firmware projects use a `flash.sh`/`monitor.sh` picker,
not a pinned `upload_port`" is captured in `~/.claude/CLAUDE.md` so
future ESP repos pick it up automatically.

## Bring-up lessons (milestone 3)

**The off-by-one second.** First field test of M3 reported a clean
`−1.009 s ± 0.004 s` offset on every `sntp` query — the ±0.004 s
matched Wi-Fi UDP jitter (expected), but the 1.009 s offset was a
real bug, not noise. Root cause was a race in `maybeUpdateTimeSync()`:

1. PPS edge `N` fires at second T → `ppsCount=N`,
   `ppsLastEdgeMicros=X`.
2. ~300 ms later, RMC for T arrives, parser updates `gps.time`. We
   sync correctly: `(unix=T, microsAtPps=X)`.
3. Second T+1 begins. PPS edge `N+1` fires →
   `ppsLastEdgeMicros = X+1_000_000`.
4. The next `maybeUpdateTimeSync()` runs *before* the RMC for T+1
   has been parsed. `gps.time` still holds T, but `ppsCount` has
   advanced. Our previous check (`sincePps < 900 ms`) was passed
   because `sincePps = a few ms`. We sync wrong:
   `(unix=T, microsAtPps=X+1_000_000)` — the *old* RMC's time
   paired with the *new* PPS edge, off by one second.

Fixed by tightening the gates so we sync only when *all four* hold:
location fresh, **RMC just parsed** (`gps.time.age() <
SYNC_RMC_MAX_AGE_MS`), PPS edge advanced, and PPS-to-RMC latency
in `[SYNC_PPS_LATENCY_MIN_US, SYNC_PPS_LATENCY_MAX_US]`. The latency
band rejects the race in step 4 (`sincePps` would be a few ms, below
`SYNC_PPS_LATENCY_MIN_US = 100 ms`) and also rejects the symmetric
case where RMC arrives unusually late (`sincePps > 800 ms`).

Post-fix `sntp` measurements showed `−0.0015 s ± 0.004 s`, indistinguishable
from a stratum-2 LAN client of Apple's NTP. The lesson: when the
firmware is doing two independent things (counting PPS in an ISR,
parsing NMEA in main-loop) that have to agree on a second boundary,
verify the cross-stream timing constraints explicitly — don't trust
that "PPS advanced + we have valid GPS data" implies the two
correspond to the same second.

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
   → **Decided in M4:** done. Retained JSON on
   `shack/esp8266-ntp/status` every 30 s via PubSubClient; LWT
   `{"event":"offline"}` retained on the same topic. Field names
   mirror the Pi's `shack/gpsntp/chrony` schema where they apply
   so Node-RED can render both servers from one widget.

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
4. ~~**M4: MQTT status broadcast.** Publish to `shack/esp8266-ntp/`
   under the conventions in "Shack conventions". LWT for offline
   detection. Mirror Pi field names where they overlap.~~ **Done
   2026-05-13.** Retained `shack/esp8266-ntp/status` published every
   30 s via PubSubClient; LWT `{"event":"offline"}` retained on the
   same topic. 16-field JSON with `host`, `ts`, `stratum`, `ref_id`,
   `leap`, `root_delay_s`, `root_dispersion_s`, `fix_mode`,
   `sat_used` mirroring Pi field names where they apply, plus ESP-
   specific `pps_count`, `pps_interval_us`, `pps_sync`,
   `ntp_requests`, `rssi_dbm`, `uptime_s`, `free_heap`. Verified
   from the Mac via `mosquitto_sub`.
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
