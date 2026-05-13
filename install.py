#!/usr/bin/env python3
"""
install.py — first-run bring-up for esp8266-gps-ntp.

Run from the repo root:
    python3 install.py

What it does:
  1. Detects (or asks) macOS vs Raspberry Pi / Linux.
  2. Ensures PlatformIO is installed (path differs per platform).
  3. Prompts for shack-specific config (Wi-Fi captive-portal AP
     name + password, ESP hostname, MQTT broker, status topic).
     Defaults come from the values currently in src/main.cpp, so
     re-running is safe — just press Enter to keep what's there.
  4. Patches src/main.cpp in place.
  5. Runs `pio run` to verify the firmware compiles cleanly.

It does NOT flash the chip. Use ./flash.sh for that — it picks the
right USB-serial port interactively (see HANDOVER.md "Known gotchas"
for why a pinned upload_port doesn't work in this shack).

Patches show up as `git diff src/main.cpp`. If you forked for your
own station, commit them. If you're just trying it out,
`git checkout -- src/main.cpp` reverts to defaults.
"""

import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
MAIN_CPP = REPO / "src" / "main.cpp"

# ── ANSI colours (no dependency) ────────────────────────────────────────
G = "\033[32m"   # green
Y = "\033[33m"   # amber
R = "\033[31m"   # red
B = "\033[1m"    # bold
D = "\033[2m"    # dim
N = "\033[0m"    # reset


def hdr(msg):  print(f"\n{B}── {msg} ──{N}")
def ok(msg):   print(f"  {G}✓{N} {msg}")
def warn(msg): print(f"  {Y}⚠{N} {msg}")
def err(msg):  print(f"  {R}✗{N} {msg}")


def ask(prompt, default):
    raw = input(f"  {prompt} [{D}{default}{N}]: ").strip()
    return raw if raw else default


def confirm(prompt, default=True):
    d = "Y/n" if default else "y/N"
    raw = input(f"\n{prompt} [{d}]: ").strip().lower()
    if not raw:
        return default
    return raw.startswith("y")


# ── Read current constexpr string values so prompts can default to them ─
def grep_constexpr(name, fallback=""):
    text = MAIN_CPP.read_text()
    m = re.search(
        rf'constexpr\s+const\s+char\s*\*\s+{re.escape(name)}\s*=\s*"([^"]*)"',
        text,
    )
    return m.group(1) if m else fallback


# ── In-place patcher for `constexpr const char* NAME = "...";` lines ────
def patch_constexpr(name, value):
    text = MAIN_CPP.read_text()
    pat = rf'(constexpr\s+const\s+char\s*\*\s+{re.escape(name)}\s*=\s*)"[^"]*"'
    new = rf'\g<1>"{value}"'
    new_text, n = re.subn(pat, new, text, count=1)
    if n == 0:
        warn(f"{name}: pattern not found in src/main.cpp (skipped)")
        return False
    if new_text == text:
        ok(f"{name} already set to {value!r}")
        return True
    MAIN_CPP.write_text(new_text)
    ok(f"{name} → {value!r}")
    return True


# ── Platform detection ──────────────────────────────────────────────────
def detect_platform():
    s = platform.system().lower()
    if s == "darwin":
        return "macos"
    if s == "linux":
        # Distinguish Pi from generic Linux by /proc/cpuinfo
        try:
            cpu = Path("/proc/cpuinfo").read_text().lower()
            if "raspberry pi" in cpu or any(t in cpu for t in ("bcm2708", "bcm2709", "bcm2711", "bcm2712")):
                return "pi"
        except Exception:
            pass
        return "linux"
    return "other"


