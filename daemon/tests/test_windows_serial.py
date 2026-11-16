#!/usr/bin/env python3
"""Tests fuer den Kabelweg (USB seriell) des Windows-Daemons.

Gemockt wird nur an der Grenze: serial.Serial und list_ports.comports.

Run: python -m pytest daemon/tests/test_windows_serial.py -q
"""
import asyncio
import json
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest
import serial as pyserial

import daemon.claude_usage_daemon_windows as mod


def _port(device, vid):
    return SimpleNamespace(device=device, vid=vid)


# ---------------------------------------------------------------------------
# Port-Auswahl
# ---------------------------------------------------------------------------

def test_vid_treffer_waehlt_espressif_port(monkeypatch):
    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)
    monkeypatch.setattr(mod.list_ports, "comports", lambda: [
        _port("COM1", 0x0403), _port("COM7", 0x303A)])
    assert mod.finde_serial_port() == "COM7"


def test_override_gilt_vor_der_suche(monkeypatch):
    monkeypatch.setenv("CLAWDMETER_SERIAL_PORT", "COM3")
    suche = MagicMock(return_value=[_port("COM7", 0x303A)])
    monkeypatch.setattr(mod.list_ports, "comports", suche)
    assert mod.finde_serial_port() == "COM3"
    suche.assert_not_called()


def test_kein_port_gibt_none(monkeypatch):
    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)
    monkeypatch.setattr(mod.list_ports, "comports", lambda: [_port("COM1", 0x0403)])
    assert mod.finde_serial_port() is None


def test_ohne_pyserial_gibt_none(monkeypatch):
    monkeypatch.setattr(mod, "serial", None)
    assert mod.finde_serial_port() is None


# ---------------------------------------------------------------------------
# Oeffnen: DTR/RTS False VOR open(), Port erst danach
# ---------------------------------------------------------------------------

class _FakeSerial:
    """Haelt die Reihenfolge der Zuweisungen fest."""
    def __init__(self):
        object.__setattr__(self, "ereignisse", [])
        object.__setattr__(self, "is_open", False)

    def __setattr__(self, name, wert):
        self.ereignisse.append((name, wert))
        object.__setattr__(self, name, wert)

    def open(self):
        self.ereignisse.append(("open", None))


def test_dtr_rts_false_vor_open(monkeypatch):
    fake = _FakeSerial()
    monkeypatch.setattr(mod.serial, "Serial", lambda *a, **k: fake)
    assert mod.oeffne_serial("COM3") is fake
    namen = [n for n, _ in fake.ereignisse]
    assert ("dtr", False) in fake.ereignisse and ("rts", False) in fake.ereignisse
    assert namen.index("dtr") < namen.index("open")
    assert namen.index("rts") < namen.index("open")
    assert namen.index("port") < namen.index("open")
    assert namen.index("dtr") < namen.index("port")


def test_serial_wird_ohne_port_angelegt(monkeypatch):
    aufrufe = []
    fake = _FakeSerial()
    monkeypatch.setattr(mod.serial, "Serial", lambda *a, **k: aufrufe.append((a, k)) or fake)
    mod.oeffne_serial("COM3")
    assert aufrufe == [((), {})]


# ---------------------------------------------------------------------------
# Senden und Rueckfall
# ---------------------------------------------------------------------------

def test_write_payload_schreibt_eine_zeile():
    ser = MagicMock()
    session = mod.SerialSession(ser)
    payload = {"s": 42, "ok": True}
    assert asyncio.run(session.write_payload(payload)) == payload
    ser.write.assert_called_once_with(b'{"s":42,"ok":true}\n')


def test_schreibfehler_gibt_none_und_schliesst_weg():
    ser = MagicMock()
    ser.write.side_effect = pyserial.SerialException("Gerät weg")
    session = mod.SerialSession(ser)
    assert asyncio.run(session.write_payload({"ok": False})) is None
    assert session.ist_offen() is False


def test_zu_lange_nutzlast_nimmt_kleinere_fassung():
    ser = MagicMock()
    session = mod.SerialSession(ser)
    gross = {"s": 1, "sn": ["x" * 40] * 40}
    klein = {"s": 1, "sn": ["a"]}
    assert asyncio.run(session.write_payload(gross, klein)) == klein
    ser.write.assert_called_once_with(json.dumps(klein, separators=(",", ":")).encode() + b"\n")


