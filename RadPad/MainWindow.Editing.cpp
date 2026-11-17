#include "pch.h"
#include "MainWindowHelpers.h"

// Editing: tabs and indentation, smart Home, matching brackets, inserting and pasting text, and undo.
namespace winrt::RadPad::implementation
{
    void MainWindow::JumpToMatchingBracket(std::shared_ptr<Document> const& doc, bool select)
    {
        // With the caret next to a bracket (after it is checked first, then before), goes to just before
        // the matching one, so pressing again goes back. Elsewhere, goes to the closing bracket of the pair
        // the caret is inside. With select, selects from one bracket to the other instead. Brackets in
        // strings and comments don't count, when the highlighter has told them apart.
        auto editor = doc->Editor;
        auto text = EditorText(editor);
        auto n = text.size();
        bool haveKinds = doc->HighlightedText == text && doc->DesiredKinds.size() == n;
        auto isCode = [&](size_t i) { return !haveKinds || (doc->DesiredKinds[i] != TokenKind::Comment && doc->DesiredKinds[i] != TokenKind::String); };
        auto opening = [](wchar_t c) { return c == L'(' || c == L'[' || c == L'{'; };
        auto closing = [](wchar_t c) { return c == L')' || c == L']' || c == L'}'; };
        auto partner = [](wchar_t c) { switch (c) { case L'(': return L')'; case L'[': return L']'; case L'{': return L'}'; case L')': return L'('; case L']': return L'['; default: return L'{'; } };

        auto selection = editor.Document().Selection();
        bool startActive = (selection.Options() & Microsoft::UI::Text::SelectionOptions::StartActive) == Microsoft::UI::Text::SelectionOptions::StartActive;
        size_t caret = std::min(static_cast<size_t>(std::max(startActive ? selection.StartPosition() : selection.EndPosition(), 0)), n);

        // The bracket to start from.
        std::optional<size_t> from;
        if (caret < n && (opening(text[caret]) || closing(text[caret])) && isCode(caret))
        {
            from = caret;
        }
        else if (caret > 0 && (opening(text[caret - 1]) || closing(text[caret - 1])) && isCode(caret - 1))
        {
            from = caret - 1;
        }
        else
        {
            // The opening bracket of the pair around the caret.
            int depth = 0;
            for (size_t i = caret; i-- > 0;)
            {
                if (!isCode(i))
                {
                    continue;
                }
                if (closing(text[i]))
                {
                    ++depth;
                }
                else if (opening(text[i]) && depth-- == 0)
                {
                    from = i;
                    break;
                }
            }
        }

        // Its partner: the next or previous bracket of the same type at the same depth.
        std::optional<size_t> to;
        if (from)
        {
            wchar_t c = text[*from];
            wchar_t other = partner(c);
            int depth = 0;
            if (opening(c))
            {
                for (size_t i = *from + 1; i < n && !to; ++i)
                {
                    if (!isCode(i)) continue;
                    if (text[i] == c) ++depth;
                    else if (text[i] == other && depth-- == 0) to = i;
                }
            }
            else
            {
                for (size_t i = *from; i-- > 0 && !to;)
                {
                    if (!isCode(i)) continue;
                    if (text[i] == c) ++depth;
                    else if (text[i] == other && depth-- == 0) to = i;
                }
            }
        }
        if (!to)
        {
            StatusMessageText().Text(from ? L"No matching bracket" : L"No bracket here");
            return;
        }

        StatusMessageText().Text(L"");
        size_t first = std::min(*from, *to);
        size_t last = std::max(*from, *to);
        if (select)
        {
            SelectAndReveal(editor, doc->ScrollViewer, static_cast<int>(first), static_cast<int>(last - first + 1));
        }
        else
        {
            SelectAndReveal(editor, doc->ScrollViewer, static_cast<int>(*to), 0);
        }
    }

    void MainWindow::InsertText(RichEditBox const& editor, std::wstring_view text)
    {
        // Replaces the selection and leaves the caret after the new text. Nothing for a read-only file.
        if (editor.IsReadOnly())
        {
            return;
        }
        auto selection = editor.Document().Selection();
        if (selection.Length() != 0 || !text.empty())
        {
            // Copied, as a view of part of a string isn't null-terminated, which passing it straight to
            // SetText needs (C++/WinRT aborts otherwise).
            selection.SetText(TextSetOptions::None, hstring{ text });
            selection.Collapse(false);
        }
    }

