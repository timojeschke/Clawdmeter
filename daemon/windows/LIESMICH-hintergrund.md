# Claude Code auf Windows im Hintergrund starten

Stand: 2026-09-27

Die Sitzung lief bisher in einem sichtbaren CMD-Fenster. Das war **Absicht**:
Vertrauensfrage und Anmeldung sollten nicht unsichtbar hängen. Beides ist
durch, die Sitzung antwortet seit Tagen über Remote Control — der Schritt in
den Hintergrund ist damit fällig.

## Was sich ändert

Die Autostart-Verknüpfung zeigt nicht mehr auf `claude-start.cmd`, sondern auf
`claude-start-unsichtbar.vbs`. Das Skript startet dieselbe `.cmd` mit
Fensterstil 0.

**Die Konsole verschwindet nicht, sie wird nur unsichtbar.** Claude Code
behält seinen Bildschirmpuffer und läuft normal weiter; es ist kein
Kopflos-Betrieb.

## Was vorher in die `.cmd` gehört

Eine unsichtbare Neustartschleife, die durchdreht, merkt niemand. Deshalb vor
der Umstellung diese Zeile **in die Schleife** aufnehmen, direkt vor das
`timeout`:

```bat
echo %date% %time% Claude Code beendet, Neustart in 10 s >> "%USERPROFILE%\claude-neustarts.log"
```

Wächst diese Datei im Minutentakt, startet Claude Code nicht durch — dann das
Fenster wieder sichtbar machen und nachsehen.

## Umstellen

1. `claude-start-unsichtbar.vbs` nach `%USERPROFILE%` legen.
2. Verknüpfung `…\Start Menu\Programs\Startup\Claude Code.lnk` auf
   `wscript.exe "%USERPROFILE%\claude-start-unsichtbar.vbs"` zeigen lassen.
3. **Zweite Verknüpfung behalten**, etwa „Claude Code (Fenster)", die weiter
   direkt auf `claude-start.cmd` zeigt. Ohne sie gibt es keinen bequemen Weg
   zurück, wenn etwas klemmt.

## Beenden

Ohne Fenster gibt es kein Strg+C. Beenden über den Task-Manager: den Prozess
`node.exe` beziehungsweise `claude` und das übergeordnete `cmd.exe` — sonst
startet die Schleife sofort neu.

## Wenn es klemmt

| Bild | wahrscheinliche Ursache |
|---|---|
| `ListAgents` zeigt Timo-PC nicht mehr | Claude Code startet nicht durch — `claude-neustarts.log` ansehen |
| Meldungsfenster „claude-start.cmd nicht gefunden" | Die `.cmd` liegt woanders als in `%USERPROFILE%` |
| Alles still, keine Log-Zeile | Die Verknüpfung zeigt noch aufs alte Ziel, oder `wscript.exe` fehlt im Ziel |
