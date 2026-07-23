#!/usr/bin/env python3
"""Drive EMS serial IR learn and write firmware/include/haier_ir.h.

Prerequisites:
  - Firmware flashed with the `learn` serial command
  - Broadlink Mini online (IR LED green on the UI)
  - Chunghop K-830ES on code 13, aimed at the Mini

Usage:
  pip install -r tools/requirements-learn.txt
  python tools/learn_ir.py
  python tools/learn_ir.py --port /dev/cu.usbmodem14101
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Install pyserial first:  pip install -r tools/requirements-learn.txt", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "firmware" / "include" / "haier_ir.h"

BANNER = """
============================================================
  AllPowers EMS — IR learn (Chunghop K-830ES code 13)
============================================================
  Mini IR LED must be green. Remote aimed at the Mini.

  You will capture three codes, in order:
    1) Cool @ 26°C  (mode COOL, fan auto) — press POWER/OK
    2) Heat @ 18°C  (mode HEAT, fan auto) — press to send
    3) Off          (POWER off)           — press to send

  Each step has ~30 seconds. Follow the [LEARN] prompts.
============================================================
"""

START_MARK = "======== PEGAR EN haier_ir.h ========"
END_MARK = "======== FIN — rebuild + flash ========"


def guess_port() -> str | None:
    preferred = []
    others = []
    for p in list_ports.comports():
        desc = f"{p.device} {p.description} {p.manufacturer or ''}".lower()
        if any(x in desc for x in ("usbmodem", "usbserial", "cp210", "ch340", "wch", "esp32", "silicon labs")):
            preferred.append(p.device)
        elif p.device.startswith(("/dev/cu.", "/dev/tty.usb", "/dev/ttyACM", "/dev/ttyUSB", "COM")):
            others.append(p.device)
    if preferred:
        return preferred[0]
    if others:
        return others[0]
    return None


def open_serial(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
    # ESP32-S3 often resets on open — give it a moment.
    time.sleep(1.5)
    ser.reset_input_buffer()
    return ser


def wait_ready(ser: serial.Serial, timeout_s: float) -> bool:
    """Wait until firmware looks alive; Broadlink ready is preferred but not required."""
    print(f"Waiting for board on {ser.port} ({timeout_s:.0f}s)…")
    deadline = time.time() + timeout_s
    saw_boot = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        try:
            line = raw.decode("utf-8", errors="replace").rstrip()
        except Exception:
            continue
        if line:
            print(f"  | {line}")
        if "arranque completo" in line or "Refugio EMS" in line:
            saw_boot = True
        if "Broadlink online" in line or "IR: listo" in line or "[BL] auth OK" in line:
            print("Broadlink looks ready.")
            return True
        if saw_boot and "serial: escribe 'learn'" in line:
            # Boot finished; Mini may still attach in background.
            print("Board up — if IR LED is green, continuing.")
            return True
    return saw_boot


def run_learn(ser: serial.Serial, out_path: Path) -> int:
    print(BANNER)
    input("Press Enter when the Chunghop is ready (code 13, aimed at Mini)… ")

    print("\nSending: learn")
    ser.write(b"learn\n")
    ser.flush()

    capturing = False
    dump_lines: list[str] = []
    failed = False
    deadline = time.time() + 120  # three 30s windows + margin

    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            continue

        # Highlight prompts for the operator.
        if line.startswith("[LEARN]") or "LEARN IR" in line or line.startswith("========"):
            print(f"\n>>> {line}")
        elif capturing:
            print(line)
        else:
            print(f"  | {line}")

        if "Broadlink no listo" in line:
            print("\nMini not ready. Wait for IR LED green, then re-run.")
            return 2
        if "FALLÓ" in line or "incompleto" in line:
            failed = True

        if START_MARK in line:
            capturing = True
            dump_lines = []
            continue
        if capturing:
            if END_MARK in line:
                capturing = False
                # Sanitize firmware dump quirks (historical double-comma rows).
                text = "\n".join(dump_lines).strip() + "\n"
                text = text.replace(", ,", ",")
                if "#pragma once" not in text or "kHaierCool26" not in text:
                    print("\nDump looked incomplete — not writing file.")
                    return 3
                out_path.parent.mkdir(parents=True, exist_ok=True)
                out_path.write_text(text, encoding="utf-8")
                print(f"\nWrote {out_path}")
                print("Next:  cd firmware && pio run -t upload")
                return 0
            dump_lines.append(line)

    print("\nTimed out waiting for a full learn session.")
    if failed:
        print("A step failed on the board — fix aim/button and try again.")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Learn IR codes via EMS serial `learn`")
    ap.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output header (default: {DEFAULT_OUT})",
    )
    ap.add_argument(
        "--wait",
        type=float,
        default=45.0,
        help="Seconds to wait for board boot/ready before sending learn",
    )
    ap.add_argument(
        "--no-wait",
        action="store_true",
        help="Skip boot wait; send learn immediately",
    )
    args = ap.parse_args()

    port = args.port or guess_port()
    if not port:
        print("No serial port found. Pass --port explicitly.", file=sys.stderr)
        print("Available:", file=sys.stderr)
        for p in list_ports.comports():
            print(f"  {p.device}  {p.description}", file=sys.stderr)
        return 1

    print(f"Opening {port} @ {args.baud}")
    try:
        ser = open_serial(port, args.baud)
    except serial.SerialException as exc:
        print(f"Cannot open {port}: {exc}", file=sys.stderr)
        return 1

    try:
        if not args.no_wait:
            if not wait_ready(ser, args.wait):
                print("Board did not come up in time; try --no-wait if it is already running.")
                return 1
        return run_learn(ser, args.out)
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
