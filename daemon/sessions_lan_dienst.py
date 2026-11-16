#!/usr/bin/env python3
"""Lesedienst: die Claude-Code-Sessions DIESES Rechners, abrufbar im LAN.

Läuft auf dem zweiten Windows-PC ("Autodarts-PC"). Der Daemon auf Timos PC
holt die Liste ab und zeigt sie zusammen mit den Server- und den eigenen
Sessions auf dem Clawdmeter-Display (siehe `lan_sessions.py`).

Gelesen wird dieselbe Quelle wie für die lokalen Sessions: `lokale_sessions.
lies_lokale_sessions()`, also nur `~/.claude/sessions/*.json`. Die Geheimnisse
im selben Ordner (`<pid>.<hash>.key`) werden nie angefasst.

Eingang: ein Endpunkt, `GET /sessions`, mit Pflicht-Header `X-Clawd-Token`.
Das Token steht in einer Datei und wird nie ausgegeben, geloggt oder in einen
Fehlertext geschrieben. Nur Python-Standardbibliothek, Python >= 3.10.

Start:  pythonw sessions_lan_dienst.py --port 8791
Port 8790 ist belegt (fremder Dienst) und deshalb nie der Standard.
"""

import argparse
import hmac
import json
import logging
import os
import secrets
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import RotatingFileHandler
from pathlib import Path

try:
    from daemon import lokale_sessions
except ImportError:
    # Beide Dateien liegen auf dem Ziel-PC in einem Ordner. Wird der
    # Interpreter mit -I gestartet (ab Python 3.11 gilt dann auch -P), steht
    # der Skriptordner NICHT in sys.path — er wird hier ausdrücklich ergänzt,
    # und zwar nur für diesen flachen Fallback. Ans ENDE, nicht an den Anfang:
    # So hat die Standardbibliothek Vorrang, und eine untergeschobene
    # `ctypes.py` im Skriptordner wird nie geladen (Sicherheitsprüfung 16.11.).
    sys.path.append(str(Path(__file__).resolve().parent))
    import lokale_sessions

log = logging.getLogger("sessions_lan_dienst")

STANDARD_PORT = 8791
STANDARD_HOST = "0.0.0.0"
# Hängende Verbindungen (Portscanner, abgebrochene Clients) binden sonst einen
# Thread, bis das Betriebssystem sie räumt.
ANFRAGE_TIMEOUT_SEKUNDEN = 5
# Der Timeout oben gilt je recv: Wer jede Sekunde ein Byte schickt, hält die
# Verbindung ewig. Deshalb zusätzlich eine Frist für die ganze Verbindung
# (Sicherheitsprüfung 16.11., Slowloris ohne Token).
GESAMTFRIST_SEKUNDEN = 5
# Mehr gleichzeitige Verbindungen bekommen keinen Thread, sondern werden sofort
# geschlossen. Der Daemon fragt im 3-Sekunden-Takt über eine Verbindung ab.
MAX_GLEICHZEITIGE_VERBINDUNGEN = 8
WARTESCHLANGE = 16
# token_urlsafe(32) liefert 43 Zeichen. Alles unter 32 ist von Hand getippt
# oder abgeschnitten und im LAN zu leicht zu raten.
MIN_TOKEN_LAENGE = 32
TOKEN_HEADER = "X-Clawd-Token"
ENDPUNKT = "/sessions"
# Nur diese Felder verlassen den Rechner.
AUSGABE_FELDER = ("name", "zustand", "seit_sekunden")
LOG_MAX_BYTES = 512 * 1024


def standard_token_datei() -> Path:
    if sys.platform == "win32":
        basis = Path(os.environ.get("LOCALAPPDATA",
                                    Path.home() / "AppData" / "Local"))
        return basis / "Clawdmeter" / "lan_sessions_token"
    return Path.home() / ".config" / "claude-usage-monitor" / "lan_sessions_token"


