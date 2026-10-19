#!/usr/bin/env python3
"""Tiny serial terminal that also writes a timestamped log file.

Usage: python3 logterm.py <port> [logfile]      (needs: pip install pyserial)
Type commands and press Enter. Press Ctrl-C to quit. Works on macOS, Linux, Windows.
"""
import sys
import threading
import time

import serial


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = sys.argv[1]
    logname = sys.argv[2] if len(sys.argv) > 2 else time.strftime("probe_log_%Y%m%d_%H%M%S.txt")
    ser = serial.Serial(port, 115200, timeout=0.1)
    log = open(logname, "a", encoding="utf-8")
    print(f"[logterm] {port} @115200, logging to {logname}. Ctrl-C to quit.")
    stop = False

    def reader() -> None:
        buf = b""
        while not stop:
            data = ser.read(256)
            if not data:
                continue
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                log.write(f"{time.strftime('%H:%M:%S')} {line.decode('utf-8', 'replace').rstrip()}\n")
            log.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        while True:
            line = sys.stdin.readline()
            if not line:
                break
            log.write(f"{time.strftime('%H:%M:%S')} >>> {line.rstrip()}\n")
            ser.write(line.rstrip("\r\n").encode() + b"\r\n")
    except KeyboardInterrupt:
        pass
    finally:
        stop = True
        log.close()
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
