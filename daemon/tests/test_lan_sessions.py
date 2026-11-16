#!/usr/bin/env python3
"""Tests fuer die LAN-Sessions auf Timos PC — Abruf vom zweiten PC.

Warum das getestet wird: Die Antwort kommt aus dem Netz und wird nie vorab
geprueft. Ein kaputter Eintrag, ein toter oder schlafender PC darf weder den
Sendeweg kippen noch die Nutzungszahlen mitreissen. Abgerufen wird gegen den
echten Lesedienst auf 127.0.0.1; fuer kaputte Antworten gegen einen minimalen
echten Testserver.

Lauf: python -m pytest daemon/tests/test_lan_sessions.py -x -q
"""
import asyncio
import json
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import httpx
import pytest

from daemon import lan_sessions, sessions_lan_dienst as dienst
from daemon.lan_sessions import (hole_alle_lan_sessions, hole_lan_sessions,
                                 lies_lan_konfig)
from daemon.lokale_sessions import ergaenze

TOKEN = "t" * 43


def _abruf(url, token=TOKEN, funktion=hole_lan_sessions):
    async def lauf():
        async with httpx.AsyncClient() as http:
            return await funktion(http, url, token)
    return asyncio.run(lauf())


def _starte(srv):
    thread = threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
    thread.start()
    return thread


def _stoppe(srv, thread):
    srv.shutdown()
    srv.server_close()
    thread.join(timeout=5)


def _url(srv):
    return f"http://127.0.0.1:{srv.server_address[1]}/sessions"


@pytest.fixture
def lesedienst(tmp_path):
    ordner = tmp_path / "sessions"
    ordner.mkdir()
    srv = dienst.SessionsDienst(("127.0.0.1", 0), TOKEN, ordner)
    thread = _starte(srv)
    srv.sessions_ordner = ordner
    yield srv
    _stoppe(srv, thread)


@pytest.fixture
def testserver():
    """Ein Server, der genau die Antwort schickt, die der Test vorgibt."""
    antwort = {"status": 200, "koerper": b"{}"}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            self.send_response(antwort["status"])
            self.send_header("Content-Length", str(len(antwort["koerper"])))
            self.end_headers()
            self.wfile.write(antwort["koerper"])

        def log_message(self, *args):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = _starte(srv)
    srv.antwort = antwort
    yield srv
    _stoppe(srv, thread)


def _gut(**aenderung):
    return {"name": "Autodarts - Remote", "zustand": "arbeitet",
            "seit_sekunden": 12, **aenderung}


def test_gueltige_antwort_des_echten_dienstes_wird_uebernommen(lesedienst):
    stempel = int(time.time() * 1000)
    (lesedienst.sessions_ordner / f"{os.getpid()}.json").write_text(json.dumps({
        "pid": os.getpid(), "name": "Autodarts - Remote", "status": "waiting",
        "statusUpdatedAt": stempel, "updatedAt": stempel}), encoding="utf-8")
    sessions = _abruf(_url(lesedienst))
    assert [(s["name"], s["zustand"]) for s in sessions] == [
        ("Autodarts - Remote", "wartet")]
    assert set(sessions[0]) == {"name", "zustand", "seit_sekunden"}


def test_falsches_token_gibt_leere_liste(lesedienst):
    assert _abruf(_url(lesedienst), token="x" * 43) == []


@pytest.mark.parametrize("eintrag", [
    "text", 5, None, [], {},
    _gut(name=5), _gut(name=None), _gut(name=""), _gut(name="   "),
    _gut(name="n" * 65),
    _gut(zustand="laeuft"), _gut(zustand=None), _gut(zustand="Arbeitet"),
    _gut(seit_sekunden=True), _gut(seit_sekunden="5"), _gut(seit_sekunden=None),
    _gut(seit_sekunden=-1), _gut(seit_sekunden=float("inf")),
    _gut(seit_sekunden=float("nan")),
])
def test_kaputter_eintrag_faellt_einzeln_weg(testserver, eintrag):
    testserver.antwort["koerper"] = json.dumps(
        {"sessions": [_gut(name="vorher"), eintrag, _gut(name="nachher")]}
    ).encode()
    assert [s["name"] for s in _abruf(_url(testserver))] == ["vorher", "nachher"]


