#!/usr/bin/env python3
"""
GaggiMate heat-up logger.

Connects to the WebSocket interface (/ws) of the GaggiMate display and writes
every 'evt:status' message (ct = current temperature, tt = target temperature,
pr = pressure, fl = flow) with a timestamp to a CSV file.

Requirement:
    pip install websocket-client

Usage:
    python3 heatup_logger.py <DEVICE-IP> [csv-file]

Example:
    python3 heatup_logger.py 192.168.1.50 heatup.csv

Stop with Ctrl+C. The CSV can then be used to fit dead time and thermal mass
and to derive Kp/Ki/Kd.
"""
import csv
import json
import sys
import time

try:
    import websocket  # websocket-client
except ImportError:
    print("Missing: pip install websocket-client")
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    host = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "heatup.csv"
    url = f"ws://{host}/ws"

    t0 = time.time()
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["t_sec", "ct", "tt", "pr", "fl"])
        f.flush()
        print(f"Writing to {out_path}. Stop with Ctrl+C.")
        try:
            while True:  # Reconnect-Schleife: ueberlebt Verbindungsabbrueche
                try:
                    print(f"Connecting to {url} ...")
                    ws = websocket.create_connection(url, timeout=10)
                    print("Connected.")
                except Exception as e:
                    print(f"Connection failed ({e}). Retrying in 2s ...")
                    time.sleep(2)
                    continue
                try:
                    while True:
                        raw = ws.recv()
                        if not raw:
                            continue
                        try:
                            msg = json.loads(raw)
                        except (json.JSONDecodeError, TypeError):
                            continue
                        if msg.get("tp") != "evt:status":
                            continue
                        t = round(time.time() - t0, 2)
                        ct = msg.get("ct")
                        tt = msg.get("tt")
                        pr = msg.get("pr")
                        fl = msg.get("fl")
                        writer.writerow([t, ct, tt, pr, fl])
                        f.flush()
                        print(f"t={t:7.1f}s  ct={ct}  tt={tt}", end="\r", flush=True)
                except websocket.WebSocketConnectionClosedException:
                    print("\nConnection lost. Reconnecting in 2s ...")
                except Exception as e:
                    print(f"\nError ({e}). Reconnecting in 2s ...")
                finally:
                    try:
                        ws.close()
                    except Exception:
                        pass
                time.sleep(2)
        except KeyboardInterrupt:
            print("\nStopped.")


if __name__ == "__main__":
    main()
