#include "pch.h"
#include "MainWindowHelpers.h"

// The overlay drawn over the editor: visible whitespace, trailing whitespace and wrap markers.
namespace winrt::RadPad::implementation
{
    void MainWindow::UpdateWhitespaceAvailability()
    {
        // Markers are placed by column, which needs a monospaced font and unwrapped lines.
        bool available = m_fontMonospaced && !m_wordWrap;
        ShowWhitespaceItem().IsEnabled(available);
        ToolTipService::SetToolTip(ShowWhitespaceItem(), available ? nullptr : box_value(L"Needs a monospaced font with word wrap off"));
        UpdateEditorOverlay();
    }

    void MainWindow::UpdateEditorOverlay()
    {
        auto overlay = EditorOverlay();
        overlay.Children().Clear();

        auto doc = CurrentDocument();
        if (!doc || !doc->ScrollViewer)
        {
            return;
        }
        auto viewer = doc->ScrollViewer;
        auto presenter = FindDescendant<ScrollContentPresenter>(viewer);
        if (!presenter || presenter.ActualHeight() <= 0)
        {
            return;
        }

        // Clip to the text area; the wrap markers widen it to include the right padding.
        auto origin = presenter.TransformToVisual(overlay).TransformPoint({ 0, 0 });
        RectangleGeometry clip;
        clip.Rect({ origin.X, origin.Y, static_cast<float>(presenter.ActualWidth()), static_cast<float>(presenter.ActualHeight()) });
        overlay.Clip(clip);

        if (m_highlightTrailingWhitespace)
        {
            DrawTrailingWhitespace(doc, presenter); // first, so markers are drawn over it
        }
        if (m_wordWrap)
        {
            DrawWrapMarkers(doc, presenter);
            return;
        }
        if (!m_showWhitespace || !m_fontMonospaced)
        {
            return;
        }

        // Every line is the same height and, in a monospaced font, every character the same width, so
        // a character's position follows from its line and column.
        auto text = EditorText(doc->Editor);
        int lineCount = LineCount(text);
        // When the text is taller than the view, the scroll extent is exactly its height. Otherwise the
        // extent is just the view's height, so ask RichEdit where the first and last lines are instead.
        // (Its rectangles are rounded, so a span of lines is more accurate than one line's height.)
        double lineHeight = viewer.ExtentHeight() / lineCount;
        if (viewer.ExtentHeight() <= viewer.ViewportHeight() + 0.5)
        {
            try
            {
                auto document = doc->Editor.Document();
                Windows::Foundation::Rect first{};
                Windows::Foundation::Rect last{};
                int hit = 0;
                document.GetRange(0, 1).GetRect(PointOptions::ClientCoordinates, first, hit);
                int lastLineStart = IndexOfLine(text, lineCount);
                document.GetRange(lastLineStart, lastLineStart + 1).GetRect(PointOptions::ClientCoordinates, last, hit);
                lineHeight = lineCount > 1 && last.Y > first.Y ? (last.Y - first.Y) / (lineCount - 1.0) : first.Height;
            }
            catch (hresult_error const&)
            {
            }
        }
        double charWidth = m_spaceAdvance * m_fontPoints * 96.0 / 72.0 * m_zoom / 100.0;
        double tabWidth = doc->Editor.Document().DefaultTabStop() * 96.0 / 72.0;
        if (lineHeight <= 0 || charWidth <= 0)
        {
            return;
        }
        if (tabWidth <= 0)
        {
            tabWidth = charWidth * 8;
        }

        // The text is laid out from the viewport's top-left corner (origin), less the scroll offset.
        double width = presenter.ActualWidth();
        double height = presenter.ActualHeight();
        double scrollX = viewer.HorizontalOffset();
        double scrollY = viewer.VerticalOffset();
        auto point = [&](double x, double y)
        {
            return Point{ static_cast<float>(origin.X + x - scrollX), static_cast<float>(origin.Y + y - scrollY) };
        };

        GeometryGroup shapes;
        auto line = [&](Point from, Point to)
        {
            LineGeometry geometry;
            geometry.StartPoint(from);
            geometry.EndPoint(to);
            shapes.Children().Append(geometry);
        };
        auto dotRadius = static_cast<float>(std::max(1.0, charWidth * 0.1));
        double arrowHead = charWidth * 0.3;

        int firstLine = static_cast<int>(scrollY / lineHeight);
        int lastLine = std::min(lineCount - 1, static_cast<int>((scrollY + height) / lineHeight) + 1);
        auto i = static_cast<size_t>(IndexOfLine(text, firstLine + 1));
        for (int lineIndex = firstLine; lineIndex <= lastLine && i <= text.size(); ++lineIndex, ++i)
        {
            double y = (lineIndex + 0.5) * lineHeight;
            double x = 0;
            for (; i < text.size() && text[i] != L'\r'; ++i)
            {
                wchar_t c = text[i];
                bool visible = x + tabWidth > scrollX && x < scrollX + width;
                if (c == L'\t')
                {
                    // A tab runs to the next tab stop: an arrow across that width.
                    double next = (std::floor(x / tabWidth + 1e-3) + 1) * tabWidth;
                    if (visible)
                    {
                        double end = next - charWidth * 0.2;
                        line(point(x + charWidth * 0.2, y), point(end, y));
                        line(point(end - arrowHead, y - arrowHead), point(end, y));
                        line(point(end - arrowHead, y + arrowHead), point(end, y));
                    }
                    x = next;
                    continue;
                }
                if (c == L' ' && visible)
                {
                    EllipseGeometry geometry;
                    geometry.Center(point(x + charWidth / 2, y));
                    geometry.RadiusX(dotRadius);
                    geometry.RadiusY(dotRadius);
                    shapes.Children().Append(geometry);
                }
                if (!IS_LOW_SURROGATE(c))
                {
                    x += charWidth;
                }
            }
        }

        Windows::UI::Color color{ 0x80, 0x80, 0x80, 0x80 };
        SolidColorBrush brush{ color };
        Shapes::Path path;
        path.Data(shapes);
        path.Fill(brush);
        path.Stroke(brush);
        path.StrokeThickness(1);
        overlay.Children().Append(path);
    }