def sorge_fuer_token(datei: Path) -> str:
    """Das Token aus der Datei; fehlt sie, wird sie einmal erzeugt.

    Ausgegeben wird nur der Pfad, nie der Inhalt. Ein zu kurzes oder nicht
    ASCII-Token bricht den Start ab — mit einer Meldung ohne Tokenwert.
    """
    if not datei.exists():
        datei.parent.mkdir(parents=True, exist_ok=True)
        try:
            # "x": legt nur an, überschreibt nie ein Token, das parallel
            # entstanden ist.
            fd = os.open(datei, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(secrets.token_urlsafe(32))
            _melde(f"Tokendatei angelegt: {datei}")

    try:
        token = datei.read_text(encoding="utf-8-sig").strip()
    except (OSError, ValueError) as e:
        raise SystemExit(f"Tokendatei nicht lesbar ({e.__class__.__name__}): {datei}")
    if len(token) < MIN_TOKEN_LAENGE:
        raise SystemExit(f"Token in {datei} ist kürzer als {MIN_TOKEN_LAENGE} "
                         "Zeichen. Datei löschen, dann erzeugt der Dienst ein neues.")
    if not token.isascii() or not token.isprintable() or " " in token:
        raise SystemExit(f"Token in {datei} enthält unzulässige Zeichen.")
    return token


def _melde(text: str) -> None:
    # Unter pythonw gibt es kein stdout.
    if sys.stdout is not None:
        print(text, flush=True)


class SessionsDienst(ThreadingHTTPServer):
    """HTTP-Server, der Token und Sessionsordner für den Handler hält."""

    daemon_threads = True
    request_queue_size = WARTESCHLANGE

    def __init__(self, adresse, token: str, verzeichnis: Path | None = None):
        self.token = token.encode("utf-8")
        self.verzeichnis = verzeichnis
        self._plaetze = threading.BoundedSemaphore(MAX_GLEICHZEITIGE_VERBINDUNGEN)
        super().__init__(adresse, AnfrageHandler)

    def process_request(self, request, client_address):
        # Ohne Blockieren: Ein voller Dienst weist ab, statt den Annahme-Thread
        # festzuhalten.
        if not self._plaetze.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._plaetze.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._plaetze.release()


class AnfrageHandler(BaseHTTPRequestHandler):
    server_version = "ClawdSessions"
    sys_version = ""
    timeout = ANFRAGE_TIMEOUT_SEKUNDEN
    _frist = None

    def setup(self):
        super().setup()
        self._frist = threading.Timer(GESAMTFRIST_SEKUNDEN, self._kappe_verbindung)
        self._frist.daemon = True
        self._frist.start()

    def finish(self):
        if self._frist is not None:
            self._frist.cancel()
        super().finish()

    def _kappe_verbindung(self):
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def log_request(self, code="-", size="-"):
        # Bewusst nur Status und Adresse: keine Anfragezeile, keine Header.
        log.info("%s %s", self.address_string(), code)

    def log_message(self, format, *args):  # noqa: A002 — Signatur der Basisklasse
        log.info("%s %s", self.address_string(), format % args)

    def _antworte(self, status: int, koerper: bytes = b"",
                  typ: str | None = None, **kopf) -> None:
        self.send_response(status)
        if typ:
            self.send_header("Content-Type", typ)
        for name, wert in kopf.items():
            self.send_header(name.replace("_", "-"), wert)
        self.send_header("Content-Length", str(len(koerper)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if koerper:
            self.wfile.write(koerper)

    def _token_stimmt(self) -> bool:
        gesendet = self.headers.get(TOKEN_HEADER, "")
        try:
            gesendet = gesendet.encode("utf-8")
        except UnicodeError:
            return False
        return hmac.compare_digest(gesendet, self.server.token)

    def do_GET(self):  # noqa: N802 — Name der Basisklasse
        if self.path.split("?", 1)[0] != ENDPUNKT:
            return self._antworte(404)
        if not self._token_stimmt():
            return self._antworte(401)
        try:
            sessions = lokale_sessions.lies_lokale_sessions(self.server.verzeichnis)
        except Exception as e:  # noqa: BLE001 — eine Anfrage darf den Dienst nie kippen
            log.warning("Sessions nicht lesbar: %s", e.__class__.__name__)
            return self._antworte(500)
        koerper = json.dumps({
            "sessions": [{feld: s[feld] for feld in AUSGABE_FELDER}
                         for s in sessions],
        }).encode("utf-8")
        self._antworte(200, koerper, "application/json")

    def _methode_nicht_erlaubt(self):
        self._antworte(405, Allow="GET")

    do_HEAD = do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = \
        _methode_nicht_erlaubt


def _argumente(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Claude-Code-Sessions dieses Rechners im LAN anbieten.")
    p.add_argument("--host", default=STANDARD_HOST,
                   help=f"Adresse zum Lauschen (Standard {STANDARD_HOST})")
    p.add_argument("--port", type=int, default=STANDARD_PORT,
                   help=f"Port (Standard {STANDARD_PORT}; 8790 ist belegt)")
    p.add_argument("--token-datei", type=Path, default=None,
                   help="Datei mit dem Token "
                        f"(Standard {standard_token_datei()})")
    return p.parse_args(argv)


def _logging_einrichten(token_datei: Path) -> None:
    fmt = "%(asctime)s %(message)s"
    if sys.stderr is not None:
        logging.basicConfig(level=logging.INFO, format=fmt)
        return
    # pythonw: ohne Konsole bliebe der Dienst stumm. Eine Zeile je Abruf im
    # Minutentakt wächst ohne Grenze, deshalb rotiert die Datei.
    datei = RotatingFileHandler(token_datei.with_name("lan_sessions_dienst.log"),
                                maxBytes=LOG_MAX_BYTES, backupCount=1,
                                encoding="utf-8")
    logging.basicConfig(level=logging.INFO, format=fmt, handlers=[datei])


def main(argv=None) -> int:
    args = _argumente(argv)
    token_datei = args.token_datei or standard_token_datei()
    token = sorge_fuer_token(token_datei)
    _logging_einrichten(token_datei)
    try:
        dienst = SessionsDienst((args.host, args.port), token)
    except OSError as e:
        raise SystemExit(f"Port {args.port} nicht verfügbar: {e.strerror or e.__class__.__name__}")
    log.info("Lausche auf %s:%s", *dienst.server_address[:2])
    try:
        dienst.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        dienst.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
