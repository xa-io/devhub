# Renders the XA DevHub application icon and packs it into src\app\devhub.ico.
# Matches the in-app branding: dark navy rounded square, blue border ring,
# "X" in white + "A" in the accent blue. Sizes 256..16, PNG-compressed ICO
# entries (Vista+). Rerun after style changes: powershell -File make_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# Fast ink-bounding-box scan (LockBits; GetPixel would take minutes on the
# supersampled scratch layers). Returns minX,minY,maxX,maxY of pixels with
# alpha >= alphaMin, or maxX=-1 when the layer is empty.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class InkScan {
    public static int[] Box(Bitmap b, int alphaMin) {
        Rectangle r = new Rectangle(0, 0, b.Width, b.Height);
        BitmapData d = b.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        int minX = int.MaxValue, maxX = -1, minY = int.MaxValue, maxY = -1;
        byte[] row = new byte[d.Stride];
        for (int y = 0; y < b.Height; y++) {
            Marshal.Copy(IntPtr.Add(d.Scan0, y * d.Stride), row, 0, d.Stride);
            for (int x = 0; x < b.Width; x++) {
                if (row[x * 4 + 3] < alphaMin) continue;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
        b.UnlockBits(d);
        return new int[] { minX, minY, maxX, maxY };
    }
}
'@

$out = Join-Path $PSScriptRoot '..\src\app\devhub.ico'
$out = [System.IO.Path]::GetFullPath($out)

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

$cBgTop   = [System.Drawing.Color]::FromArgb(255, 0x22, 0x2f, 0x4a)
$cBgBot   = [System.Drawing.Color]::FromArgb(255, 0x0d, 0x11, 0x17)
$cAccent  = [System.Drawing.Color]::FromArgb(255, 0x4f, 0x8f, 0xf7)
$cWhite   = [System.Drawing.Color]::FromArgb(255, 0xe8, 0xed, 0xf5)

$sizes = 256, 128, 64, 48, 32, 24, 16
$entries = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $scale = $s / 256.0
    $inset = [Math]::Max(1.0, 6.0 * $scale)
    $w = $s - 2 * $inset
    $rad = [Math]::Max(2.0, 46.0 * $scale)

    $path = New-RoundRect $inset $inset $w $w $rad
    $rect = New-Object System.Drawing.RectangleF(0, 0, $s, $s)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $cBgTop, $cBgBot, 65.0)
    $g.FillPath($brush, $path)

    # accent border ring (skip on the tiniest sizes - it would smear)
    if ($s -ge 32) {
        $penW = [Math]::Max(2.0, 11.0 * $scale)
        $pen = New-Object System.Drawing.Pen($cAccent, $penW)
        $bi = $inset + $penW / 2.0
        $bpath = New-RoundRect $bi $bi ($s - 2 * $bi) ($s - 2 * $bi) ([Math]::Max(2.0, $rad - $penW / 2.0))
        $g.DrawPath($pen, $bpath)
        $pen.Dispose(); $bpath.Dispose()
    }

    # "XA" - X white, A accent (the app's own header styling). The letters
    # are rendered supersampled on a transparent scratch layer, the layer's
    # actual ink bounding box is measured, and the layer is composited so
    # that ink box lands dead-center. Centering on MeasureString layout
    # metrics (the old way) drifts left: they include per-glyph side
    # bearings that aren't visible ink.
    $fontPx = 118.0 * $scale
    if ($s -le 24) { $fontPx = 130.0 * $scale }  # chunkier when tiny
    # At tiny sizes the accent-blue A sinks into the dark fill and only the
    # white X registers, which reads as a left-shifted icon. Lighten the A
    # there; full sizes keep the true accent.
    $cA = if ($s -le 24) { [System.Drawing.Color]::FromArgb(255, 0x8a, 0xb8, 0xff) } else { $cAccent }

    $ss = 4  # supersample factor - also smooths the tiny sizes
    $scr = New-Object System.Drawing.Bitmap(($s * $ss), ($s * $ss), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    $gs = [System.Drawing.Graphics]::FromImage($scr)
    $gs.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $gs.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $gs.Clear([System.Drawing.Color]::Transparent)
    $font = New-Object System.Drawing.Font('Segoe UI', ($fontPx * $ss), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $szX = $gs.MeasureString('X', $font, [System.Drawing.PointF]::Empty, $fmt)
    $pad = 2.0 * $ss
    $bX = New-Object System.Drawing.SolidBrush($cWhite)
    $bA = New-Object System.Drawing.SolidBrush($cA)
    $gs.DrawString('X', $font, $bX, $pad, $pad, $fmt)
    $gs.DrawString('A', $font, $bA, ($pad + $szX.Width), $pad, $fmt)
    $gs.Dispose()

    $box = [InkScan]::Box($scr, 24)
    if ($box[2] -lt 0) { throw "icon text rendered empty at size $s" }
    $inkCx = ($box[0] + $box[2] + 1) / 2.0
    $inkCy = ($box[1] + $box[3] + 1) / 2.0
    $ox = $s / 2.0 - $inkCx / $ss
    $oy = $s / 2.0 - $inkCy / $ss
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $dest = New-Object System.Drawing.RectangleF($ox, $oy, $s, $s)
    $g.DrawImage($scr, $dest)

    $bX.Dispose(); $bA.Dispose(); $font.Dispose(); $scr.Dispose()
    $brush.Dispose(); $path.Dispose(); $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += , @{ Size = $s; Data = $ms.ToArray() }
    $ms.Dispose(); $bmp.Dispose()
}

# ICO container: 6-byte header + 16-byte directory entries + PNG payloads.
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)                # reserved
$bw.Write([uint16]1)                # type: icon
$bw.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $b = if ($e.Size -ge 256) { 0 } else { $e.Size }
    $bw.Write([byte]$b)             # width  (0 = 256)
    $bw.Write([byte]$b)             # height (0 = 256)
    $bw.Write([byte]0)              # palette
    $bw.Write([byte]0)              # reserved
    $bw.Write([uint16]1)            # planes
    $bw.Write([uint16]32)           # bpp
    $bw.Write([uint32]$e.Data.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write($e.Data) }
$bw.Close(); $fs.Close()

Write-Host "wrote $out ($((Get-Item $out).Length) bytes, $($entries.Count) sizes)"
