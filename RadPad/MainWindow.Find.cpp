#include "pch.h"
#include "MainWindowHelpers.h"

// The find and replace panel.
namespace winrt::RadPad::implementation
{
    void MainWindow::ShowFindPanel(bool showReplace)
    {
        FindPanel().Visibility(Visibility::Visible);
        ReplaceRow().Visibility(showReplace ? Visibility::Visible : Visibility::Collapsed);
        StatusMessageText().Text(L"");

        if (auto doc = CurrentDocument())
        {
            std::wstring selected = SelectedText(doc->Editor);
            if (!selected.empty() && std::wstring_view{ selected }.find(L'\r') == std::wstring_view::npos)
            {
                // With regular expressions on, escaped so the pattern finds this exact text.
                FindBox().Text(IsChecked(RegexCheck()) ? EscapeRegex(selected) : selected);
            }
        }
        FindBox().Focus(FocusState::Programmatic);
        FindBox().SelectAll();
    }

    void MainWindow::HideFindPanel()
    {
        FindPanel().Visibility(Visibility::Collapsed);
        if (auto doc = CurrentDocument())
        {
            FocusEditor(doc);
        }
    }

    bool MainWindow::IsFocusWithin(UIElement const& element)
    {
        auto focused = FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>();
        while (focused)
        {
            if (focused == element)
            {
                return true;
            }
            focused = VisualTreeHelper::GetParent(focused);
        }
        return false;
    }

    bool MainWindow::FindNext(bool forward)
    {
        auto doc = CurrentDocument();
        if (!doc)
        {
            return false;
        }

        std::wstring needle{ FindBox().Text() };
        if (needle.empty())
        {
            ShowFindPanel(false);
            return false;
        }

        bool wrap = IsChecked(WrapAroundCheck());
        size_t start = static_cast<size_t>(SelectionStart(doc->Editor));
        size_t length = static_cast<size_t>(std::max(SelectionLength(doc->Editor), 0));
        size_t pos = std::wstring::npos;
        size_t matchLength = needle.size();

        if (IsChecked(RegexCheck()))
        {
            auto pattern = FindPattern();
            if (!pattern)
            {
                return false;
            }
            auto text = ForRegex(EditorText(doc->Editor));
            std::optional<std::pair<size_t, size_t>> match;
            try
            {
                if (forward)
                {
                    match = RegexSearch(text, *pattern, start + length);
                    // An empty match (^, say) right where the caret is would be found again and again; move on.
                    if (match && length == 0 && match->first == start && match->second == 0)
                    {
                        match = RegexSearch(text, *pattern, start + 1);
                    }
                    if (!match && wrap)
                    {
                        match = RegexSearch(text, *pattern, 0);
                    }
                }
                else
                {
                    // The last match starting before the selection; with wrap around, else the last of all.
                    std::optional<std::pair<size_t, size_t>> before, last;
                    for (std::wsregex_iterator it(text.cbegin(), text.cend(), *pattern), end; it != end; ++it)
                    {
                        std::pair<size_t, size_t> found{ static_cast<size_t>(it->position(0)), static_cast<size_t>(it->length(0)) };
                        if (found.first < start)
                        {
                            before = found;
                        }
                        last = found;
                    }
                    match = before ? before : wrap ? last : std::nullopt;
                }
            }
            catch (std::regex_error const& e)
            {
                StatusMessageText().Text(L"Can't search: " + RegexErrorText(e));
                return false;
            }
            if (match)
            {
                pos = match->first;
                matchLength = match->second;
            }
        }
        else
        {
            std::wstring haystack = EditorText(doc->Editor);
            if (!IsChecked(MatchCaseCheck()))
            {
                haystack = ToLower(std::move(haystack));
                needle = ToLower(std::move(needle));
            }
            if (forward)
            {
                pos = haystack.find(needle, start + length);
                if (pos == std::wstring::npos && wrap)
                {
                    pos = haystack.find(needle);
                }
            }
            else
            {
                pos = start > 0 ? haystack.rfind(needle, start - 1) : std::wstring::npos;
                if (pos == std::wstring::npos && wrap)
                {
                    pos = haystack.rfind(needle);
                }
            }
        }

        if (pos == std::wstring::npos)
        {
            StatusMessageText().Text(std::format(L"Cannot find \"{}\"", std::wstring_view{ FindBox().Text() }));
            return false;
        }

        StatusMessageText().Text(L"");
        bool returnFocusToFindBox = IsFocusWithin(FindPanel());
        SelectAndReveal(doc->Editor, doc->ScrollViewer, static_cast<int>(pos), static_cast<int>(matchLength));
        doc->Editor.Focus(FocusState::Programmatic);
        if (returnFocusToFindBox)
        {
            DispatcherQueue().TryEnqueue([this, strong = get_strong()] { FindBox().Focus(FocusState::Programmatic); });
        }
        return true;
    }

