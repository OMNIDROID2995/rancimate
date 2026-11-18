#!/usr/bin/env python3
"""
GaggiMate Aufheiz-Logger.

Verbindet sich mit der WebSocket-Schnittstelle (/ws) des GaggiMate-Displays
und schreibt jede 'evt:status'-Nachricht (ct = Ist-Temp, tt = Soll-Temp,
pr = Druck, fl = Fluss) mit Zeitstempel in eine CSV-Datei.

Voraussetzung:
    pip install websocket-client

Aufruf:
    python3 heatup_logger.py <GERAETE-IP> [csv-datei]

Beispiel:
    python3 heatup_logger.py 192.168.1.50 heatup.csv

Beenden mit Strg+C. Danach die CSV hier im Chat liefern -> ich fitte
Totzeit/thermische Masse und rechne die finalen Kp/Ki/Kd aus.
"""
import csv
import json
import sys
import time

try:
    import websocket  # websocket-client
except ImportError:
    print("Fehlt: pip install websocket-client")
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
        print(f"Schreibe nach {out_path}. Beenden mit Strg+C.")
        try:
            while True:  # Reconnect-Schleife: ueberlebt Verbindungsabbrueche
                try:
                    print(f"Verbinde mit {url} ...")
                    ws = websocket.create_connection(url, timeout=10)
                    print("Verbunden.")
                except Exception as e:
                    print(f"Verbindung fehlgeschlagen ({e}). Neuer Versuch in 2s ...")
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
                    print("\nVerbindung verloren. Reconnect in 2s ...")
                except Exception as e:
                    print(f"\nFehler ({e}). Reconnect in 2s ...")
                finally:
                    try:
                        ws.close()
                    except Exception:
                        pass
                time.sleep(2)
        except KeyboardInterrupt:
            print("\nGestoppt.")


if __name__ == "__main__":
    main()
