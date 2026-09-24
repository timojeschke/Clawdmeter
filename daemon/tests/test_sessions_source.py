#!/usr/bin/env python3
"""Unit tests for the session source — config, fetch, and the size-aware merge.

The merge is the part worth testing: the firmware truncates anything past 511
bytes, so a payload that grows past the limit is not a cosmetic problem, it is
corrupted JSON on the device.

Run: python -m pytest daemon/tests/test_sessions_source.py -x -q
"""
import json
from unittest.mock import AsyncMock, MagicMock

import pytest

from daemon.sessions_source import (
    MAX_NAME_CHARS,
    PAYLOAD_LIMIT_BYTES,
    fetch_sessions,
    merge_into_payload,
    read_sessions_config,
)

BASIS = {"s": 42, "sr": 180, "w": 17, "wr": 8820, "st": "active", "ok": True}


def _stand(wartend, arbeitend=1, geparkt=20, frisch=True, alter=4):
    return {
        "erzeugt_um": 1790000000,
        "alter_sekunden": alter,
        "frisch": frisch,
        "anzahl": {
            "wartet": len(wartend),
            "arbeitet": arbeitend,
            "geparkt": geparkt,
            "gesamt": len(wartend) + arbeitend + geparkt,
        },
        "sessions": [{"name": n, "zustand": "wartet"} for n in wartend]
        + [{"name": "laeuft gerade", "zustand": "arbeitet"}],
    }


def _groesse(payload):
    # Bewusst die Produktionsfunktion, keine Nachbildung: Als der Daemon auf
    # ensure_ascii=False umgestellt wurde, mass eine eigene Kopie hier noch die
    # Escape-Schreibweise und schlug fehl, obwohl die Nutzlast passte. Zwei
    # Stellen, die dasselbe berechnen, laufen irgendwann auseinander.
    from daemon.sessions_source import _serialised_size
    return _serialised_size(payload)


# --- Konfiguration ---------------------------------------------------------

def test_fehlende_datei_schaltet_die_quelle_ab(tmp_path):
    assert read_sessions_config(tmp_path / "gibtsnicht") == (None, None)


def test_halb_konfiguriert_gilt_als_aus(tmp_path):
    # Nur die URL ohne Token waere ein Aufruf ohne Authentisierung — der soll
    # gar nicht erst stattfinden.
    f = tmp_path / "config"
    f.write_text("sessions_url = https://example.invalid/sessions\n", encoding="utf-8")
    assert read_sessions_config(f) == (None, None)


def test_vollstaendige_konfiguration_wird_gelesen(tmp_path):
    f = tmp_path / "config"
    f.write_text(
        "# Kommentar\n"
        "sessions_url = https://example.invalid/sessions   # nachgestellt\n"
        "sessions_token = geheim123\n",
        encoding="utf-8",
    )
    assert read_sessions_config(f) == ("https://example.invalid/sessions", "geheim123")


# --- Abruf -----------------------------------------------------------------

@pytest.mark.parametrize(
    "bezeichnung,antwort",
    [
        ("HTTP 401", MagicMock(status_code=401)),
        ("HTTP 503", MagicMock(status_code=503)),
    ],
)
def test_fehlerhafte_antworten_ergeben_none(bezeichnung, antwort):
    client = MagicMock()
    client.get = AsyncMock(return_value=antwort)
    import asyncio

    assert asyncio.run(fetch_sessions(client, "http://x", "t")) is None


def test_netzwerkfehler_ergibt_none_statt_absturz():
    # Diese Quelle ist eine Zugabe. Faellt sie aus, darf das die
    # Auslastungszahlen nicht beruehren — deshalb None statt einer Ausnahme.
    client = MagicMock()
    client.get = AsyncMock(side_effect=OSError("Netz weg"))
    import asyncio

    assert asyncio.run(fetch_sessions(client, "http://x", "t")) is None


def test_unerwartete_form_ergibt_none():
    antwort = MagicMock(status_code=200)
    antwort.json = MagicMock(return_value={"irgendwas": 1})
    client = MagicMock()
    client.get = AsyncMock(return_value=antwort)
    import asyncio

    assert asyncio.run(fetch_sessions(client, "http://x", "t")) is None


# --- Zusammenfuehren -------------------------------------------------------

def test_ohne_daten_bleibt_die_nutzlast_unveraendert():
    assert merge_into_payload(BASIS, None) == BASIS


def test_die_uebergebene_nutzlast_wird_nicht_veraendert():
    original = dict(BASIS)
    merge_into_payload(BASIS, _stand(["eine"]))
    assert BASIS == original


def test_zahlen_kommen_mit():
    r = merge_into_payload(BASIS, _stand(["A", "B"]))
    assert (r["sw"], r["sa"], r["sg"]) == (2, 1, 20)