def test_leseschleife_verwirft_eingang_und_erkennt_fehler(monkeypatch):
    monkeypatch.setattr(mod, "SERIAL_LESEN_S", 0.001)
    ser = MagicMock()
    ser.in_waiting = 12
    session = mod.SerialSession(ser)

    async def lauf():
        aufgabe = asyncio.ensure_future(session.eingang_verwerfen())
        await asyncio.sleep(0.02)
        assert session.ist_offen()
        ser.read.assert_called_with(12)
        type(ser).in_waiting = property(lambda self: (_ for _ in ()).throw(OSError("weg")))
        await asyncio.wait_for(aufgabe, 1)

    asyncio.run(lauf())
    assert session.ist_offen() is False


def test_oeffnen_fehlgeschlagen_gibt_false(monkeypatch):
    def kaputt(port):
        raise pyserial.SerialException("Zugriff verweigert")
    monkeypatch.setattr(mod, "oeffne_serial", kaputt)
    stop = asyncio.run(_ereignis())
    assert asyncio.run(mod.serial_run("COM3", stop)) is False


async def _ereignis():
    return asyncio.Event()


# ---------------------------------------------------------------------------
# main(): Kabel zuerst, Rueckfall auf BLE, spaeter wieder Kabel
# ---------------------------------------------------------------------------

def _main_mit_fakes(monkeypatch, ports, serial_ergebnis):
    """Faehrt main() bis zum zweiten BLE-Versuch. Gibt (serial_ports, ble_aufrufe)."""
    ble = []
    seriell = []
    ports = iter(ports)

    async def fake_serial_run(port, stop, tray=None):
        seriell.append(port)
        return serial_ergebnis

    async def fake_acquire():
        return MagicMock(address="AA")

    async def fake_connect(device, stop, tray=None):
        ble.append(mod._kabel_abbruch)
        stop.set()
        return True

    monkeypatch.setattr(mod, "finde_serial_port", lambda: next(ports, None))
    monkeypatch.setattr(mod, "serial_run", fake_serial_run)
    monkeypatch.setattr(mod, "acquire_target", fake_acquire)
    monkeypatch.setattr(mod, "connect_and_run", fake_connect)
    asyncio.run(mod.main())
    return seriell, ble


def test_main_ohne_port_nimmt_ble(monkeypatch):
    seriell, ble = _main_mit_fakes(monkeypatch, [None], False)
    assert seriell == []
    assert len(ble) == 1


def test_main_mit_port_faellt_nach_ausfall_auf_ble_zurueck(monkeypatch):
    seriell, ble = _main_mit_fakes(monkeypatch, ["COM7"], False)
    assert seriell == ["COM7"]
    assert len(ble) == 1


def test_ble_schleife_wechselt_zum_kabel_erst_nach_sperrzeit(monkeypatch):
    seriell, ble = _main_mit_fakes(monkeypatch, ["COM7", "COM7"], False)
    abbruch = ble[0]
    # Frisch nach dem Kabelfehler: keine Rueckkehr, auch wenn der Port da ist.
    assert abbruch() is False
    # Nach der Sperrzeit und dem Pruefabstand: Kabel wieder da.
    echte_zeit = mod.time.monotonic
    monkeypatch.setattr(mod.time, "monotonic",
                        lambda: echte_zeit() + mod.SERIAL_NEUVERSUCH_S + 1)
    assert abbruch() is True


def test_serial_run_sendet_nutzlast_zeile_und_faellt_bei_schreibfehler_aus(monkeypatch):
    ser = MagicMock()
    ser.in_waiting = 0
    ser.write.side_effect = [None, pyserial.SerialException("Kabel gezogen")]
    monkeypatch.setattr(mod, "oeffne_serial", lambda port: ser)
    monkeypatch.setattr(mod, "read_token", lambda: None)
    monkeypatch.setattr(mod, "uhr_felder", lambda: {})
    monkeypatch.setattr(mod, "SESSIONS_PUSH_INTERVAL", 0)
    monkeypatch.setattr(mod, "TICK", 0.01)

    async def ohne_sessions(payload):
        return payload, payload

    monkeypatch.setattr(mod, "add_session_fields_paar", ohne_sessions)

    async def lauf():
        return await asyncio.wait_for(mod.serial_run("COM3", asyncio.Event()), 5)

    # Erster Schreibvorgang gelingt (Zeile mit Zeilenende), der zweite scheitert:
    # die Schleife endet, der Port wird geschlossen.
    monkeypatch.setattr(mod, "HERZSCHLAG_S", 0)
    assert asyncio.run(lauf()) is True
    assert ser.write.call_args_list[0].args[0] == b'{"ok":false}\n'
    ser.close.assert_called_once()
