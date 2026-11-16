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


def test_leseschleife_zaehlt_acks_und_verwirft_den_rest(monkeypatch):
    monkeypatch.setattr(mod, "SERIAL_LESEN_S", 0.001)
    ser = MagicMock()
    ser.in_waiting = 40
    ser.read.return_value = b'init: touch\n{"ack":true}\n{"ack":fal'
    session = mod.SerialSession(ser)

    async def lauf():
        aufgabe = asyncio.ensure_future(session.eingang_lesen())
        await asyncio.sleep(0.01)
        assert session.quittiert >= 1
        type(ser).in_waiting = property(lambda self: (_ for _ in ()).throw(OSError("weg")))
        await asyncio.wait_for(aufgabe, 1)

    asyncio.run(lauf())
    assert session.ist_offen() is False


def test_halbe_ack_zeile_wird_erst_mit_zeilenende_gezaehlt():
    session = mod.SerialSession(MagicMock())
    session._quittungen_auswerten(b'{"ack":tr')
    assert session.quittiert == 0
    session._quittungen_auswerten(b'ue}\r\n')
    assert session.quittiert == 1


def test_oeffnen_fehlgeschlagen_gibt_false(monkeypatch):
    def kaputt(port):
        raise pyserial.SerialException("Zugriff verweigert")
    monkeypatch.setattr(mod, "oeffne_serial", kaputt)
    assert asyncio.run(mod.serial_run("COM3", asyncio.Event())) is False


class _Geraet:
    """Fake-Port: quittiert jede Nutzlastzeile je nach Modus."""
    def __init__(self, antwort):
        self.antwort = antwort      # b'{"ack":true}\n', b'{"ack":false}\n' oder b''
        self.geschrieben = []
        self.puffer = b""
        self.close = MagicMock()

    def write(self, daten):
        self.geschrieben.append(daten)
        if daten.startswith(b"{"):
            self.puffer += self.antwort

    @property
    def in_waiting(self):
        return len(self.puffer)

    def read(self, n):
        daten, self.puffer = self.puffer[:n], self.puffer[n:]
        return daten


def _serial_run_mit(monkeypatch, geraet, frist=0.0):
    monkeypatch.setattr(mod, "oeffne_serial", lambda port: geraet)
    monkeypatch.setattr(mod, "read_token", lambda: None)
    monkeypatch.setattr(mod, "uhr_felder", lambda: {})
    monkeypatch.setattr(mod, "SESSIONS_PUSH_INTERVAL", 0)
    monkeypatch.setattr(mod, "HERZSCHLAG_S", 0)
    monkeypatch.setattr(mod, "TICK", 0.01)
    monkeypatch.setattr(mod, "SERIAL_LESEN_S", 0.001)
    monkeypatch.setattr(mod, "SERIAL_ACK_FRIST_S", frist)

    async def ohne_sessions(payload):
        return payload, payload

    monkeypatch.setattr(mod, "add_session_fields_paar", ohne_sessions)

    async def lauf():
        stop = asyncio.Event()
        ergebnis = asyncio.ensure_future(mod.serial_run("COM3", stop))
        await asyncio.sleep(0.3)
        if not ergebnis.done():
            stop.set()
        return await asyncio.wait_for(ergebnis, 5)

    return asyncio.run(lauf())


def test_sendet_zuerst_leerzeile_dann_nutzlast_und_quittung_haelt_kabel(monkeypatch):
    geraet = _Geraet(b'{"ack":true}\n')
    assert _serial_run_mit(monkeypatch, geraet) is True
    assert geraet.geschrieben[0] == b"\n"
    assert geraet.geschrieben[1] == b'{"ok":false}\n'
    assert len(geraet.geschrieben) > 3      # lief weiter, wurde nicht abgebrochen
    geraet.close.assert_called_once()


def test_keine_quittung_gilt_kabel_als_ausgefallen(monkeypatch, capsys):
    geraet = _Geraet(b"")                   # alte Firmware schweigt
    assert _serial_run_mit(monkeypatch, geraet) is False
    nutzlasten = [z for z in geraet.geschrieben if z.startswith(b"{")]
    assert len(nutzlasten) == mod.SERIAL_OHNE_ACK_MAX
    assert "Kabel: keine Quittung vom Gerät (alte Firmware?)" in capsys.readouterr().out
    geraet.close.assert_called_once()


def test_zweimal_ack_false_gilt_kabel_als_ausgefallen(monkeypatch, capsys):
    geraet = _Geraet(b'{"ack":false}\n')
    assert _serial_run_mit(monkeypatch, geraet) is False
    assert "ack:false" in capsys.readouterr().out
    nutzlasten = [z for z in geraet.geschrieben if z.startswith(b"{")]
    assert len(nutzlasten) == mod.SERIAL_NACK_MAX


