# Lance le dock avec le port de débogage et chaque jeu d'options Chromium, ouvre le panneau,
# affiche le mode de compositing (GPU ou logiciel) puis la fluidité (frames.ps1).
# Usage : tools/dev/frames-flags.ps1 -Flags @('', '--in-process-gpu')
param([string[]]$Flags = @(''), [int]$Port = 9333)
$exe = Join-Path $PSScriptRoot '..\..\build\Aeria.exe'
foreach ($f in $Flags) {
    Get-Process Aeria -ErrorAction SilentlyContinue | Stop-Process
    Start-Sleep -Seconds 3
    $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=$Port $f".Trim()
    Start-Process $exe
    for ($i = 0; $i -lt 30; $i++) { try { Invoke-RestMethod "http://127.0.0.1:$Port/json" -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep 1 } }
    Start-Sleep 2
    & "$PSScriptRoot\cdp.ps1" -Expr "chrome.webview.postMessage('open'); 'ok'" -Port $Port | Out-Null
    Start-Sleep 1.5
    "=== [$f]"
    & "$PSScriptRoot\frames.ps1" -Port $Port
}
Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS -ErrorAction SilentlyContinue
