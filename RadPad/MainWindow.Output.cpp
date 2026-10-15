#include "pch.h"
#include "MainWindowHelpers.h"

// The output panel, for tools run with "output": "panel".
namespace winrt::RadPad::implementation
{
    fire_and_forget MainWindow::RunToolInPanelAsync(tools::Tool tool, tools::FileContext file)
    {
        auto strong = get_strong();
        // One tool at a time in the panel.
        if (m_runningTool && m_runningTool->IsRunning())
        {
            ContentDialog dialog;
            dialog.Title(box_value(hstring{ AppName }));
            dialog.Content(box_value(hstring{ std::format(L"\"{}\" is still running. Stop it and run \"{}\"?", m_runName, tool.Name) }));
            dialog.PrimaryButtonText(L"Stop and run");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary)
            {
                co_return;
            }
            // Its last output and "Stopped" line are shown before the next tool's.
            auto previous = m_runningTool;
            previous->Stop();
            for (int wait = 0; previous->IsRunning() && wait < 100; ++wait)
            {
                co_await resume_after(std::chrono::milliseconds(50));
                co_await ResumeOn(DispatcherQueue());
            }
            co_await ResumeOn(DispatcherQueue()); // after what it queued
        }

        ShowOutputPanel(true);
        if (!EditorText(OutputBox()).empty())
        {
            AppendOutput(L"\r", OutputColor::Normal);
        }

        // Output arrives on background threads, often a line at a time. It's collected here and added to
        // the panel in batches: one per turn of the UI thread, however much arrived meanwhile. Each run has
        // an id so that output from an earlier one can't land after the next one's header.
        struct PendingOutput
        {
            std::mutex Lock;
            std::vector<std::pair<std::wstring, bool>> Pieces; // text and whether it's from the error stream
            bool FlushQueued{ false };
        };
        auto pending = std::make_shared<PendingOutput>();
        auto run = ++m_runId;
        auto weak = get_weak();
        auto queue = DispatcherQueue();
        tools::RunningTool::Events events;
        events.Output = [weak, queue, run, pending](std::wstring text, bool error)
        {
            std::lock_guard lock{ pending->Lock };
            if (!pending->Pieces.empty() && pending->Pieces.back().second == error)
            {
                pending->Pieces.back().first += text;
            }
            else
            {
                pending->Pieces.emplace_back(std::move(text), error);
            }
            if (!pending->FlushQueued)
            {
                pending->FlushQueued = true;
                queue.TryEnqueue([weak, run, pending]
                {
                    std::vector<std::pair<std::wstring, bool>> pieces;
                    {
                        std::lock_guard lock{ pending->Lock };
                        pieces.swap(pending->Pieces);
                        pending->FlushQueued = false;
                    }
                    if (auto self = weak.get(); self && self->m_runId == run)
                    {
                        for (auto const& [text, error] : pieces)
                        {
                            self->AppendOutput(text, error ? OutputColor::Error : OutputColor::Normal);
                        }
                    }
                });
            }
        };
        events.Finished = [weak, queue, run](std::optional<DWORD> exitCode)
        {
            queue.TryEnqueue([weak, run, exitCode]
            {
                if (auto self = weak.get(); self && self->m_runId == run)
                {
                    self->OnToolFinished(exitCode);
                }
            });
        };

