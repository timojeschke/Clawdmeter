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


def _stand(namen, wartend=1, geparkt=20, frisch=True, alter=4):
    """Ein Serverstand, in dem `namen` die LAUFENDEN Sessions sind.

    Seit dem 2026-09-25 zeigt das Geraet die laufenden statt der wartenden —
    "wartet auf Eingabe" ist bei Claude Code kein trennscharfer Zustand, weil
    die App jede untaetige Session so nennt.
    """
    return {
        "erzeugt_um": 1790000000,
        "alter_sekunden": alter,
        "frisch": frisch,
        "anzahl": {
            "wartet": wartend,
            "arbeitet": len(namen),
            "geparkt": geparkt,
            "gesamt": len(namen) + wartend + geparkt,
        },
        "sessions": [{"name": n, "zustand": "arbeitet"} for n in namen]
        + [{"name": "wartet gerade", "zustand": "wartet"}],
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
    assert (r["sw"], r["sa"], r["sg"]) == (1, 2, 20)


def test_kurze_liste_kommt_vollstaendig_durch():
    r = merge_into_payload(BASIS, _stand(["AckerMind - Orga", "TJCreate - CRM"]))
    assert r["sn"] == ["AckerMind - Orga", "TJCreate - CRM"]
    assert "sx" not in r


def test_lange_namen_enden_mit_auslassungszeichen():
    # Frueher wurde hart nach MAX_NAME_CHARS geschnitten. Das ergab Bruchstuecke
    # wie "Stoetefalke - Webse"; die Wortgrenze liefert den Teil, der die
    # Session tatsaechlich identifiziert.
    # Einziger Name: Er bekommt so viel Platz, wie das Budget hergibt, und
    # wird hinten gekuerzt. Der Anfang bleibt erhalten.
    lang = "Stoetefalke - Webseite Personal Training"
    r = merge_into_payload(BASIS, _stand([lang]))
    assert len(r["sn"]) == 1
    assert r["sn"][0].startswith("Stoetefalke - Webseite")
    assert r["sn"][0].endswith("\u2026")


def test_viele_sessions_sprengen_die_grenze_nicht():
    # Der reale Fall: 29 Sessions ergeben beim Server 3039 Byte, das Geraet
    # fasst 512. Ohne Kuerzen kaeme abgeschnittenes JSON an.
    viele = [f"Projekt Nummer {i} mit langem Namen" for i in range(29)]
    r = merge_into_payload(BASIS, _stand(viele))
    assert _groesse(r) <= PAYLOAD_LIMIT_BYTES
    assert r["sa"] == 29
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
    assert r["sa"] == 2 and r["sx"] == 2


def test_ohne_laufende_keine_namensliste():
    r = merge_into_payload(BASIS, _stand([]))
    assert "sn" not in r
    assert r["sa"] == 0


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
    nutzlast = {"s": 1, "f": fuellung}
    erfolg = asyncio.run(_session(client).write_payload(nutzlast))
    # Vertragswechsel (2026-09-25): write_payload liefert die tatsaechlich
    # gesendete Nutzlast zurueck, nicht mehr True — sonst merkt sich der
    # Aufrufer die grosse Fassung, obwohl nur die kleine ankam.
    assert erfolg == nutzlast
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

    # Vertragswechsel: erfolgreich zurueckgesendet wird die tatsaechlich
    # gesendete (gekuerzte) Fassung, nicht True — genau die Fassung ohne
    # Sessionfelder, die unten am zweiten Schreibvorgang geprueft wird.
    assert erfolg == {"s": 37, "w": 92}
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
    # Vertragswechsel: endgueltiger Fehlschlag ist None, nicht False — write_payload
    # gibt seit 2026-09-25 dict|None zurueck.
    assert erfolg is None
    assert len(client.schreibvorgaenge) == 1


# --- Namenskuerzung ---------------------------------------------------------

@pytest.mark.parametrize("roh, grenze, erwartet", [
    # Passt: unveraendert, kein Auslassungszeichen. Eines zu setzen wuerde
    # behaupten, dass noch etwas fehlt.
    ("TJCreate - CRM",       18, "TJCreate - CRM"),
    ("Heimatschutzverein",   18, "Heimatschutzverein"),
    # Passt nicht: am Ende schneiden, Auslassungszeichen dahinter. Timo,
    # 2026-09-25: "Privat - clawdme…. als beispiel, das abkuerzen am ende
    # bitte." Ausdruecklich KEIN Schnitt an der Wortgrenze — der Anfang bleibt
    # so stehen, wie der Name anfaengt.
    ("Privat - Clawdmeter",  18, "Privat - Clawdmet\u2026"),
    ("Stötefalke - Webseite Personal Training", 22, "Stötefalke - Webseite\u2026"),
    ("Donaudampfschifffahrtsgesellschaft",      18, "Donaudampfschifff\u2026"),
])
def test_zu_lange_namen_werden_hinten_gekuerzt(roh, grenze, erwartet):
    from daemon.sessions_source import _kuerzen
    ergebnis = _kuerzen(roh, grenze)
    assert ergebnis == erwartet
    assert len(ergebnis) <= grenze



# --- Zwei Namen duerfen nie gleich aussehen --------------------------------

def _namen(*roh):
    from daemon.sessions_source import _waiting_names
    return _waiting_names({"sessions": [{"zustand": "wartet", "name": n}
                                        for n in roh]})


def test_gleich_aussehende_namen_werden_nicht_gezeigt():
    """Lieber weniger Zeilen als zwei, die gleich heissen.

    Feldfall 2026-09-25: "Heimatschutzverein - Dokumente" und
    "… - Webseite" wurden beide zu "Heimatschutzverei…". Zwei identische
    Zeilen sind schlimmer als eine fehlende — die Liste soll sagen, welche
    Session man oeffnen muss. Geloest wird das jetzt ueber die Laenge: Es wird
    die Variante gewaehlt, in der alle gezeigten Namen verschieden sind.
    """
    r = merge_into_payload(BASIS, _stand(["Heimatschutzverein - Dokumente",
                                          "Heimatschutzverein - Webseite"]))
    assert len(set(r["sn"])) == len(r["sn"])


def test_dubletten_werden_auch_bei_vielen_sessions_vermieden():
    r = merge_into_payload(BASIS, _stand(["Heimatschutzverein - Dokumente",
                                          "Heimatschutzverein - Webseite",
                                          "Privat - Clawdmeter",
                                          "Privat - IPTV"]))
    assert len(set(r["sn"])) == len(r["sn"])
    # Was nicht gezeigt wird, wird gezaehlt — die Summe muss stimmen.
    assert len(r["sn"]) + r.get("sx", 0) == r["sa"]


def test_namen_bekommen_den_platz_der_uebrig_ist():
    # Ein kurzer Name soll nicht auf 18 Zeichen beschnitten werden, nur weil
    # das mal die Obergrenze war. Timo: "bis dahin soll der Platz genutzt
    # werden."
    r = merge_into_payload(BASIS, _stand(["Stoetefalke - Webseite Fachbuecher"]))
    assert r["sn"] == ["Stoetefalke - Webseite Fachbuecher"]
