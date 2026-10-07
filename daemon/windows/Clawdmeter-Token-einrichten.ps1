<#
  Richtet das langlebige Token fuer den Clawdmeter-Daemon ein (oder entfernt es wieder).

  Timo startet das selbst per Doppelklick - es laeuft ausserhalb jeder Claude-Sitzung.
  Der Token-Wert wird nie angezeigt, nie in eine Datei, ein Protokoll oder eine
  Befehlszeile geschrieben. Er landet ausschliesslich in der Benutzer-Umgebungsvariable
  CLAWDMETER_OAUTH_TOKEN. Nicht angefasst werden: CLAUDE_CODE_OAUTH_TOKEN und
  .credentials.json - Claude Code selbst meldet sich weiter an wie bisher.

  Aufruf:
    Clawdmeter-Token-einrichten.cmd              einrichten
    Clawdmeter-Token-einrichten.cmd -Entfernen   Variable loeschen, alter Weg gilt wieder
    ... -Trockenlauf   nur zum Pruefen: Platzhalterwert statt setup-token, raeumt danach auf
#>
param([switch]$Entfernen, [switch]$Trockenlauf)

$ErrorActionPreference = 'Stop'
$VarName = 'CLAWDMETER_OAUTH_TOKEN'
$LogDatei = Join-Path $env:LOCALAPPDATA 'Clawdmeter\daemon.log'

function Sag([string]$t, [string]$f = 'Gray') { Write-Host $t -ForegroundColor $f }

function Ende([int]$code) {
    if (-not $Trockenlauf) { Write-Host ''; Write-Host 'Fertig. Beliebige Taste zum Schliessen ...'; [void][Console]::ReadKey($true) }
    exit $code
}

# Startbefehl des Daemons aus dem Autostart lesen, damit wir ihn genauso starten wie Windows.
function DaemonBefehl {
    $roh = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).Clawdmeter
    if ($roh -and $roh -match '^\s*"([^"]+)"\s+"([^"]+)"') { return @{ Exe = $Matches[1]; Skript = $Matches[2] } }
    return @{ Exe = 'C:\Python314\pythonw.exe'; Skript = (Join-Path $env:USERPROFILE 'Documents\clawdmeter-daemon-windows\daemon\tray_windows.py') }
}

function LogZeilen { if (Test-Path $LogDatei) { @(Get-Content $LogDatei -ErrorAction SilentlyContinue).Count } else { 0 } }

# Beendet den laufenden Daemon und startet ihn neu. $wert = $null heisst: ohne Variable.
function DaemonNeustart($wert) {
    $b = DaemonBefehl
    if (-not (Test-Path $b.Exe) -or -not (Test-Path $b.Skript)) { Sag "Daemon nicht gefunden: $($b.Exe) / $($b.Skript)" 'Red'; return $false }
    $alle = @(Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'")
    foreach ($p in $alle) {
        if ($null -eq $p.CommandLine) {
            Sag '  Hinweis: Ein pythonw-Prozess laeuft mit Adminrechten und ist von hier nicht pruefbar.' 'Yellow'
            continue
        }
        if ($p.CommandLine -match 'tray_windows\.py') {
            try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop; Sag "  Alter Daemon (PID $($p.ProcessId)) beendet." }
            catch { Sag "  Alter Daemon (PID $($p.ProcessId)) liess sich nicht beenden: $($_.Exception.Message)" 'Red'; return $false }
        }
    }
    # Dem Bluetooth-Stack Zeit geben, die alte Verbindung abzubauen - bei 2 s meldete der neue
    # Daemon im Test "Characteristic ... was not found" und musste neu verbinden.
    Start-Sleep -Seconds 6
    # Der neue Prozess erbt die Umgebung dieses Fensters - deshalb hier setzen und danach sofort wieder loeschen.
    if ($wert) { Set-Item -Path "Env:\$VarName" -Value $wert } else { Remove-Item "Env:\$VarName" -ErrorAction SilentlyContinue }
    try { Start-Process -FilePath $b.Exe -ArgumentList ('"{0}"' -f $b.Skript) -WorkingDirectory (Split-Path -Parent $b.Skript) }
    finally { Remove-Item "Env:\$VarName" -ErrorAction SilentlyContinue }
    Sag '  Daemon neu gestartet.'
    return $true
}

