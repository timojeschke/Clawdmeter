#!/usr/bin/env python3
"""Tests fuer die lokalen Sessions — die Sessions auf Timos eigenem PC.

Warum das ueberhaupt getestet wird: Die Quelle ist ein Verzeichnis voller
Zustandsdateien, von denen manche Ueberbleibsel toter Prozesse sind. Eine tote
Session als "laeuft gerade" anzuzeigen, ist genau der Fehler, den die Anzeige
seit dem 2026-09-25 vermeiden soll.

Lauf: python -m pytest daemon/tests/test_lokale_sessions.py -x -q
"""
import json
import os
import subprocess
import sys
import time

import pytest

from daemon.lokale_sessions import ergaenze, lies_lokale_sessions


def _schreibe(ordner, pid, name, status, alter_sekunden=0):
    stempel = int((time.time() - alter_sekunden) * 1000)
    (ordner / f"{pid}.json").write_text(json.dumps({
        "pid": pid,
        "name": name,
        "status": status,
        "statusUpdatedAt": stempel,
        "updatedAt": stempel,
    }), encoding="utf-8")


@pytest.fixture
def tote_pid():
    """Eine PID, die sicher niemandem mehr gehoert: eben beendet."""
    p = subprocess.Popen([sys.executable, "-c", "pass"])
    p.wait()
    return p.pid


@pytest.mark.parametrize("status, erwartet", [
    ("busy", "arbeitet"),
    ("shell", "geparkt"),      # fertig, nur ein Hintergrundprozess lebt (2026-09-27)
    ("waiting", "wartet"),
    ("idle", "geparkt"),
    ("etwas neues", "geparkt"),  # unbekannter Wert darf nichts behaupten
])
def test_status_wird_uebersetzt(tmp_path, status, erwartet):
    _schreibe(tmp_path, os.getpid(), "Privat - Clawdmeter", status)
    sessions = lies_lokale_sessions(tmp_path)
    assert [s["zustand"] for s in sessions] == [erwartet]


def test_session_ohne_lebenden_prozess_faellt_weg(tmp_path, tote_pid):
    _schreibe(tmp_path, os.getpid(), "lebt", "busy")
    _schreibe(tmp_path, tote_pid, "tot", "busy")
    namen = [s["name"] for s in lies_lokale_sessions(tmp_path)]
    assert namen == ["lebt"]


def test_unlesbare_datei_kostet_nur_diesen_eintrag(tmp_path):
    _schreibe(tmp_path, os.getpid(), "heil", "busy")
    (tmp_path / "kaputt.json").write_text("{das ist kein JSON", encoding="utf-8")
    assert [s["name"] for s in lies_lokale_sessions(tmp_path)] == ["heil"]


def test_fehlendes_verzeichnis_liefert_leere_liste(tmp_path):
    assert lies_lokale_sessions(tmp_path / "gibtsnicht") == []


def test_lokale_session_kommt_zur_serverantwort_dazu():
    server = {
        "frisch": True,
        "alter_sekunden": 2,
        "anzahl": {"wartet": 1, "arbeitet": 1, "geparkt": 27, "gesamt": 29},
        "sessions": [
            {"name": "Privat - Clawdmeter", "zustand": "arbeitet", "seit_sekunden": 5},
            {"name": "TJCreate - Orga", "zustand": "wartet", "seit_sekunden": 60},
        ],
    }
    ergebnis = ergaenze(server, lokale=[
        {"name": "Timo-PC", "zustand": "arbeitet", "seit_sekunden": 1},
    ])

    assert ergebnis["anzahl"] == {"wartet": 1, "arbeitet": 2,
                                  "geparkt": 0, "gesamt": 3}
    assert [s["name"] for s in ergebnis["sessions"]][:2] == [
        "Timo-PC", "Privat - Clawdmeter"]
    # Die Serverantwort selbst bleibt unangetastet — der Aufrufer haelt sie noch.
    assert server["anzahl"]["gesamt"] == 29


def test_ohne_server_wird_die_pc_session_nicht_allein_gezeigt():
    """Kein Server, eine lokale Session: Das Geraet soll den Leerzustand zeigen.

    Timo, 2026-09-28: "wenn keine Daten vom Server kommen, dann auch No Data
    anzeigen, wie als wenn nix verbunden waere. Und nicht nur die lokale PC
    Session." Eine Liste mit einer einzigen Session saehe aus wie ein gueltiger
    Stand ("Total 1") und waere keiner.
    """
    assert ergaenze(None, lokale=[
        {"name": "Timo-PC", "zustand": "arbeitet", "seit_sekunden": 3},
    ]) is None


def test_ohne_lokale_sessions_bleibt_die_serverantwort_inhaltlich_gleich():
    server = {"anzahl": {"wartet": 0, "arbeitet": 1, "geparkt": 2, "gesamt": 3},
              "sessions": [{"name": "A", "zustand": "arbeitet", "seit_sekunden": 1}]}
    assert ergaenze(server, lokale=[]) == server
    assert ergaenze(None, lokale=[]) is None
