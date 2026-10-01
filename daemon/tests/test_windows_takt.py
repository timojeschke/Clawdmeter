#!/usr/bin/env python3
"""Verhalten des Session-Takts, der Poll-Planung und der Eingabehaertung.

Hintergrund (Review 2026-10-01): Ohne Planung lief der Poll nach einem
Fehlschlag bei jedem Schleifendurchlauf; der Session-Takt wiederholte die
eingefrorene Nutzung mit `ok:true`; die Konfig scheiterte an BOM, cp1252 und
einem `#` im Token; fremde Antworten konnten connect_and_run abstuerzen lassen.

Die Schleife selbst braucht BLE und wird hier nicht getrieben — getestet sind
die reinen Entscheidungen, die sie aufruft.
"""
import asyncio
import json
from unittest.mock import AsyncMock, MagicMock

import httpx
import pytest

import daemon.claude_usage_daemon_windows as mod
from daemon.claude_usage_daemon_windows import (
    HERZSCHLAG_S,
    NUTZUNG_GILT_S,
    POLL_INTERVAL,
    POLL_WIEDERHOLUNG_NACH_FEHLER_S,
    Session,
    basis_nutzlast,
    naechster_poll_zeitpunkt,
    poll_scoped_limit,
    read_token,
    sendung_faellig,
)
from daemon.lokale_sessions import lies_lokale_sessions
from daemon.sessions_source import merge_into_payload, read_sessions_config

UHR = {"t": 1_000_000, "tf": 24}
NUTZUNG = {"s": 40, "sr": 120, "w": 10, "wr": 5000, "st": "allowed",
           "acct": "pro", "ok": True, "t": 5_000, "tf": 24}


# ---------------------------------------------------------------------------
# Poll-Planung
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("gelungen, abstand", [
    (True, POLL_INTERVAL),
    (False, POLL_WIEDERHOLUNG_NACH_FEHLER_S),
])
def test_naechster_poll_haengt_vom_ausgang_des_versuchs_ab(gelungen, abstand):
    assert naechster_poll_zeitpunkt(1000.0, gelungen) == 1000.0 + abstand


def test_fehlschlag_wird_nicht_im_sekundentakt_wiederholt():
    # Regression 2026-10-01: last_poll blieb nach einem Fehler stehen, der Poll
    # lief dann bei jedem Durchlauf (~1 s) mit einer Logzeile je Versuch.
    assert naechster_poll_zeitpunkt(0.0, False) > 1.0


# ---------------------------------------------------------------------------
# Basis des Session-Takts
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("vergangen, t, sr, wr", [
    (0, 5_000, 120, 5000),
    (59, 5_059, 120, 5000),     # unter einer Minute: Restzeit unveraendert
    (125, 5_125, 118, 4998),    # zwei ganze Minuten
    (NUTZUNG_GILT_S, 5_150, 118, 4998),   # die Grenze selbst gilt noch
])
def test_frische_nutzung_wird_mit_der_zeit_fortgeschrieben(vergangen, t, sr, wr):
    basis = basis_nutzlast(NUTZUNG, 100.0, 100.0 + vergangen, UHR)
    assert (basis["ok"], basis["t"], basis["sr"], basis["wr"]) == (True, t, sr, wr)
    assert basis["s"] == 40 and basis["w"] == 10


def test_restzeit_faellt_nie_unter_null():
    nutzung = {**NUTZUNG, "sr": 1, "wr": 0}
    basis = basis_nutzlast(nutzung, 0.0, 140.0, UHR)
    assert (basis["sr"], basis["wr"]) == (0, 0)


def test_basis_veraendert_die_gemerkte_nutzung_nicht():
    vorher = dict(NUTZUNG)
    basis_nutzlast(NUTZUNG, 0.0, 130.0, UHR)
    assert NUTZUNG == vorher


