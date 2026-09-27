# Run the Lightroom import under gdb until it crashes (max N attempts), keep the backtrace.
param([int]$Attempts = 3, [int]$Seconds = 150)
$env:PATH = "C:\lightspeed\install\bin;C:\msys64\ucrt64\bin;$env:PATH"
$gdbcmd = "C:\lightspeed\qa\gdb_lr.cmd"
Set-Content $gdbcmd @"
set pagination off
set confirm off
handle SIGSEGV stop print
run
bt 30
thread apply all bt 8
quit
"@ -Encoding ascii
for ($i = 1; $i -le $Attempts; $i++) {
  Get-Process darktable,gdb -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 2
  python C:\lightspeed\qa\make_lrcat.py | Out-Null
  $cfg = "C:\lightspeed\qa\lrimport-cfg"
  Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
  New-Item -ItemType Directory $cfg | Out-Null
  Set-Content "$cfg\darktablerc" "ui/show_welcome_screen=FALSE" -Encoding ascii
  $log = "C:\lightspeed\qa\gdb_try$i.log"
  $p = Start-Process -FilePath C:\msys64\ucrt64\bin\gdb.exe -PassThru -NoNewWindow `
    -ArgumentList '-batch','-x',$gdbcmd,'--args','C:\lightspeed\install\bin\darktable.exe','--configdir','C:/lightspeed/qa/lrimport-cfg','--import-lightroom','"C:/lightspeed/qa/lrtest/Lightroom/Test Catalog.lrcat"' `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err"
  $deadline = (Get-Date).AddSeconds($Seconds)
  while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep 3 }
  $crashed = Select-String -Path $log -Pattern "SIGSEGV|Segmentation|exception" -Quiet
  "attempt $i crashed=$crashed"
  if ($crashed) { break }
}
Get-Process darktable,gdb -ErrorAction SilentlyContinue | Stop-Process -Force
