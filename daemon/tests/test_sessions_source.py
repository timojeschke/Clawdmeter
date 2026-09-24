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
    return len(json.dumps(payload, separators=(",", ":")).encode("utf-8"))


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


def test_lange_namen_werden_gekuerzt():
    lang = "Stoetefalke - Webseite Personal Training"
    r = merge_into_payload(BASIS, _stand([lang]))
    assert r["sn"] == [lang[:MAX_NAME_CHARS]]


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