def test_schreibfehler_beendet_lauf_ohne_quittung(monkeypatch):
    geraet = _Geraet(b'{"ack":true}\n')
    echtes_write = geraet.write
    anzahl = []

    def write(daten):
        anzahl.append(daten)
        if len(anzahl) > 2:
            raise pyserial.SerialException("Kabel gezogen")
        echtes_write(daten)

    geraet.write = write
    assert _serial_run_mit(monkeypatch, geraet) is True   # eine Quittung kam
    geraet.close.assert_called_once()


# ---------------------------------------------------------------------------
# Sperrzeiten und Wechsel von BLE zum Kabel
# ---------------------------------------------------------------------------

def test_sperrzeit_waechst_60_120_300_und_geht_nur_nach_quittiertem_lauf_zurueck(monkeypatch):
    jetzt = [1000.0]
    monkeypatch.setattr(mod.time, "monotonic", lambda: jetzt[0])
    wahl = mod.KabelWahl()
    abstaende = []
    for _ in range(4):
        wahl.fehlschlag(quittiert=False)
        abstaende.append(wahl.gesperrt_bis - jetzt[0])
    assert abstaende == [60, 120, 300, 300]
    wahl.fehlschlag(quittiert=True)         # Lauf mit Quittung, dann ausgefallen
    assert wahl.gesperrt_bis - jetzt[0] == 60
    assert wahl.fehler_in_folge == 1


def test_gesperrt_liefert_keinen_port(monkeypatch):
    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)
    monkeypatch.setattr(mod.list_ports, "comports", lambda: [_port("COM7", 0x303A)])
    wahl = mod.KabelWahl()
    assert asyncio.run(wahl.naechster_port()) == "COM7"
    wahl.fehlschlag(quittiert=False)
    assert asyncio.run(wahl.naechster_port()) is None


def test_port_da_aber_oeffnen_scheitert_trennt_ble_nicht_und_sperrt_laenger(monkeypatch):
    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)
    monkeypatch.setattr(mod.list_ports, "comports", lambda: [_port("COM7", 0x303A)])
    jetzt = [1000.0]
    monkeypatch.setattr(mod.time, "monotonic", lambda: jetzt[0])

    versuche = []

    class Kaputt:
        def __setattr__(self, name, wert):
            object.__setattr__(self, name, wert)

        def open(self):
            versuche.append(1)
            raise pyserial.SerialException("Zugriff verweigert")

    monkeypatch.setattr(mod.serial, "Serial", lambda *a, **k: Kaputt())
    wahl = mod.KabelWahl()
    sperren = []
    for _ in range(3):
        jetzt[0] += 400                     # Sperre und Pruefabstand abgelaufen
        assert asyncio.run(wahl.wieder_da()) is False   # BLE-Schleife laeuft weiter
        sperren.append(wahl.gesperrt_bis - jetzt[0])
    assert len(versuche) == 3
    assert sperren == [60, 120, 300]


def test_wieder_da_prueft_nur_alle_30_s(monkeypatch):
    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)
    suche = MagicMock(return_value=[])
    monkeypatch.setattr(mod.list_ports, "comports", suche)
    jetzt = [1000.0]
    monkeypatch.setattr(mod.time, "monotonic", lambda: jetzt[0])
    wahl = mod.KabelWahl()
    asyncio.run(wahl.wieder_da())
    jetzt[0] += 10
    asyncio.run(wahl.wieder_da())
    assert suche.call_count == 1
    jetzt[0] += 25
    asyncio.run(wahl.wieder_da())
    assert suche.call_count == 2


def test_wieder_da_meldet_true_wenn_port_sich_oeffnen_laesst(monkeypatch):
    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)
    monkeypatch.setattr(mod.list_ports, "comports", lambda: [_port("COM7", 0x303A)])
    fake = _FakeSerial()
    fake.close = MagicMock()
    monkeypatch.setattr(mod.serial, "Serial", lambda *a, **k: fake)
    wahl = mod.KabelWahl()
    assert asyncio.run(wahl.wieder_da()) is True
    fake.close.assert_called_once()
    assert ("dtr", False) in fake.ereignisse     # Pruefung ohne Reset


# ---------------------------------------------------------------------------
# main(): Kabel zuerst, Rueckfall auf BLE
# ---------------------------------------------------------------------------

def _main_mit_fakes(monkeypatch, ports, serial_ergebnis):
    """Faehrt main() bis zum ersten BLE-Lauf. Gibt (serial_ports, ble_aufrufe)."""
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
