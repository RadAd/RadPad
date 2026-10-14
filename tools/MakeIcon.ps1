# Draws the RadPad app icon at each standard Windows size and writes it as a .ico.
# The design: magenta < - > on a dark rounded square.
# Usage: powershell -File tools/MakeIcon.ps1 -OutIco RadPad/RadPad.ico [-PreviewDir <folder for PNG previews>]
param([string]$OutIco, [string]$PreviewDir)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Drawing; using System.Drawing.Drawing2D;
public static class IconArt {
    static GraphicsPath Round(RectangleF r, float rad) {
        var p = new GraphicsPath(); float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure(); return p;
    }
    // Draws on a 256-unit canvas scaled to size.
    public static Bitmap Draw(int size) {
        var bmp = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        float unitsPerPixel = 256f / size;
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias; g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.ScaleTransform(size / 256f, size / 256f);

            // The square, shading slightly darker towards the bottom, with a faint lighter edge so it
            // stands out on a dark taskbar. Small sizes fill more of the canvas.
            float pad = size <= 24 ? unitsPerPixel * 0.5f : 8;
            var square = new RectangleF(pad, pad, 256 - 2 * pad, 256 - 2 * pad);
            using (var path = Round(square, square.Width * 0.15f)) {
                using (var b = new LinearGradientBrush(square, Color.FromArgb(255, 0x24, 0x27, 0x2B), Color.FromArgb(255, 0x19, 0x1A, 0x1C), LinearGradientMode.Vertical))
                    g.FillPath(b, path);
                using (var pen = new Pen(Color.FromArgb(255, 0x34, 0x37, 0x3C), Math.Max(3f, unitsPerPixel)))
                    g.DrawPath(pen, path);
            }

            // < - > in magenta with round ends. Strokes stay at least 2 pixels wide, and the shape is
            // drawn a little larger at small sizes, so it stays legible.
            float scale = size <= 24 ? 1.12f : 1f;
            float stroke = Math.Max(17f, 2f * unitsPerPixel);
            float cx = 128, cy = 128, tip = 80 * scale, arm = 40 * scale, reach = 41 * scale, dash = 19 * scale;
            using (var pen = new Pen(Color.FromArgb(255, 0xD6, 0x35, 0xB6), stroke) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round }) {
                g.DrawLines(pen, new[] { new PointF(cx - tip + reach, cy - arm), new PointF(cx - tip, cy), new PointF(cx - tip + reach, cy + arm) });
                g.DrawLines(pen, new[] { new PointF(cx + tip - reach, cy - arm), new PointF(cx + tip, cy), new PointF(cx + tip - reach, cy + arm) });
                g.DrawLine(pen, cx - dash, cy, cx + dash, cy);
            }
        }
        return bmp;
    }
}
'@
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = foreach ($size in $sizes) {
    $bmp = [IconArt]::Draw($size); $ms = New-Object IO.MemoryStream; $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    if ($PreviewDir) { $bmp.Save((Join-Path $PreviewDir "icon_$size.png"), [Drawing.Imaging.ImageFormat]::Png) }
    ,$ms.ToArray()
}
# ICO: header, one directory entry per image, then the PNG data.
$out = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$len); $w.Write([UInt32]$offset); $offset += $len
}
foreach ($p in $pngs) { $w.Write([byte[]]$p) }
[IO.File]::WriteAllBytes($OutIco, $out.ToArray())
"wrote $OutIco ($($out.Length) bytes)"
