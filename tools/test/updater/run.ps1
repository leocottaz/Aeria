# Banc d'essai de l'updater Windows (src/features/updater.cpp) : le vrai code, un faux pont, un serveur
# HTTP local et un faux Setup.exe. Ne contacte jamais GitHub et ne touche pas à l'installation de
# l'utilisateur. Prérequis : g++ (MSYS2 MINGW64) dans le PATH.
#
#   powershell -File tools\test\updater\run.ps1
#
# Scénarios : tag invalide ou piégé, fichier altéré, empreinte absente de SHA256SUMS, installateur ou
# SHA256SUMS en 404, puis une bonne version (le faux installateur doit être lancé avec les bons arguments).
$ErrorActionPreference = 'Stop'
if (-not (Get-Command g++ -EA 0)) { $env:PATH = "C:\msys64\mingw64\bin;$env:PATH" }
$t   = $PSScriptRoot
$src = (Resolve-Path "$t\..\..\..\src").Path
$work = Join-Path $env:TEMP 'aeria-updater-test'
$bin = "$work\bin"; $web = "$work\web"
Remove-Item $work -Recurse -Force -EA 0
New-Item -ItemType Directory $bin, "$web\dl" -Force | Out-Null

# 1. Compilation : faux Setup.exe et banc d'essai (updater.cpp compilé avec un serveur local et HTTP autorisé)
gcc -O1 "$t\fake_setup.c" -o "$bin\fake_setup.exe"
g++ -std=c++20 -O1 -include "$t\test_defs.h" -I $src "$t\upd_test.cpp" "$src\features\updater.cpp" "$src\features\updater_common.cpp" "$src\common\util.cpp" "$src\common\util_common.cpp" -lwinhttp -lbcrypt -lshell32 -lole32 -luuid -o "$bin\upd_test.exe"
if ($LASTEXITCODE -ne 0) { throw "compilation du banc d'essai échouée" }
New-Item "$bin\unins000.exe" -ItemType File | Out-Null   # « copie installée » pour CanSelfUpdate

# 2. Une « release » par scénario ; le faux installateur sert de fichier à télécharger.
$fake = [IO.File]::ReadAllBytes("$bin\fake_setup.exe")
$hash = (Get-FileHash "$bin\fake_setup.exe" -Algorithm SHA256).Hash.ToLower()
function Release($tag, $sumsLine, [bool]$withFile = $true, [bool]$withSums = $true) {
    $d = "$web\dl\$tag"; New-Item -ItemType Directory $d -Force | Out-Null
    $name = "Aeria-$tag-windows-x64-setup.exe"
    if ($withFile) { [IO.File]::WriteAllBytes("$d\$name", $fake) }
    if ($withSums) { [IO.File]::WriteAllText("$d\SHA256SUMS", ($sumsLine -replace '@NAME@', $name) + "`n") }
}
Release 'v9.9.9' "$hash  @NAME@"                       # correct
Release 'v9.9.8' ('0' * 64 + "  @NAME@")               # empreinte fausse : fichier « altéré »
Release 'v9.9.7' "$hash  autre-fichier.exe"            # le fichier n'est pas dans SHA256SUMS
Release 'v9.9.6' "$hash  @NAME@" -withFile $false      # installateur manquant
Release 'v9.9.5' '' -withSums $false                   # SHA256SUMS manquant

# 3. Serveur local, banc d'essai, nettoyage
$log = "$env:TEMP\aeria-fake-setup.log"
Remove-Item $log, "$env:TEMP\Aeria-update" -Recurse -Force -EA 0
$srv = Start-Job -FilePath "$t\serve.ps1" -ArgumentList $web, 8799
try {
    Start-Sleep -Seconds 2
    & "$bin\upd_test.exe"
    $code = $LASTEXITCODE
    Start-Sleep -Milliseconds 600
    "--- arguments reçus par le faux installateur (un seul lancement attendu) :"
    Get-Content $log -EA 0
    "code de sortie : $code"
    exit $code
} finally {
    Stop-Job $srv -EA 0; Remove-Job $srv -Force -EA 0
    Remove-Item "$env:TEMP\Aeria-update", $log, $work -Recurse -Force -EA 0
}