# Liest ab Zeile $ab im Daemon-Log mit: Token-Quelle und das erste Ergebnis des Nutzungsabrufs.
function LogBeobachten([int]$ab, [int]$sekunden) {
    $quelle = $null; $ergebnis = $null; $usage = $null
    $bis = (Get-Date).AddSeconds($sekunden)
    while ((Get-Date) -lt $bis -and -not $ergebnis) {
        Start-Sleep -Seconds 2
        $neu = @(Get-Content $LogDatei -ErrorAction SilentlyContinue | Select-Object -Skip $ab)
        foreach ($z in $neu) {
            if (-not $quelle -and $z -match 'Token source:\s*(.+)$') { $quelle = $Matches[1].Trim() }
            # Der Daemon fragt zwei Stellen ab. "usage endpoint" liefert die Modell-Zeile (z. B. Fable);
            # ein 403 dort heisst: Hauptwerte ja, Modell-Zeile nein (so beobachtet am 2026-10-07).
            if ($z -match 'usage endpoint HTTP (\d{3})') { $usage = "HTTP $($Matches[1])" }
            if ($z -match 'API HTTP (\d{3})') { $ergebnis = "HTTP $($Matches[1])"; break }
            if ($z -match 'API call failed') { $ergebnis = 'kein Internet / Abruf fehlgeschlagen'; break }
            if ($z -match 'Sending:.*"ok":true') { $ergebnis = 'OK - Nutzungsdaten kommen an'; break }
        }
    }
    return @{ Quelle = $quelle; Ergebnis = $ergebnis; Usage = $usage }
}

function Zeig($r) {
    Sag ("  Token-Quelle laut Daemon: {0}" -f $(if ($r.Quelle) { $r.Quelle } else { '(keine Zeile gefunden)' })) 'Cyan'
    Sag ("  Erster Abruf:             {0}" -f $(if ($r.Ergebnis) { $r.Ergebnis } else { '(noch kein Ergebnis - spaeter im Log nachsehen)' })) 'Cyan'
    if ($r.Usage) {
        Sag ("  Modell-Zeile (Fable):     {0}" -f $r.Usage) 'Yellow'
        Sag '  Hinweis: Mit diesem Token kommen die Hauptwerte an, die Modell-Zeile (Fable) aber nicht.' 'Yellow'
        Sag '  Wenn dir die fehlt: "Clawdmeter Token entfernen" stellt den alten Weg wieder her.' 'Yellow'
    }
}

function VariableGesetzt { [bool]([Environment]::GetEnvironmentVariable($VarName, 'User')) }

function Entferne {
    [Environment]::SetEnvironmentVariable($VarName, $null, 'User')
    Sag ("  {0}: {1}" -f $VarName, $(if (VariableGesetzt) { 'IMMER NOCH gesetzt' } else { 'nicht gesetzt' })) 'Green'
    $ab = LogZeilen
    if (DaemonNeustart $null) { Zeig (LogBeobachten $ab 40) }
}

function ZwischenablageLeeren {
    try { Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Clipboard]::Clear() } catch { }
    try {
        $null = [Windows.ApplicationModel.DataTransfer.Clipboard, Windows.ApplicationModel.DataTransfer, ContentType = WindowsRuntime]
        [void][Windows.ApplicationModel.DataTransfer.Clipboard]::ClearHistory()
    } catch { }
}

