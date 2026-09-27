# Launch the installed Lightspeed build for QA and capture its window once it is up.
param([string]$Cfg = "C:\lightspeed\qa\gui-cfg3", [switch]$Fresh, [string]$Args2 = "C:/lightspeed/qa/hlrecovery.arw",
      [string]$Out = "C:\lightspeed\qa\shot.png", [int]$Wait = 20, [string]$Lua = "", [string]$Exe = "darktable.exe")
Get-Process darktable,Lightspeed -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 5
if ($Fresh) { Remove-Item -Recurse -Force $Cfg -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $Cfg | Out-Null
Remove-Item "$Cfg\*.lock" -ErrorAction SilentlyContinue
if (-not (Test-Path "$Cfg\darktablerc")) { Set-Content -Path "$Cfg\darktablerc" -Value "ui/show_welcome_screen=FALSE" -Encoding ascii }
$argList = @('--configdir', ($Cfg -replace '\\','/'))
if ($Lua -ne "") { $argList += @('--luacmd', $Lua) }
if ($Args2 -ne "") { $argList += $Args2 }
$argList = $argList | ForEach-Object { if ($_ -match '\s') { '"' + ($_ -replace '"','\"') + '"' } else { $_ } }
Start-Process -FilePath "C:\lightspeed\install\bin\$Exe" -ArgumentList $argList -WorkingDirectory "C:\lightspeed\install\bin"
$deadline = (Get-Date).AddSeconds(240)
do {
  Start-Sleep 3
  $p = Get-Process darktable,Lightspeed -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -eq "Lightspeed" }
} while (-not $p -and (Get-Date) -lt $deadline)
Start-Sleep $Wait
powershell -ExecutionPolicy Bypass -File C:\lightspeed\capture.ps1 -Out $Out -ProcName ([IO.Path]::GetFileNameWithoutExtension($Exe))
