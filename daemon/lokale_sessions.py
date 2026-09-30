#!/usr/bin/env python3
"""Claude-Code-Sessions auf DIESEM Rechner, für die Sessionliste des Geräts.

Die Liste auf dem Gerät kam bisher ausschließlich vom Server (`tmux
list-sessions` plus Zustand je Name). Eine Session auf Timos eigenem PC tauchte
darin nicht auf — Timo, 2026-09-27: "ja das wäre top wenn die PC Session da
auch mit drin ist."

Gelesen wird dieselbe Quelle wie auf dem Server: `~/.claude/sessions/<pid>.json`
mit `status`, `statusUpdatedAt` und `name`. Das ist die Stelle, aus der auch die
Claude-App ihre Anzeige speist — abgelesen, nicht gedeutet.

Der Modul liefert die lokalen Sessions in genau der Form, die der Serverdienst
liefert. Dadurch läuft alles Weitere (Kürzen der Namen, Budget, "+N more")
unverändert durch den vorhandenen Weg in `sessions_source.py`, statt ein
zweites Mal gebaut zu werden.
"""

import json
import logging
import os
import sys
import time
from pathlib import Path

log = logging.getLogger(__name__)

# Dieselbe Abbildung wie im Sammler auf dem Server
# (`collector/sammle-sessions.py`).
#
# `shell` ist KEINE Arbeit im Vordergrund: Die Antwort ist fertig, nur ein
# Hintergrundprozess lebt noch (Agent, Render, Messlauf). Eine Session, die im
# Vordergrund einen Befehl ausführt, meldet `busy` — gemessen am 2026-09-27,
# nachdem das Gerät eine seit 37 Minuten fertige Session als laufend zeigte.
#
# Bis 2026-09-27 zählte `shell` deshalb wie `idle` — als "geparkt", nicht als
# laufend. Timo, 2026-09-30: "manchmal haben diese Sessions auf den Servern ja
# auch Hintergrundaufgaben, stehen dann aber nicht mehr auf running. Können
# wir das auch fixen?" `shell` bekommt jetzt einen eigenen Zustand
# ("hintergrund"), sichtbar abgesetzt von "arbeitet", aber weiterhin als
# laufend gezählt statt als geparkt.
ZUSTAND_ABBILDUNG = {
    "busy": "arbeitet",
    "shell": "hintergrund",
    "waiting": "wartet",
    "idle": None,
}

# Eine Zustandsdatei ohne lebenden Prozess ist ein Überbleibsel. Auf dem Server
# fällt das nicht auf, weil dort tmux die Liste vorgibt; hier gibt es nichts,
# was gegenprüft — also wird die PID selbst geprüft. Bleibt die Prüfung
# unmöglich, entscheidet das Alter.
VERWAIST_NACH_SEKUNDEN = 10 * 60


def _prozess_lebt(pid: int) -> bool | None:
    """True/False, oder None wenn es sich auf dieser Plattform nicht sagen lässt."""
    if pid <= 0:
        return False
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                      False, pid)
        if not handle:
            return False
        try:
            code = wintypes.DWORD()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return None
            return code.value == STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True      # existiert, gehört nur jemand anderem
    except OSError:
        return None
    return True


def _ist_aktuell(eintrag: dict, jetzt: float) -> bool:
    """Lebt der Prozess — oder ist die Datei wenigstens frisch genug?"""
    lebt = _prozess_lebt(int(eintrag.get("pid", 0) or 0))
    if lebt is not None:
        return lebt
    gestempelt = eintrag.get("updatedAt") or eintrag.get("statusUpdatedAt") or 0
    # Die Zeitstempel stehen in Millisekunden.
    alter = jetzt - float(gestempelt) / 1000.0
    return alter < VERWAIST_NACH_SEKUNDEN


def _sessions_verzeichnis() -> Path:
    return Path(os.environ.get("CLAUDE_SESSIONS_DIR",
                               str(Path.home() / ".claude" / "sessions")))


def lies_lokale_sessions(verzeichnis: Path | None = None,
                         jetzt: float | None = None) -> list[dict]:
    """Die Sessions dieses Rechners, in der Form des Serverdienstes.

    Fehlt das Verzeichnis oder ist eine Datei unlesbar, kommt eine leere Liste
    beziehungsweise ein Eintrag weniger zurück: Diese Quelle ist eine Zugabe und
    darf die Nutzungszahlen des Geräts nie gefährden.
    """
    ordner = verzeichnis or _sessions_verzeichnis()
    jetzt = time.time() if jetzt is None else jetzt
    try:
        dateien = sorted(ordner.glob("*.json"))
    except OSError as e:
        log.debug("lokale Sessions nicht lesbar: %s", e.__class__.__name__)
        return []

    sessions = []
    for datei in dateien:
        try:
            eintrag = json.loads(datei.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if not isinstance(eintrag, dict):
            continue
        name = str(eintrag.get("name") or "").strip()
        if not name:
            continue
        if not _ist_aktuell(eintrag, jetzt):
            continue
        zustand = ZUSTAND_ABBILDUNG.get(str(eintrag.get("status") or ""))
        gestempelt = eintrag.get("statusUpdatedAt") or eintrag.get("updatedAt") or 0
        sessions.append({
            "name": name,
            "zustand": zustand or "geparkt",
            "seit_sekunden": max(0, int(jetzt - float(gestempelt) / 1000.0)),
        })
    return sessions


def ergaenze(daten: dict | None, lokale: list[dict] | None = None,
             jetzt: float | None = None) -> dict | None:
    """Hängt die lokalen Sessions an die Antwort des Servers.

    Gibt ein NEUES Wörterbuch zurück; `daten` bleibt unangetastet.

    Ohne Serverantwort (`daten` ist None) kommt None zurück — die lokalen
    Sessions werden dann NICHT allein gezeigt. Timo, 2026-09-28: "wenn keine
    Daten vom Server kommen, dann auch No Data anzeigen, wie als wenn nix
    verbunden wäre. Und nicht nur die lokale PC Session." Eine Liste mit einer
    einzigen Session sähe aus wie ein gültiger Stand ("Total 1") und wäre
    keiner; der Leerzustand sagt ehrlich, dass der Server fehlt. Bis
    2026-09-28 entstand hier aus den lokalen Sessions allein eine Antwort.
    """
    if daten is None:
        return None

    sessions_lokal = lies_lokale_sessions(jetzt=jetzt) if lokale is None else lokale
    if not sessions_lokal:
        return dict(daten)

    vorhanden = daten.get("sessions")
    sessions = (list(vorhanden) if isinstance(vorhanden, list) else []) \
        + list(sessions_lokal)

    # Arbeitende zuerst, dann Hintergrund, darunter die zuletzt gewechselten —
    # dieselbe Reihenfolge wie im Sammler, damit die Liste auf dem Gerät nicht
    # je nach Quelle springt.
    RANG = {"arbeitet": 0, "hintergrund": 1}
    sessions.sort(key=lambda s: (RANG.get(s.get("zustand"), 2),
                                 s.get("seit_sekunden", 0),
                                 s.get("name", "")))

    ergebnis = dict(daten)
    ergebnis["sessions"] = sessions
    ergebnis["anzahl"] = {
        "wartet": sum(1 for s in sessions if s.get("zustand") == "wartet"),
        "arbeitet": sum(1 for s in sessions if s.get("zustand") == "arbeitet"),
        "hintergrund": sum(1 for s in sessions if s.get("zustand") == "hintergrund"),
        "geparkt": sum(1 for s in sessions if s.get("zustand") == "geparkt"),
        "gesamt": len(sessions),
    }
    return ergebnis
