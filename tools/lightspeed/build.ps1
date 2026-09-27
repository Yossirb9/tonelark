# Incremental build + install of Lightspeed. Usage: powershell -File C:\lightspeed\build.ps1 [-NoInstall]
param([switch]$NoInstall)
Get-Process darktable,Lightspeed,darktable-cli -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
$log = "C:\lightspeed\build_last.log"
$cmd = "cd /c/lightspeed/build && ninja -j12 > /c/lightspeed/build_last.log 2>&1; rc=`$?; echo NINJA_RC=`$rc >> /c/lightspeed/build_last.log"
if (-not $NoInstall) { $cmd += "; if [ `$rc -eq 0 ]; then cmake --install . > /c/lightspeed/install_last.log 2>&1; echo INSTALL_RC=`$? >> /c/lightspeed/build_last.log; fi" }
powershell -ExecutionPolicy Bypass -File C:\lightspeed\ucrt.ps1 $cmd
Select-String -Path $log -Pattern "error:|CMake Error|FAILED:" | Select-Object -First 15 | ForEach-Object { $_.Line }
Get-Content $log -Tail 2
