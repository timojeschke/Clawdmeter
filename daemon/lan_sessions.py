#!/usr/bin/env python3
"""Sessions eines zweiten PCs im LAN, für die Sessionliste des Geräts.

Gegenstück zu `sessions_lan_dienst.py`, das auf dem anderen PC läuft. Timo,
2026-11-16: "sicherstellen, dass die laufenden Sessions korrekt angezeigt
werden" — die Remote-Control-Session auf dem Autodarts-PC fehlte auf dem
Display.

Eigenes Modul statt Erweiterung von `sessions_source.py`: Dort steht der Weg
zum Serverdienst samt Budget und Kürzung der Namen; hier geht es um eine
zweite, unabhängige Quelle mit eigener Konfig und eigener strenger Prüfung.
So bleibt `sessions_source.py` unangetastet.

Wie `lokale_sessions` ist diese Quelle eine Zugabe und darf die Nutzungsdaten
nie gefährden: Jeder Fehler ergibt für diese URL eine leere Liste. Ein
schlafender PC (der Autodarts-PC geht nach 30 Minuten in den Ruhezustand) ist
der Normalfall, keine Störung. Das Token steht weder in Logs noch in
Fehlertexten.

Konfig (dieselbe Datei wie `sessions_url`):
    lan_sessions_url   = http://192.168.2.X:8791/sessions[, weitere URLs]
    lan_sessions_token = <Token aus der Datei des Lesedienstes>
Fehlt eines von beiden, ist die Funktion aus.
"""

import asyncio
import json
import logging

try:
    from daemon.sessions_source import lies_konfig_eintraege
except ImportError:
    from sessions_source import lies_konfig_eintraege

log = logging.getLogger(__name__)

# Kurz, weil der Abruf im selben Takt wie der Serverabruf läuft: Ein
# schlafender PC verlängert den Takt höchstens um diese Zeit.
LAN_TIMEOUT_SEKUNDEN = 2.0
MAX_EINTRAEGE_JE_QUELLE = 50
MAX_NAME_LAENGE = 64
# Über einem Jahr Stillstand ist jeder Wert Fremddaten. Die Grenze steht VOR
# jeder Fließkomma-Umwandlung: `math.isfinite(10**400)` wirft OverflowError
# (Sicherheitsprüfung 16.11.).
MAX_SEIT_SEKUNDEN = 10**9
# Eine ehrliche Antwort mit 50 Einträgen hat rund 5 KB.
MAX_ANTWORT_BYTES = 256 * 1024
ZUSTAENDE = ("arbeitet", "hintergrund", "wartet", "geparkt")
TOKEN_HEADER = "X-Clawd-Token"


def lies_lan_konfig(config_file) -> tuple[list[str], str | None]:
    """(URLs, Token) aus der Konfig; ([], None) wenn eines von beiden fehlt.

    Halb eingerichtet zählt als aus — so geht nie ein Abruf ohne Token raus.
    """
    urls: list[str] = []
    token = None
    for schluessel, wert in lies_konfig_eintraege(config_file):
        if schluessel == "lan_sessions_url":
            urls = [u.strip() for u in wert.split(",")
                    if u.strip().lower().startswith(("http://", "https://"))]
        elif schluessel == "lan_sessions_token":
            token = wert or None
    if not urls or not token:
        return [], None
    return urls, token


