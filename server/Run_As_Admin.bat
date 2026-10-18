@echo off
title UltraDisplay Host Server (Administrator)
cd /d "%~dp0"
echo Stopping old server instances...
taskkill /f /im UltraDisplayServer_Admin.exe >nul 2>&1
taskkill /f /im UltraDisplayServer.exe >nul 2>&1
taskkill /f /im UltraDisplayServer_v2.exe >nul 2>&1
echo Launching UltraDisplay Server v2 with Windows Ink Administrator Privileges...
start "" UltraDisplayServer_v2.exe
