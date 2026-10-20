# Renders gamefiles/wii-hbc/icon.png (128x48, the Homebrew Channel's icon size).
# The mark is the project's own reVC logo at the left; the right half is the
# Vice City wordmark in Pricedown with one small Wii plate below it.  The
# background is a midnight-to-violet-to-teal sky with a hot-pink and cyan edge,
# not a scene: at 128x48 a skyline or palms turn into texture.
#
# Drawn at 8x and downscaled.  Regenerate any time; deploy-wii.bat copies
# icon.png with the rest of the app folder so the Homebrew Channel always shows
# the current artwork.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$S = 8
$W = 128 * $S
$H = 48 * $S
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$gfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$gfx.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

# Midnight -> violet -> teal.  The right half gets a soft violet bloom so the
# cream wordmark has some air behind it instead of sitting on the gradient.
$skyRect = New-Object System.Drawing.RectangleF(0, 0, $W, $H)
$skyBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $skyRect,
    [System.Drawing.Color]::FromArgb(255, 8, 3, 30),
    [System.Drawing.Color]::FromArgb(255, 0, 58, 78),
    [float](90))
$blend = New-Object System.Drawing.Drawing2D.ColorBlend
$blend.Colors = [System.Drawing.Color[]]@(
    [System.Drawing.Color]::FromArgb(255, 8, 3, 30),
    [System.Drawing.Color]::FromArgb(255, 38, 7, 70),
    [System.Drawing.Color]::FromArgb(255, 0, 58, 78))
$blend.Positions = [float[]]@(0.0, 0.58, 1.0)
$skyBrush.InterpolationColors = $blend
$gfx.FillRectangle($skyBrush, $skyRect)

$glowPath = New-Object System.Drawing.Drawing2D.GraphicsPath
$glowPath.AddEllipse([float]($W * 0.30), [float]($H * -0.15),
    [float]($W * 0.90), [float]($H * 1.30))
$glowBrush = New-Object System.Drawing.Drawing2D.PathGradientBrush($glowPath)
$glowBrush.CenterColor = [System.Drawing.Color]::FromArgb(95, 116, 20, 134)
$glowBrush.SurroundColors = [System.Drawing.Color[]]@(
    [System.Drawing.Color]::FromArgb(0, 116, 20, 134))
$gfx.FillPath($glowBrush, $glowPath)
$glowPath.Dispose(); $glowBrush.Dispose()

# Very faint horizontal scanlines, then the split between mark and wordmark.
$scanPen = New-Object System.Drawing.Pen(
    ([System.Drawing.Color]::FromArgb(8, 255, 255, 255)), [float]($S / 4.0))
for($y = 0; $y -lt $H; $y += 3 * $S){
    $gfx.DrawLine($scanPen, 0, $y, $W, $y)
}
$scanPen.Dispose()

$splitPen = New-Object System.Drawing.Pen(
    ([System.Drawing.Color]::FromArgb(30, 255, 255, 255)), [float](1.0 * $S))
$gfx.DrawLine($splitPen, 50 * $S, 0, 50 * $S, $H)
$splitPen.Dispose()

# The actual reVC mark.  The source is a square PNG with transparent padding;
# drawing the whole square means the transparent margin falls away and the
# visible mark keeps its breathing room against the split.
$logoPath = Join-Path $PSScriptRoot '..\res\images\logo_1024.png'
$logo = [System.Drawing.Image]::FromFile($logoPath)
$logoSize = 47 * $S
$logoRect = New-Object System.Drawing.RectangleF(
    0, [float](($H - $logoSize) / 2.0), [float]$logoSize, [float]$logoSize)
$gfx.DrawImage($logo, $logoRect)
$logo.Dispose()

$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = [System.Drawing.StringAlignment]::Center
$fmt.LineAlignment = [System.Drawing.StringAlignment]::Center