def test_eintrag_wird_bereinigt_und_auf_die_drei_felder_gekuerzt(testserver):
    testserver.antwort["koerper"] = json.dumps({"sessions": [
        _gut(name="  Name  ", seit_sekunden=7.9, peerToken="GEHEIM", pid=4)]}).encode()
    assert _abruf(_url(testserver)) == [
        {"name": "Name", "zustand": "arbeitet", "seit_sekunden": 7}]


def test_hoechstens_50_eintraege_je_quelle(testserver):
    testserver.antwort["koerper"] = json.dumps(
        {"sessions": [_gut(name=f"S{i}") for i in range(80)]}).encode()
    assert len(_abruf(_url(testserver))) == 50


@pytest.mark.parametrize("status, koerper", [
    (500, b'{"sessions": []}'),
    (404, b""),
    (200, b"kein json"),
    (200, b"[]"),
    (200, b'{"sessions": "viele"}'),
    (200, b'{"andere": []}'),
    (200, b"null"),
    (200, b'{"sessions": [' + b'{"name":"x"},' * 40000 + b'{"name":"x"}]}'),
])
def test_fremde_antworten_ergeben_leere_liste(testserver, status, koerper):
    testserver.antwort.update(status=status, koerper=koerper)
    assert _abruf(_url(testserver)) == []


def test_geschlossener_port_ergibt_leere_liste_ohne_ausnahme():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    assert _abruf(f"http://127.0.0.1:{port}/sessions") == []


def test_schweigender_pc_haelt_hoechstens_das_timeout_auf(monkeypatch):
    """Ein schlafender PC nimmt Verbindungen an oder gar nicht — er antwortet nie."""
    monkeypatch.setattr(lan_sessions, "LAN_TIMEOUT_SEKUNDEN", 0.3)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        url = f"http://127.0.0.1:{s.getsockname()[1]}/sessions"
        beginn = time.monotonic()
        assert _abruf(url) == []
    assert time.monotonic() - beginn < 3


def test_token_steht_nicht_im_debug_log(testserver, caplog):
    caplog.set_level("DEBUG")
    testserver.antwort["status"] = 500
    _abruf(_url(testserver), token="geheim-" + "g" * 40)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    _abruf(f"http://127.0.0.1:{port}/sessions", token="geheim-" + "g" * 40)
    assert caplog.records
    assert "geheim-" not in caplog.text


def test_mehrere_urls_werden_zusammengefuehrt_und_eine_tote_stoert_nicht(
        lesedienst, testserver):
    testserver.antwort["koerper"] = json.dumps(
        {"sessions": [_gut(name="zweiter PC")]}).encode()
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        tot = f"http://127.0.0.1:{s.getsockname()[1]}/sessions"

    async def lauf():
        async with httpx.AsyncClient() as http:
            return await hole_alle_lan_sessions(
                http, [tot, _url(testserver)], TOKEN)
    assert [s["name"] for s in asyncio.run(lauf())] == ["zweiter PC"]


def test_ohne_urls_oder_token_wird_nichts_abgerufen():
    async def lauf(urls, token):
        async with httpx.AsyncClient() as http:
            return await hole_alle_lan_sessions(http, urls, token)
    assert asyncio.run(lauf([], TOKEN)) == []
    assert asyncio.run(lauf(["http://127.0.0.1:1/sessions"], None)) == []


# --- Sicherheitsprüfung 16.11. ----------------------------------------------

def _eigener_server(do_get):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            try:
                do_get(self)
            except OSError:
                pass

        def log_message(self, *args):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    return srv, _starte(srv)


