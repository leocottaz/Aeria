# Compare la mémoire du dock avec différentes options Chromium (sans recompiler).
# Usage : tools/dev/mem-flags.ps1 -Flags @('', '--in-process-gpu', ...)
param([string[]]$Flags = @(''))
$exe = Join-Path $PSScriptRoot '..\..\build\Aeria.exe'
$mem = Join-Path $PSScriptRoot 'mem.ps1'
foreach ($f in $Flags) {
    Get-Process Aeria -ErrorAction SilentlyContinue | Stop-Process
    Start-Sleep -Seconds 4
    if ($f) { $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = $f } else { Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS -ErrorAction SilentlyContinue }
    Start-Process $exe; Start-Sleep -Seconds 22
    & $mem -Label "$f | cache"
    Start-Process $exe; Start-Sleep -Seconds 4   # 2e instance = ouvrir le panneau
    & $mem -Label "$f | ouvert"
    Start-Process $exe; Start-Sleep -Seconds 18  # 2e appel = refermer
    & $mem -Label "$f | recache"
}
Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS -ErrorAction SilentlyContinue