@pytest.mark.parametrize("nutzung, erfolg, jetzt", [
    (NUTZUNG, 0.0, NUTZUNG_GILT_S + 1),   # veraltet
    (NUTZUNG, 100.0, 50.0),               # Uhr zurueckgestellt
    (None, None, 10.0),                   # nie gepollt
    (None, 0.0, 10.0),                    # nach 401/ohne Token verworfen
    (NUTZUNG, None, 10.0),
])
def test_ohne_gueltige_nutzung_kommt_ok_false_mit_uhr(nutzung, erfolg, jetzt):
    assert basis_nutzlast(nutzung, erfolg, jetzt, UHR) == {"ok": False, **UHR}


def test_ohne_uhr_in_der_konfig_bleibt_nur_ok_false():
    assert basis_nutzlast(None, None, 10.0, {}) == {"ok": False}


def test_fehlende_oder_unsinnige_zeitfelder_werden_nicht_erfunden():
    nutzung = {"s": 1, "ok": True, "sr": -1, "wr": "bald"}
    basis = basis_nutzlast(nutzung, 0.0, 130.0, UHR)
    assert basis == nutzung


# ---------------------------------------------------------------------------
# Wann der Takt sendet
# ---------------------------------------------------------------------------

GESENDET = {**NUTZUNG, "sw": 1, "sa": 2}


@pytest.mark.parametrize("neu, zuletzt, seit_schreiben, erwartet", [
    # nur Zeitfelder laufen weiter: nichts zu senden
    ({**GESENDET, "t": 9_999, "sr": 100, "wr": 4000}, GESENDET, 5, False),
    # eine Session-Zahl aendert sich
    ({**GESENDET, "sa": 3}, GESENDET, 5, True),
    # die Nutzung kippt auf ok:false
    ({"ok": False, "sw": 1, "sa": 2}, GESENDET, 5, True),
    # nichts geaendert, aber der Herzschlag ist faellig
    (GESENDET, GESENDET, HERZSCHLAG_S, True),
    (GESENDET, GESENDET, HERZSCHLAG_S - 1, False),
    # noch nie etwas gesendet
    (GESENDET, None, 0, True),
])
def test_takt_sendet_bei_aenderung_oder_herzschlag(neu, zuletzt, seit_schreiben,
                                                   erwartet):
    jetzt = 1000.0
    assert sendung_faellig(neu, zuletzt, jetzt, jetzt - seit_schreiben) is erwartet


# ---------------------------------------------------------------------------
# Konfig lesen: BOM, cp1252, "#" im Token
# ---------------------------------------------------------------------------

BOM = b"\xef\xbb\xbf"


@pytest.mark.parametrize("rohbytes", [
    BOM + b"sessions_url = https://x.invalid/s\r\nsessions_token = ab#cd\r\n",
    "# Küche und Büro\nsessions_url = https://x.invalid/s\nsessions_token = ab#cd\n"
    .encode("cp1252"),
    b"sessions_url=https://x.invalid/s  # Kommentar\nsessions_token = ab#cd # Rest\n",
    b"  # nur Kommentar\nsessions_url = https://x.invalid/s\nsessions_token = ab#cd\n",
])
def test_konfig_ueberlebt_bom_cp1252_und_raute_im_token(tmp_path, rohbytes):
    datei = tmp_path / "config"
    datei.write_bytes(rohbytes)
    assert read_sessions_config(datei) == ("https://x.invalid/s", "ab#cd")


@pytest.mark.parametrize("rohbytes, chime, clock", [
    (BOM + b"chime = on\nclock = 24\n", "on", "24"),
    ("# Größe\nchime = on\nclock = auto\n".encode("cp1252"), "on", "auto"),
    (b"chime = on # laut\nclock = 12\n", "on", "12"),
    (b"chime = vielleicht\n", "off", "off"),
])
def test_chime_und_clock_lesen_dieselbe_robuste_konfig(tmp_path, monkeypatch,
                                                       rohbytes, chime, clock):
    datei = tmp_path / "config"
    datei.write_bytes(rohbytes)
    monkeypatch.setattr(mod, "CONFIG_FILE", datei)
    assert (mod.read_chime_setting(), mod.read_clock_setting()) == (chime, clock)