def test_tropfender_server_haelt_den_abruf_nicht_ueber_die_gesamtfrist(monkeypatch):
    """Sicherheitsprüfung 16.11.: Das httpx-Timeout gilt je Phase, nicht für alles."""
    monkeypatch.setattr(lan_sessions, "LAN_TIMEOUT_SEKUNDEN", 0.5)

    def tropfe(h):
        h.send_response(200)
        h.send_header("Content-Length", "100000")
        h.end_headers()
        for _ in range(100):
            h.wfile.write(b" ")
            h.wfile.flush()
            time.sleep(0.1)

    srv, thread = _eigener_server(tropfe)
    try:
        beginn = time.monotonic()
        assert _abruf(_url(srv)) == []
        assert time.monotonic() - beginn < 2.5
    finally:
        _stoppe(srv, thread)


def test_gzip_wird_nicht_entpackt_und_identity_wird_verlangt():
    """Sicherheitsprüfung 16.11.: Eine gzip-Bombe darf nie vor der Größenprüfung entpackt werden."""
    import gzip
    gesehen = {}
    nutzlast = gzip.compress(json.dumps({"sessions": [_gut()]}).encode())

    def sende_gzip(h):
        gesehen["encoding"] = h.headers.get("Accept-Encoding")
        h.send_response(200)
        h.send_header("Content-Encoding", "gzip")
        h.send_header("Content-Length", str(len(nutzlast)))
        h.end_headers()
        h.wfile.write(nutzlast)

    srv, thread = _eigener_server(sende_gzip)
    try:
        assert _abruf(_url(srv)) == []
    finally:
        _stoppe(srv, thread)
    assert gesehen["encoding"] == "identity"


def test_zu_grosse_antwort_ohne_laengenangabe_bricht_beim_lesen_ab():
    """Sicherheitsprüfung 16.11.: Gezählt wird beim Lesen, nicht erst danach."""
    gesendet = {"bytes": 0}

    def sende_endlos(h):
        h.send_response(200)
        h.send_header("Connection", "close")
        h.end_headers()
        stueck = b" " * 65536
        for _ in range(2000):
            h.wfile.write(stueck)
            gesendet["bytes"] += len(stueck)

    srv, thread = _eigener_server(sende_endlos)
    try:
        assert _abruf(_url(srv)) == []
    finally:
        _stoppe(srv, thread)
    assert gesendet["bytes"] < 50 * 1024 * 1024


def test_weiterleitung_wird_nicht_verfolgt():
    """Sicherheitsprüfung 16.11.: Kein Folgen von Weiterleitungen (und kein Token an Dritte)."""
    ziel_gesehen = []

    def ziel(h):
        ziel_gesehen.append(h.headers.get("X-Clawd-Token"))
        h.send_response(200)
        body = json.dumps({"sessions": [_gut()]}).encode()
        h.send_header("Content-Length", str(len(body)))
        h.end_headers()
        h.wfile.write(body)

    ziel_srv, ziel_thread = _eigener_server(ziel)

    def leite_um(h):
        h.send_response(302)
        h.send_header("Location", _url(ziel_srv))
        h.send_header("Content-Length", "0")
        h.end_headers()

    srv, thread = _eigener_server(leite_um)
    try:
        assert _abruf(_url(srv)) == []
    finally:
        _stoppe(srv, thread)
        _stoppe(ziel_srv, ziel_thread)
    assert ziel_gesehen == []


@pytest.mark.parametrize("eintrag", [
    _gut(name="\ud800x"),            # einzelnes Surrogat: nicht als UTF-8 schreibbar
    _gut(name="a\nb"),               # Steuerzeichen
    _gut(name="a\u202eb"),           # Umkehr der Schreibrichtung
    _gut(seit_sekunden=10**400),     # passt in kein float
    _gut(seit_sekunden=10**9 + 1),
])
def test_sicherheitspruefung_fremde_eintraege_fallen_einzeln_weg(testserver, eintrag):
    testserver.antwort["koerper"] = json.dumps(
        {"sessions": [_gut(name="vorher"), eintrag, _gut(name="nachher")]}
    ).encode()
    sessions = _abruf(_url(testserver))
    assert [s["name"] for s in sessions] == ["vorher", "nachher"]
    for s in sessions:
        s["name"].encode("utf-8")