    fire_and_forget MainWindow::PasteAsync(std::weak_ptr<Document> weakDoc)
    {
        // Reads the clipboard ourselves rather than letting RichEditBox paste, so only plain text arrives.
        auto strong = get_strong();
        hstring text;
        try
        {
            auto content = Clipboard::GetContent();
            if (!content.Contains(StandardDataFormats::Text()))
            {
                co_return;
            }
            text = co_await content.GetTextAsync();
        }
        catch (hresult_error const&)
        {
            co_return; // the clipboard is busy or its contents can't be read
        }

        if (auto doc = weakDoc.lock())
        {
            InsertText(doc->Editor, textfile::NormalizeLineBreaks(text));
        }
    }

    void MainWindow::UpdateFontMetrics()
    {
        auto metrics = GetFontMetrics(m_fontFamily, m_fontBold, m_fontItalic).value_or(FontMetrics{});
        m_fontMonospaced = metrics.Monospaced;
        m_spaceAdvance = metrics.SpaceAdvance;
    }

    void MainWindow::InsertTab(RichEditBox const& editor)
    {
        if (!m_insertSpaces)
        {
            InsertText(editor, L"\t");
            return;
        }

        // Insert spaces up to the next multiple of the indent size, counting a tab as reaching the
        // next multiple of the tab width, so indentation lines up with the existing text.
        auto text = EditorText(editor);
        size_t caret = std::min(static_cast<size_t>(std::max(SelectionStart(editor), 0)), text.size());
        size_t lineStart = text.find_last_of(L'\r', caret == 0 ? 0 : caret - 1);
        lineStart = lineStart == std::wstring::npos || caret == 0 ? 0 : lineStart + 1;
        int column = 0;
        for (size_t i = lineStart; i < caret; ++i)
        {
            column = text[i] == L'\t' ? (column / m_tabWidth + 1) * m_tabWidth : column + 1;
        }
        InsertText(editor, std::wstring(static_cast<size_t>(m_indentSize - column % m_indentSize), L' '));
    }

    bool MainWindow::SmartHome(RichEditBox const& editor, bool extend)
    {
        // Moves the caret (the selection's active end) to the first non-whitespace character of its line,
        // or to the start of the line if it's already there. Returns false to leave Home to the editor.
        auto selection = editor.Document().Selection();
        bool startActive = (selection.Options() & Microsoft::UI::Text::SelectionOptions::StartActive) == Microsoft::UI::Text::SelectionOptions::StartActive;
        int caret = startActive ? selection.StartPosition() : selection.EndPosition();
        int anchor = startActive ? selection.EndPosition() : selection.StartPosition();

        auto text = EditorText(editor);
        auto position = std::min(static_cast<size_t>(std::max(caret, 0)), text.size());
        size_t lineStart = position == 0 ? std::wstring::npos : text.find_last_of(L'\r', position - 1);
        lineStart = lineStart == std::wstring::npos ? 0 : lineStart + 1;
        size_t firstText = lineStart;
        while (firstText < text.size() && (text[firstText] == L' ' || text[firstText] == L'\t'))
        {
            ++firstText;
        }

        // On a later row of a wrapped line, Home first goes to the start of that row, as usual.
        auto row = editor.Document().GetRange(caret, caret);
        row.StartOf(Microsoft::UI::Text::TextRangeUnit::Line, false);
        if (row.StartPosition() > static_cast<int>(lineStart) && row.StartPosition() != caret)
        {
            return false;
        }

        int target = static_cast<int>(position == firstText ? lineStart : firstText);
        // SetRange's first position is the anchor and the second the active end, so Shift+Home keeps
        // extending from the same place.
        selection.SetRange(extend ? anchor : target, target);
        return true;
    }

    bool MainWindow::SelectionSpansLines(RichEditBox const& editor)
    {
        auto selection = editor.Document().Selection();
        int start = selection.StartPosition();
        int end = selection.EndPosition();
        if (end <= start)
        {
            return false;
        }
        auto text = EditorText(editor);
        end = std::min(end, static_cast<int>(text.size()));
        return std::find(text.begin() + std::min(start, end), text.begin() + end, L'\r') != text.begin() + end;
    }