try {
    Sag '=== Clawdmeter: langlebiges Token ===' 'White'
    Sag ''

    if ($Entfernen) {
        Sag 'Die Variable wird geloescht. Danach nutzt der Daemon wieder die Anmeldung von Claude Code.'
        Entferne
        Ende 0
    }

    Sag 'Was gleich passiert:'
    Sag '  1. "claude setup-token" startet. Es oeffnet den Browser - dort anmelden und bestaetigen.'
    Sag '  2. Danach zeigt es hier im Fenster ein Token an (beginnt mit sk-ant-oat...).'
    Sag '  3. Das Token markieren und kopieren, dann unten einfuegen. Die Eingabe bleibt unsichtbar.'
    Sag '  4. Der Daemon wird neu gestartet und geprueft.'
    Sag ''
    Sag 'Das Token niemandem schicken und in keinen Chat kopieren - auch nicht zu Claude.' 'Yellow'
    Sag ''

    $wert = $null
    if ($Trockenlauf) {
        # Der Trockenlauf schreibt einen Platzhalter und loescht ihn wieder - ein echtes Token ginge dabei verloren.
        if (VariableGesetzt) { Sag 'Trockenlauf abgebrochen: Es ist schon ein Token gesetzt, das dabei verloren ginge.' 'Red'; Ende 3 }
        Sag '[Trockenlauf: setup-token wird uebersprungen, Platzhalterwert]' 'DarkYellow'
        $wert = 'sk-ant-oat01-TROCKENLAUF-kein-echter-Wert-0000000000000000'
    } else {
        $claude = Join-Path $env:USERPROFILE '.local\bin\claude.exe'
        if (-not (Test-Path $claude)) { $claude = 'claude' }
        Write-Host 'Weiter mit einer beliebigen Taste ...'; [void][Console]::ReadKey($true)
        Sag ''
        & $claude setup-token
        Sag ''
        Sag '------------------------------------------------------------------' 'White'
        Sag 'Oben steht jetzt eine lange Zeile, die mit  sk-ant-oat01-  beginnt.' 'White'
        Sag 'DAS ist das Token (bei manchen Farbschemata orange dargestellt).' 'White'
        Sag '  - Mit der Maus die ganze Zeile markieren, vom s bis zum letzten Zeichen.'
        Sag '  - Kopieren: Enter oder Rechtsklick (bzw. Strg+C).'
        Sag '  - Unten einfuegen: Rechtsklick oder Strg+V. Es erscheint NICHTS - das ist Absicht.'
        Sag 'Bitte KEIN Foto und keinen Screenshot von diesem Fenster machen, solange' 'Yellow'
        Sag 'das Token zu sehen ist. Wer das Bild hat, hat das Token.' 'Yellow'
        Sag '------------------------------------------------------------------' 'White'
        Sag ''
        for ($i = 1; $i -le 3 -and -not $wert; $i++) {
            $sicher = Read-Host -AsSecureString 'Token hier einfuegen (Rechtsklick oder Strg+V), dann Enter. Leer = abbrechen'
            $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sicher)
            try { $roh = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
            $roh = ($roh -replace '\s', '')
            if (-not $roh) { Sag 'Abgebrochen - es wurde nichts geaendert.' 'Yellow'; Ende 1 }
            if ($roh -match '^sk-ant-oat\d\d-[A-Za-z0-9_\-]{40,}$') { $wert = $roh }
            else { Sag ("  Das sieht nicht nach einem setup-token aus (Laenge {0}, erwartet: beginnt mit sk-ant-oat). Bitte nochmal." -f $roh.Length) 'Red' }
            $roh = $null
        }
        if (-not $wert) { Sag 'Dreimal ungueltig - es wurde nichts geaendert.' 'Red'; Ende 1 }
        ZwischenablageLeeren
        Sag '  Zwischenablage geleert.'
    }

    [Environment]::SetEnvironmentVariable($VarName, $wert, 'User')
    $ok = ([Environment]::GetEnvironmentVariable($VarName, 'User') -ceq $wert)
    Sag ("  {0}: {1}" -f $VarName, $(if ($ok) { 'gesetzt' } else { 'NICHT gesetzt' })) $(if ($ok) { 'Green' } else { 'Red' })
    if (-not $ok) { $wert = $null; Ende 1 }

    $ab = LogZeilen
    $gestartet = DaemonNeustart $wert
    $wert = $null
    if (-not $gestartet) { Sag 'Die Variable ist gesetzt, aber der Daemon wurde nicht neu gestartet. Nach dem naechsten Abmelden/Anmelden gilt sie von selbst.' 'Yellow'; Ende 1 }

    $r = LogBeobachten $ab 60
    Zeig $r

    if ($r.Ergebnis -eq 'HTTP 401' -or $r.Ergebnis -eq 'HTTP 403') {
        Sag ''
        Sag 'Der Dienst lehnt dieses Token fuer die Nutzungsabfrage ab.' 'Red'
        $antwort = if ($Trockenlauf) { 'j' } else { Read-Host 'Variable wieder entfernen und zum alten Weg zurueck? (J/N)' }
        if ($antwort -match '^[jJyY]') { Entferne }
    } elseif ($Trockenlauf) {
        Sag '[Trockenlauf: raeume auf]' 'DarkYellow'; Entferne
    } else {
        Sag ''
        Sag 'Rueckweg jederzeit: "Clawdmeter Token entfernen" im Startmenue (oder diese Datei mit -Entfernen).'
    }
    Ende 0
}
catch {
    Sag ("FEHLER: {0}" -f $_.Exception.Message) 'Red'
    Ende 2
}
