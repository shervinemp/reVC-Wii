# Renders gamefiles/wii-hbc/icon.png (128x48, Homebrew Channel's icon size).
# Drawn at 4x, downscaled for clean antialiasing. Vice-City sunset:
# indigo->violet->teal gradient, low sun + glow, palm silhouette, wordmark.
# Regenerate any time; deploy-wii.bat copies icon.png with the rest of the
# app folder so the Homebrew Channel always shows the current artwork.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$S = 4
$W = 128 * $S; $H = 48 * $S
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$gfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# 1. Sky: 4-stop sunset gradient
$skyRect = New-Object System.Drawing.Rectangle(0, 0, $W, $H)
$skyBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $skyRect,
    [System.Drawing.Color]::FromArgb(255, 14, 4, 48),
    [System.Drawing.Color]::FromArgb(255, 0, 208, 190),
    [float](90))
$blend = New-Object System.Drawing.Drawing2D.ColorBlend
$blend.Colors = [System.Drawing.Color[]]@(
    [System.Drawing.Color]::FromArgb(255, 14, 4, 48),
    [System.Drawing.Color]::FromArgb(255, 102, 18, 126),
    [System.Drawing.Color]::FromArgb(255, 232, 90, 168),
    [System.Drawing.Color]::FromArgb(255, 0, 208, 190))
$blend.Positions = [float[]]@(0.0, 0.40, 0.74, 1.0)
$skyBrush.InterpolationColors = $blend
$gfx.FillRectangle($skyBrush, $skyRect)

# 2. Low sun with soft glow rings, drawn first so everything sits above.  It
#    sits half-off the right edge at horizon height, out of the wordmark's way.
for($i = 6; $i -ge 1; $i--){
    $alpha = [int](6 + 9 * (6 - $i))
    $r = [int]($i * 2.6 * $S)
    $glowPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb($alpha, 255, 216, 132), [float](3.0*$S))
    $gfx.DrawEllipse($glowPen, [int](120*$S - $r), [int](36*$S - $r/2), $r*2, $r)
    $glowPen.Dispose()
}
$sunBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 238, 160))
$gfx.FillEllipse($sunBrush, [int](116*$S), [int](32*$S), [int](9*$S), [int](9*$S))

# 3. Palm silhouette, left third. Crown radius list keeps the fronds
# symmetric around the trunk tip so the silhouette reads as one tree.
$dark = [System.Drawing.Color]::FromArgb(255, 15, 5, 38)
$trunkX = 24.0*$S
$topX = 29.0*$S; $topY = 11.0*$S; $baseY = 47.5*$S
$trunkPen = New-Object System.Drawing.Pen($dark, [float](2.8*$S))
$trunkPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$trunkPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$gfx.DrawBezier($trunkPen, $trunkX, $baseY, ($trunkX+2.2*$S), ($baseY-14*$S), ($trunkX-2.8*$S), ($topY+9*$S), $topX, $topY)
$trunkPen.Dispose()
$frondPen = New-Object System.Drawing.Pen($dark, [float](2.1*$S))
$frondPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$frondPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$fronds = @(
    @(0, -7, -16, 4),      # upper-left droop
    @(4, -9, 14, -2),      # upper-right droop
    @(16, -3, 23, 3),      # right droop
    @(-13, 3, -20, 10),    # left-down droop
    @(11, 4, 17, 12),      # right-down droop
    @(-4, -10, 4, -9)      # straight-up frond
)
foreach ($f in $fronds) {
    $a = @($topX, $topY)
    $b = @(($topX + $f[0]*$S*0.5), ($topY + $f[1]*$S*0.5))
    $c = @(($topX + $f[0]*$S*0.8), ($topY + $f[1]*$S*0.85))
    $d = @(($topX + $f[0]*$S), ($topY + $f[1]*$S))
    $gfx.DrawBezier($frondPen, $a[0], $a[1], $b[0], $b[1], $c[0], $c[1], $d[0], $d[1])
}
$frondPen.Dispose()
# crown knot joins the fronds
$gfx.FillEllipse((New-Object System.Drawing.SolidBrush($dark)), [float]($topX-2.4*$S), [float]($topY-2.4*$S), [float](4.8*$S), [float](4.8*$S))

# 4. Wordmark
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = [System.Drawing.StringAlignment]::Near
$fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
# The real GTA brand face, Pricedown (free desktop license, shipped beside the
# icon and loaded as a private font so rendering is identical everywhere).
$privateFonts = New-Object System.Drawing.Text.PrivateFontCollection
$fontPath = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc\fonts\Pricedown Bl.otf'
$privateFonts.AddFontFile($fontPath)
$fontBig = New-Object System.Drawing.Font($privateFonts.Families[0], [float](48.0), [System.Drawing.FontStyle]::Bold)
$vcX = 52.0*$S
$vcRect = New-Object System.Drawing.RectangleF([float]$vcX, [float](8.0*$S), [float](50.0*$S), [float](30.0*$S))
$shadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(190, 10, 2, 28))
$shadowRect = New-Object System.Drawing.RectangleF([float]($vcRect.X + 2.5*$S), [float]($vcRect.Y + 2.5*$S), $vcRect.Width, $vcRect.Height)
$gfx.DrawString('VC', $fontBig, $shadowBrush, $shadowRect, $fmt)
$whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 252, 255))
$gfx.DrawString('VC', $fontBig, $whiteBrush, $vcRect, $fmt)
# VICE CITY caps ribbon under the mark in the Rage face (the scratch script
# logotype the actual Vice City box art uses)
$fontRage = New-Object System.Drawing.Text.PrivateFontCollection
$fontPathRage = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc\fonts\Rage.ttf'
$fontRage.AddFontFile($fontPathRage)
$fontCap = New-Object System.Drawing.Font($fontRage.Families[0], [float](15.0), [System.Drawing.FontStyle]::Bold)
# soft shadow behind the caps so they read on both the violet and teal bands
$capsShadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(150, 10, 2, 28))
$capShadowRect = New-Object System.Drawing.RectangleF([float](59.0*$S + 1.2*$S), [float](38.0*$S + 1.2*$S), [float](64.0*$S), [float](9.0*$S))
$gfx.DrawString('VICE CITY', $fontCap, $capsShadowBrush, $capShadowRect, $fmt)
$capsBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(240, 250, 236, 250))
$capRect = New-Object System.Drawing.RectangleF([float](59.0*$S), [float](38.0*$S), [float](64.0*$S), [float](9.0*$S))
$gfx.DrawString('VICE CITY', $fontCap, $capsBrush, $capRect, $fmt)

# 5. 1px highlight frame
$edgePen = New-Object System.Drawing.Pen(([System.Drawing.Color]::FromArgb(150, 255, 255, 255)), $S)
$gfx.DrawRectangle($edgePen, 0, 0, $W-1, $H-1)

# Downscale to 128x48
$out = New-Object System.Drawing.Bitmap(128, 48)
$outGfx = [System.Drawing.Graphics]::FromImage($out)
$outGfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$outGfx.DrawImage($bmp, 0, 0, 128, 48)

$outDir = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc'
$outPath = Join-Path $outDir 'icon.png'
$out.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
$outGfx.Dispose(); $out.Dispose(); $gfx.Dispose(); $bmp.Dispose()
Write-Host "wrote $outPath"
