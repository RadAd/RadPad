# Draws the RadPad app icon at each standard Windows size and writes it as a .ico.
# Usage: powershell -File tools/MakeIcon.ps1 -OutIco RadPad/RadPad.ico [-PreviewDir <folder for PNG previews>]
param([string]$OutIco, [string]$PreviewDir)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Drawing; using System.Drawing.Drawing2D;
public static class IconArt {
    static GraphicsPath Round(RectangleF r, float rad, bool top, bool bottom) {
        var p = new GraphicsPath(); float d = rad * 2;
        if (top) { p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90); }
        else { p.AddLine(r.X, r.Y, r.Right, r.Y); }
        if (bottom) { p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90); }
        else { p.AddLine(r.Right, r.Bottom, r.X, r.Bottom); }
        p.CloseFigure(); return p;
    }
    // Draws the icon on a 256-unit canvas scaled to size; small sizes get less detail.
    // Pixel-aligned art for 16-24 px, laid out on a 16-unit grid and rounded to whole pixels.
    static Bitmap DrawSmall(int size) {
        var bmp = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        double u = size / 16.0;
        Func<double, int> P = v => (int)Math.Round(v * u, MidpointRounding.AwayFromZero);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.None;
            Action<Color, int, int, int, int> box = (c, x1, y1, x2, y2) => { using (var b = new SolidBrush(c)) g.FillRectangle(b, x1, y1, Math.Max(1, x2 - x1), Math.Max(1, y2 - y1)); };
            int t = Math.Max(1, P(1));
            int left = P(2), top = P(1), right = P(14), bottom = P(15);
            Color paper = Color.FromArgb(255, 250, 251, 253);
            box(Color.FromArgb(255, 96, 112, 136), left, top, right, bottom);         // 1px outline
            box(paper, left + 1, top + 1, right - 1, bottom - 1);
            box(Color.FromArgb(255, 36, 116, 216), left + 1, top + 1, right - 1, P(5)); // tab strip
            box(paper, P(4), P(3), P(8), P(5));                                       // selected tab
            int row1 = P(7), row2 = P(9.5), row3 = Math.Min(P(12), bottom - 2 - t);
            box(Color.FromArgb(255, 30, 110, 220), P(4), row1, P(6), row1 + t);
            box(Color.FromArgb(255, 120, 130, 145), P(7), row1, P(12), row1 + t);
            box(Color.FromArgb(255, 222, 110, 40), P(5), row2, P(10), row2 + t);
            box(Color.FromArgb(255, 70, 160, 70), P(5), row3, P(12), row3 + t);
        }
        return bmp;
    }
    public static Bitmap Draw(int size) {
        if (size <= 24) return DrawSmall(size);
        var bmp = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias; g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.ScaleTransform(size / 256f, size / 256f);
            bool small = size <= 24, medium = size <= 48 && !small;
            float pad = small ? 12 : 24;
            var page = new RectangleF(pad + 20, pad, 256 - 2 * pad - 40, 256 - 2 * pad);
            float radius = small ? 28 : 20;
            if (!small) using (var shadow = Round(new RectangleF(page.X + 2, page.Y + 6, page.Width, page.Height), radius, true, true))
                using (var b = new SolidBrush(Color.FromArgb(36, 0, 0, 0))) g.FillPath(b, shadow);
            using (var path = Round(page, radius, true, true)) {
                using (var b = new SolidBrush(Color.FromArgb(255, 250, 251, 253))) g.FillPath(b, path);
                // Tab strip across the top of the page.
                float header = small ? 70 : 62;
                var band = new RectangleF(page.X, page.Y, page.Width, header);
                g.SetClip(path);
                using (var b = new LinearGradientBrush(band, Color.FromArgb(255, 28, 104, 204), Color.FromArgb(255, 56, 142, 236), LinearGradientMode.Vertical))
                    g.FillRectangle(b, band);
                // The selected tab joins the page; a second tab sits beside it.
                float tabTop = page.Y + (small ? 26 : 22), tabH = page.Y + header - tabTop + 1;
                using (var tab = Round(new RectangleF(page.X + 16, tabTop, 70, tabH), small ? 12 : 10, true, false))
                using (var b = new SolidBrush(Color.FromArgb(255, 250, 251, 253))) g.FillPath(b, tab);
                if (!small)
                    using (var tab2 = Round(new RectangleF(page.X + 94, tabTop + 4, 54, tabH - 4), 10, true, false))
                    using (var b = new SolidBrush(Color.FromArgb(90, 255, 255, 255))) g.FillPath(b, tab2);
                g.ResetClip();
                using (var pen = new Pen(Color.FromArgb(255, 160, 172, 190), small ? 10 : 5)) { if (!small) g.DrawPath(pen, path); }
            }
            // Syntax-coloured lines of code.
            Color blue = Color.FromArgb(255, 30, 110, 220), teal = Color.FromArgb(255, 20, 150, 130), orange = Color.FromArgb(255, 222, 110, 40), gray = Color.FromArgb(255, 120, 130, 145), green = Color.FromArgb(255, 70, 160, 70);
            float x0 = page.X + 22, th = small ? 22 : (medium ? 16 : 13);
            float[][] rows; Color[][] cols;
            if (small) {
                rows = new[] { new[] { 0f, 50f, 64f, 150f }, new[] { 30f, 120f } };
                cols = new[] { new[] { blue, gray }, new[] { orange } };
            } else {
                rows = new[] { new[] { 0f, 36f, 48f, 100f }, new[] { 20f, 52f, 64f, 116f }, new[] { 20f, 120f }, new[] { 0f, 26f } };
                cols = new[] { new[] { blue, gray }, new[] { teal, orange }, new[] { green }, new[] { gray } };
            }
            float y = page.Y + (small ? 112 : 104), step = small ? 52 : (medium ? 34 : 30);
            if (medium) { rows = new[] { rows[0], rows[1], rows[2] }; }
            for (int r = 0; r < rows.Length; r++, y += step)
                for (int k = 0; k + 1 < rows[r].Length; k += 2)
                    using (var pen = new Pen(cols[r][k / 2], th) { StartCap = LineCap.Round, EndCap = LineCap.Round })
                        g.DrawLine(pen, x0 + rows[r][k] + th / 2, y, x0 + rows[r][k + 1] - th / 2, y);
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