def test_kurze_liste_kommt_vollstaendig_durch():
    r = merge_into_payload(BASIS, _stand(["AckerMind - Orga", "TJCreate - CRM"]))
    assert r["sn"] == ["AckerMind - Orga", "TJCreate - CRM"]
    assert "sx" not in r


def test_lange_namen_enden_mit_auslassungszeichen():
    # Frueher wurde hart nach MAX_NAME_CHARS geschnitten. Das ergab Bruchstuecke
    # wie "Stoetefalke - Webse"; die Wortgrenze liefert den Teil, der die
    # Session tatsaechlich identifiziert.
    lang = "Stoetefalke - Webseite Personal Training"
    r = merge_into_payload(BASIS, _stand([lang]))
    assert r["sn"] == ["Stoetefalke\u2026"]


def test_viele_sessions_sprengen_die_grenze_nicht():
    # Der reale Fall: 29 Sessions ergeben beim Server 3039 Byte, das Geraet
    # fasst 512. Ohne Kuerzen kaeme abgeschnittenes JSON an.
    viele = [f"Projekt Nummer {i} mit langem Namen" for i in range(29)]
    r = merge_into_payload(BASIS, _stand(viele))
    assert _groesse(r) <= PAYLOAD_LIMIT_BYTES
    assert r["sw"] == 29
    assert r["sx"] == 29 - len(r.get("sn", []))


def test_gekuerzte_liste_meldet_die_fehlenden():
    viele = [f"Sitzung {i:02d} mit reichlich Text" for i in range(20)]
    r = merge_into_payload(BASIS, _stand(viele))
    assert r["sx"] >= 1
    assert len(r["sn"]) + r["sx"] == 20


def test_veralteter_stand_liefert_zahlen_ohne_namen():
    # Ein Name, der vor Minuten noch stimmte, schickt den Nutzer ins falsche
    # Fenster. Die Zahl bleibt naeherungsweise richtig, der Name nicht.
    r = merge_into_payload(BASIS, _stand(["A", "B"], frisch=False, alter=600))
    assert "sn" not in r
    assert r["sw"] == 2 and r["sx"] == 2


def test_ohne_wartende_keine_namensliste():
    r = merge_into_payload(BASIS, _stand([]))
    assert "sn" not in r
    assert r["sw"] == 0


def test_die_auslastungsfelder_ueberleben_das_zusammenfuehren():
    r = merge_into_payload(BASIS, _stand(["A"]))
    for k, v in BASIS.items():
        assert r[k] == v


# --- Diagnose der Ratelimit-Header ----------------------------------------
#
# Diese beiden Tests importieren den Windows-Daemon, der `bleak` braucht — den
# Bluetooth-Stapel, den es nur auf Timos PC gibt, nicht auf dem Linux-Server.
# Dort werden sie uebersprungen statt rot zu laufen: ein Fehlschlag, der nur
# "hier fehlt eine Bibliothek" bedeutet, bringt einem bei, rote Tests zu
# ignorieren. Auf dem PC laufen sie regulaer mit.
try:
    import daemon.claude_usage_daemon_windows as windows_daemon
except ImportError:                      # bleak fehlt
    windows_daemon = None

nur_mit_bleak = pytest.mark.skipif(
    windows_daemon is None,
    reason="benoetigt bleak (nur auf dem Windows-PC vorhanden)",
)


@nur_mit_bleak
def test_unbekannte_ratelimit_header_werden_gemeldet(capsys, monkeypatch):
    # Beantwortet empirisch, ob es ein eigenes Wochenlimit je Modell gibt,
    # ohne dass jemand Zugangsdaten anfassen muss.
    d = windows_daemon

    monkeypatch.setattr(d, "_ratelimit_header_gemeldet", False)
    d.report_unknown_ratelimit_headers({
        "anthropic-ratelimit-unified-5h-utilization": "0.4",
        "anthropic-ratelimit-unified-opus-7d-utilization": "0.9",
        "content-type": "application/json",
    })
    ausgabe = capsys.readouterr().out
    assert "anthropic-ratelimit-unified-opus-7d-utilization" in ausgabe
    # Werte sind Kontodaten und gehoeren nicht ins Log.
    assert "0.9" not in ausgabe


@nur_mit_bleak
def test_meldung_erfolgt_nur_einmal(capsys, monkeypatch):
    d = windows_daemon

    monkeypatch.setattr(d, "_ratelimit_header_gemeldet", False)
    kopf = {"anthropic-ratelimit-neu-utilization": "1"}
    d.report_unknown_ratelimit_headers(kopf)
    capsys.readouterr()
    d.report_unknown_ratelimit_headers(kopf)
    assert capsys.readouterr().out == ""


