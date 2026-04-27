@echo off
echo ========================================================
echo   Starting BandLock Pro Web Dashboard
echo ========================================================
echo.

echo [1] Pushing Web Files to phone...
adb shell "mkdir -p /data/local/tmp/web"
adb push web\server.py /data/local/tmp/web/
adb push web\index.html /data/local/tmp/web/

echo.
echo [2] Starting Web Server in background...
echo (Anda perlukan Python di dalam Termux: 'pkg install python')
adb shell "su -c 'cp -r /data/local/tmp/web /data/data/com.termux/files/home/'"
adb shell "su -c 'cd /data/data/com.termux/files/home/web && nohup /data/data/com.termux/files/usr/bin/python server.py > server.log 2>&1 &'"

echo.
echo ========================================================
echo SERVER STARTED! 
echo.
echo Buka Google Chrome di phone anda dan taip:
echo http://localhost:8080
echo ========================================================
pause