        std::wstring commandLine, error;
        m_runName = tool.Name;
        m_runErrorPattern = tool.ErrorPattern;
        m_runStart = std::chrono::steady_clock::now();
        m_runningTool = tools::StartCaptured(tool, file, std::move(events), commandLine, m_runDirectory, error);
        AppendOutput(std::format(L"> {}\r", commandLine), OutputColor::Info);
        if (!m_runningTool)
        {
            AppendOutput(std::format(L"Couldn't start \"{}\": {}\r", tool.Name, error), OutputColor::Error);
            OutputStatusText().Text(std::format(L"{}: couldn't start", tool.Name));
            co_return;
        }
        OutputStatusText().Text(std::format(L"{}: running...", tool.Name));
        OutputStopButton().IsEnabled(true);
    }

    void MainWindow::OnToolFinished(std::optional<DWORD> exitCode)
    {
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_runStart).count();
        std::wstring summary = exitCode
            ? std::format(L"Finished with exit code {} in {:.1f} s", static_cast<int>(*exitCode), seconds)
            : std::format(L"Stopped after {:.1f} s", seconds);
        AppendOutput(summary + L"\r", exitCode && *exitCode == 0 ? OutputColor::Info : OutputColor::Error);
        OutputStatusText().Text(std::format(L"{}: {}", m_runName, exitCode ? std::format(L"exit code {}", static_cast<int>(*exitCode)) : std::wstring{ L"stopped" }));
        OutputStopButton().IsEnabled(false);
    }

    void MainWindow::ShowOutputPanel(bool show)
    {
        m_outputPanelVisible = show;
        auto visibility = show ? Visibility::Visible : Visibility::Collapsed;
        OutputPanel().Visibility(visibility);
        OutputSplitter().Visibility(visibility);
        OutputPanel().Height(m_outputPanelHeight);
        OutputPanelItem().IsChecked(show);
        auto box = OutputBox();
        box.FontFamily(Media::FontFamily{ m_fontFamily });
        box.FontSize(m_fontPoints * 96.0 / 72.0);
    }

    void MainWindow::AppendOutput(std::wstring_view text, OutputColor color)
    {
        // The box is read-only to the user, so it's unlocked while text is added. If the caret is at the end,
        // the view follows the new text; if the user has moved it to read something, the view stays put.
        // Keep the last megabyte or so, so a tool that writes without end can't slow RadPad down.
        constexpr int MaxLength = 1'000'000;
        if (text.size() > MaxLength)
        {
            text = text.substr(text.size() - MaxLength * 3 / 4);
            if (auto lineEnd = text.find(L'\r'); lineEnd != std::wstring_view::npos)
            {
                text = text.substr(lineEnd + 1); // from the start of a line
            }
        }

        auto box = OutputBox();
        auto document = box.Document();
        int end = document.GetRange(0, INT_MAX).EndPosition() - 1; // before the final paragraph mark
        auto selection = document.Selection();
        bool following = selection.StartPosition() >= end;

        box.IsReadOnly(false);
        auto range = document.GetRange(end, end);
        range.SetText(TextSetOptions::None, text);
        bool dark = IsDarkTheme();
        Windows::UI::Color rgb = color == OutputColor::Error ? (dark ? Windows::UI::Color{ 255, 0xF4, 0x87, 0x71 } : Windows::UI::Color{ 255, 0xCD, 0x31, 0x31 })
                               : color == OutputColor::Info ? (dark ? Windows::UI::Color{ 255, 0x9D, 0x9D, 0x9D } : Windows::UI::Color{ 255, 0x6E, 0x6E, 0x6E })
                               : (dark ? Windows::UI::Color{ 255, 0xE0, 0xE0, 0xE0 } : Windows::UI::Color{ 255, 0x1E, 0x1E, 0x1E });
        range.CharacterFormat().ForegroundColor(rgb);

        int length = document.GetRange(0, INT_MAX).EndPosition();
        if (length > MaxLength)
        {
            auto cut = document.GetRange(0, length - MaxLength * 3 / 4);
            cut.Expand(Microsoft::UI::Text::TextRangeUnit::Paragraph); // to the end of a line
            cut.SetText(TextSetOptions::None, L"");
        }
        box.IsReadOnly(true);

        if (following)
        {
            int newEnd = document.GetRange(0, INT_MAX).EndPosition() - 1;
            selection.SetRange(newEnd, newEnd);
            selection.ScrollIntoView(PointOptions::None);
        }
    }

    void MainWindow::OutputStop_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_runningTool)
        {
            m_runningTool->Stop();
        }
    }

    void MainWindow::OutputClear_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto box = OutputBox();
        box.IsReadOnly(false);
        box.Document().SetText(TextSetOptions::None, L"");
        box.IsReadOnly(true);
        // The heading still says what's running, but not how a finished tool ended.
        if (!m_runningTool || !m_runningTool->IsRunning())
        {
            OutputStatusText().Text(L"");
        }
    }

    void MainWindow::OutputClose_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowOutputPanel(false);
        if (auto doc = CurrentDocument())
        {
            FocusEditor(doc);
        }
    }

    void MainWindow::OutputPanel_Click(IInspectable const&, RoutedEventArgs const&)
    {
        ShowOutputPanel(OutputPanelItem().IsChecked());
    }

    void MainWindow::OutputSplitter_PointerPressed(IInspectable const& sender, PointerRoutedEventArgs const& e)
    {
        // Dragging the bar above the panel, or the panel's heading, resizes it. Its buttons handle their own presses.
        if (e.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed() && sender.as<UIElement>().CapturePointer(e.Pointer()))
        {
            m_splitterDragStart = e.GetCurrentPoint(RootGrid()).Position().Y;
            m_splitterStartHeight = m_outputPanelHeight;
            HighlightOutputSplitter(true);
            e.Handled(true);
        }
    }

    void MainWindow::HighlightOutputSplitter(bool highlight)
    {
        // The line above the panel thickens in the accent colour while the pointer is on something that
        // resizes the panel, or while dragging.
        OutputSplitterHighlight().Opacity(highlight || m_splitterDragStart >= 0 ? 1 : 0);
    }

    void MainWindow::OutputSplitter_PointerMoved(IInspectable const&, PointerRoutedEventArgs const& e)
    {
        if (m_splitterDragStart < 0)
        {
            return;
        }
        double y = e.GetCurrentPoint(RootGrid()).Position().Y;
        double maxHeight = std::max(80.0, RootGrid().ActualHeight() - 200);
        m_outputPanelHeight = std::clamp(m_splitterStartHeight + (m_splitterDragStart - y), 60.0, maxHeight);
        OutputPanel().Height(m_outputPanelHeight);
        e.Handled(true);
    }

    void MainWindow::OutputSplitter_PointerReleased(IInspectable const& sender, PointerRoutedEventArgs const& e)
    {
        sender.as<UIElement>().ReleasePointerCapture(e.Pointer());
        m_splitterDragStart = -1;
    }

    void MainWindow::OutputSplitter_PointerCaptureLost(IInspectable const& sender, PointerRoutedEventArgs const& e)
    {
        m_splitterDragStart = -1;
        // Still highlighted if the pointer is still over it.
        auto bounds = sender.as<FrameworkElement>();
        auto point = e.GetCurrentPoint(bounds).Position();
        HighlightOutputSplitter(point.X >= 0 && point.Y >= 0 && point.X < bounds.ActualWidth() && point.Y < bounds.ActualHeight());
    }

    bool MainWindow::OpenOutputLocationAtCaret()
    {
        // The output line the caret is on, if it names a file and line, opens that file there.
        auto box = OutputBox();
        auto text = EditorText(box);
        auto position = static_cast<size_t>(std::clamp(box.Document().Selection().StartPosition(), 0, static_cast<int>(text.size())));
        size_t lineStart = position == 0 ? std::wstring::npos : text.find_last_of(L'\r', position - 1);
        lineStart = lineStart == std::wstring::npos ? 0 : lineStart + 1;
        size_t lineEnd = std::min(text.find(L'\r', position), text.size());
        return OpenOutputLocation(text.substr(lineStart, lineEnd - lineStart));
    }

    bool MainWindow::OpenOutputLocation(std::wstring const& line)
    {
        // The tool's own pattern first, then MSVC's file(line,col):, Python's File "file", line n, and
        // file:line:col: as used by gcc, clang, Rust, TypeScript, grep -n and many others.
        std::vector<std::wstring> patterns;
        if (!m_runErrorPattern.empty())
        {
            patterns.push_back(m_runErrorPattern);
        }
        patterns.push_back(LR"re(^\s*(?:\d+>)?\s*(.+?)\((\d+)(?:,(\d+))?(?:,\d+,\d+)?\)\s*:)re");
        patterns.push_back(LR"re(File "(.+?)", line (\d+))re");
        patterns.push_back(LR"re(^\s*(?:-->\s*)?(.+?):(\d+)(?::(\d+))?(?::|\s|$))re");

        for (auto const& pattern : patterns)
        {
            std::wsmatch match;
            try
            {
                if (!std::regex_search(line, match, std::wregex{ pattern, std::regex_constants::ECMAScript }) || match.size() < 3)
                {
                    continue;
                }
            }
            catch (std::regex_error const&)
            {
                continue;
            }

            // A relative name is looked for in the folder the tool ran in, then beside the current file.
            std::filesystem::path name{ match[1].str() };
            std::vector<std::filesystem::path> candidates{ name };
            if (name.is_relative())
            {
                candidates = { std::filesystem::path{ m_runDirectory } / name };
                if (auto doc = CurrentDocument(); doc && !doc->Path.empty())
                {
                    candidates.push_back(std::filesystem::path{ doc->Path }.parent_path() / name);
                }
            }
            std::error_code ec;
            auto found = std::find_if(candidates.begin(), candidates.end(), [&](auto const& c) { return std::filesystem::is_regular_file(c, ec); });
            if (found == candidates.end())
            {
                continue;
            }

            int lineNumber = std::max(1, _wtoi(match[2].str().c_str()));
            int column = match.size() > 3 && match[3].matched ? std::max(1, _wtoi(match[3].str().c_str())) : 1;
            auto path = FullPath(found->wstring());
            OpenFile(path);
            if (auto doc = FindDocumentByPath(path))
            {
                Tabs().SelectedItem(doc->Tab);
                auto text = EditorText(doc->Editor);
                int lineCount = LineCount(text);
                int start = IndexOfLine(text, std::min(lineNumber, lineCount));
                size_t lineEnd = std::min(text.find(L'\r', static_cast<size_t>(start)), text.size());
                int position = std::min(start + column - 1, static_cast<int>(lineEnd));
                SelectAndReveal(doc->Editor, doc->ScrollViewer, position, 0);
                FocusEditor(doc);
                StatusMessageText().Text(L"");
            }
            return true;
        }
        StatusMessageText().Text(L"No file and line found on that line of output");
        return false;
    }
}
