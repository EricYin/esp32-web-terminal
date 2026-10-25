@echo off
setlocal

:: SPDX-License-Identifier: MIT
:: Copyright (c) 2026 Dennis Gilmore
::
:: Generate a self-signed ECC P-256 certificate for the ESP32 HTTPS server.
:: Run this once before building. The generated cert/key are embedded in firmware.

:: 获取脚本所在目录并切换至该路径
set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%" || exit /b 1

echo Generating ECC P-256 private key...
openssl ecparam -genkey -name prime256v1 -noout -out server.key
if %errorlevel% neq 0 (echo Error: Failed to generate private key. & exit /b 1)

echo Generating self-signed certificate (valid 10 years)...
openssl req -new -x509 -key server.key -out server.crt -days 3650 ^
    -subj "/CN=ESP32 Web Terminal/O=ESP32/C=US" ^
    -addext "subjectAltName=IP:192.168.4.1,DNS:esp-terminal.local"
if %errorlevel% neq 0 (echo Error: Failed to generate certificate. & exit /b 1)

echo Done. Files created:
echo   "%SCRIPT_DIR%server.crt"
echo   "%SCRIPT_DIR%server.key"
echo.
echo Certificate fingerprint:
openssl x509 -in server.crt -noout -fingerprint -sha256
if %errorlevel% neq 0 (echo Error: Failed to print fingerprint. & exit /b 1)

endlocal
