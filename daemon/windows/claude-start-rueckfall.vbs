' Rueckfall fuer den Autostart der Claude-Code-Hintergrundsitzung.
'
' Liegt im Autostart-Ordner und laeuft bei jeder Anmeldung ohne Adminrechte.
' Wartet 90 Sekunden und prueft dann, ob die Sitzung laeuft. Wenn nicht, loest
' es die geplante Aufgabe "Claude Code Hintergrund (Admin)" aus (das darf der
' Benutzer ohne UAC-Abfrage). Greift auch das nicht, startet es die Sitzung
' ohne Adminrechte - lieber eine Sitzung ohne Admin als gar keine.
'
' Anlass, 2026-10-07: Nach dem Hochfahren um 17:04 lief keine Sitzung, bis
' Timo sie um 17:22 von Hand startete. Ob die Aufgabe bei der Anmeldung
' ausgeloest hatte, liess sich nicht mehr feststellen.
Option Explicit

Dim shell, fso, heim, grund, rc
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
heim = shell.ExpandEnvironmentStrings("%USERPROFILE%")

WScript.Sleep 90000

grund = LaeuftSchon()
If grund <> "" Then
    Protokoll "in Ordnung - Sitzung laeuft: " & grund
    WScript.Quit 0
End If

Protokoll "KEINE Sitzung 90 s nach der Anmeldung - loese die Admin-Aufgabe aus"
rc = shell.Run("schtasks /run /tn ""Claude Code Hintergrund (Admin)""", 0, True)
WScript.Sleep 20000

grund = LaeuftSchon()
If grund <> "" Then
    Protokoll "Admin-Aufgabe hat gestartet: " & grund
    WScript.Quit 0
End If

Protokoll "Admin-Aufgabe ohne Wirkung (schtasks rc=" & rc & ") - starte ohne Adminrechte"
shell.Run "wscript.exe """ & heim & "\claude-start-unsichtbar.vbs""", 0, False

' Wie in claude-start-unsichtbar.vbs: "" = keine Sitzung, sonst Begruendung.
Function LaeuftSchon()
    Dim wmi, p, zeile
    LaeuftSchon = ""
    On Error Resume Next
    Set wmi = GetObject("winmgmts:\\.\root\cimv2")
    If Err.Number <> 0 Then
        Protokoll "WMI nicht erreichbar (" & Err.Description & ")"
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

Sub Protokoll(text)
    Dim f
    On Error Resume Next
    Set f = fso.OpenTextFile(heim & "\claude-autostart.log", 8, True)
    f.WriteLine Now & " [normal] claude-start-rueckfall: " & text
    f.Close
    On Error GoTo 0
End Sub
