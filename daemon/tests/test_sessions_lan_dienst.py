#!/usr/bin/env python3
"""Tests fuer den LAN-Lesedienst auf dem zweiten PC.

Warum das getestet wird: Der Dienst haengt im Heimnetz an einem offenen Port.
Er darf nur mit dem richtigen Token antworten, nie das Token oder die Geheimnisse
(`<pid>.<hash>.key`) aus dem Sessionsordner preisgeben und muss auf dem Ziel-PC
auch mit `python -I` starten. Getestet wird gegen einen echten Server auf
127.0.0.1, nicht gegen Attrappen.

Lauf: python -m pytest daemon/tests/test_sessions_lan_dienst.py -x -q
"""
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from daemon import sessions_lan_dienst as dienst

TOKEN = "t" * 43
DAEMON_ORDNER = Path(dienst.__file__).resolve().parent


def _schreibe(ordner, pid, name, status):
    stempel = int(time.time() * 1000)
    (ordner / f"{pid}.json").write_text(json.dumps({
        "pid": pid, "name": name, "status": status,
        "statusUpdatedAt": stempel, "updatedAt": stempel,
        "peerToken": "GEHEIM-IN-DER-JSON",
    }), encoding="utf-8")


@pytest.fixture
def server(tmp_path):
    ordner = tmp_path / "sessions"
    ordner.mkdir()
    srv = dienst.SessionsDienst(("127.0.0.1", 0), TOKEN, ordner)
    thread = threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
    thread.start()
    srv.sessions_ordner = ordner
    yield srv
    srv.shutdown()
    srv.server_close()
    thread.join(timeout=5)


def _anfrage(srv, methode="GET", pfad="/sessions", header=None):
    verbindung = http.client.HTTPConnection("127.0.0.1", srv.server_address[1],
                                            timeout=5)
    try:
        verbindung.request(methode, pfad, headers=header or {})
        antwort = verbindung.getresponse()
        return antwort.status, antwort.read(), antwort
    finally:
        verbindung.close()


def test_richtiges_token_liefert_genau_die_drei_felder(server):
    _schreibe(server.sessions_ordner, os.getpid(), "Autodarts - Remote", "busy")
    status, koerper, antwort = _anfrage(server, header={"X-Clawd-Token": TOKEN})
    assert status == 200
    assert antwort.getheader("Content-Type") == "application/json"
    daten = json.loads(koerper)
    assert list(daten) == ["sessions"]
    assert len(daten["sessions"]) == 1
    assert set(daten["sessions"][0]) == {"name", "zustand", "seit_sekunden"}
    assert daten["sessions"][0]["name"] == "Autodarts - Remote"
    assert daten["sessions"][0]["zustand"] == "arbeitet"
    assert b"GEHEIM" not in koerper


def test_key_dateien_im_sessionsordner_bleiben_unberuehrt(server):
    """`<pid>.<hash>.key` enthaelt Geheimnisse (peerToken) — nur *.json wird gelesen."""
    _schreibe(server.sessions_ordner, os.getpid(), "Sichtbar", "busy")
    (server.sessions_ordner / f"{os.getpid()}.abc123.key").write_text(
        json.dumps({"pid": os.getpid(), "name": "LeakName", "status": "busy",
                    "peerToken": "GEHEIMER-WERT-4711",
                    "updatedAt": int(time.time() * 1000)}), encoding="utf-8")
    status, koerper, _ = _anfrage(server, header={"X-Clawd-Token": TOKEN})
    assert status == 200
    assert b"GEHEIMER-WERT-4711" not in koerper
    assert b"LeakName" not in koerper
    assert [s["name"] for s in json.loads(koerper)["sessions"]] == ["Sichtbar"]


@pytest.mark.parametrize("header", [
    {},
    {"X-Clawd-Token": "falsch"},
    {"X-Clawd-Token": ""},
    {"X-Clawd-Token": TOKEN[:-1]},
    {"X-Clawd-Token": TOKEN + "x"},
    {"X-Clawd-Token": "äöü"},
])
def test_falsches_oder_fehlendes_token_gibt_401_ohne_inhalt(server, header):
    status, koerper, _ = _anfrage(server, header=header)
    assert status == 401
    assert koerper == b""


@pytest.mark.parametrize("pfad", ["/", "/sessions/", "/x", "/sessionsx"])
def test_anderer_pfad_gibt_404(server, pfad):
    status, _, _ = _anfrage(server, pfad=pfad, header={"X-Clawd-Token": TOKEN})
    assert status == 404


@pytest.mark.parametrize("methode", ["POST", "PUT", "DELETE", "HEAD"])
def test_andere_methode_gibt_405(server, methode):
    status, _, _ = _anfrage(server, methode=methode, header={"X-Clawd-Token": TOKEN})
    assert status == 405


def test_fehlende_tokendatei_wird_erzeugt_ohne_den_inhalt_auszugeben(tmp_path, capsys):
    datei = tmp_path / "unterordner" / "lan_sessions_token"
    token = dienst.sorge_fuer_token(datei)

    assert datei.read_text(encoding="utf-8") == token
    assert len(token) >= dienst.MIN_TOKEN_LAENGE
    aus = capsys.readouterr()
    assert str(datei) in aus.out
    assert token not in aus.out + aus.err
    # Ein zweiter Start nimmt dasselbe Token, ohne es zu ueberschreiben.
    assert dienst.sorge_fuer_token(datei) == token


