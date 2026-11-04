# Banc d'essai de la commande vocale (src/features/voice.cpp) : le vrai code avec un faux micro qui lit des
# phrases WAV dites par la voix française de Windows. Aucun micro réel, aucun message envoyé à Claude.
# Prérequis : g++ (MSYS2 MINGW64), un dossier de build Windows déjà compilé (pour les bibliothèques whisper)
# et le modèle ggml-small-q5_1.bin (celui que l'appli télécharge dans %APPDATA%\Aeria\models).
# Usage : run.ps1 [-Build build\release]   → attendu : « OK » partout, puis « RESULTAT heard=1 » ×2
param([string]$Build = 'build\release', [string]$Model = "$env:APPDATA\Aeria\models\ggml-small-q5_1.bin")
$ErrorActionPreference = 'Stop'
if (-not (Get-Command g++ -EA 0)) { $env:PATH = "C:\msys64\mingw64\bin;$env:PATH" }
$root = Resolve-Path "$PSScriptRoot\..\..\.."
$src = "$root\src"
$b = Resolve-Path (Join-Path $root $Build)
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'aeria-voice-test'
New-Item -ItemType Directory -Force $tmp | Out-Null

& "$PSScriptRoot\make-wavs.ps1" -Out "$tmp\wavs"
g++ -std=c++20 -O2 -I $src -I "$root\third_party\whisper.cpp\include" -I "$root\third_party\whisper.cpp\ggml\include" `
    "$PSScriptRoot\voice_test.cpp" "$src\features\voice.cpp" "$src\common\util.cpp" "$src\common\util_common.cpp" `
    "$b\whisper\src\libwhisper.a" "$b\whisper\ggml\src\ggml.a" "$b\whisper\ggml\src\ggml-cpu.a" "$b\whisper\ggml\src\ggml-base.a" `
    -lshell32 -lole32 -luuid -static-libgcc -static-libstdc++ -o "$tmp\voice_test.exe"

Write-Host '== « Aeria, mets un minuteur… » dit d’une traite'
& "$tmp\voice_test.exe" $Model "$tmp\wavs\wake_cmd.wav"
Write-Host '== « Aeria » puis la commande, deux phrases séparées'
& "$tmp\voice_test.exe" $Model "$tmp\wavs\wake_only.wav" "$tmp\wavs\command.wav" | Select-String -NotMatch '^OK'
Write-Host '== une phrase sans « Aeria » : rien ne doit se passer'
& "$tmp\voice_test.exe" $Model "$tmp\wavs\other.wav" | Select-String -NotMatch '^OK'
