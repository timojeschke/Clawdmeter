#!/usr/bin/env python3
"""Second data source: Claude Code session states from a remote server.

The usage numbers come from Anthropic's rate-limit headers (see poll_api).
This module adds an unrelated source: how many Claude Code sessions on a
remote machine are waiting for input, and which ones. Both end up in the same
GATT payload because the device has exactly one input channel.

Disabled unless `sessions_url` and `sessions_token` are set in the config
file. No URL, no calls, no behaviour change — an existing install that never
touches the config keeps working exactly as before.

The firmware's receive buffer is 512 bytes and it truncates anything longer
(BLE_BUF_SIZE in firmware/src/ble.cpp), so the merge below is size-aware:
counts always fit, names are added while there is room and dropped from the
end when there is not. Measured against a real server response: 29 sessions
serialise to 3039 bytes, so dropping names is the normal case, not the edge.
"""

import json
import logging

log = logging.getLogger(__name__)

# The firmware buffer holds 511 bytes, and that number is a trap: it is only
# reachable with a write that asks for a response. The daemon's normal write
# asks for none, and such a write is capped at ATT_MTU-3 — 244 bytes at the
# usual negotiated MTU of 247. Windows rejects anything longer outright
# ("Falscher Parameter") instead of truncating it.
#
# Measured on the real link 2026-09-25: base payload without sessions ~125
# bytes, with five waiting names 271 — which failed seven times in a row and
# took the usage numbers down with it.
#
# So the budget is sized for the SMALL write, not the buffer. Sessions then
# arrive on their own merit instead of depending on long-write support, and
# the names that do not fit are counted in "sx" rather than dropped silently.
PAYLOAD_LIMIT_BYTES = 230

# Optimistisches Budget fuer den bestaetigten Schreibvorgang (Long Write), der
# bis an den 511-Byte-Puffer der Firmware reicht. Der Daemon versucht es zuerst
# damit und faellt auf PAYLOAD_LIMIT_BYTES zurueck, wenn der Schreibvorgang
# scheitert — so nutzt die Anzeige den Platz, ohne sich darauf zu verlassen,
# dass der grosse Weg auf jeder Windows-Version funktioniert.
PAYLOAD_LIMIT_GROSS = 460

# Long session names ("Stötefalke - Webseite Personal Training") eat the budget
# without adding information — the first words identify the session. Every
# umlaut costs six bytes here, not two: the payload is serialised with
# ensure_ascii, so "ö" travels as ö.
# Kandidaten fuer die Namenslaenge, von lang nach kurz durchprobiert. Oben
# beginnt es dort, wo auch lange Projektnamen noch unterscheidbar sind; unten
# endet es, wo ein Name nichts mehr aussagt.
NAME_LAENGEN = (34, 30, 26, 22, 18, 14)
MAX_NAME_CHARS = NAME_LAENGEN[0]

# One character, so the cut costs the name only one letter. Two bytes over the
# wire now that the payload travels as UTF-8 — as three dots it would be three.
ELLIPSE = "\u2026"

# A session list is worth showing only while it is current. Past this the
# device shows counts without names rather than a plausible-looking lie.
MAX_AGE_SECONDS = 300

REQUEST_TIMEOUT_SECONDS = 5.0


def read_sessions_config(config_file) -> tuple[str | None, str | None]:
    """Read `sessions_url` and `sessions_token` from the config file.

    Returns (None, None) when either is missing — the feature is opt-in and
    half-configured counts as off.
    """
    url = token = None
    try:
        if not config_file.exists():
            return None, None
        for line in config_file.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            key, val = line.split("=", 1)
            key = key.strip().lower()
            if key == "sessions_url":
                url = val.strip() or None
            elif key == "sessions_token":
                token = val.strip() or None
    except OSError:
        return None, None

    if not url or not token:
        return None, None
    return url, token