def test_zu_kurzes_token_bricht_ab_ohne_den_wert_zu_nennen(tmp_path):
    datei = tmp_path / "token"
    datei.write_text("kurz-aber-geheim", encoding="utf-8")
    with pytest.raises(SystemExit) as fehler:
        dienst.sorge_fuer_token(datei)
    assert "kurz-aber-geheim" not in str(fehler.value)
    assert "32" in str(fehler.value)


def test_port_und_host_standard():
    args = dienst._argumente([])
    assert args.port == 8791 != 8790
    assert args.host == "0.0.0.0"


def test_dienst_startet_mit_isolierter_python_aus_einem_ordner_mit_beiden_dateien(tmp_path):
    """Der Ziel-PC startet mit -I; ab Python 3.11 fehlt dann der Skriptordner im Pfad."""
    ordner = tmp_path / "dienst"
    ordner.mkdir()
    for datei in ("sessions_lan_dienst.py", "lokale_sessions.py"):
        shutil.copy(DAEMON_ORDNER / datei, ordner / datei)
    ergebnis = subprocess.run(
        [sys.executable, "-I", str(ordner / "sessions_lan_dienst.py"), "--help"],
        capture_output=True, text=True, timeout=30, cwd=tmp_path)
    assert ergebnis.returncode == 0, ergebnis.stderr
    assert "--token-datei" in ergebnis.stdout


def _oeffne_tropf(srv, anzahl, pause=0.1):
    """Verbindungen, die den Kopf nie beenden und alle `pause` Sekunden ein Byte schicken."""
    verbindungen, stop = [], threading.Event()

    def tropfe(sock):
        try:
            sock.sendall(b"GET /sessions HTTP/1.1\r\n")
            while not stop.is_set():
                time.sleep(pause)
                sock.sendall(b"X-A: b\r\n")
        except OSError:
            pass

    threads = []
    for _ in range(anzahl):
        sock = socket.create_connection(("127.0.0.1", srv.server_address[1]), timeout=5)
        verbindungen.append(sock)
        t = threading.Thread(target=tropfe, args=(sock,), daemon=True)
        t.start()
        threads.append(t)
    return verbindungen, threads, stop


def test_tropfverbindungen_halten_den_dienst_nicht_fest(server, monkeypatch):
    """Sicherheitsprüfung 16.11.: Slowloris ohne Token.

    Der Timeout je recv reicht nicht, wenn jede Sekunde ein Byte kommt. Es gibt
    nur 8 Plätze, und jede Verbindung hat eine Gesamtfrist.
    """
    monkeypatch.setattr(dienst, "GESAMTFRIST_SEKUNDEN", 1.0)
    # Neu bauen: Die Plätze entstehen im Konstruktor.
    server.shutdown()
    server.server_close()
    srv = dienst.SessionsDienst(("127.0.0.1", 0), TOKEN, server.sessions_ordner)
    thread = threading.Thread(target=srv.serve_forever,
                              kwargs={"poll_interval": 0.02}, daemon=True)
    thread.start()
    try:
        grundstand = threading.active_count()
        beginn = time.monotonic()
        verbindungen, tropfer, stop = _oeffne_tropf(
            srv, dienst.MAX_GLEICHZEITIGE_VERBINDUNGEN)
        time.sleep(0.3)

        # Die neunte Verbindung wird sofort geschlossen, nicht in die Warteschlange gelegt.
        neunte = socket.create_connection(("127.0.0.1", srv.server_address[1]), timeout=3)
        neunte.settimeout(0.9)
        try:
            assert neunte.recv(1) == b""
        except ConnectionResetError:
            pass
        neunte.close()

        # Nach der Gesamtfrist sind alle Tropfverbindungen getrennt, obwohl sie weiter senden.
        for t in tropfer:
            t.join(timeout=4)
        stop.set()
        assert not any(t.is_alive() for t in tropfer)
        assert time.monotonic() - beginn < 5
        for sock in verbindungen:
            sock.close()

        # Danach antwortet der Dienst wieder normal ...
        status, _, _ = _anfrage(srv, header={"X-Clawd-Token": TOKEN})
        assert status == 200
        # ... und die Threads sind wieder weg.
        ende = time.monotonic() + 3
        while threading.active_count() > grundstand and time.monotonic() < ende:
            time.sleep(0.05)
        assert threading.active_count() <= grundstand
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join(timeout=5)


def test_untergeschobene_stdlib_datei_im_skriptordner_wird_nie_geladen(tmp_path):
    """Sicherheitsprüfung 16.11.: Der Skriptordner kommt ans ENDE von sys.path."""
    ordner = tmp_path / "dienst"
    ordner.mkdir()
    for datei in ("sessions_lan_dienst.py", "lokale_sessions.py"):
        shutil.copy(DAEMON_ORDNER / datei, ordner / datei)
    (ordner / "ctypes.py").write_text("raise RuntimeError('untergeschoben')\n",
                                      encoding="utf-8")
    code = ("import runpy, sys; "
            f"runpy.run_path({str(ordner / 'sessions_lan_dienst.py')!r}, run_name='x'); "
            "import ctypes; print(ctypes.__file__)")
    ergebnis = subprocess.run([sys.executable, "-I", "-c", code],
                              capture_output=True, text=True, timeout=30, cwd=tmp_path)
    assert ergebnis.returncode == 0, ergebnis.stderr
    assert str(ordner) not in ergebnis.stdout