def _pruefe_eintrag(eintrag) -> dict | None:
    """Ein Eintrag der LAN-Antwort, oder None bei Fremddaten.

    Die Antwort kommt von aussen: Was nicht passt, fällt einzeln weg, der Rest
    der Liste bleibt. Ausgegeben werden genau die drei Felder.
    """
    if not isinstance(eintrag, dict):
        return None
    name = eintrag.get("name")
    if not isinstance(name, str):
        return None
    name = name.strip()
    if not name or len(name) > MAX_NAME_LAENGE or not name.isprintable():
        return None
    try:
        # Ein einzelnes Surrogat ("\ud800x") besteht jede Textprüfung, wirft
        # aber später beim Serialisieren des Namens als UTF-8 und beendete den
        # Daemon (Sicherheitsprüfung 16.11.).
        name.encode("utf-8")
    except UnicodeEncodeError:
        return None
    zustand = eintrag.get("zustand")
    if zustand not in ZUSTAENDE:
        return None
    seit = eintrag.get("seit_sekunden")
    if isinstance(seit, bool) or not isinstance(seit, (int, float)):
        return None
    # Bereichsprüfung statt isfinite: schließt NaN, Unendlich, negative und
    # riesige Ganzzahlen ohne Umwandlung aus.
    if not 0 <= seit <= MAX_SEIT_SEKUNDEN:
        return None
    return {"name": name, "zustand": zustand, "seit_sekunden": int(seit)}


async def _lies_antwort(http_client, url: str, token: str) -> bytes | None:
    """Der Antwortkörper, höchstens MAX_ANTWORT_BYTES; sonst None.

    Gestreamt und ohne Kompression gelesen (`identity`, `aiter_raw`): Eine
    gzip-Bombe würde sonst VOR der Größenprüfung entpackt. Weiterleitungen
    werden nicht verfolgt — der LAN-Dienst leitet nie weiter.
    """
    async with http_client.stream(
            "GET", url,
            headers={TOKEN_HEADER: token, "Accept-Encoding": "identity"},
            timeout=LAN_TIMEOUT_SEKUNDEN,
            follow_redirects=False) as resp:
        if resp.status_code != 200:
            log.debug("LAN-Sessions antworteten mit HTTP %s", resp.status_code)
            return None
        antwort = bytearray()
        async for stueck in resp.aiter_raw():
            antwort.extend(stueck)
            if len(antwort) > MAX_ANTWORT_BYTES:
                log.debug("LAN-Sessions: Antwort zu gross")
                return None
    return bytes(antwort)


async def _hole(http_client, url: str, token: str) -> list[dict]:
    roh = await _lies_antwort(http_client, url, token)
    if roh is None:
        return []
    try:
        daten = json.loads(roh)
    except (ValueError, RecursionError):
        log.debug("LAN-Sessions: kein gültiges JSON")
        return []

    sessions = daten.get("sessions") if isinstance(daten, dict) else None
    if not isinstance(sessions, list):
        log.debug("LAN-Sessions: unerwartete Form")
        return []

    geprueft = (_pruefe_eintrag(e) for e in sessions[:MAX_EINTRAEGE_JE_QUELLE])
    return [e for e in geprueft if e is not None]


async def hole_lan_sessions(http_client, url: str, token: str) -> list[dict]:
    """Die Sessions einer LAN-Quelle; bei jedem Fehler eine leere Liste.

    Das Timeout von httpx gilt je Phase: Ein Server, der jede Sekunde ein Byte
    schickt, hielte den Abruf ewig offen. Deshalb liegt die Frist um den GANZEN
    Abruf (Sicherheitsprüfung 16.11.).
    """
    try:
        return await asyncio.wait_for(_hole(http_client, url, token),
                                      LAN_TIMEOUT_SEKUNDEN)
    except asyncio.TimeoutError:
        log.debug("LAN-Sessions: Gesamtfrist überschritten")
        return []
    except Exception as e:  # noqa: BLE001 — httpx wirft hier eine breite Familie
        log.debug("LAN-Sessions nicht erreichbar: %s", e.__class__.__name__)
        return []


async def hole_alle_lan_sessions(http_client, urls: list[str],
                                 token: str | None) -> list[dict]:
    """Alle LAN-Quellen gleichzeitig; ohne URL oder Token eine leere Liste."""
    if not urls or not token:
        return []
    ergebnisse = await asyncio.gather(
        *(hole_lan_sessions(http_client, u, token) for u in urls),
        return_exceptions=True)
    return [e for liste in ergebnisse if isinstance(liste, list) for e in liste]
