' Startet claude-start.cmd ohne sichtbares Fenster.
'
' Timo, 2026-09-27: "Ist es möglich die PC session so zu machen, dass nicht die
' ganze Zeit ein CMD Fenster auf sein muss? Sondern so im Hintergrund?"
'
' Das Fenster war bis hierher Absicht (HANDOFF, 2026-09-24): Vertrauensfrage
' und Anmeldung sollten nicht unsichtbar haengen. Beides ist durch, die Sitzung
' laeuft seit Tagen und antwortet ueber Remote Control — die Bedingung fuer
' diesen Schritt ist damit erfuellt.
'
' Die Konsole ist weiterhin da, nur nicht sichtbar (Run ..., 0). Claude Code
' behaelt also seinen Bildschirmpuffer; es laeuft nicht "ohne Terminal".
'
' Was unsichtbar wird, muss eine Spur hinterlassen: claude-start.cmd schreibt
' jeden Neustart nach claude-neustarts.log. Eine durchdrehende Schleife ist
' sonst genau das, was niemand bemerkt.
Option Explicit

Dim shell, fso, heim, cmdPfad
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

heim = shell.ExpandEnvironmentStrings("%USERPROFILE%")
cmdPfad = heim & "\claude-start.cmd"

' Lieber einmal sichtbar scheitern als dauerhaft unsichtbar nichts tun.
If Not fso.FileExists(cmdPfad) Then
    MsgBox "claude-start.cmd nicht gefunden:" & vbCrLf & cmdPfad, _
           vbCritical, "Clawdmeter - Autostart"
    WScript.Quit 1
End If

' Laeuft schon eine Kette, nichts tun: Eine laufende Sitzung wird hier nie
' beendet (Anmeldung, versehentlicher Doppelklick). Gesucht wird das cmd.exe
' der Schleife bzw. eines offenen Fensters; dieses Skript selbst ist wscript,
' kann sich also nicht selbst finden. (2026-09-28, nachdem zwei Ketten liefen.)
If LaeuftSchon() Then WScript.Quit 0

shell.Run """" & cmdPfad & """", 0, False

Function LaeuftSchon()
    Dim wmi, p, zeile
    LaeuftSchon = False
    Set wmi = GetObject("winmgmts:\\.\root\cimv2")
    For Each p In wmi.ExecQuery("SELECT CommandLine FROM Win32_Process WHERE Name='cmd.exe'")
        zeile = LCase(p.CommandLine & "")
        If InStr(zeile, "claude-start.cmd") > 0 Or InStr(zeile, "claude-fenster.cmd") > 0 Then
            LaeuftSchon = True
            Exit Function
        End If
    Next
End Function