# --- BLE-Schreibweg: Groesse entscheidet ueber die Schreibart --------------
#
# Feldfehler vom 2026-09-25: Eine Nutzlast von 271 Byte mit fuenf
# Sessionnamen schlug sieben Mal hintereinander fehl ("Falscher Parameter"),
# und in dieser Zeit kamen auch keine Nutzungsdaten mehr an. Ursache: ein
# Write ohne Antwort ist auf ATT_MTU-3 begrenzt, nicht auf die 512 Byte des
# Firmware-Puffers.

class _AttrappeClient:
    """Steht fuer den BLE-Client — die Systemgrenze, an der gemockt wird."""

    def __init__(self, mtu_size=247, schlaegt_fehl_ab=None):
        self.mtu_size = mtu_size
        self.schlaegt_fehl_ab = schlaegt_fehl_ab
        self.schreibvorgaenge = []          # (nutzdaten, response)

    async def write_gatt_char(self, uuid, data, response):
        self.schreibvorgaenge.append((data, response))
        if self.schlaegt_fehl_ab is not None and len(data) >= self.schlaegt_fehl_ab:
            raise OSError("[WinError -2147024809] Falscher Parameter.")


def _session(client):
    sitzung = windows_daemon.Session.__new__(windows_daemon.Session)
    sitzung.client = client
    return sitzung


@nur_mit_bleak
@pytest.mark.parametrize("fuellung, erwartet_response", [
    ("x" * 10,  False),    # passt bequem in MTU-3 = 244
    ("x" * 400, True),     # zu gross -> muss den Long Write nehmen
])
def test_grosse_nutzlast_erzwingt_write_mit_antwort(fuellung, erwartet_response):
    import asyncio
    client = _AttrappeClient()
    erfolg = asyncio.run(_session(client).write_payload({"s": 1, "f": fuellung}))
    assert erfolg is True
    assert len(client.schreibvorgaenge) == 1
    _, response = client.schreibvorgaenge[0]
    assert response is erwartet_response


@nur_mit_bleak
def test_fehlgeschlagener_schreibvorgang_liefert_nutzung_ohne_sessions_nach():
    # Die Nutzungszahlen sind der Zweck des Geraets; die Sessions sind das
    # Entbehrliche. Eine zu grosse Sessionliste darf die Nutzungsdaten nicht
    # mitreissen.
    import asyncio
    client = _AttrappeClient(schlaegt_fehl_ab=100)
    nutzlast = {"s": 37, "w": 92, "sw": 5, "sa": 0, "sg": 24,
                "sn": ["A" * 22] * 5, "sx": 1}

    erfolg = asyncio.run(_session(client).write_payload(nutzlast))

    assert erfolg is True
    assert len(client.schreibvorgaenge) == 2
    zweiter = json.loads(client.schreibvorgaenge[1][0])
    assert zweiter == {"s": 37, "w": 92}


@nur_mit_bleak
def test_nutzlast_ohne_sessions_wird_nicht_zweimal_versucht():
    # Ohne Sessionfelder gibt es nichts wegzulassen — ein zweiter Versuch
    # waere nur eine weitere Sekunde Verzoegerung vor dem Reconnect.
    import asyncio
    client = _AttrappeClient(schlaegt_fehl_ab=1)
    erfolg = asyncio.run(_session(client).write_payload({"s": 37}))
    assert erfolg is False
    assert len(client.schreibvorgaenge) == 1


# --- Namenskuerzung ---------------------------------------------------------

@pytest.mark.parametrize("roh, erwartet", [
    # Wortgrenze vorhanden und weit genug hinten: dort wird geschnitten.
    ("Stötefalke - Webseite Personal Training", "Stötefalke…"),
    ("AckerMind - Social Media",                "AckerMind…"),
    # Grenzfall aus dem Feld (PC-Session, 2026-09-25): 19 Zeichen, Grenze bei
    # Index 8. Frueher wurde daraus "Privat - Clawdmete".
    ("Privat - Clawdmeter",                     "Privat…"),
    # Passt vollstaendig — kein Auslassungszeichen, sonst behauptet es etwas.
    ("TJCreate - CRM",                          "TJCreate - CRM"),
    # Genau auf der Grenze: 18 Zeichen bleiben unangetastet.
    ("Heimatschutzverein",                      "Heimatschutzverein"),
    # Keine brauchbare Wortgrenze: harter Schnitt, aber markiert.
    ("Donaudampfschifffahrtsgesellschaft",      "Donaudampfschifff…"),
])
def test_zu_lange_namen_enden_mit_auslassungszeichen(roh, erwartet):
    # Timos Regel: Ein gekuerzter Name muss sagen, dass er gekuerzt ist. Ein
    # blosser Schnitt liefert "Stötefalke - Webse" — auf einem Blick-Display
    # liest sich das wie ein Tippfehler, nicht wie eine Abkuerzung.
    from daemon.sessions_source import _kuerzen, MAX_NAME_CHARS
    ergebnis = _kuerzen(roh)
    assert ergebnis == erwartet
    assert len(ergebnis) <= MAX_NAME_CHARS
