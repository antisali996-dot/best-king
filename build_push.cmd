@echo off
rem ============================================================
rem best-king build_push.cmd -- build with NDK r26d and push
rem   to /data/adb/JAY/King.sh
rem usage: build_push.cmd
rem   NDK 路径解析顺序: %%BESTKING_NDK%% -> ..\xiaogu-kernel\tools\_vendor\android-ndk-r26d
rem ============================================================
setlocal
cd /d "%~dp0"

set "NDK=%~dp0..\xiaogu-kernel\tools\_vendor\android-ndk-r26d"
if defined BESTKING_NDK set "NDK=%BESTKING_NDK%"
if not exist "%NDK%\ndk-build.cmd" (
    echo [x] NDK not found: %NDK%
    echo     set BESTKING_NDK=C:\path\to\android-ndk-r26d  and retry
    exit /b 2
)

call "%NDK%\ndk-build.cmd" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk NDK_APPLICATION_MK=./Application.mk NDK_OUT=obj NDK_LIBS_OUT=libs -j8
if errorlevel 1 (
    echo [x] build failed
    exit /b 1
)

if not exist "libs\arm64-v8a\King.sh" (
    echo [x] artifact missing: libs\arm64-v8a\King.sh
    exit /b 1
)
echo [ok] built: libs\arm64-v8a\King.sh

set "PUSH=%~dp0..\xiaogu-kernel\tools\jay_push.cmd"
if exist "%PUSH%" (
    call "%PUSH%" "libs\arm64-v8a\King.sh" King.sh
    if errorlevel 1 exit /b 1
) else (
    set "ADB=adb"
    "%ADB%" shell "su -c 'mkdir -p /data/adb/JAY && chmod 700 /data/adb/JAY'"
    "%ADB%" exec-in su -c "cat > /data/adb/JAY/King.sh" < "libs\arm64-v8a\King.sh"
    if errorlevel 1 ( echo [x] push failed & exit /b 1 )
    "%ADB%" shell "su -c 'chmod 777 /data/adb/JAY/King.sh'"
    echo [ok] pushed: /data/adb/JAY/King.sh
)

echo.
echo run on device:
echo   adb shell "su -c '/data/adb/JAY/King.sh selftest'"
echo   adb shell "su -c '/data/adb/JAY/King.sh dumpgame /data/adb/JAY/game.bin /data/adb/JAY/game.idx'"
exit /b 0