# ── PlatformIO ensure (Mac vs Pi/Linux branch) ──────────────────────────
def ensure_platformio(plat):
    if shutil.which("pio"):
        v = subprocess.run(["pio", "--version"], capture_output=True, text=True).stdout.strip()
        ok(f"PlatformIO already installed: {v}")
        return True

    warn("PlatformIO (`pio`) not found on PATH.")

    if plat == "macos":
        print(f"  Install options on macOS:")
        print(f"    {D}brew install platformio{N}    (recommended; Apple Silicon Macs)")
        print(f"    {D}python3 -m pip install --user platformio{N}")
        if confirm("Install via pip --user now?", default=True):
            cmd = [sys.executable, "-m", "pip", "install", "--user", "platformio"]
            return subprocess.call(cmd) == 0
        warn("Install PlatformIO manually then re-run this script.")
        return False

    if plat in ("pi", "linux"):
        print(f"  Install option on Pi/Linux:")
        print(f"    {D}python3 -m pip install --user platformio{N}")
        print(f"  On Raspberry Pi OS Bookworm+ pip refuses by default (PEP 668).")
        print(f"  This script will retry with --break-system-packages if asked.")
        if not confirm("Install via pip --user now?", default=True):
            warn("Install PlatformIO manually then re-run this script.")
            return False
        cmd = [sys.executable, "-m", "pip", "install", "--user", "platformio"]
        if subprocess.call(cmd) == 0:
            return True
        warn("Plain pip install failed (likely PEP 668).")
        if confirm("Retry with --break-system-packages?", default=False):
            return subprocess.call(cmd + ["--break-system-packages"]) == 0
        return False

    warn("Unknown platform; install PlatformIO manually then re-run.")
    return False


# ── Main flow ───────────────────────────────────────────────────────────
def main():
    print(f"\n{B}esp8266-gps-ntp install script{N}")
    print(f"{D}Detects your platform, installs PlatformIO if needed,")
    print(f"prompts for shack-specific config, and verifies the firmware builds.{N}")

    if not MAIN_CPP.exists():
        err(f"src/main.cpp not found — are you running this from the repo root?")
        return 1

    # 1. Platform detection (with confirm + manual override)
    hdr("Platform")
    auto = detect_platform()
    auto_label = {"macos": "macOS", "pi": "Raspberry Pi",
                  "linux": "Linux", "other": "unknown"}[auto]
    ok(f"Auto-detected: {auto_label}")
    plat = auto
    if not confirm("Is this correct?", default=True):
        choice = ask("Which platform? [macos / pi / linux]", auto if auto != "other" else "macos")
        plat = choice.lower().strip()
        if plat not in ("macos", "pi", "linux"):
            err(f"Unknown platform {plat!r}; aborting.")
            return 1
        ok(f"Using: {plat}")

    # 2. PlatformIO
    hdr("Toolchain")
    if not ensure_platformio(plat):
        err("PlatformIO not available — cannot verify build. Aborting.")
        return 1

    # 3. Config prompts
    hdr("Wi-Fi onboarding (captive portal)")
    ap_name  = ask("Setup AP SSID",      grep_constexpr("WIFI_AP_NAME",     "vu2cpl-esp8266-ntp-setup"))
    ap_pass  = ask("Setup AP password",  grep_constexpr("WIFI_AP_PASSWORD", "vu2cpl1234"))
    hostname = ask("ESP hostname",       grep_constexpr("WIFI_HOSTNAME",    "esp8266-ntp"))

    hdr("MQTT status publish")
    broker = ask("Broker IP/host",   grep_constexpr("MQTT_BROKER",       "192.168.1.169"))
    topic  = ask("Status topic",     grep_constexpr("MQTT_TOPIC_STATUS", "shack/esp8266-ntp/status"))

    # 4. Apply patches
    hdr("Patching src/main.cpp")
    patch_constexpr("WIFI_AP_NAME", ap_name)
    patch_constexpr("WIFI_AP_PASSWORD", ap_pass)
    patch_constexpr("WIFI_HOSTNAME", hostname)
    patch_constexpr("MQTT_BROKER", broker)
    patch_constexpr("MQTT_CLIENT_ID", hostname)   # match hostname for clarity
    patch_constexpr("MQTT_TOPIC_STATUS", topic)

    # 5. Build verification
    hdr("Verifying build")
    print(f"  {D}Running `pio run`...{N}")
    if subprocess.call(["pio", "run"]) != 0:
        err("Build failed. Check the output above.")
        return 1
    ok("Firmware compiles cleanly.")

    # 6. Next steps
    hdr("Done")
    print(f"  Plug in the NodeMCU, then:")
    print(f"    {B}./flash.sh{N}     # interactive upload (picks the right USB port)")
    print(f"    {B}./monitor.sh{N}   # serial console, same picker")
    print()
    print(f"  Your config changes show as a git diff against src/main.cpp.")
    print(f"  Commit if persistent, or {D}git checkout -- src/main.cpp{N} to revert.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        err("Interrupted.")
        sys.exit(130)
