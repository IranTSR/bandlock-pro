@echo off
echo ========================================================
echo   Auto-Compile qmi_tool in Termux via ADB
echo ========================================================
echo.

echo [1] Pushing source code to /data/local/tmp/...
adb push native\qmi_common.h /data/local/tmp/
adb push native\qmi_tool.c /data/local/tmp/

echo.
echo [2] Compiling in Termux via ADB shell...
echo (Pastikan aplikasi Termux dah terinstall dan 'pkg install clang' dah dibuat)
adb shell "su -c 'cp /data/local/tmp/qmi_common.h /data/data/com.termux/files/home/ && cp /data/local/tmp/qmi_tool.c /data/data/com.termux/files/home/ && chown -R 10243:10243 /data/data/com.termux/files/home/qmi_*'"
adb shell "su -c 'cd /data/data/com.termux/files/home && /data/data/com.termux/files/usr/bin/clang -o qmi_tool qmi_tool.c -Wall -O2 && cp qmi_tool /data/local/tmp/ && chmod 755 /data/local/tmp/qmi_tool'"

echo.
echo [3] Testing qmi_tool connection...
adb shell su -c "/data/local/tmp/qmi_tool test"

echo.
echo ========================================================
echo Done! Jika status:"connected", bermakna berjaya!
echo ========================================================
pause
