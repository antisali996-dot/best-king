@echo off
rem ============================================================
rem pull.cmd -- pull a file from /data/adb/JAY to local (binary safe)
rem usage: pull.cmd <remote_name> <local_file>
rem ============================================================
setlocal
if "%~1"=="" (
    echo usage: pull.cmd ^<remote_name^> ^<local_file^>
    exit /b 2
)
if "%~2"=="" (
    echo usage: pull.cmd ^<remote_name^> ^<local_file^>
    exit /b 2
)
set "ADB=%~dp0..\xiaogu-kernel\tools\_vendor\platform-tools\adb.exe"
if not exist "%ADB%" set "ADB=adb"
"%ADB%" exec-out su -c "cat /data/adb/JAY/%~1" > "%~2"
if errorlevel 1 (
    echo [x] pull failed: /data/adb/JAY/%~1
    exit /b 1
)
for %%F in ("%~2") do echo [ok] saved: %%~fF (%%~zF bytes)
exit /b 0
