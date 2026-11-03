# Regenere src/app/aeria.ico : le meme orbe que MakeOrbIcon (src/app/tray.cpp), en plusieurs tailles.
# Le fichier genere est commite : ce script ne sert que si le dessin de l'orbe change.
param([string]$Out = (Join-Path $PSScriptRoot '..\..\src\app\aeria.ico'))
Add-Type -AssemblyName System.Drawing

function Get-Orb([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $c = $size / 2.0; $r = $size / 2.0 - 0.5
    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $dx = $x + 0.5 - $c; $dy = $y + 0.5 - $c
            $cov = [Math]::Min(1.0, [Math]::Max(0.0, $r - [Math]::Sqrt($dx * $dx + $dy * $dy) + 0.5))
            $t = ($x + $y) / (2.0 * $size)
            $R = 150 + (80 - 150) * $t; $G = 110 + (210 - 110) * $t; $B = 255 + (250 - 255) * $t
            $hx = $x - $size * 0.34; $hy = $y - $size * 0.3
            $h = [Math]::Max(0.0, 1.0 - [Math]::Sqrt($hx * $hx + $hy * $hy) / ($size * 0.38)) * 0.65
            $R += (255 - $R) * $h; $G += (255 - $G) * $h; $B += (255 - $B) * $h
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb([int][Math]::Floor($cov * 255), [int]$R, [int]$G, [int]$B))
        }
    }
    return $bmp
}

# Image d'un .ico en BMP 32 bits : en-tete sans BITMAPFILEHEADER, hauteur doublee, lignes du bas vers le haut, puis masque.
function Get-DibBytes($bmp) {
    $size = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([int]40); $w.Write([int]$size); $w.Write([int]($size * 2))
    $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0); $w.Write([int]0)
    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $w.Write([byte]$p.B); $w.Write([byte]$p.G); $w.Write([byte]$p.R); $w.Write([byte]$p.A)
        }
    }
    $w.Write((New-Object byte[] ([Math]::Floor(($size + 31) / 32) * 4 * $size))) # masque vide : l'alpha fait foi
    return , $ms.ToArray()
}

$images = foreach ($size in 16, 20, 24, 32, 40, 48, 64, 128, 256) {
    $bmp = Get-Orb $size
    if ($size -eq 256) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png) # 256 px : PNG, comme le font les outils Windows
        $data = $ms.ToArray()
    } else {
        $data = Get-DibBytes $bmp
    }
    $bmp.Dispose()
    [pscustomobject]@{ Size = $size; Data = $data }
}

$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {
    $dim = if ($i.Size -eq 256) { 0 } else { $i.Size } # 0 = 256 dans un en-tete .ico
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([int16]1); $w.Write([int16]32); $w.Write([int]$i.Data.Length); $w.Write([int]$offset)
    $offset += $i.Data.Length
}
foreach ($i in $images) { $w.Write([byte[]]$i.Data) }
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($Out), $ms.ToArray())
