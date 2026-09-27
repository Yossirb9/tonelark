# Run a Lightroom catalog import in Lightspeed and wait for it to finish.
param([string[]]$DebugArgs = @(), [string]$Cfg = "C:\lightspeed\qa\lrimport-cfg", [switch]$FreshConfig, [switch]$FreshPhotos,
      [string]$Catalog = "C:/lightspeed/qa/lrtest/Lightroom/Test Catalog.lrcat", [string]$Log = "C:\lightspeed\qa\lrimport_run.log")
Get-Process darktable -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
if ($FreshPhotos) { python C:\lightspeed\qa\make_lrcat.py | Out-Null }
if ($FreshConfig) {
  Remove-Item -Recurse -Force $Cfg -ErrorAction SilentlyContinue
  if (Test-Path $Cfg) { throw "config dir still exists" }
}
New-Item -ItemType Directory -Force $Cfg | Out-Null
Remove-Item "$Cfg\*.lock" -ErrorAction SilentlyContinue
if (-not (Test-Path "$Cfg\darktablerc")) { Set-Content "$Cfg\darktablerc" "ui/show_welcome_screen=FALSE" -Encoding ascii }
Remove-Item $Log -ErrorAction SilentlyContinue
$argList = @('--configdir', ($Cfg -replace '\\', '/'), '--import-lightroom', ('"' + $Catalog + '"')) + $DebugArgs
$p = Start-Process -FilePath C:\lightspeed\install\bin\darktable.exe -PassThru -ArgumentList $argList `
  -WorkingDirectory C:\lightspeed\install\bin -RedirectStandardOutput $Log -RedirectStandardError "$Log.err"
$deadline = (Get-Date).AddSeconds(300)
do { Start-Sleep 2; $done = Select-String -Path $Log -Pattern "lightroom catalog\] imported" -Quiet -ErrorAction SilentlyContinue }
while (-not $done -and -not $p.HasExited -and (Get-Date) -lt $deadline)
Start-Sleep 3
if ($p.HasExited) { "PROCESS EXITED (code $($p.ExitCode))" }
Select-String -Path $Log,"$Log.err" -Pattern "lightroom catalog\]|caught exception" | ForEach-Object { $_.Line }
