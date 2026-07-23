#!/usr/bin/env python3
"""Emulate an AllPowers R4000 over BLE (protocol v2) for EMS bench testing.

Requires: pip install -r tools/requirements-emulate.txt
macOS: allow Bluetooth for Terminal/Python when prompted.

Interactive commands (type while running):
  soc 85       set battery %
  +5 / -10     adjust SOC
  in 400       input watts
  out 200      output watts
  min 60       minutes remaining
  ac on|off    AC flag
  dc on|off    DC flag
  drain 5      SOC drop per minute (0=off)
  status       show current state
  help         this list
  quit         stop emulator
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import threading
from typing import Any, Dict, Optional, Tuple

from bless import (  # type: ignore
    BlessGATTCharacteristic,
    BlessServer,
    GATTAttributePermissions,
    GATTCharacteristicProperties,
)

SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
WRITE_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"

HELP_TEXT = """commands:
  soc <0-100>     set battery %
  +N / -N         adjust SOC (e.g. +5  -10)
  in <watts>      solar/input watts
  out <watts>     load/output watts
  min <n>         minutes remaining
  ac on|off       AC outlet flag
  dc on|off       DC rail flag
  drain <n>       auto SOC drop %% per minute (0=off)
  status          show state
  help            this help
  quit            exit"""

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
log = logging.getLogger("r4000-emu")


def crc_xor(buf: bytes | bytearray) -> int:
    c = buf[0]
    for b in buf[1:-1]:
        c ^= b
    return c


def parse_on_off(token: str) -> Optional[bool]:
    t = token.strip().lower()
    if t in ("1", "on", "true", "yes"):
        return True
    if t in ("0", "off", "false", "no"):
        return False
    return None


class BatteryState:
    def __init__(
        self,
        soc: int = 92,
        in_w: int = 400,
        out_w: int = 200,
        minutes: int = 60,
        dc: bool = True,
        ac: bool = False,
        hz60: bool = True,
        drain: float = 0.0,
    ) -> None:
        self.soc = soc
        self.in_w = in_w
        self.out_w = out_w
        self.minutes = minutes
        self.dc = dc
        self.ac = ac
        self.hz60 = hz60
        self.beep = False
        self.led = False
        self.screen = True
        self.voice = False
        self.drain = drain
        self._lock = threading.Lock()

    def snapshot(self) -> str:
        with self._lock:
            return (
                f"SOC={self.soc}%  in={self.in_w}W  out={self.out_w}W  "
                f"min={self.minutes}  AC={'on' if self.ac else 'off'}  "
                f"DC={'on' if self.dc else 'off'}  drain={self.drain}%/min"
            )

    def flags_byte(self) -> int:
        # Notify decode uses 1-indexed bits: DC, AC, 60Hz, beep, LED, screen, voice
        f = 0
        if self.dc:
            f |= 1 << 0
        if self.ac:
            f |= 1 << 1
        if self.hz60:
            f |= 1 << 2
        if self.beep:
            f |= 1 << 3
        if self.led:
            f |= 1 << 4
        if self.screen:
            f |= 1 << 5
        if self.voice:
            f |= 1 << 6
        return f

    def status_notify(self) -> bytearray:
        with self._lock:
            pkt = bytearray(16)
            pkt[0] = 0xA5
            pkt[1] = 0x65
            pkt[2] = 0x00
            pkt[3] = 0x00
            pkt[4] = 0x00
            pkt[5] = 0x08  # payload len → total 16
            pkt[6] = 0x01  # status command
            pkt[7] = self.flags_byte()
            pkt[8] = self.soc & 0xFF
            pkt[9] = (self.in_w >> 8) & 0xFF
            pkt[10] = self.in_w & 0xFF
            pkt[11] = (self.out_w >> 8) & 0xFF
            pkt[12] = self.out_w & 0xFF
            pkt[13] = (self.minutes >> 8) & 0xFF
            pkt[14] = self.minutes & 0xFF
            pkt[15] = crc_xor(pkt)
            return pkt

    def apply_write(self, data: bytes) -> None:
        # A5 65 00 B1 01 01 00 <status> <crc>
        if len(data) < 9 or data[0] != 0xA5 or data[1] != 0x65 or data[6] != 0x00:
            log.warning("ignored write: %s", data.hex(" "))
            return
        if data[-1] != crc_xor(data):
            log.warning("bad CRC on write: %s", data.hex(" "))
            return
        status = data[7]
        with self._lock:
            self.dc = bool(status & (1 << 0))
            self.ac = bool(status & (1 << 1))
            self.hz60 = bool(status & (1 << 3))
            self.beep = bool(status & (1 << 4))
            self.led = bool(status & (1 << 5))
            self.screen = bool(status & (1 << 6))
            self.voice = bool(status & (1 << 7))
            log.info(
                "write applied: AC=%s DC=%s flags=0x%02X",
                self.ac,
                self.dc,
                status,
            )

    def apply_command(self, line: str) -> Tuple[bool, str]:
        """Returns (should_quit, message)."""
        line = line.strip()
        if not line:
            return False, ""

        # Relative SOC: +5 / -10
        if len(line) >= 2 and line[0] in "+-" and line[1:].isdigit():
            delta = int(line)
            with self._lock:
                self.soc = max(0, min(100, self.soc + delta))
                return False, f"SOC → {self.soc}%"

        parts = line.split()
        cmd = parts[0].lower()

        if cmd in ("q", "quit", "exit"):
            return True, "stopping…"
        if cmd in ("h", "help", "?"):
            return False, HELP_TEXT
        if cmd in ("s", "status", "st"):
            return False, self.snapshot()

        if cmd == "soc" and len(parts) == 2:
            try:
                v = int(parts[1])
            except ValueError:
                return False, "usage: soc <0-100>"
            with self._lock:
                self.soc = max(0, min(100, v))
                return False, f"SOC → {self.soc}%"

        if cmd == "in" and len(parts) == 2:
            try:
                v = int(parts[1])
            except ValueError:
                return False, "usage: in <watts>"
            with self._lock:
                self.in_w = max(0, v)
                return False, f"in → {self.in_w}W"

        if cmd == "out" and len(parts) == 2:
            try:
                v = int(parts[1])
            except ValueError:
                return False, "usage: out <watts>"
            with self._lock:
                self.out_w = max(0, v)
                return False, f"out → {self.out_w}W"

        if cmd in ("min", "minutes") and len(parts) == 2:
            try:
                v = int(parts[1])
            except ValueError:
                return False, "usage: min <n>"
            with self._lock:
                self.minutes = max(0, v)
                return False, f"minutes → {self.minutes}"

        if cmd == "ac" and len(parts) == 2:
            v = parse_on_off(parts[1])
            if v is None:
                return False, "usage: ac on|off"
            with self._lock:
                self.ac = v
                return False, f"AC → {'on' if self.ac else 'off'}"

        if cmd == "dc" and len(parts) == 2:
            v = parse_on_off(parts[1])
            if v is None:
                return False, "usage: dc on|off"
            with self._lock:
                self.dc = v
                return False, f"DC → {'on' if self.dc else 'off'}"

        if cmd == "drain" and len(parts) == 2:
            try:
                v = float(parts[1])
            except ValueError:
                return False, "usage: drain <%%/min>"
            with self._lock:
                self.drain = max(0.0, v)
                return False, f"drain → {self.drain}%/min"

        return False, f"unknown: {line!r}  (type help)"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Emulate AllPowers R4000 BLE battery")
    p.add_argument("--name", default="AP R4000 V3.0", help="Advertised name")
    p.add_argument("--soc", type=int, default=92, help="Battery %%")
    p.add_argument("--in-w", type=int, default=400, dest="in_w", help="Input watts")
    p.add_argument("--out-w", type=int, default=200, dest="out_w", help="Output watts")
    p.add_argument("--minutes", type=int, default=60, help="Minutes remaining")
    p.add_argument("--ac", action="store_true", help="Start with AC on")
    p.add_argument("--interval", type=float, default=1.0, help="Notify interval (s)")
    p.add_argument(
        "--drain",
        type=float,
        default=0.0,
        help="SOC drop per minute (0=off), for reserve-cut tests",
    )
    p.add_argument(
        "--quiet",
        action="store_true",
        help="Don't print every notify (cleaner for typing)",
    )
    return p.parse_args()


async def stdin_command_loop(state: BatteryState, stop: asyncio.Event) -> None:
    print(HELP_TEXT, flush=True)
    print(f"r4000> {state.snapshot()}", flush=True)
    while not stop.is_set():
        try:
            line = await asyncio.to_thread(sys.stdin.readline)
        except (asyncio.CancelledError, EOFError):
            stop.set()
            return
        if line == "":
            # EOF
            stop.set()
            return
        quit_, msg = state.apply_command(line)
        if msg:
            print(msg, flush=True)
        if quit_:
            stop.set()
            return
        print("r4000> ", end="", flush=True)


async def main() -> None:
    args = parse_args()
    state = BatteryState(
        soc=max(0, min(100, args.soc)),
        in_w=max(0, args.in_w),
        out_w=max(0, args.out_w),
        minutes=max(0, args.minutes),
        ac=args.ac,
        drain=max(0.0, args.drain),
    )

    loop = asyncio.get_running_loop()
    server = BlessServer(name=args.name, loop=loop, name_overwrite=True)
    stop = asyncio.Event()

    def _uuid(u: Any) -> str:
        return str(u).lower()

    def on_write(characteristic: BlessGATTCharacteristic, value: Any, **kwargs: Any) -> None:
        raw = bytes(value) if value is not None else b""
        log.info("EMS write → %s", raw.hex(" "))
        if _uuid(characteristic.uuid) == WRITE_UUID:
            state.apply_write(raw)
            characteristic.value = bytearray(raw)

    def on_read(characteristic: BlessGATTCharacteristic, **kwargs: Any) -> bytearray:
        if _uuid(characteristic.uuid) == NOTIFY_UUID:
            return state.status_notify()
        return characteristic.value or bytearray()

    server.read_request_func = on_read
    server.write_request_func = on_write

    # CoreBluetooth: any non-None initial Value must be read-only. Both chars
    # change at runtime, so leave Value as None (dynamic).
    gatt: Dict = {
        SERVICE_UUID: {
            NOTIFY_UUID: {
                "Properties": (
                    GATTCharacteristicProperties.read
                    | GATTCharacteristicProperties.notify
                ),
                "Permissions": GATTAttributePermissions.readable,
                "Value": None,
            },
            WRITE_UUID: {
                "Properties": (
                    GATTCharacteristicProperties.write
                    | GATTCharacteristicProperties.write_without_response
                ),
                "Permissions": GATTAttributePermissions.writeable,
                "Value": None,
            },
        }
    }

    await server.add_gatt(gatt)
    await server.start()
    log.info("Advertising as %r", args.name)
    log.info("Service %s  notify %s  write %s", SERVICE_UUID, NOTIFY_UUID, WRITE_UUID)
    log.info("Type commands below (help). Ctrl+C or quit to stop.")

    cmd_task = asyncio.create_task(stdin_command_loop(state, stop))
    print("r4000> ", end="", flush=True)

    elapsed = 0.0
    try:
        while not stop.is_set():
            pkt = state.status_notify()
            char = server.get_characteristic(NOTIFY_UUID)
            char.value = pkt
            server.update_value(SERVICE_UUID, NOTIFY_UUID)
            if not args.quiet:
                log.info(
                    "notify SOC=%d%% AC=%d in=%d out=%d | %s",
                    state.soc,
                    int(state.ac),
                    state.in_w,
                    state.out_w,
                    pkt.hex(" "),
                )

            try:
                await asyncio.wait_for(stop.wait(), timeout=args.interval)
            except asyncio.TimeoutError:
                pass

            elapsed += args.interval
            with state._lock:
                drain = state.drain
            if drain > 0 and elapsed >= 60.0:
                steps = int(elapsed // 60.0)
                elapsed -= steps * 60.0
                with state._lock:
                    state.soc = max(0, state.soc - int(drain * steps))
    except asyncio.CancelledError:
        pass
    finally:
        stop.set()
        cmd_task.cancel()
        try:
            await cmd_task
        except asyncio.CancelledError:
            pass
        await server.stop()
        log.info("Stopped")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
