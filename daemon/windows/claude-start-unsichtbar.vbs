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
           vbCritical, "Clawdmeter — Autostart"
    WScript.Quit 1
End If

shell.Run """" & cmdPfad & """", 0, False