# The Vice City face, Pricedown.  Pixel units rather than points so the final
# 128x48 icon has a real 18-pixel wordmark even though it is rendered at 8x.
$cityFonts = New-Object System.Drawing.Text.PrivateFontCollection
$cityFontPath = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc\fonts\Pricedown Bl.otf'
$cityFonts.AddFontFile($cityFontPath)
$cityFont = New-Object System.Drawing.Font(
    $cityFonts.Families[0], [float](18 * $S),
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

$cityRect = [System.Drawing.RectangleF]::new(
    [float](50 * $S), [float](5 * $S), [float](78 * $S), [float](24 * $S))
$pinkRect = [System.Drawing.RectangleF]::new(
    [float](50 * $S + 1.3 * $S), [float](5 * $S + 1.0 * $S),
    [float](78 * $S), [float](24 * $S))
$darkRect = [System.Drawing.RectangleF]::new(
    [float](50 * $S + 0.6 * $S), [float](5 * $S + 0.5 * $S),
    [float](78 * $S), [float](24 * $S))

$pinkBrush = New-Object System.Drawing.SolidBrush(
    [System.Drawing.Color]::FromArgb(225, 236, 32, 152))
$darkBrush = New-Object System.Drawing.SolidBrush(
    [System.Drawing.Color]::FromArgb(195, 8, 2, 22))
$creamBrush = New-Object System.Drawing.SolidBrush(
    [System.Drawing.Color]::FromArgb(255, 255, 250, 242))

$gfx.DrawString('VICE CITY', $cityFont, $pinkBrush, $pinkRect, $fmt)
$gfx.DrawString('VICE CITY', $cityFont, $darkBrush, $darkRect, $fmt)
$gfx.DrawString('VICE CITY', $cityFont, $creamBrush, $cityRect, $fmt)

# A small Wii plate; not a second wordmark competing with the title, just the
# platform cue that fits under it.
$plateFont = New-Object System.Drawing.Font(
    'Arial', [float](8 * $S),
    [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$plateText = 'Wii'
$plateSize = $gfx.MeasureString($plateText, $plateFont)
$plateW = [float]($plateSize.Width + 8.0 * $S)
$plateH = [float](11 * $S)
$plateX = [float](88 * $S - $plateW / 2.0)
$plateY = [float](36 * $S - $plateH / 2.0)
$plateR = [float](3 * $S)

$platePath = New-Object System.Drawing.Drawing2D.GraphicsPath
$platePath.AddArc($plateX, $plateY, 2 * $plateR, 2 * $plateR, 180, 90)
$platePath.AddArc($plateX + $plateW - 2 * $plateR, $plateY, 2 * $plateR, 2 * $plateR, 270, 90)
$platePath.AddArc($plateX + $plateW - 2 * $plateR, $plateY + $plateH - 2 * $plateR,
    2 * $plateR, 2 * $plateR, 0, 90)
$platePath.AddArc($plateX, $plateY + $plateH - 2 * $plateR,
    2 * $plateR, 2 * $plateR, 90, 90)
$platePath.CloseFigure()

$plateShadow = New-Object System.Drawing.SolidBrush(
    [System.Drawing.Color]::FromArgb(150, 8, 2, 22))
$plateBrush = New-Object System.Drawing.SolidBrush(
    [System.Drawing.Color]::FromArgb(242, 236, 32, 152))
$plateShadowPath = New-Object System.Drawing.Drawing2D.GraphicsPath
$plateShadowPath.AddPath($platePath, $false)
$shadowMatrix = New-Object System.Drawing.Drawing2D.Matrix
$shadowMatrix.Translate([float]$S, [float]$S)
$plateShadowPath.Transform($shadowMatrix)
$gfx.FillPath($plateShadow, $plateShadowPath)
$gfx.FillPath($plateBrush, $platePath)

$plateRect = New-Object System.Drawing.RectangleF($plateX, $plateY, $plateW, $plateH)
$plateWhite = New-Object System.Drawing.SolidBrush(
    [System.Drawing.Color]::FromArgb(255, 255, 255, 255))
$gfx.DrawString($plateText, $plateFont, $plateWhite, $plateRect, $fmt)

# One hot-pink edge above, one cyan edge below: the icon's frame, not a bevel.
$topPen = New-Object System.Drawing.Pen(
    ([System.Drawing.Color]::FromArgb(235, 236, 32, 152)), [float](2 * $S))
$bottomPen = New-Object System.Drawing.Pen(
    ([System.Drawing.Color]::FromArgb(215, 0, 208, 190)), [float](2 * $S))
$gfx.DrawLine($topPen, 0, 0, $W, 0)
$gfx.DrawLine($bottomPen, 0, $H - 2 * $S, $W, $H - 2 * $S)

# Downscale and write the HBC-sized icon.
$out = New-Object System.Drawing.Bitmap(128, 48)
$outGfx = [System.Drawing.Graphics]::FromImage($out)
$outGfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$outGfx.DrawImage($bmp, 0, 0, 128, 48)

$outDir = Join-Path $PSScriptRoot '..\gamefiles\wii-hbc'
$outPath = Join-Path $outDir 'icon.png'
$out.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)

$outGfx.Dispose(); $out.Dispose(); $gfx.Dispose(); $bmp.Dispose()
$cityFont.Dispose(); $cityFonts.Dispose()
$plateFont.Dispose(); $platePath.Dispose(); $plateShadowPath.Dispose()
$shadowMatrix.Dispose(); $plateShadow.Dispose(); $plateBrush.Dispose()
$plateWhite.Dispose(); $pinkBrush.Dispose(); $darkBrush.Dispose()
$creamBrush.Dispose(); $topPen.Dispose(); $bottomPen.Dispose()
$skyBrush.Dispose()
Write-Host "wrote $outPath"
