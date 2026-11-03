# Génère les phrases de test de la commande vocale avec la voix française de Windows (Hortense) :
# WAV 16 kHz mono 16 bits. Usage : make-wavs.ps1 -Out <dossier>
param([Parameter(Mandatory)][string]$Out)
Add-Type -AssemblyName System.Speech
New-Item -ItemType Directory -Force $Out | Out-Null
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice('Microsoft Hortense Desktop')
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$phrases = [ordered]@{
  'wake_cmd'  = 'Aeria, mets un minuteur de dix minutes.'
  'wake_only' = 'Aeria.'
  'command'   = 'Quelle heure est-il ?'
  'other'     = "Bonjour, il fait très beau aujourd'hui, je vais au marché."
}
foreach ($k in $phrases.Keys) {
  $s.SetOutputToWaveFile((Join-Path $Out "$k.wav"), $fmt)
  $s.Speak($phrases[$k])
  $s.SetOutputToNull()
}
