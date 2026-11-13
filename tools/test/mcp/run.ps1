# Banc d'essai du transport MCP Windows (src/features/mcp.cpp) : le vrai code, un faux pont et un faux serveur
# (fake_server.cpp). Aucun vrai serveur MCP, aucun réseau, aucun vrai dock. Prérequis : g++ (MSYS2 MINGW64) dans le PATH.
#
#   powershell -File tools\test\mcp\run.ps1
#
# Laisse un dossier vide « mcp » dans %LOCALAPPDATA%\Aeria (le dossier de travail des serveurs, créé aussi à l'usage réel).
$ErrorActionPreference = 'Stop'
if (-not (Get-Command g++ -EA 0)) { $env:PATH = "C:\msys64\mingw64\bin;$env:PATH" }
$t   = $PSScriptRoot
$src = (Resolve-Path "$t\..\..\..\src").Path
$work = Join-Path $env:TEMP 'aeria-mcp-test'
Remove-Item $work -Recurse -Force -EA 0
New-Item -ItemType Directory $work | Out-Null

g++ -std=c++20 -O1 "$t\fake_server.cpp" -o "$work\fake_server.exe"
if ($LASTEXITCODE -ne 0) { throw "compilation du faux serveur échouée" }
g++ -std=c++20 -O1 -I $src "$t\mcp_test.cpp" "$src\features\mcp.cpp" "$src\common\util.cpp" "$src\common\util_common.cpp" -lshell32 -lole32 -luuid -o "$work\mcp_test.exe"
if ($LASTEXITCODE -ne 0) { throw "compilation du banc d'essai échouée" }

try {
    & "$work\mcp_test.exe" "$work\fake_server.exe" "$work\cmd"
    $code = $LASTEXITCODE
    "code de sortie : $code"
    exit $code
} finally {
    Get-Process fake_server -EA 0 | Stop-Process -Force -EA 0
    Remove-Item $work -Recurse -Force -EA 0
}
