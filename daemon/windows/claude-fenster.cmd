@echo off
chcp 65001 >nul
title Claude Code - Fenster
rem Oeffnet Claude Code sichtbar (Desktop-Verknuepfung "Claude Code — Fenster oeffnen").
rem Laeuft schon ein Start - unsichtbar ueber claude-start-unsichtbar.vbs oder ein
rem frueheres Fenster -, wird er vorher beendet: Zwei Sitzungen mit
rem --remote-control Timo-PC sollen nie parallel laufen.
rem
rem Gesucht wird das cmd.exe der Schleife, nicht wscript.exe: Die .vbs startet mit
rem Run ..., 0, False und beendet sich sofort. taskkill /T nimmt claude.exe mit,
rem sonst startete die Schleife neu. Das eigene cmd (Eltern von powershell) ist
rem ausgenommen. Kein for /f: Dessen Hilfs-cmd traegt den Suchbegriff selbst in der
rem Befehlszeile, und die Anfuehrungszeichen zerlegt es ohnehin.
powershell -NoProfile -Command "$ich = (Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -eq $PID }).ParentProcessId; Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'cmd.exe' -and $_.ProcessId -ne $ich -and ($_.CommandLine -like '*claude-start.cmd*' -or $_.CommandLine -like '*claude-fenster.cmd*') } | ForEach-Object { Write-Host ('Laufender Start (PID ' + $_.ProcessId + ') wird beendet.'); taskkill /PID $_.ProcessId /T /F }"
timeout /t 2 /nobreak >nul
call "%USERPROFILE%\claude-start.cmd"
