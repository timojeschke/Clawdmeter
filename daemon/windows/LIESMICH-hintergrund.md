# Claude Code auf Windows im Hintergrund starten

Stand: 2026-09-28

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
3. `claude-fenster.cmd` nach `%USERPROFILE%` legen und eine
   **Desktop**-Verknüpfung „Claude Code — Fenster öffnen" darauf anlegen.
   Nicht in den Autostart-Ordner — dort starteten beim Anmelden zwei.

## Die zwei Verknüpfungen

| Verknüpfung | Ort | tut |
|---|---|---|
| Claude Code — Hintergrund | Autostart | startet unsichtbar — **tut nichts**, wenn schon eine Sitzung läuft |
| Claude Code — Fenster öffnen | Desktop | beendet jede laufende Sitzung, öffnet frisch und sichtbar |

Beide sind gegen Doppelstart gesichert, aber **absichtlich verschieden**:

- `claude-start-unsichtbar.vbs` fragt per WMI, ob schon ein `cmd.exe` mit
  `claude-start.cmd` oder `claude-fenster.cmd` in der Befehlszeile läuft, und
  beendet sich dann still. Der Autostart darf eine laufende, erreichbare
  Sitzung nie abschießen.
- `claude-fenster.cmd` beendet jede solche Kette per `taskkill /T` und startet
  danach sichtbar — wer das Fenster öffnet, will an die Sitzung heran. Das
  eigene `cmd.exe` nimmt es über die Elternprozess-ID der PowerShell aus.

**Gesucht wird das `cmd.exe`, nicht `wscript.exe`:** `Run …, 0, False` wartet
nicht, wscript beendet sich sofort. Ein erster Entwurf suchte genau diesen
Prozess und hätte nie etwas gefunden.

## Die eine Regel

**„Timo-PC" ist weg → „Fenster öffnen" nehmen.** Nicht den Hintergrundstart.

Eine Sitzung kann ihre Remote-Control-Verbindung verlieren und trotzdem
weiterlaufen. Von außen sieht sie beendet aus, blockiert aber den
Hintergrundstart — der Klick bliebe wirkungslos. „Fenster öffnen" räumt sie
weg. So geschehen in der Nacht zum 2026-09-28.

## Beenden

Ohne Fenster gibt es kein Strg+C. Am einfachsten „Fenster öffnen" und dort
beenden. Sonst über den Task-Manager: `claude.exe` **und** das übergeordnete
`cmd.exe` — sonst startet die Schleife sofort neu.

## Wenn es klemmt

| Bild | wahrscheinliche Ursache |
|---|---|
| Timo-PC ist nicht mehr erreichbar | Entweder hat die Sitzung ihre Remote-Control-Verbindung verloren und läuft weiter — dann „Fenster öffnen" (siehe oben). Oder Claude Code startet nicht durch — dann wächst `claude-neustarts.log`. |
| Meldungsfenster „claude-start.cmd nicht gefunden" | Die `.cmd` liegt woanders als in `%USERPROFILE%` |
| Alles still, keine Log-Zeile | Die Verknüpfung zeigt noch aufs alte Ziel, oder `wscript.exe` fehlt im Ziel |