    void MainWindow::ChangeIndent(std::shared_ptr<Document> const& doc, bool outdent)
    {
        // Indents or outdents whole lines: those the selection touches, or the caret's line. A selection
        // ending at the very start of a line doesn't include that line.
        auto editor = doc->Editor;
        auto text = EditorText(editor);
        auto selection = editor.Document().Selection();
        auto size = text.size();
        size_t start = std::min(static_cast<size_t>(std::max(selection.StartPosition(), 0)), size);
        size_t end = std::min(static_cast<size_t>(std::max(selection.EndPosition(), 0)), size);
        bool hadSelection = end > start;
        size_t last = hadSelection && text[end - 1] == L'\r' ? end - 1 : end;

        size_t blockStart = start == 0 ? std::wstring::npos : text.find_last_of(L'\r', start - 1);
        blockStart = blockStart == std::wstring::npos ? 0 : blockStart + 1;
        size_t blockEnd = std::min(text.find(L'\r', last), size);

        // One level of indentation: what Tab inserts at the start of a line, and what Shift+Tab removes
        // (a tab, or up to that many columns of spaces).
        std::wstring unit = m_insertSpaces ? std::wstring(static_cast<size_t>(m_indentSize), L' ') : L"\t";
        size_t columns = static_cast<size_t>(m_insertSpaces ? m_indentSize : m_tabWidth);

        std::wstring block;
        size_t removedBeforeCaret = 0; // for keeping the caret in place when outdenting one line
        for (size_t lineStart = blockStart; lineStart <= blockEnd;)
        {
            size_t lineEnd = std::min(text.find(L'\r', lineStart), blockEnd);
            std::wstring_view line{ text.data() + lineStart, lineEnd - lineStart };
            if (outdent)
            {
                size_t remove = 0;
                if (!line.empty() && line[0] == L'\t')
                {
                    remove = 1;
                }
                else
                {
                    while (remove < line.size() && remove < columns && line[remove] == L' ')
                    {
                        ++remove;
                    }
                }
                if (lineStart == blockStart)
                {
                    removedBeforeCaret = std::min(remove, start - lineStart);
                }
                block += line.substr(remove);
            }
            else
            {
                if (!line.empty()) // don't leave whitespace on empty lines
                {
                    block += unit;
                }
                block += line;
            }
            if (lineEnd >= blockEnd)
            {
                break;
            }
            block += L'\r';
            lineStart = lineEnd + 1;
        }

        if (block != std::wstring_view{ text.data() + blockStart, blockEnd - blockStart })
        {
            doc->StartNewUndoRecord = true; // its own undo step
            Select(editor, static_cast<int>(blockStart), static_cast<int>(blockEnd - blockStart));
            InsertText(editor, block);
            doc->StartNewUndoRecord = true;
        }

        if (hadSelection)
        {
            // Keep the lines selected, so Tab can be pressed again.
            Select(editor, static_cast<int>(blockStart), static_cast<int>(block.size()));
        }
        else
        {
            Select(editor, static_cast<int>(start - removedBeforeCaret), 0);
        }
    }

    void MainWindow::SetIndentation(bool insertSpaces, int indentSize, int tabWidth)
    {
        bool tabWidthChanged = tabWidth != m_tabWidth;
        m_insertSpaces = insertSpaces;
        m_indentSize = std::clamp(indentSize, 1, 8);
        m_tabWidth = std::clamp(tabWidth, 1, 8);
        if (tabWidthChanged)
        {
            ApplyEditorSettingsToAll(); // sets each editor's tab stops
        }
        UpdateIndentUI();
        SaveSettings();
        if (auto doc = CurrentDocument())
        {
            FocusEditor(doc);
        }
    }

    void MainWindow::BuildIndentMenus()
    {
        // The same menu appears under Format > Tabs and on the status bar button.
        auto build = [this](Collections::IVector<MenuFlyoutItemBase> const& items, std::wstring const& group)
        {
            for (bool spaces : { false, true })
            {
                RadioMenuFlyoutItem item;
                item.Text(spaces ? L"Indent using spaces" : L"Indent using tabs");
                item.GroupName(group + L"Kind");
                item.Click([this, spaces](auto&&, auto&&) { SetIndentation(spaces, m_indentSize, m_tabWidth); });
                m_indentMenuUpdaters.push_back([this, item, spaces] { item.IsChecked(m_insertSpaces == spaces); });
                items.Append(item);
            }
            items.Append(MenuFlyoutSeparator{});

            for (bool indent : { true, false })
            {
                MenuFlyoutSubItem sizes;
                sizes.Text(indent ? L"Indent size (spaces)" : L"Tab width");
                for (int n = 1; n <= 8; ++n)
                {
                    RadioMenuFlyoutItem item;
                    item.Text(to_hstring(n));
                    item.GroupName(group + (indent ? L"Size" : L"Width"));
                    item.Click([this, n, indent](auto&&, auto&&)
                    {
                        SetIndentation(m_insertSpaces, indent ? n : m_indentSize, indent ? m_tabWidth : n);
                    });
                    m_indentMenuUpdaters.push_back([this, item, n, indent] { item.IsChecked((indent ? m_indentSize : m_tabWidth) == n); });
                    sizes.Items().Append(item);
                }
                items.Append(sizes);
            }
        };
        build(TabsMenu().Items(), L"TabsMenu");
        build(IndentFlyout().Items(), L"IndentStatus");
        UpdateIndentUI();
    }

