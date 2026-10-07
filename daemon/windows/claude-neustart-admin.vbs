' Startet die Claude-Code-Hintergrundsitzung neu - unsichtbar und mit Adminrechten.
'
' Ablauf: laufende Kette beenden (cmd.exe mit claude-start.cmd / claude-fenster.cmd,
' samt claude.exe), dann die geplante Aufgabe "Claude Code Hintergrund (Admin)"
' ausloesen. Die Aufgabe laeuft mit hoechsten Rechten; ausloesen darf sie der
' Benutzer ohne UAC-Abfrage.
'
' Grenze: Ohne Adminrechte gestartet kann dieses Skript eine Sitzung, die selbst
' mit Adminrechten laeuft, nicht beenden (und nicht einmal sehen). Dann meldet
' es das, statt eine zweite Sitzung daneben zu starten.
Option Explicit

Dim shell, wmi, p, zeile, gefunden, rest, ausgabe
Set shell = CreateObject("WScript.Shell")
Set wmi = GetObject("winmgmts:\\.\root\cimv2")

WScript.Sleep 3000   ' dem Ausloeser Zeit geben, sich zu verabschieden

gefunden = 0
For Each p In wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='cmd.exe'")
    zeile = LCase(p.CommandLine & "")
    If InStr(zeile, "claude-start.cmd") > 0 Or InStr(zeile, "claude-fenster.cmd") > 0 Then
        gefunden = gefunden + 1
        shell.Run "taskkill /PID " & p.ProcessId & " /T /F", 0, True
    End If
Next

WScript.Sleep 3000

' Laeuft noch ein claude.exe, war die Sitzung fuer uns nicht beendbar (Adminrechte).
rest = 0
' Die Claude-Desktop-App heisst ebenfalls claude.exe und zaehlt nicht: Ihre
' Befehlszeile ist lesbar und enthaelt kein --remote-control. Bei einer
' Admin-Sitzung ist die Befehlszeile fuer uns leer (Null).
For Each p In wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='claude.exe'")
    If IsNull(p.CommandLine) Then
        rest = rest + 1
    ElseIf InStr(LCase(p.CommandLine), "--remote-control") > 0 Then
        rest = rest + 1
    End If
Next
If rest > 0 Then
    MsgBox "Es laeuft noch eine Claude-Code-Sitzung, die sich nicht beenden liess " & _
           "(vermutlich mit Adminrechten gestartet)." & vbCrLf & vbCrLf & _
           "Bitte 'Claude Code starten - mit sichtbarem Fenster' nehmen - das fragt " & _
           "nach Adminrechten und kann sie beenden.", vbExclamation, "Claude Code - Neustart"
    WScript.Quit 1
End If

shell.Run "schtasks /run /tn ""Claude Code Hintergrund (Admin)""", 0, True