async def fetch_sessions(http_client, url: str, token: str) -> dict | None:
    """Fetch the session state. Returns None on any failure.

    None is deliberately indistinguishable between "server down", "wrong
    token" and "malformed answer": this source is an extra, and none of those
    cases should disturb the usage numbers, which are the device's main job.
    """
    try:
        resp = await http_client.get(
            url,
            headers={"X-Clawd-Token": token},
            timeout=REQUEST_TIMEOUT_SECONDS,
        )
    except Exception as e:  # noqa: BLE001 — httpx raises a wide family here
        log.debug("sessions source unreachable: %s", e.__class__.__name__)
        return None

    if resp.status_code != 200:
        log.debug("sessions source returned HTTP %s", resp.status_code)
        return None

    try:
        daten = resp.json()
    except ValueError:
        log.debug("sessions source returned no valid JSON")
        return None

    if not isinstance(daten, dict) or "anzahl" not in daten:
        log.debug("sessions source returned an unexpected shape")
        return None
    return daten


def _laufende_namen(daten: dict) -> list[str]:
    """Namen der laufenden Sessions — erst die arbeitenden, dann die im Hintergrund.

    Timo, 2026-09-25: "anstatt waiting for input listest du einfach alle
    running sessions auf, das geht leichter."

    Der Grund dahinter: "wartet auf Eingabe" ist bei Claude Code kein
    trennscharfer Zustand — die App nennt JEDE untaetige Session so, und bei
    29 Sessions sind das fast alle. "Laeuft gerade" ist dagegen eindeutig: Der
    Zustand kommt direkt aus Claude Code und ist entweder wahr oder nicht.

    Timo, 2026-09-30: "manchmal haben diese Sessions auf den Servern ja auch
    Hintergrundaufgaben, stehen dann aber nicht mehr auf running. Koennen wir
    das auch fixen?" Seitdem zaehlt "hintergrund" (Status `shell`: Antwort
    fertig, ein Hintergrundprozess laeuft noch) ebenfalls als laufend, aber
    nachrangig — die Reihenfolge bestimmt, welche Namen beim Kuerzen zuerst
    wegfallen (siehe merge_into_payload), und da sollen die wirklich
    arbeitenden Sessions den Vorrang behalten.
    """
    sessions = daten.get("sessions")
    if not isinstance(sessions, list):
        return []
    arbeitet, hintergrund = [], []
    for s in sessions:
        if not isinstance(s, dict):
            continue
        # Ein leerer Name bekommt einen Platzhalter statt zu fehlen: Das Geraet
        # dimmt ab Eintrag `sa`, jede Luecke verschoebe diese Grenze.
        name = str(s.get("name", "")).strip() or "(unnamed)"
        if s.get("zustand") == "arbeitet":
            arbeitet.append(name)
        elif s.get("zustand") == "hintergrund":
            hintergrund.append(name)
    return arbeitet + hintergrund


def _kuerzen(name: str, grenze: int) -> str:
    """Am Ende abschneiden und das Auslassungszeichen dahinter setzen.

    Timos Regel, 2026-09-25: "Privat - clawdme…. als beispiel, das abkuerzen am
    ende bitte." Kein Schnitt an der Wortgrenze und kein Auslassungszeichen in
    der Mitte — der Anfang bleibt so stehen, wie der Name anfaengt.
    """
    if len(name) <= grenze:
        return name
    return name[:grenze - 1].rstrip() + ELLIPSE


def _mindestlaengen(namen: list[str]) -> list[int]:
    """Je Name die kuerzeste Laenge, bei der er noch eindeutig ist.

    Eine feste Laenge fuer alle verschenkt Platz: "Privat - IPTV" ist schon
    nach neun Zeichen unverwechselbar, "Heimatschutzverein - Dokumente" erst
    nach zweiundzwanzig. Wer beide gleich lang macht, zeigt entweder zu wenige
    Namen oder zwei gleiche.

    Gemessen wird gegen alle anderen Namen: Gesucht ist die erste Stelle, an
    der sich dieser Name von jedem anderen unterscheidet.
    """
    laengen = []
    for i, name in enumerate(namen):
        noetig = 1
        for j, anderer in enumerate(namen):
            if i == j:
                continue
            # Erste Stelle, an der sich die beiden unterscheiden.
            stelle = 0
            while (stelle < len(name) and stelle < len(anderer)
                   and name[stelle] == anderer[stelle]):
                stelle += 1
            noetig = max(noetig, stelle + 1)
        laengen.append(min(noetig, len(name)))
    return laengen


