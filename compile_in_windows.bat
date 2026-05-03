@echo off
echo ========================================================
echo   Auto-Compile qmi_tool on Windows via Android NDK
echo ========================================================
echo.

:: Detect the NDK clang compiler path automatically based on the known installation
set NDK_PATH=C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\27.0.12077973\toolchains\llvm\prebuilt\windows-x86_64\bin
set COMPILER="%NDK_PATH%\aarch64-linux-android30-clang.cmd"

echo [1] Compiling native\qmi_tool.c for Android (aarch64)...
call %COMPILER% -o qmi_tool native\qmi_tool.c -Wall -O2

if %errorlevel% neq 0 (
    echo.
    echo Compilation FAILED! Sila periksa error di atas.
    pause
    exit /b %errorlevel%
)

echo.
echo [2] Compilation SUCCESS. Pushing qmi_tool to /data/local/tmp/...
adb push qmi_tool /data/local/tmp/

echo.
echo [3] Setting executable permissions...
adb shell "su -c 'chmod 755 /data/local/tmp/qmi_tool'"

echo.
echo [4] Testing qmi_tool connection...
adb shell "su -c '/data/local/tmp/qmi_tool test'"

echo.
echo ========================================================
echo Done! Jika status:"PASS", bermakna berjaya di-compile!
echo ========================================================
pause
