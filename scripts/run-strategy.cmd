@echo off
REM Launcher for the scheduled 08:55 strategy run.
REM NO credentials here - strategy.mjs reads CTP_USER / CTP_PASS (and optional
REM CTP_MD_FRONT / CTP_TD_FRONT / STRAT_* tunables) from the environment. Set them
REM once with `setx` so this scheduled task (running as you) inherits them.
cd /d D:\projects\ctp-node
echo ==================== strategy launch %DATE% %TIME% ==================== >> strategy.out.log
"C:\nvm4w\nodejs\node.exe" scripts\strategy.mjs >> strategy.out.log 2>&1
