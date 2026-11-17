#include "pch.h"
#include "MainWindowHelpers.h"
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

// Documents: creating, opening, saving, closing, and reloading files changed by other programs.
namespace winrt::RadPad::implementation
{
    std::wstring Document::DisplayName() const
    {
        if (Path.empty())
        {
            return Url.empty() ? L"Untitled" : UrlFileName(Url);
        }
        auto slash = Path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? Path : Path.substr(slash + 1);
    }

    std::shared_ptr<Document> MainWindow::CreateDocument()
    {
        auto doc = std::make_shared<Document>();
        std::weak_ptr<Document> weakDoc = doc;

        // A RichEditBox rather than a TextBox so that text can be coloured. It's used as a plain-text
        // editor: the highlighter owns all formatting, and text is copied and pasted as plain text.
        RichEditBox editor;
        editor.IsSpellCheckEnabled(false);
        editor.IsTextPredictionEnabled(false);
        editor.ClipboardCopyFormat(RichEditClipboardFormat::PlainText);
        editor.DisabledFormattingAccelerators(DisabledFormattingAccelerators::All);
        editor.Document().UndoLimit(0); // see Document::UndoStack
        editor.BorderThickness(Thickness{ 0, 0, 0, 0 });
        editor.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 0, 0, 0, 0 });
        editor.Visibility(Visibility::Collapsed);
        ScrollViewer::SetHorizontalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
        ScrollViewer::SetVerticalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
        if (auto accent = Application::Current().Resources().TryLookup(box_value(L"SystemAccentColor")))
        {
            // Keep find results visible while focus is in the find box.
            auto color = unbox_value<Windows::UI::Color>(accent);
            color.A = 0x80;
            editor.SelectionHighlightColorWhenNotFocused(SolidColorBrush{ color });
        }
        doc->Editor = editor;
        ApplyEditorSettings(doc);

        editor.TextChanging([this, weakDoc](RichEditBox const&, RichEditBoxTextChangingEventArgs const& e)
        {
            // Formatting changes, including our own highlighting, raise this event too; only react to edits.
            if (auto d = weakDoc.lock(); d && e.IsContentChanging())
            {
                if (!d->RefreshPending)
                {
                    // The selection hasn't been updated for this edit yet, so it's the text being replaced.
                    d->EditStart = d->SelectionStart;
                    d->EditEnd = d->SelectionEnd;
                }
                ScheduleRefresh(d);
            }
        });
        editor.SelectionChanged([this, weakDoc](auto&&, auto&&)
        {
            if (auto d = weakDoc.lock())
            {
                auto selection = d->Editor.Document().Selection();
                d->SelectionStart = selection.StartPosition();
                d->SelectionEnd = selection.EndPosition();
                if (d == CurrentDocument())
                {
                    UpdateCaretPosition();
                    if (m_highlightTrailingWhitespace)
                    {
                        ScheduleOverlayUpdate(); // whitespace at the caret isn't highlighted
                    }
                }
            }
        });
        // Only visible text is coloured, so colour more when the view moves or grows. The editor's
        // ScrollViewer only exists once its template is applied, which doesn't happen while the
        // editor is collapsed, so look for it whenever the editor is resized (including when shown).
        editor.SizeChanged([this, weakDoc](IInspectable const& sender, auto&&)
        {
            auto d = weakDoc.lock();
            if (!d)
            {
                return;
            }
            if (!d->ScrollViewer)
            {
                if (auto viewer = FindDescendant<ScrollViewer>(sender.as<DependencyObject>()))
                {
                    d->ScrollViewer = viewer;
                    auto onViewChanged = [this, weakDoc]()
                    {
                        if (auto scrolled = weakDoc.lock())
                        {
                            ScheduleRepaint(scrolled);
                            if (scrolled == CurrentDocument())
                            {
                                UpdateEditorOverlay(); // keep the markers in step while scrolling
                            }
                        }
                    };
                    viewer.ViewChanged([onViewChanged](auto&&, auto&&) { onViewChanged(); });
                    // The extent changes when the text is laid out again, e.g. after word wrap is turned on or off,
                    // which can be after the change that caused it has finished.
                    viewer.RegisterPropertyChangedCallback(ScrollViewer::ExtentHeightProperty(), [onViewChanged](auto&&, auto&&) { onViewChanged(); });
                    viewer.RegisterPropertyChangedCallback(ScrollViewer::ExtentWidthProperty(), [onViewChanged](auto&&, auto&&) { onViewChanged(); });
                }
            }
            ScheduleRepaint(d);
        });
        editor.Paste([this, weakDoc](IInspectable const&, TextControlPasteEventArgs const& e)
        {
            // Paste without the source's formatting.
            e.Handled(true);
            PasteAsync(weakDoc);
        });
        editor.PreviewKeyDown([this, weakDoc](IInspectable const&, KeyRoutedEventArgs const& e)
        {
            bool ctrl = IsKeyDown(VK_CONTROL);
            bool shift = IsKeyDown(VK_SHIFT);
            bool alt = IsKeyDown(VK_MENU);
            int key = static_cast<int>(e.Key());
            auto d = weakDoc.lock();

            // Insert tabs ourselves instead of letting Tab move focus. With several lines selected, Tab
            // indents them; Shift+Tab outdents the selected lines, or the current line.
            if (key == VK_TAB && !ctrl && !alt && d)
            {
                if (shift || SelectionSpansLines(d->Editor))
                {
                    ChangeIndent(d, shift);
                }
                else
                {
                    InsertTab(d->Editor);
                }
                e.Handled(true);
                return;
            }
            // Enter starts the new line with the current line's indentation. Shift+Enter too, which would
            // otherwise insert a line break that isn't a new line.
            if (key == VK_RETURN && !ctrl && !alt && d && !d->Editor.IsReadOnly())
            {
                InsertNewLine(d->Editor);
                e.Handled(true);
                return;
            }
            // Home goes to the first non-whitespace character of the line, or to the start if already there.
            if (key == VK_HOME && !ctrl && !alt && d && SmartHome(d->Editor, shift))
            {
                e.Handled(true);
                return;
            }
            // Undo and redo use our own history (see Document::UndoStack).
            bool undo = (ctrl && !alt && key == 'Z') || (alt && !ctrl && !shift && key == VK_BACK);
            bool redo = ctrl && !alt && (key == 'Y' || (shift && key == 'Z'));
            if (d && (undo || redo))
            {
                UndoOrRedo(d, redo);
                e.Handled(true);
                return;
            }
            if (!ctrl || alt)
            {
                return;
            }

            switch (key)
            {
            case VK_OEM_6:
                // Ctrl+] goes to the matching bracket (Ctrl+Shift+] selects to it), as in Visual Studio.
                // RichEditBox would otherwise use it to make the font bigger.
                if (d)
                {
                    JumpToMatchingBracket(d, shift);
                }
                e.Handled(true);
                break;
            case 'E': case 'J': case 'L': case 'R': case VK_OEM_4: case VK_OEM_COMMA: case VK_OEM_PERIOD:
                // RichEditBox shortcuts for alignment, lists and font size don't apply to plain text.
                e.Handled(true);
                break;
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                // Ctrl+Left Shift / Ctrl+Right Shift switch the paragraph's reading direction.
                e.Handled(true);
                break;
            case 'A':
                // Ctrl+Shift+A is all caps; Ctrl+A (select all) is left alone.
                e.Handled(shift);
                break;
            case VK_OEM_PLUS: case VK_ADD:
                // Handled here because RichEditBox uses Ctrl+= for subscript.
                SetZoom(m_zoom + ZoomStep);
                e.Handled(true);
                break;
            case VK_OEM_MINUS: case VK_SUBTRACT:
                SetZoom(m_zoom - ZoomStep);
                e.Handled(true);
                break;
            }
        });
        editor.AddHandler(UIElement::PointerWheelChangedEvent(), box_value(PointerEventHandler{ [this](IInspectable const& sender, PointerRoutedEventArgs const& e)
        {
            if ((e.KeyModifiers() & VirtualKeyModifiers::Control) == VirtualKeyModifiers::Control)
            {
                int delta = e.GetCurrentPoint(sender.as<UIElement>()).Properties().MouseWheelDelta();
                SetZoom(m_zoom + (delta > 0 ? ZoomStep : -ZoomStep));
                e.Handled(true);
            }
        } }), true);
        // Double-clicking selects a word and the spaces after it; drop the spaces, so the word alone is
        // selected. A double-click on spaces still selects them.
        editor.AddHandler(UIElement::DoubleTappedEvent(), box_value(DoubleTappedEventHandler{ [](IInspectable const& sender, DoubleTappedRoutedEventArgs const&)
        {
            auto selection = sender.as<RichEditBox>().Document().Selection();
            if (selection.Length() == 0)
            {
                return;
            }
            hstring text;
            selection.GetText(TextGetOptions::None, text);
            std::wstring_view view{ text };
            size_t end = view.find_last_not_of(L" \t ");
            if (end != std::wstring_view::npos && end + 1 < view.size() && view.find(L'\r') == std::wstring_view::npos)
            {
                selection.SetRange(selection.StartPosition(), selection.StartPosition() + static_cast<int>(end + 1));
            }
        } }), true);

        TabViewItem tab;
        FontIconSource icon;
        icon.Glyph(L"");
        tab.IconSource(icon);

        doc->Tab = tab;
        tab.ContextFlyout(RootGrid().Resources().Lookup(box_value(L"TabMenu")).as<MenuFlyout>());
        m_documents.push_back(doc);
        EditorHost().Children().Append(editor);
        UpdateDocumentState(doc);
        Tabs().TabItems().Append(tab);
        Tabs().SelectedItem(tab);
        return doc;
    }

    std::shared_ptr<Document> MainWindow::CurrentDocument()
    {
        return FindDocument(Tabs().SelectedItem());
    }

    std::shared_ptr<Document> MainWindow::FindDocument(IInspectable const& tab)
    {
        if (!tab)
        {
            return nullptr;
        }
        auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](auto const& d) { return d->Tab == tab; });
        return it == m_documents.end() ? nullptr : *it;
    }

    std::shared_ptr<Document> MainWindow::FindDocumentByPath(std::wstring const& path)
    {
        auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](auto const& d) { return !d->Path.empty() && PathsEqual(d->Path, path); });
        return it == m_documents.end() ? nullptr : *it;
    }

    void MainWindow::OpenFile(std::wstring const& path)
    {
        // Everything that opens files (the command line, Recent files, the jump list) can also open a URL.
        if (instance::IsUrl(path))
        {
            OpenUrlAsync(path);
            return;
        }

        std::wstring fullPath = FullPath(path);
        if (auto existing = FindDocumentByPath(fullPath))
        {
            Tabs().SelectedItem(existing->Tab);
            return;
        }

        textfile::LoadedText loaded;
        try
        {
            loaded = textfile::Load(fullPath);
        }
        catch (hresult_error const& e)
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"Couldn't open \"{}\".\n\n{}", fullPath, std::wstring_view{ e.message() }) });
            return;
        }

        ShowLoadedText(loaded, fullPath, {});
        SHAddToRecentDocs(SHARD_PATHW, fullPath.c_str());
        AddRecentFile(fullPath);
    }

    void MainWindow::ShowLoadedText(textfile::LoadedText const& loaded, std::wstring const& path, std::wstring const& url)
    {
        // Into the current tab if it's an untouched blank document, as Notepad does, or a new one.
        auto doc = CurrentDocument();
        if (!doc || !doc->Path.empty() || !doc->Url.empty() || doc->IsDirty || !EditorText(doc->Editor).empty())
        {
            doc = CreateDocument();
        }

        doc->Path = path;
        doc->Url = url;
        doc->Encoding = doc->SavedEncoding = loaded.Encoding;
        doc->Eol = doc->SavedEol = loaded.Eol;
        doc->Language = syntax::LanguageFromPath(path.empty() ? doc->DisplayName() : path);
        doc->Editor.Document().SetText(TextSetOptions::None, loaded.Text);
        doc->SavedText = EditorText(doc->Editor);
        doc->DiskStamp = path.empty() ? std::nullopt : FileStamp(path);
        doc->PendingDiskStamp.reset();
        doc->DeletedOnDisk = false;
        Select(doc->Editor, 0, 0);
        UpdateHighlighting(doc, true);
        ClearUndoHistory(doc);
        UpdateReadOnly(doc);
        UpdateDocumentState(doc);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
    }

    fire_and_forget MainWindow::OpenUrlAsync(std::wstring url)
    {
        // Downloaded into a read-only tab, which isn't tied to a file: saving it asks where, as for a new
        // document, and the saved copy can then be edited.
        if (auto it = std::find_if(m_documents.begin(), m_documents.end(), [&](auto const& d) { return d->Url == url; }); it != m_documents.end())
        {
            Tabs().SelectedItem((*it)->Tab);
            co_return;
        }

        auto strong = get_strong();
        apartment_context ui;
        StatusMessageText().Text(std::format(L"Downloading {}...", url));
        textfile::LoadedText loaded;
        std::wstring error;
        try
        {
            Windows::Web::Http::HttpClient client;
            auto response = co_await client.GetAsync(Uri{ url }, Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead);
            auto length = response.Content().Headers().ContentLength();
            if (!response.IsSuccessStatusCode())
            {
                error = std::format(L"The server answered {} {}.", static_cast<int>(response.StatusCode()), std::wstring_view{ response.ReasonPhrase() });
            }
            else if (length && length.Value() > textfile::MaxFileSize)
            {
                error = L"The file is too large to open.";
            }
            else
            {
                auto buffer = co_await response.Content().ReadAsBufferAsync();
                if (buffer.Length() > textfile::MaxFileSize)
                {
                    error = L"The file is too large to open.";
                }
                else
                {
                    loaded = textfile::FromBytes({ reinterpret_cast<char const*>(buffer.data()), buffer.Length() });
                }
            }
        }
        catch (hresult_error const& e)
        {
            error = e.message();
        }
        co_await ui;
        StatusMessageText().Text(L"");
        if (m_closed)
        {
            co_return;
        }
        if (!error.empty())
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"Couldn't download \"{}\".\n\n{}", url, error) });
            co_return;
        }

        ShowLoadedText(loaded, {}, url);
        AddRecentFile(url);
    }

    fire_and_forget MainWindow::AskForUrlAsync()
    {
        auto strong = get_strong();
        TextBox urlBox;
        urlBox.Header(box_value(L"Address"));
        urlBox.PlaceholderText(L"https://");
        urlBox.MinWidth(400);
        // Filled in from the clipboard when it holds an address.
        try
        {
            auto content = Clipboard::GetContent();
            if (content.Contains(StandardDataFormats::Text()))
            {
                std::wstring text{ co_await content.GetTextAsync() };
                if (instance::IsUrl(text) && text.find_first_of(L" \t\r\n") == std::wstring::npos)
                {
                    urlBox.Text(text);
                }
            }
        }
        catch (hresult_error const&)
        {
        }
        urlBox.Loaded([weakBox = make_weak(urlBox)](auto&&, auto&&)
        {
            if (auto box = weakBox.get())
            {
                box.Focus(FocusState::Programmatic);
                box.SelectAll();
            }
        });

        ContentDialog dialog;
        dialog.Title(box_value(L"Open URL"));
        dialog.Content(urlBox);
        dialog.PrimaryButtonText(L"Open");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary)
        {
            co_return;
        }

        std::wstring url{ urlBox.Text() };
        url.erase(0, url.find_first_not_of(L" \t"));
        url.erase(url.find_last_not_of(L" \t") + 1);
        if (!instance::IsUrl(url))
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"\"{}\" isn't a web address. It should start with http:// or https://.", url) });
            co_return;
        }
        OpenUrlAsync(url);
    }

    void MainWindow::OpenWithDialog()
    {
        try
        {
            for (auto const& path : ShowOpenFileDialog(WindowHandle()))
            {
                OpenFile(path);
            }
        }
        catch (hresult_error const& e)
        {
            ShowMessageAsync(hstring{ AppName }, e.message());
        }
    }

    bool MainWindow::SaveDocument(std::shared_ptr<Document> const& doc, bool saveAs)
    {
        std::wstring path = doc->Path;
        TextEncoding encoding = doc->Encoding;

        if (saveAs || path.empty())
        {
            try
            {
                // A new document is suggested the first extension of its language, or .txt; a downloaded
                // one, its name in the URL.
                std::wstring suggested = doc->DisplayName();
                if (doc->Path.empty() && doc->Url.empty())
                {
                    auto patterns = syntax::LanguagePatterns(doc->Language);
                    bool hasExtension = !patterns.empty() && patterns.front().starts_with(L"*.");
                    suggested = L"Untitled." + (hasExtension ? patterns.front().substr(2) : std::wstring{ L"txt" });
                }
                auto result = ShowSaveFileDialog(WindowHandle(), doc->Path, suggested, doc->Encoding, doc->Language);
                if (!result)
                {
                    return false;
                }
                path = result->Path;
                encoding = result->Encoding;
            }
            catch (hresult_error const& e)
            {
                ShowMessageAsync(hstring{ AppName }, e.message());
                return false;
            }
        }

        std::wstring text = EditorText(doc->Editor);
        try
        {
            textfile::Save(path, text, encoding, doc->Eol);
        }
        catch (hresult_error const& e)
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"Couldn't save \"{}\".\n\n{}", path, std::wstring_view{ e.message() }) });
            return false;
        }

        bool pathChanged = !PathsEqual(path, doc->Path);
        doc->Path = path;
        doc->Url.clear(); // a downloaded document is now this file, and can be edited
        doc->Encoding = doc->SavedEncoding = encoding;
        doc->SavedEol = doc->Eol;
        doc->SavedText = std::move(text);
        doc->DiskStamp = FileStamp(path); // our own save isn't a change by another program
        doc->PendingDiskStamp.reset();
        if (std::exchange(doc->DeletedOnDisk, false))
        {
            StatusMessageText().Text(L""); // no longer true that it was deleted
        }
        UpdateReadOnly(doc); // saved under a new name, it's that file's attribute
        UpdateDocumentState(doc);

        // Saving under a new name switches to its extension's language, unless that would turn highlighting off.
        if (auto language = syntax::LanguageFromPath(path); pathChanged && language != syntax::PlainText && language != doc->Language)
        {
            SetLanguage(doc, language);
        }
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
        SHAddToRecentDocs(SHARD_PATHW, path.c_str());
        AddRecentFile(path);
        if (PathsEqual(path, ToolsFilePath()))
        {
            ReloadTools(); // edited with Tools > Edit tools
        }
        return true;
    }

    void MainWindow::CheckFilesOnDisk()
    {
        auto documents = m_documents; // reloading can't change the list, but a dialog's continuation might
        for (auto const& doc : documents)
        {
            CheckDocumentFile(doc);
        }
    }

    void MainWindow::CheckDocumentFile(std::shared_ptr<Document> const& doc)
    {
        // Runs every second for each open file. As for the tools file, acts once a change has stayed the
        // same for a tick, so a program part-way through writing the file isn't caught.
        if (doc->Path.empty() || doc->AskingToReload || doc->IsClosing)
        {
            return;
        }
        if (IsReadOnlyFile(doc->Path) != doc->ReadOnly)
        {
            UpdateReadOnly(doc); // changed in Explorer, say; that doesn't change the stamp
        }
        auto stamp = FileStamp(doc->Path);
        if (stamp == doc->DiskStamp)
        {
            doc->PendingDiskStamp.reset();
            return;
        }
        if (!doc->PendingDiskStamp || stamp != *doc->PendingDiskStamp)
        {
            doc->PendingDiskStamp = stamp;
            return;
        }

        if (!stamp)
        {
            // Deleted (or renamed or moved). Keep the text, marked unsaved so saving puts the file back.
            doc->PendingDiskStamp.reset();
            doc->DiskStamp.reset();
            doc->DeletedOnDisk = true;
            UpdateDocumentState(doc);
            StatusMessageText().Text(std::format(L"{} was deleted by another program; save to keep it", doc->DisplayName()));
            return;
        }
        if (doc->IsDirty && !doc->DeletedOnDisk)
        {
            // Unsaved changes would be lost, so ask. One question at a time: try again next tick if a
            // dialog is already open.
            if (!m_dialogOpen)
            {
                AskToReloadAsync(doc, stamp);
            }
            return;
        }
        ReloadDocument(doc);
    }

    fire_and_forget MainWindow::AskToReloadAsync(std::shared_ptr<Document> doc, std::optional<std::pair<uint64_t, uint64_t>> stamp)
    {
        auto strong = get_strong();
        doc->AskingToReload = true;
        Tabs().SelectedItem(doc->Tab);

        ContentDialog dialog;
        dialog.Title(box_value(hstring{ AppName }));
        dialog.Content(box_value(hstring{ std::format(L"\"{}\" has been changed by another program.\n\nReload it? Your unsaved changes will be replaced, though Undo can bring them back.", doc->Path) }));
        dialog.PrimaryButtonText(L"Reload");
        dialog.CloseButtonText(L"Keep my version");
        dialog.DefaultButton(ContentDialogButton::Close);
        auto result = co_await ShowDialogAsync(dialog);

        doc->AskingToReload = false;
        doc->PendingDiskStamp.reset();
        if (result == ContentDialogResult::Primary)
        {
            ReloadDocument(doc);
        }
        else
        {
            doc->DiskStamp = stamp; // don't ask again until the file changes again
        }
    }

    void MainWindow::UpdateReadOnly(std::shared_ptr<Document> const& doc)
    {
        // Follows the file's read-only attribute and the user's permission to write it. A downloaded document
        // is read-only until it's saved. A tab being moved to another window stays read-only until that's
        // done (see MoveTabToNewProcessAsync).
        doc->ReadOnly = !doc->Url.empty() || (!doc->Path.empty() && IsReadOnlyFile(doc->Path));
        doc->CanWrite = doc->Path.empty() || doc->DeletedOnDisk || CanWriteFile(doc->Path);
        if (!doc->IsClosing)
        {
            doc->Editor.IsReadOnly(doc->ReadOnly || !doc->CanWrite);
        }
        if (doc == CurrentDocument())
        {
            UpdateReadOnlyItem(doc);
        }
    }

    void MainWindow::UpdateReadOnlyItem(std::shared_ptr<Document> const& doc)
    {
        // Without permission to write the file, the attribute makes no difference, so it can't be changed.
        auto item = ReadOnlyItem();
        item.IsChecked(doc->ReadOnly || !doc->CanWrite);
        item.IsEnabled(!doc->Path.empty() && doc->CanWrite);
        item.Text(!doc->Url.empty() ? L"Read-only (downloaded; save a copy to edit)"
                  : doc->CanWrite   ? L"Read-only"
                                    : L"Read-only (no permission to change the file)");
    }

    bool MainWindow::ReloadDocument(std::shared_ptr<Document> const& doc)
    {
        // Replaces the text with the file's, as one undoable edit, keeping the caret and scroll position.
        auto stamp = FileStamp(doc->Path);
        doc->PendingDiskStamp.reset();
        doc->DiskStamp = stamp; // also when reading fails, so a file that can't be read isn't retried every second
        textfile::LoadedText loaded;
        try
        {
            loaded = textfile::Load(doc->Path);
        }
        catch (hresult_error const& e)
        {
            StatusMessageText().Text(std::format(L"Couldn't reload {}: {}", doc->DisplayName(), std::wstring_view{ e.message() }));
            return false;
        }

        auto editor = doc->Editor;
        auto oldText = EditorText(editor);
        auto newText = textfile::NormalizeLineBreaks(loaded.Text);
        if (oldText != newText)
        {
            // Only the part that changed is replaced, so the caret, highlighting and undo record stay small.
            size_t prefix = 0;
            while (prefix < oldText.size() && prefix < newText.size() && oldText[prefix] == newText[prefix])
            {
                ++prefix;
            }
            size_t suffix = 0;
            while (suffix < oldText.size() - prefix && suffix < newText.size() - prefix
                && oldText[oldText.size() - 1 - suffix] == newText[newText.size() - 1 - suffix])
            {
                ++suffix;
            }
            size_t oldEnd = oldText.size() - suffix;
            int delta = static_cast<int>(newText.size()) - static_cast<int>(oldText.size());

            // The caret moves with the text after the change; inside the change it goes to its start.
            int caret = SelectionStart(editor);
            if (caret >= static_cast<int>(oldEnd))
            {
                caret += delta;
            }
            else if (caret > static_cast<int>(prefix))
            {
                caret = static_cast<int>(prefix);
            }

            double horizontal = doc->ScrollViewer ? doc->ScrollViewer.HorizontalOffset() : 0;
            double vertical = doc->ScrollViewer ? doc->ScrollViewer.VerticalOffset() : 0;
            WritableScope writable{ editor }; // a read-only file is still kept up to date
            doc->StartNewUndoRecord = true;
            Select(editor, static_cast<int>(prefix), static_cast<int>(oldEnd - prefix));
            InsertText(editor, std::wstring_view{ newText }.substr(prefix, newText.size() - suffix - prefix));
            doc->StartNewUndoRecord = true;
            Select(editor, std::clamp(caret, 0, static_cast<int>(newText.size())), 0);

            // Selecting scrolls the caret into view; put the view back where it was once that's done.
            if (auto viewer = doc->ScrollViewer)
            {
                DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [viewer, horizontal, vertical]
                {
                    viewer.ChangeView(horizontal, vertical, nullptr, true);
                });
            }
        }

        doc->SavedText = newText;
        doc->Encoding = doc->SavedEncoding = loaded.Encoding;
        doc->Eol = doc->SavedEol = loaded.Eol;
        doc->DeletedOnDisk = false;
        UpdateDocumentState(doc);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
            StatusMessageText().Text(std::format(L"Reloaded {}: it was changed by another program", doc->DisplayName()));
        }
        return true;
    }

    void MainWindow::SaveAll()
    {
        auto documents = m_documents;
        for (auto const& doc : documents)
        {
            if (doc->IsDirty)
            {
                Tabs().SelectedItem(doc->Tab);
                if (!SaveDocument(doc, false))
                {
                    return;
                }
            }
        }
    }

    IAsyncOperation<bool> MainWindow::ConfirmCloseDocumentAsync(std::shared_ptr<Document> doc)
    {
        if (!doc->IsDirty)
        {
            co_return true;
        }

        auto strong = get_strong();
        Tabs().SelectedItem(doc->Tab);

        ContentDialog dialog;
        dialog.Title(box_value(hstring{ AppName }));
        dialog.Content(box_value(hstring{ L"Do you want to save changes to " + doc->DisplayName() + L"?" }));
        dialog.PrimaryButtonText(L"Save");
        dialog.SecondaryButtonText(L"Don't save");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);

        auto result = co_await ShowDialogAsync(dialog);
        if (result == ContentDialogResult::Primary)
        {
            co_return SaveDocument(doc, false);
        }
        co_return result == ContentDialogResult::Secondary;
    }

    fire_and_forget MainWindow::CloseDocumentAsync(std::shared_ptr<Document> doc)
    {
        if (doc->IsClosing)
        {
            co_return;
        }

        auto strong = get_strong();
        doc->IsClosing = true;
        bool canClose = co_await ConfirmCloseDocumentAsync(doc);
        doc->IsClosing = false;
        if (canClose)
        {
            RemoveDocument(doc);
        }
    }

    fire_and_forget MainWindow::CloseDocumentsAsync(std::vector<std::shared_ptr<Document>> docs, std::shared_ptr<Document> keep)
    {
        // Closes the documents in order, asking about unsaved changes; Cancel stops at that document.
        auto strong = get_strong();
        for (auto const& doc : docs)
        {
            if (doc->IsClosing)
            {
                continue;
            }
            doc->IsClosing = true;
            bool canClose = co_await ConfirmCloseDocumentAsync(doc);
            doc->IsClosing = false;
            if (!canClose)
            {
                co_return;
            }
            RemoveDocument(doc);
        }
        Tabs().SelectedItem(keep->Tab);
    }

    void MainWindow::OpenContainingFolder(std::wstring const& path)
    {
        // Opens Explorer with the file selected.
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
        HRESULT hr = item ? SHOpenFolderAndSelectItems(item, 0, nullptr, 0) : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        if (item)
        {
            ILFree(item);
        }
        if (FAILED(hr))
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"Couldn't open the folder containing \"{}\".\n\n{}", path, std::wstring_view{ hresult_error(hr).message() }) });
        }
    }

    void MainWindow::RemoveDocument(std::shared_ptr<Document> const& doc)
    {
        uint32_t index = 0;
        if (Tabs().TabItems().IndexOf(doc->Tab, index))
        {
            Tabs().TabItems().RemoveAt(index);
        }
        if (EditorHost().Children().IndexOf(doc->Editor, index))
        {
            EditorHost().Children().RemoveAt(index);
        }
        std::erase(m_documents, doc);
        std::erase(m_tabCycle, doc);
        EndTabCycle();
        NoteRecentTab(CurrentDocument()); // also drops the closed document

        // Like Notepad, closing the last tab closes the window. Not straight away: this can be in the middle of
        // dropping this window's last tab on another window, which closes this one itself when it's done.
        if (m_documents.empty())
        {
            m_forceClose = true;
            DispatcherQueue().TryEnqueue([self = get_strong()]
            {
                if (!self->m_closed && self->m_documents.empty())
                {
                    self->Close();
                }
            });
        }
    }
}
