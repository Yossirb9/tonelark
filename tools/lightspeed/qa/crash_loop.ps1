# Repeat the clean Lightroom import until it crashes; darktable writes the backtrace to %TEMP%\darktable_bt_*.txt
param([int]$Attempts = 5)
$env:LIGHTSPEED_NO_CRASH_DIALOG = "1"
Get-ChildItem $env:TEMP -Filter "darktable_bt_*.txt" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
for ($i = 1; $i -le $Attempts; $i++) {
  $r = powershell -ExecutionPolicy Bypass -File C:\lightspeed\qa\run_lrimport.ps1 -FreshConfig -FreshPhotos -Log "C:\lightspeed\qa\crash_try$i.log"
  $crashed = ($r -match "PROCESS EXITED|caught exception")
  "attempt ${i}: " + ($r -join " | ")
  if ($crashed) { break }
}
Get-Process darktable -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 3
Get-ChildItem $env:TEMP -Filter "darktable_bt_*.txt" -ErrorAction SilentlyContinue | Select-Object FullName, Length
