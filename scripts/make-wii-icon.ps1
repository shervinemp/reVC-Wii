# Renders gamefiles/wii-hbc/icon.png (128x48, the Homebrew Channel's icon size).
# Pure type mark: sunset gradient, Pricedown "VC" centered, Rage "VICE CITY"
# beneath.  No scene elements -- at 128x48 a silhouette scene reads as noise,
# while the two typefaces alone carry the brand.  Drawn at 6x, downscaled.
# Regenerate any time; deploy-wii.bat copies icon.png with the rest of the
# app folder so the Homebrew Channel always shows the current artwork.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$S = 6
$W = 128 * $S; $H = 48 * $S
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$gfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# Sky: the Vice-City sunset as three quiet bands, indigo -> violet -> teal
$skyRect = New-Object System.Drawing.Rectangle(0, 0, $W, $H)
$skyBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $skyRect,
    [System.Drawing.Color]::FromArgb(255, 14, 4, 48),
    [System.Drawing.Color]::FromArgb(255, 0, 208, 190),
    [float](90))
$blend = New-Object System.Drawing.Drawing2D.ColorBlend
$blend.Colors = [System.Drawing.Color[]]@(
    [System.Drawing.Color]::FromArgb(255, 14, 4, 48),
    [System.Drawing.Color]::FromArgb(255, 118, 20, 134),
    [System.Drawing.Color]::FromArgb(255, 0, 208, 190))
$blend.Positions = [float[]]@(0.0, 0.62, 1.0)
$skyBrush.InterpolationColors = $blend
$gfx.FillRectangle($skyBrush, $skyRect)

# A single quiet glow behind the wordmark so the white letters sit on
# something rather than floating on the gradient.
$glowSpot = [System.Drawing.PointF]::new($W * 0.5, $H * 0.52)
$halo = New-Object System.Drawing.Drawing2D.GraphicsPath
$halo.AddEllipse([float]($glowSpot.X - 34*$S), [float]($glowSpot.Y - 15*$S), [float](68*$S), [float](30*$S))
$haloBrush = New-Object System.Drawing.Drawing2D.PathGradientBrush($halo)
$haloBrush.CenterColor = [System.Drawing.Color]::FromArgb(70, 255, 220, 160)
$haloBrush.SurroundColors = [System.Drawing.Color[]]@([System.Drawing.Color]::FromArgb(0, 255, 220, 160))
$gfx.FillEllipse($haloBrush, [float]($glowSpot.X - 34*$S), [float]($glowSpot.Y - 15*$S), [float](68*$S), [float](30*$S))
$halo.Dispose(); $haloBrush.Dispose()

# Wordmark, both lines centered on the icon.
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = [System.Drawing.StringAlignment]::Center
$fmt.LineAlignment = [System.Drawing.StringAlignment]::Center

# The real GTA brand face, Pricedown (free desktop license, shipped beside the
# icon and loaded as a private font so rendering is identical everywhere).
$privateFonts = New-Object System.Drawing.Text.PrivateFontCollection
$fontPath = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc\fonts\Pricedown Bl.otf'
$privateFonts.AddFontFile($fontPath)
$fontBig = New-Object System.Drawing.Font($privateFonts.Families[0], [float](52.0))

$vcRect = New-Object System.Drawing.RectangleF(0, [float](-2*$S), [float]$W, [float](32*$S))
# The box-art trick: a hot-pink shadow behind white letters gives the neon
# depth the official Vice City logotype has, instead of a plain black drop.
$pinkShadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(220, 236, 32, 152))
$pinkShadowRect = New-Object System.Drawing.RectangleF([float](2.0*$S), [float](1.0*$S), [float]$W, [float](32*$S))
$gfx.DrawString('VC', $fontBig, $pinkShadowBrush, $pinkShadowRect, $fmt)
$darkShadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(190, 10, 2, 28))
$darkShadowRect = New-Object System.Drawing.RectangleF([float](1.0*$S), [float](0.4*$S), [float]$W, [float](32*$S))
$gfx.DrawString('VC', $fontBig, $darkShadowBrush, $darkShadowRect, $fmt)
$whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 252, 255))
$gfx.DrawString('VC', $fontBig, $whiteBrush, $vcRect, $fmt)

$fontRage = New-Object System.Drawing.Text.PrivateFontCollection
$fontPathRage = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc\fonts\Rage.ttf'
$fontRage.AddFontFile($fontPathRage)
$fontCap = New-Object System.Drawing.Font($fontRage.Families[0], [float](21.0))

# VICE CITY centered under the mark, in the Rage face (the scratch script
# logotype the actual Vice City box art uses), with a hot-pink shadow under a
# near-white fill to match the mark above it.
$capShadowBrushPink = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(200, 236, 32, 152))
$capShadowRectPink = New-Object System.Drawing.RectangleF([float](1.6*$S), [float](29.0*$S), [float]$W, [float](19*$S))
$gfx.DrawString('VICE CITY', $fontCap, $capShadowBrushPink, $capShadowRectPink, $fmt)
$capShadowBrushDark = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(160, 10, 2, 28))
$capShadowRectDark = New-Object System.Drawing.RectangleF([float](0.8*$S), [float](28.3*$S), [float]$W, [float](19*$S))
$gfx.DrawString('VICE CITY', $fontCap, $capShadowBrushDark, $capShadowRectDark, $fmt)
$capsBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(245, 250, 236, 250))
$capRect = New-Object System.Drawing.RectangleF(0, [float](28.6*$S), [float]$W, [float](19*$S))
$gfx.DrawString('VICE CITY', $fontCap, $capsBrush, $capRect, $fmt)

# 1px highlight frame
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