    void MainWindow::UpdateIndentUI()
    {
        for (auto const& update : m_indentMenuUpdaters)
        {
            update();
        }
        IndentButton().Content(box_value(hstring{ m_insertSpaces
            ? std::format(L"Spaces: {}", m_indentSize)
            : std::format(L"Tab width: {}", m_tabWidth) }));
    }

    void MainWindow::RecordEdit(std::shared_ptr<Document> const& doc, size_t start, std::wstring removed, std::wstring inserted)
    {
        constexpr size_t MaxUndoRecords = 1000;

        auto& undo = doc->UndoStack;
        doc->RedoStack.clear();

        // Typing and deleting merge into one record per word, like Visual Studio.
        if (!undo.empty() && !std::exchange(doc->StartNewUndoRecord, false))
        {
            auto& last = undo.back();
            bool lastIsInsertion = last.Removed.empty() && !last.Inserted.empty();
            bool lastIsDeletion = last.Inserted.empty() && !last.Removed.empty();
            if (removed.empty() && lastIsInsertion && start == last.Start + last.Inserted.size()
                && !(IsSpace(inserted.front()) && !IsSpace(last.Inserted.back())))
            {
                last.Inserted += inserted;
                return;
            }
            if (inserted.empty() && lastIsDeletion && start + removed.size() == last.Start && !IsSpace(removed.back()))
            {
                last.Start = start; // Backspace
                last.Removed = removed + last.Removed;
                return;
            }
            if (inserted.empty() && lastIsDeletion && start == last.Start && !IsSpace(removed.front()))
            {
                last.Removed += removed; // Delete
                return;
            }
        }
        doc->StartNewUndoRecord = false;

        undo.push_back({ start, std::move(removed), std::move(inserted) });
        if (undo.size() > MaxUndoRecords)
        {
            undo.erase(undo.begin());
        }
    }

    void MainWindow::UndoOrRedo(std::shared_ptr<Document> const& doc, bool redo)
    {
        if (doc->Editor.IsReadOnly())
        {
            return;
        }

        // Record any edit that hasn't been processed yet, so it's the first thing undone.
        UpdateHighlighting(doc, false);

        auto& from = redo ? doc->RedoStack : doc->UndoStack;
        auto& to = redo ? doc->UndoStack : doc->RedoStack;
        if (from.empty())
        {
            return;
        }
        UndoRecord record = std::move(from.back());
        from.pop_back();

        auto const& current = redo ? record.Removed : record.Inserted; // the text now in the document
        auto const& replacement = redo ? record.Inserted : record.Removed;
        int start = static_cast<int>(record.Start);
        int end = start + static_cast<int>(current.size());

        // Whether the edit is out of view, before changing the text makes the editor scroll.
        auto outOfView = OutOfView(doc->Editor.Document(), start, doc->ScrollViewer);
        doc->ApplyingUndo = true;
        doc->Editor.Document().GetRange(start, end).SetText(TextSetOptions::None, replacement);
        doc->EditStart = start;
        doc->EditEnd = end;
        UpdateHighlighting(doc, false);
        doc->ApplyingUndo = false;
        doc->StartNewUndoRecord = true;

        Select(doc->Editor, start + static_cast<int>(replacement.size()), 0);
        CentreInView(doc->Editor, doc->ScrollViewer, start, outOfView);
        to.push_back(std::move(record));
        UpdateDocumentState(doc);
    }

    void MainWindow::ClearUndoHistory(std::shared_ptr<Document> const& doc)
    {
        doc->UndoStack.clear();
        doc->RedoStack.clear();
        doc->StartNewUndoRecord = true;
    }
}
