# Reports recent application crashes, with the FAULTING MODULE -- which is the one fact that says
# whether a game death is our payload or the game's own.
$ev = Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'; StartTime=(Get-Date).AddHours(-4)} -ErrorAction SilentlyContinue
if (-not $ev) { Write-Output "no Application Error events in the last 4 hours"; exit 0 }
foreach ($e in ($ev | Select-Object -First 6)) {
    $f = $e.Properties | ForEach-Object { $_.Value }
    Write-Output ("{0}  app={1}  faulting_module={2}  code={3}  offset={4}" -f `
        $e.TimeCreated.ToString('HH:mm:ss'), $f[0], $f[3], $f[6], $f[7])
}