    std::optional<std::wregex> MainWindow::FindPattern()
    {
        // The find box as a regular expression; a mistake in it is shown in the status bar.
        auto flags = std::regex_constants::ECMAScript;
        if (!IsChecked(MatchCaseCheck()))
        {
            flags |= std::regex_constants::icase;
        }
        try
        {
            return std::wregex{ std::wstring{ FindBox().Text() }, flags };
        }
        catch (std::regex_error const& e)
        {
            StatusMessageText().Text(L"Invalid regular expression: " + RegexErrorText(e));
            return std::nullopt;
        }
    }

    void MainWindow::ReplaceOne()
    {
        auto doc = CurrentDocument();
        std::wstring needle{ FindBox().Text() };
        if (!doc || needle.empty())
        {
            return;
        }

        // Replace the selection if it's a match (found by Find next), then find the next one.
        if (IsChecked(RegexCheck()))
        {
            auto pattern = FindPattern();
            if (!pattern)
            {
                return;
            }
            auto text = ForRegex(EditorText(doc->Editor));
            size_t start = static_cast<size_t>(SelectionStart(doc->Editor));
            size_t length = static_cast<size_t>(std::max(SelectionLength(doc->Editor), 0));
            try
            {
                std::wsmatch match;
                auto flags = std::regex_constants::match_continuous | (start > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default);
                if (start <= text.size() && std::regex_search(text.cbegin() + start, text.cend(), match, *pattern, flags)
                    && static_cast<size_t>(match.length(0)) == length)
                {
                    auto replacement = match.format(ReplacementWithEscapes(ReplaceBox().Text()));
                    InsertText(doc->Editor, textfile::NormalizeLineBreaks(replacement));
                }
            }
            catch (std::regex_error const& e)
            {
                StatusMessageText().Text(L"Can't replace: " + RegexErrorText(e));
                return;
            }
        }
        else
        {
            std::wstring selected = SelectedText(doc->Editor);
            bool matches = IsChecked(MatchCaseCheck()) ? selected == needle : ToLower(selected) == ToLower(needle);
            if (matches)
            {
                InsertText(doc->Editor, ReplaceBox().Text());
            }
        }
        FindNext(true);
    }

    void MainWindow::ReplaceAll()
    {
        auto doc = CurrentDocument();
        std::wstring needle{ FindBox().Text() };
        if (!doc || needle.empty())
        {
            return;
        }

        std::wstring text = EditorText(doc->Editor);
        std::wstring result;
        size_t count = 0;
        if (IsChecked(RegexCheck()))
        {
            auto pattern = FindPattern();
            if (!pattern)
            {
                return;
            }
            try
            {
                auto searchText = ForRegex(text);
                count = static_cast<size_t>(std::distance(std::wsregex_iterator(searchText.cbegin(), searchText.cend(), *pattern), std::wsregex_iterator()));
                if (count > 0)
                {
                    result = textfile::NormalizeLineBreaks(std::regex_replace(searchText, *pattern, ReplacementWithEscapes(ReplaceBox().Text())));
                }
            }
            catch (std::regex_error const& e)
            {
                StatusMessageText().Text(L"Can't replace: " + RegexErrorText(e));
                return;
            }
        }
        else
        {
            bool matchCase = IsChecked(MatchCaseCheck());
            std::wstring searchText = matchCase ? text : ToLower(text);
            std::wstring searchKey = matchCase ? needle : ToLower(needle);
            std::wstring replacement{ ReplaceBox().Text() };
            size_t last = 0;
            for (size_t pos; (pos = searchText.find(searchKey, last)) != std::wstring::npos; last = pos + searchKey.size())
            {
                result.append(text, last, pos - last);
                result += replacement;
                ++count;
            }
            result.append(text, last);
        }

        if (count == 0)
        {
            StatusMessageText().Text(std::format(L"Cannot find \"{}\"", needle));
            return;
        }

        // Replace via the selection so the whole operation is a single undo step. The new text all takes
        // one format, so the whole document is recoloured.
        Select(doc->Editor, 0, static_cast<int>(text.size()));
        doc->Editor.Document().Selection().SetText(TextSetOptions::None, result);
        Select(doc->Editor, 0, 0);
        UpdateHighlighting(doc, true);
        StatusMessageText().Text(std::format(L"Replaced {} occurrence{}", count, count == 1 ? L"" : L"s"));
    }

    void MainWindow::FindBox_KeyDown(IInspectable const&, KeyRoutedEventArgs const& e)
    {
        if (e.Key() == VirtualKey::Enter)
        {
            FindNext(!IsKeyDown(VK_SHIFT));
            e.Handled(true);
        }
    }

    void MainWindow::ReplaceBox_KeyDown(IInspectable const&, KeyRoutedEventArgs const& e)
    {
        if (e.Key() == VirtualKey::Enter)
        {
            ReplaceOne();
            e.Handled(true);
        }
    }
}
