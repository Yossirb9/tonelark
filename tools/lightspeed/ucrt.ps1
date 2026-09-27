# Run a bash command inside the MSYS2 UCRT64 environment.
# usage: powershell -File C:\lightspeed\ucrt.ps1 "command"
param([Parameter(Mandatory=$true)][string]$Cmd)
$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"
$env:MSYS2_PATH_TYPE = "minimal"
& C:\msys64\usr\bin\bash.exe -lc $Cmd
exit $LASTEXITCODE