def _serialised_size(payload: dict) -> int:
    # Must match how write_payload serialises, or the budget measures the wrong
    # string. ensure_ascii=False is the point: "ö" costs two bytes as UTF-8 and
    # six as an escape, and the firmware reads UTF-8 directly.
    return len(json.dumps(payload, separators=(",", ":"),
                          ensure_ascii=False).encode("utf-8"))


def merge_into_payload(payload: dict, daten: dict | None,
                       budget: int = PAYLOAD_LIMIT_BYTES) -> dict:
    """Add session fields to the usage payload, trimming names to fit.

    Returns a new dict; the caller's payload is not modified. With `daten`
    None the payload comes back unchanged, so a dead sessions server never
    costs the device its usage numbers.

    Field names are short because every byte competes with the usage data:
      sw  waiting count      sa  working count
      sb  background count   sg  parked count   (sa+sb sind die "Namen")
      sn  running names — working first, then background, as many as fit
      sx  names dropped for space (or sa+sb when the data is stale)
    """
    if daten is None:
        return dict(payload)

    anzahl = daten.get("anzahl")
    if not isinstance(anzahl, dict):
        return dict(payload)

    alter = daten.get("alter_sekunden")
    veraltet = isinstance(alter, int) and alter > MAX_AGE_SECONDS

    merged = dict(payload)
    merged["sw"] = int(anzahl.get("wartet", 0))
    merged["sa"] = int(anzahl.get("arbeitet", 0))
    merged["sb"] = int(anzahl.get("hintergrund", 0))
    merged["sg"] = int(anzahl.get("geparkt", 0))

    # Stale data: counts are still roughly right, a name list is not. Showing
    # a session as waiting when it finished four minutes ago sends the user to
    # the wrong window.
    if veraltet or not daten.get("frisch", True):
        # Die verschwiegenen Namen sind die LAUFENDEN — seit dem 2026-09-25
        # zeigt das Geraet die, nicht die wartenden; seit dem 2026-09-30
        # zaehlen die Hintergrund-Sessions mit dazu.
        merged["sx"] = merged["sa"] + merged["sb"]
        return merged

    namen = _laufende_namen(daten)
    if not namen:
        return merged

    # Zwei Groessen konkurrieren um dasselbe Budget: wie viele Namen gezeigt
    # werden und wie lang jeder sein darf. Lange Namen sind nicht Kosmetik —
    # "Heimatschutzverein - Dokumente" und "… - Webseite" sind erst ab einer
    # gewissen Laenge ueberhaupt auseinanderzuhalten.
    #
    # Deshalb wird nicht geraten, sondern durchprobiert: erst moeglichst viele
    # Namen, bei gleicher Anzahl die groesste Laenge. Timo will den Platz
    # genutzt sehen, und was uebrig bleibt, kommt den Namen zugute.
    # Eine Zeile mehr ist nichts wert, wenn dafuer zwei Zeilen gleich heissen:
    # Dann sagt die Liste nicht mehr, welche Session gemeint ist, und genau
    # dafuer ist sie da. Deshalb zwei getrennte Bestwerte — Dubletten werden
    # nur genommen, wenn es ueberhaupt keine dublettenfreie Loesung gibt.
    # Jeder Name bekommt mindestens so viel Platz, wie er zur Unterscheidung
    # braucht — und der Rest des Budgets wird gleichmaessig daraufgelegt.
    mindest = _mindestlaengen(namen)

    bester = None
    bester_eindeutig = None
    for laenge in NAME_LAENGEN:
        gekuerzt = [_kuerzen(n, max(laenge, m + 1)) for n, m in zip(namen, mindest)]
        passend = list(gekuerzt)
        while passend:
            kandidat = dict(merged)
            kandidat["sn"] = passend
            if len(passend) < len(namen):
                kandidat["sx"] = len(namen) - len(passend)
            if _serialised_size(kandidat) <= budget:
                if bester is None or len(passend) > len(bester["sn"]):
                    bester = kandidat
                if len(set(passend)) == len(passend) and (
                        bester_eindeutig is None
                        or len(passend) > len(bester_eindeutig["sn"])):
                    bester_eindeutig = kandidat
                break
            passend.pop()

    if bester_eindeutig is not None:
        return bester_eindeutig
    if bester is not None:
        return bester

    # Nicht ein einziger Name passt: Die Zahlen allein sind immer noch wahr.
    merged["sx"] = len(namen)
    return merged
