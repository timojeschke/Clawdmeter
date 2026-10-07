' Startet claude-start.cmd ohne sichtbares Fenster.
'
' Timo, 2026-09-27: "Ist es moeglich die PC session so zu machen, dass nicht die
' ganze Zeit ein CMD Fenster auf sein muss? Sondern so im Hintergrund?"
'
' Die Konsole ist weiterhin da, nur nicht sichtbar (Run ..., 0). Claude Code
' behaelt also seinen Bildschirmpuffer; es laeuft nicht "ohne Terminal".
'
' Was unsichtbar wird, muss eine Spur hinterlassen:
'   - claude-start.cmd schreibt jeden Neustart nach claude-neustarts.log.
'   - Dieses Skript schreibt jeden Aufruf nach claude-autostart.log (seit
'     2026-10-07: An dem Tag lief nach dem Hochfahren keine Sitzung, und es
'     liess sich nicht mehr feststellen, ob der Autostart ueberhaupt ausgeloest
'     hatte).
Option Explicit

Dim shell, fso, heim, cmdPfad, grund
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

heim = shell.ExpandEnvironmentStrings("%USERPROFILE%")
cmdPfad = heim & "\claude-start.cmd"

' Lieber einmal sichtbar scheitern als dauerhaft unsichtbar nichts tun.
If Not fso.FileExists(cmdPfad) Then
    Protokoll "FEHLER: claude-start.cmd fehlt"
    MsgBox "claude-start.cmd nicht gefunden:" & vbCrLf & cmdPfad, _
           vbCritical, "Clawdmeter - Autostart"
    WScript.Quit 1
End If

' Laeuft schon eine Sitzung, nichts tun: Eine laufende Sitzung wird hier nie
' beendet (Anmeldung, versehentlicher Doppelklick). (2026-09-28, nachdem zwei
' Ketten liefen.)
grund = LaeuftSchon()
If grund <> "" Then
    Protokoll "nichts getan - laeuft schon: " & grund
    WScript.Quit 0
End If

shell.Run """" & cmdPfad & """", 0, False
Protokoll "claude-start.cmd gestartet"

' Gibt "" zurueck, wenn keine Sitzung laeuft, sonst eine kurze Begruendung.
' Erkannt wird (a) das cmd.exe der Schleife bzw. eines offenen Fensters und
' (b) ein claude.exe mit --remote-control. Laeuft die Sitzung mit Adminrechten
' und dieses Skript ohne, ist deren Befehlszeile nicht lesbar (Null) - ein
' claude.exe ohne lesbare Befehlszeile zaehlt deshalb ebenfalls. Die Claude-
' Desktop-App heisst auch claude.exe, hat aber eine lesbare Befehlszeile ohne
' --remote-control und zaehlt nicht.
Function LaeuftSchon()
    Dim wmi, p, zeile
    LaeuftSchon = ""
    On Error Resume Next
    Set wmi = GetObject("winmgmts:\\.\root\cimv2")
    If Err.Number <> 0 Then
        ' Ohne WMI laesst sich nichts pruefen. Dann lieber starten als gar nicht laufen.
        Protokoll "WMI nicht erreichbar (" & Err.Description & ") - starte ohne Pruefung"
        Err.Clear
        Exit Function
    End If
    For Each p In wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='cmd.exe'")
        zeile = LCase(p.CommandLine & "")
        If InStr(zeile, "claude-start.cmd") > 0 Or InStr(zeile, "claude-fenster.cmd") > 0 Then
            LaeuftSchon = "cmd-Kette PID " & p.ProcessId
            Exit Function
        End If
    Next
    For Each p In wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='claude.exe'")
        If IsNull(p.CommandLine) Then
            LaeuftSchon = "claude.exe mit Adminrechten, PID " & p.ProcessId
            Exit Function
        ElseIf InStr(LCase(p.CommandLine), "--remote-control") > 0 Then
            LaeuftSchon = "claude.exe PID " & p.ProcessId
            Exit Function
        End If
    Next
    On Error GoTo 0
End Function

' Haengt eine Zeile an claude-autostart.log an. Scheitert das, laeuft der Start trotzdem.
Sub Protokoll(text)
    Dim f, rechte, dummy
    On Error Resume Next
    rechte = "normal"
    dummy = shell.RegRead("HKEY_USERS\S-1-5-19\Environment\TEMP")
    If Err.Number = 0 Then rechte = "Admin"
    Err.Clear
    Set f = fso.OpenTextFile(heim & "\claude-autostart.log", 8, True)
    f.WriteLine Now & " [" & rechte & "] claude-start-unsichtbar: " & text
    f.Close
    On Error GoTo 0
End Sub