def test_tiefe_verschachtelung_ergibt_leere_liste(testserver):
    """Sicherheitsprüfung 16.11.: Rekursionsfehler im JSON-Parser kippen nichts."""
    testserver.antwort["koerper"] = b"[" * 100000 + b"]" * 100000
    assert _abruf(_url(testserver)) == []


def test_unerwarteter_fehler_im_lan_teil_kostet_nur_den_lan_teil(
        daemon_modul, tmp_path, monkeypatch):
    """Sicherheitsprüfung 16.11.: Zweite Absicherung in add_session_fields_paar.

    Der Fehler wird in die LAN-Abfrage eingebaut (eine der wenigen Stellen, an
    denen sich so etwas nicht mit echtem Netz auslösen lässt); der Server ist echt.
    """
    leer = tmp_path / "eigene"
    leer.mkdir()
    monkeypatch.setenv("CLAUDE_SESSIONS_DIR", str(leer))

    async def kaputt(*args, **kwargs):
        raise RuntimeError("unerwartet")
    monkeypatch.setattr(daemon_modul, "hole_alle_lan_sessions", kaputt)
    server, thread = _server_dienst(_server_antwort())
    try:
        monkeypatch.setattr(daemon_modul, "CONFIG_FILE", _konfig(
            tmp_path, f"sessions_url = {_url(server)}\nsessions_token = serverx\n"
                      "lan_sessions_url = http://127.0.0.1:1/sessions\n"
                      f"lan_sessions_token = {TOKEN}\n"))
        gross, _ = asyncio.run(daemon_modul.add_session_fields_paar({"ok": True}))
    finally:
        _stoppe(server, thread)
    assert gross["sa"] == 1
    assert gross["sn"] == ["Server"]


# --- Konfig ---------------------------------------------------------------

def _konfig(tmp_path, text):
    datei = tmp_path / "config"
    datei.write_text(text, encoding="utf-8")
    return datei


def test_konfig_mit_url_und_token(tmp_path):
    datei = _konfig(tmp_path, "lan_sessions_url = http://192.168.2.5:8791/sessions"
                    ", http://192.168.2.6:8791/sessions\n"
                    "lan_sessions_token = abc#def\n")
    assert lies_lan_konfig(datei) == (
        ["http://192.168.2.5:8791/sessions", "http://192.168.2.6:8791/sessions"],
        "abc#def")


@pytest.mark.parametrize("text", [
    "lan_sessions_url = http://192.168.2.5:8791/sessions\n",
    "lan_sessions_token = abc\n",
    "lan_sessions_url = \nlan_sessions_token = abc\n",
    "lan_sessions_url = ftp://x/sessions\nlan_sessions_token = abc\n",
    "# lan_sessions_url = http://x/sessions\n# lan_sessions_token = abc\n",
    "",
])
def test_konfig_mit_nur_einem_wert_ist_aus(tmp_path, text):
    assert lies_lan_konfig(_konfig(tmp_path, text)) == ([], None)


def test_fehlende_konfig_ist_aus(tmp_path):
    assert lies_lan_konfig(tmp_path / "gibtsnicht") == ([], None)


# --- Zusammenspiel mit ergaenze -------------------------------------------

def _server_antwort():
    return {"frisch": True, "alter_sekunden": 1,
            "anzahl": {"wartet": 0, "arbeitet": 1, "geparkt": 0, "gesamt": 1},
            "sessions": [{"name": "Server", "zustand": "arbeitet", "seit_sekunden": 5}]}


def test_lan_sessions_zaehlen_in_gesamt_mit():
    lan = [{"name": "Autodarts", "zustand": "arbeitet", "seit_sekunden": 1},
           {"name": "Alt", "zustand": "geparkt", "seit_sekunden": 900}]
    ergebnis = ergaenze(_server_antwort(), lan)
    assert ergebnis["anzahl"]["gesamt"] == 3
    assert ergebnis["anzahl"]["arbeitet"] == 2
    assert ergebnis["anzahl"]["geparkt"] == 1