    void MainWindow::ScheduleOverlayUpdate()
    {
        // Once per batch of changes, after layout has caught up with them.
        if (std::exchange(m_overlayUpdatePending, true))
        {
            return;
        }
        DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this, strong = get_strong()]
        {
            m_overlayUpdatePending = false;
            UpdateEditorOverlay();
        });
    }

    void MainWindow::DrawTrailingWhitespace(std::shared_ptr<Document> const& doc, ScrollContentPresenter const& presenter)
    {
        // Shades spaces and tabs at the ends of lines on screen. Whitespace ending at the caret is left alone,
        // so typing a space before the next word doesn't flash.
        auto overlay = EditorOverlay();
        auto viewer = doc->ScrollViewer;
        auto document = doc->Editor.Document();
        auto text = EditorText(doc->Editor);
        auto length = static_cast<int>(text.size());
        try
        {
            auto row = FirstVisibleRow(document, length, viewer);
            if (!row)
            {
                return;
            }
            auto selection = document.Selection();
            int caret = selection.Length() == 0 ? selection.StartPosition() : -1;
            auto origin = presenter.TransformToVisual(overlay).TransformPoint({ 0, 0 });
            double height = presenter.ActualHeight();

            GeometryGroup shapes;
            auto addRect = [&](Windows::Foundation::Rect const& rect)
            {
                RectangleGeometry geometry;
                geometry.Rect({ origin.X + rect.X, origin.Y + rect.Y, rect.Width, rect.Height });
                shapes.Children().Append(geometry);
            };

            // Each line from the one holding the first row in view, until a line starts below the view.
            size_t lineStart = text.find_last_of(L'\r', row.StartPosition() == 0 ? 0 : row.StartPosition() - 1);
            lineStart = lineStart == std::wstring::npos || row.StartPosition() == 0 ? 0 : lineStart + 1;
            for (int count = 0; count < 1000 && lineStart <= text.size(); ++count)
            {
                if (count > 0 && CharRectInView(document, static_cast<int>(lineStart), length, viewer).Y >= height)
                {
                    break;
                }
                size_t lineEnd = std::min(text.find(L'\r', lineStart), text.size());
                size_t trailing = lineEnd;
                while (trailing > lineStart && (text[trailing - 1] == L' ' || text[trailing - 1] == L'\t'))
                {
                    --trailing;
                }

                if (trailing < lineEnd && static_cast<int>(lineEnd) != caret)
                {
                    // One rectangle per row: with word wrap, the whitespace can run onto the next row.
                    // Long runs are capped, as each character's position is a separate request.
                    std::optional<Windows::Foundation::Rect> current;
                    for (size_t i = trailing; i < lineEnd && i < trailing + 500; ++i)
                    {
                        auto rect = CharRectInView(document, static_cast<int>(i), length, viewer);
                        if (current && std::abs(rect.Y - current->Y) < 0.5f)
                        {
                            current->Width = rect.X + rect.Width - current->X;
                            continue;
                        }
                        if (current)
                        {
                            addRect(*current);
                        }
                        current = rect;
                    }
                    if (current)
                    {
                        addRect(*current);
                    }
                }

                if (lineEnd >= text.size())
                {
                    break;
                }
                lineStart = lineEnd + 1;
            }

            Shapes::Path path;
            path.Data(shapes);
            path.Fill(SolidColorBrush{ Windows::UI::Color{ 0x50, 0xE0, 0x40, 0x40 } });
            overlay.Children().Append(path);
        }
        catch (hresult_error const&)
        {
            // A convenience; leave it off if RichEdit can't report positions.
        }
    }

    void MainWindow::DrawWrapMarkers(std::shared_ptr<Document> const& doc, ScrollContentPresenter const& presenter)
    {
        // Marks each display row that continues on the next row with a ↵ in the editor's right padding.
        // RichEdit's Line unit is a display row, so a row whose range doesn't end with a line break has wrapped.
        // Only rows on screen are examined (see FirstVisibleRow).
        auto overlay = EditorOverlay();
        auto viewer = doc->ScrollViewer;
        auto document = doc->Editor.Document();
        auto text = EditorText(doc->Editor);
        auto length = static_cast<int>(text.size());
        if (length == 0 || viewer.ExtentHeight() <= 0)
        {
            return;
        }

        try
        {
            double height = presenter.ActualHeight();
            auto row = FirstVisibleRow(document, length, viewer);
            if (!row)
            {
                return;
            }

            auto origin = presenter.TransformToVisual(overlay).TransformPoint({ 0, 0 });
            double width = presenter.ActualWidth();
            double rightPadding = doc->Editor.Padding().Right;

            // The markers sit in the right padding, just left of the vertical scrollbar, which is drawn over
            // the padding and widens when the pointer is over it.
            double markerRight = origin.X + width + rightPadding - 2;
            if (auto bar = FindVerticalScrollBar(viewer); bar && bar.Visibility() == Visibility::Visible && bar.ActualWidth() > 0)
            {
                markerRight = std::min(markerRight, static_cast<double>(bar.TransformToVisual(overlay).TransformPoint({ 0, 0 }).X) - 2);
            }

            // The clip includes the padding.
            RectangleGeometry clip;
            clip.Rect({ origin.X, origin.Y, static_cast<float>(width + rightPadding), static_cast<float>(height) });
            overlay.Clip(clip);

            GeometryGroup shapes;
            auto line = [&](double x1, double y1, double x2, double y2)
            {
                LineGeometry geometry;
                geometry.StartPoint({ static_cast<float>(x1), static_cast<float>(y1) });
                geometry.EndPoint({ static_cast<float>(x2), static_cast<float>(y2) });
                shapes.Children().Append(geometry);
            };

            for (int count = 0; count < 1000; ++count)
            {
                auto rect = CharRectInView(document, row.StartPosition(), length, viewer);
                if (rect.Y >= height)
                {
                    break;
                }
                row.Expand(TextRangeUnit::Line);
                int end = row.EndPosition();
                if (end <= row.StartPosition() || end >= length)
                {
                    break; // the last row
                }
                if (text[end - 1] != L'\r')
                {
                    // ↵: down from the top right, then left to an arrowhead.
                    double size = std::clamp(rect.Height * 0.5, 4.0, WrapMarkerWidth - 4);
                    double right = markerRight;
                    double left = right - size;
                    double y = origin.Y + rect.Y + rect.Height / 2;
                    double bottom = y + size / 4;
                    double head = size / 3;
                    line(right, y - size / 2, right, bottom);
                    line(right, bottom, left, bottom);
                    line(left, bottom, left + head, bottom - head);
                    line(left, bottom, left + head, bottom + head);
                }
                row.SetRange(end, end);
            }

            Windows::UI::Color color{ 0x80, 0x80, 0x80, 0x80 };
            Shapes::Path path;
            path.Data(shapes);
            path.Stroke(SolidColorBrush{ color });
            path.StrokeThickness(1);
            overlay.Children().Append(path);
        }
        catch (hresult_error const&)
        {
            // Markers are a convenience; leave them off if RichEdit can't report the rows.
        }
    }
}
