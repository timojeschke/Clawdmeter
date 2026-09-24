#!/usr/bin/env python3
"""Second data source: Claude Code session states from a remote server.

The usage numbers come from Anthropic's rate-limit headers (see poll_api).
This module adds an unrelated source: how many Claude Code sessions on a
remote machine are waiting for input, and which ones. Both end up in the same
GATT payload because the device has exactly one input channel.

Disabled unless `sessions_url` and `sessions_token` are set in the config
file. No URL, no calls, no behaviour change — an existing install that never
touches the config keeps working exactly as before.

The firmware's receive buffer is 512 bytes and it truncates anything longer
(BLE_BUF_SIZE in firmware/src/ble.cpp), so the merge below is size-aware:
counts always fit, names are added while there is room and dropped from the
end when there is not. Measured against a real server response: 29 sessions
serialise to 3039 bytes, so dropping names is the normal case, not the edge.
"""

import json
import logging

log = logging.getLogger(__name__)

# Firmware truncates at BLE_BUF_SIZE - 1 = 511 bytes. Stay below it with room
# for the fields poll_api adds after this merge runs.
PAYLOAD_LIMIT_BYTES = 480

# Long session names ("Stötefalke - Webseite Personal Training") eat the budget
# without adding information — the first words identify the session.
MAX_NAME_CHARS = 22

# A session list is worth showing only while it is current. Past this the
# device shows counts without names rather than a plausible-looking lie.
MAX_AGE_SECONDS = 300

REQUEST_TIMEOUT_SECONDS = 5.0


def read_sessions_config(config_file) -> tuple[str | None, str | None]:
    """Read `sessions_url` and `sessions_token` from the config file.

    Returns (None, None) when either is missing — the feature is opt-in and
    half-configured counts as off.
    """
    url = token = None
    try:
        if not config_file.exists():
            return None, None
        for line in config_file.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            key, val = line.split("=", 1)
            key = key.strip().lower()
            if key == "sessions_url":
                url = val.strip() or None
            elif key == "sessions_token":
                token = val.strip() or None
    except OSError:
        return None, None

    if not url or not token:
        return None, None
    return url, token


async def fetch_sessions(http_client, url: str, token: str) -> dict | None:
    """Fetch the session state. Returns None on any failure.

    None is deliberately indistinguishable between "server down", "wrong
    token" and "malformed answer": this source is an extra, and none of those
    cases should disturb the usage numbers, which are the device's main job.
    """
    try:
        resp = await http_client.get(
            url,
            headers={"X-Clawd-Token": token},
            timeout=REQUEST_TIMEOUT_SECONDS,
        )
    except Exception as e:  # noqa: BLE001 — httpx raises a wide family here
        log.debug("sessions source unreachable: %s", e.__class__.__name__)
        return None

    if resp.status_code != 200:
        log.debug("sessions source returned HTTP %s", resp.status_code)
        return None

    try:
        daten = resp.json()
    except ValueError:
        log.debug("sessions source returned no valid JSON")
        return None

    if not isinstance(daten, dict) or "anzahl" not in daten:
        log.debug("sessions source returned an unexpected shape")
        return None
    return daten


def _waiting_names(daten: dict) -> list[str]:
    sessions = daten.get("sessions")
    if not isinstance(sessions, list):
        return []
    namen = []
    for s in sessions:
        if isinstance(s, dict) and s.get("zustand") == "wartet":
            name = str(s.get("name", "")).strip()
            if name:
                namen.append(name[:MAX_NAME_CHARS])
    return namen


def _serialised_size(payload: dict) -> int:
    return len(json.dumps(payload, separators=(",", ":")).encode("utf-8"))


def merge_into_payload(payload: dict, daten: dict | None) -> dict:
    """Add session fields to the usage payload, trimming names to fit.

    Returns a new dict; the caller's payload is not modified. With `daten`
    None the payload comes back unchanged, so a dead sessions server never
    costs the device its usage numbers.

    Field names are short because every byte competes with the usage data:
      sw  waiting count      sa  working count
      sg  parked count       sn  waiting names, as many as fit
      sx  names dropped for space
    """
    if daten is None:
        return dict(payload)

    anzahl = daten.get("anzahl")
    if not isinstance(anzahl, dict):
        return dict(payload)

    alter = daten.get("alter_sekunden")
    veraltet = isinstance(alter, int) and alter > MAX_AGE_SECONDS

    merged = dict(payload)
    merged["sw"] = int(anzahl.get("wartet", 0))
    merged["sa"] = int(anzahl.get("arbeitet", 0))
    merged["sg"] = int(anzahl.get("geparkt", 0))

    # Stale data: counts are still roughly right, a name list is not. Showing
    # a session as waiting when it finished four minutes ago sends the user to
    # the wrong window.
    if veraltet or not daten.get("frisch", True):
        merged["sx"] = merged["sw"]
        return merged

    namen = _waiting_names(daten)
    if not namen:
        return merged

    # Add names while they fit. The server sorts waiting sessions first, so
    # dropping from the end drops the least relevant.
    passend = list(namen)
    while passend:
        kandidat = dict(merged)
        kandidat["sn"] = passend
        if len(passend) < len(namen):
            kandidat["sx"] = len(namen) - len(passend)
        if _serialised_size(kandidat) <= PAYLOAD_LIMIT_BYTES:
            return kandidat
        passend.pop()

    # Not even one name fits — report how many were suppressed.
    merged["sx"] = len(namen)
    return merged