def test_ohne_serverantwort_bleiben_lan_sessions_allein_ohne_wirkung():
    assert ergaenze(None, [{"name": "Autodarts", "zustand": "arbeitet",
                            "seit_sekunden": 1}]) is None


# --- add_session_fields_paar mit echten Servern ---------------------------

@pytest.fixture
def daemon_modul():
    pytest.importorskip("bleak")
    import daemon.claude_usage_daemon_windows as mod
    return mod


def _server_dienst(koerper):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            roh = json.dumps(koerper).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(roh)))
            self.end_headers()
            self.wfile.write(roh)

        def log_message(self, *args):
            pass
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    return srv, _starte(srv)


def test_add_session_fields_paar_zeigt_lan_sessions(
        daemon_modul, lesedienst, tmp_path, monkeypatch):
    stempel = int(time.time() * 1000)
    (lesedienst.sessions_ordner / f"{os.getpid()}.json").write_text(json.dumps({
        "pid": os.getpid(), "name": "Autodarts - Remote", "status": "busy",
        "statusUpdatedAt": stempel, "updatedAt": stempel}), encoding="utf-8")
    leer = tmp_path / "eigene"
    leer.mkdir()
    monkeypatch.setenv("CLAUDE_SESSIONS_DIR", str(leer))
    server, thread = _server_dienst(_server_antwort())
    try:
        monkeypatch.setattr(daemon_modul, "CONFIG_FILE", _konfig(
            tmp_path, f"sessions_url = {_url(server)}\nsessions_token = serverx\n"
                      f"lan_sessions_url = {_url(lesedienst)}\n"
                      f"lan_sessions_token = {TOKEN}\n"))
        gross, sicher = asyncio.run(daemon_modul.add_session_fields_paar({"ok": True}))
    finally:
        _stoppe(server, thread)
    assert gross["sa"] == 2
    assert set(gross["sn"]) == {"Server", "Autodarts - Remote"}
    assert sicher["sa"] == 2


def test_add_session_fields_paar_ohne_serverantwort_keine_sessionfelder(
        daemon_modul, lesedienst, tmp_path, monkeypatch):
    """Server fehlt, LAN-PC antwortet: weiter gar keine Sessionfelder (Timo, 2026-09-28)."""
    stempel = int(time.time() * 1000)
    (lesedienst.sessions_ordner / f"{os.getpid()}.json").write_text(json.dumps({
        "pid": os.getpid(), "name": "Autodarts", "status": "busy",
        "statusUpdatedAt": stempel, "updatedAt": stempel}), encoding="utf-8")
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        tot = f"http://127.0.0.1:{s.getsockname()[1]}/sessions"
    monkeypatch.setattr(daemon_modul, "CONFIG_FILE", _konfig(
        tmp_path, f"sessions_url = {tot}\nsessions_token = serverx\n"
                  f"lan_sessions_url = {_url(lesedienst)}\n"
                  f"lan_sessions_token = {TOKEN}\n"))
    gross, sicher = asyncio.run(daemon_modul.add_session_fields_paar({"ok": True}))
    assert gross == {"ok": True} == sicher


def test_add_session_fields_paar_ohne_lan_konfig_bleibt_wie_vorher(
        daemon_modul, tmp_path, monkeypatch):
    leer = tmp_path / "eigene"
    leer.mkdir()
    monkeypatch.setenv("CLAUDE_SESSIONS_DIR", str(leer))
    server, thread = _server_dienst(_server_antwort())
    try:
        monkeypatch.setattr(daemon_modul, "CONFIG_FILE", _konfig(
            tmp_path, f"sessions_url = {_url(server)}\nsessions_token = serverx\n"))
        gross, _ = asyncio.run(daemon_modul.add_session_fields_paar({"ok": True}))
    finally:
        _stoppe(server, thread)
    assert gross["sa"] == 1
    assert gross["sn"] == ["Server"]