def test_nicht_lesbare_konfig_faellt_auf_die_standards(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path)   # ein Verzeichnis
    assert (mod.read_chime_setting(), mod.read_clock_setting()) == ("off", "off")


# ---------------------------------------------------------------------------
# Fremddaten
# ---------------------------------------------------------------------------

def _scoped(modell="Fable", prozent=42):
    return {"limits": [{"kind": "weekly_scoped", "percent": prozent,
                        "scope": {"model": {"display_name": modell}}}]}


@pytest.mark.parametrize("koerper, erwartet", [
    (_scoped(), ("Fable", 42)),
    (None, None),
    ([], None),
    ([_scoped()], None),
    ({"limits": "viele"}, None),
    ({"limits": None}, None),
    ({"limits": [{"kind": "weekly_scoped", "percent": 1, "scope": []}]}, None),
    ({"limits": [{"kind": "weekly_scoped", "percent": 1, "scope": {"model": "x"}}]},
     None),
    ({"limits": [{"kind": "weekly_scoped", "percent": float("inf"),
                  "scope": {"model": {"display_name": "Fable"}}}]}, None),
])
def test_scoped_limit_ueberlebt_fremde_antworten(monkeypatch, koerper, erwartet):
    echt = httpx.AsyncClient
    transport = httpx.MockTransport(
        lambda _anfrage: httpx.Response(200, content=json.dumps(koerper)))
    monkeypatch.setattr(
        httpx, "AsyncClient", lambda **kw: echt(transport=transport, **kw))
    assert asyncio.run(poll_scoped_limit("tok")) == erwartet


@pytest.mark.parametrize("anzahl", [
    {"wartet": None, "arbeitet": "viele", "hintergrund": [], "geparkt": float("nan")},
    {"wartet": True, "arbeitet": {}, "hintergrund": None, "geparkt": None},
])
def test_unbrauchbare_zaehlwerte_zaehlen_null(anzahl):
    merged = merge_into_payload({"ok": True}, {"anzahl": anzahl, "frisch": True})
    assert (merged["sw"], merged["sa"], merged["sb"], merged["sg"]) == (0, 0, 0, 0)


def test_pid_ohne_zahl_ueberspringt_nur_diese_datei(tmp_path):
    import os
    jetzt_ms = 10**13
    (tmp_path / "a.json").write_text(json.dumps(
        {"pid": "abc", "name": "kaputt", "status": "busy",
         "statusUpdatedAt": jetzt_ms}), encoding="utf-8")
    (tmp_path / "b.json").write_text(json.dumps(
        {"pid": os.getpid(), "name": "heil", "status": "busy",
         "statusUpdatedAt": jetzt_ms}), encoding="utf-8")
    assert [s["name"] for s in lies_lokale_sessions(tmp_path)] == ["heil"]


def test_sendeweg_ueberlebt_nicht_kodierbare_zeichen():
    client = MagicMock()
    client.write_gatt_char = AsyncMock()
    client.mtu_size = 247
    # Ein einzelnes Surrogat (kaputter Sessionname) ist in UTF-8 nicht kodierbar.
    ok = asyncio.run(Session(client)._sende({"sn": "Sess\ud800ion", "ok": True}))
    assert ok is True
    client.write_gatt_char.assert_awaited_once()


# ---------------------------------------------------------------------------
# Token: kaputte erste Datei blockiert den naechsten Kandidaten nicht
# ---------------------------------------------------------------------------

def test_kaputte_credentials_datei_ueberspringt_zum_naechsten_kandidaten(
        tmp_path, monkeypatch):
    kaputt = tmp_path / "a.json"
    kaputt.write_text('{"claudeAiOauth": {"accessTo', encoding="utf-8")
    heil = tmp_path / "b.json"
    heil.write_text(json.dumps({"accessToken": "sk-ant-test-ZWEITE"}))
    monkeypatch.delenv("CLAWDMETER_OAUTH_TOKEN", raising=False)
    monkeypatch.setattr(mod, "_windows_credential_candidates",
                        lambda: [kaputt, heil])
    assert read_token() == "sk-ant-test-ZWEITE"
