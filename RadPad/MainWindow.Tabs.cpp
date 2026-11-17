#include "pch.h"
#include "MainWindowHelpers.h"
#include <fstream>
#include <winrt/Windows.Storage.Streams.h>

// The tabs: switching, the recently used order for Ctrl+Tab, and the tab's right-click menu.
namespace winrt::RadPad::implementation
{
    void MainWindow::TabMenu_Opening(IInspectable const& sender, IInspectable const&)
    {
        // The menu is shared by all tabs; Target is the tab that was right-clicked.
        auto doc = FindDocument(sender.as<MenuFlyout>().Target());
        m_tabMenuDocument = doc;
        if (!doc)
        {
            return;
        }

        // An untitled document has no file name or folder yet; a downloaded one has a name and URL but no folder.
        bool hasPath = !doc->Path.empty();
        TabCopyNameItem().IsEnabled(hasPath || !doc->Url.empty());
        TabCopyPathItem().IsEnabled(hasPath || !doc->Url.empty());
        TabOpenFolderItem().IsEnabled(hasPath);

        uint32_t index = 0;
        Tabs().TabItems().IndexOf(doc->Tab, index);
        TabCloseOthersItem().IsEnabled(m_documents.size() > 1);
        TabCloseRightItem().IsEnabled(index + 1 < Tabs().TabItems().Size());
    }

    void MainWindow::TabCopyName_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            CopyTextToClipboard(doc->DisplayName());
        }
    }

    void MainWindow::TabCopyPath_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            CopyTextToClipboard(doc->Path.empty() ? doc->Url : doc->Path);
        }
    }

    void MainWindow::TabOpenFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            OpenContainingFolder(doc->Path);
        }
    }

    void MainWindow::TabClose_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            CloseDocumentAsync(doc);
        }
    }

    void MainWindow::TabCloseOthers_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            std::vector<std::shared_ptr<Document>> others;
            std::copy_if(m_documents.begin(), m_documents.end(), std::back_inserter(others), [&](auto const& d) { return d != doc; });
            CloseDocumentsAsync(std::move(others), doc);
        }
    }

    void MainWindow::TabCloseRight_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto doc = m_tabMenuDocument.lock();
        uint32_t index = 0;
        if (!doc || !Tabs().TabItems().IndexOf(doc->Tab, index))
        {
            return;
        }

        // In tab order, which can differ from m_documents after tabs are dragged.
        std::vector<std::shared_ptr<Document>> right;
        for (uint32_t i = index + 1; i < Tabs().TabItems().Size(); ++i)
        {
            if (auto d = FindDocument(Tabs().TabItems().GetAt(i)))
            {
                right.push_back(d);
            }
        }
        CloseDocumentsAsync(std::move(right), doc);
    }

    void MainWindow::CycleRecentTabs(bool backwards)
    {
        // Like Visual Studio: Ctrl+Tab goes to the tab used before this one, and pressing Tab again while
        // Ctrl is held goes further back. The order stays fixed until Ctrl is released (EndTabCycle), so
        // repeated presses visit every tab rather than swapping between two.
        if (m_tabCycle.empty())
        {
            for (auto const& weak : m_recentTabs)
            {
                if (auto doc = weak.lock())
                {
                    m_tabCycle.push_back(doc);
                }
            }
            for (auto const& doc : m_documents) // any not yet shown, oldest last
            {
                if (std::find(m_tabCycle.begin(), m_tabCycle.end(), doc) == m_tabCycle.end())
                {
                    m_tabCycle.push_back(doc);
                }
            }
            m_tabCycleIndex = 0;
        }

        int count = static_cast<int>(m_tabCycle.size());
        if (count < 2)
        {
            m_tabCycle.clear();
            return;
        }
        m_tabCycleIndex = (m_tabCycleIndex + (backwards ? count - 1 : 1)) % count;
        Tabs().SelectedItem(m_tabCycle[m_tabCycleIndex]->Tab);
    }

    void MainWindow::EndTabCycle()
    {
        if (!m_tabCycle.empty())
        {
            m_tabCycle.clear();
            NoteRecentTab(CurrentDocument());
        }
    }

    void MainWindow::NoteRecentTab(std::shared_ptr<Document> const& doc)
    {
        // Moves doc to the front of the most-recently-used order, dropping closed documents.
        std::erase_if(m_recentTabs, [&](auto const& weak)
        {
            auto other = weak.lock();
            return !other || other == doc;
        });
        if (doc)
        {
            m_recentTabs.insert(m_recentTabs.begin(), doc);
        }
    }

    void MainWindow::SelectTabAt(int index)
    {
        if (index >= 0 && index < static_cast<int>(Tabs().TabItems().Size()))
        {
            Tabs().SelectedIndex(index);
        }
    }

    void MainWindow::Tabs_AddTabButtonClick(TabView const&, IInspectable const&)
    {
        CreateDocument();
    }

    void MainWindow::Tabs_TabCloseRequested(TabView const&, TabViewTabCloseRequestedEventArgs const& e)
    {
        if (auto doc = FindDocument(e.Tab()))
        {
            CloseDocumentAsync(doc);
        }
    }

    void MainWindow::Tabs_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        OnCurrentDocumentChanged();
    }

    void MainWindow::OnCurrentDocumentChanged()
    {
        auto current = CurrentDocument();
        if (m_tabCycle.empty()) // while cycling with Ctrl+Tab, the order changes when Ctrl is released
        {
            NoteRecentTab(current);
        }
        for (auto const& doc : m_documents)
        {
            doc->Editor.Visibility(doc == current ? Visibility::Visible : Visibility::Collapsed);
        }
        StatusMessageText().Text(L"");
        UpdateTitle();
        UpdateStatusBar();
        UpdateWhitespaceAvailability(); // depends on the tab's word wrap; also redraws the overlay
        if (current)
        {
            FocusEditor(current);
            ScheduleRepaint(current);
        }
    }

    void MainWindow::FocusEditor(std::shared_ptr<Document> const& doc)
    {
        // Deferred so a just-made-visible editor has been laid out and can take focus.
        DispatcherQueue().TryEnqueue([weakDoc = std::weak_ptr<Document>(doc)]
        {
            if (auto d = weakDoc.lock())
            {
                d->Editor.Focus(FocusState::Programmatic);
            }
        });
    }

    namespace
    {
        // Drag data for a tab: the path of the file holding it.
        constexpr wchar_t TabFormat[] = L"RadPad.Tab";
        constexpr int64_t TabFileVersion = 4;

        struct TabFileWriter
        {
            std::ofstream& Out;
            void Number(int64_t value) { Out.write(reinterpret_cast<char const*>(&value), sizeof value); }
            void Text(std::wstring_view text)
            {
                Number(static_cast<int64_t>(text.size()));
                Out.write(reinterpret_cast<char const*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
            }
        };

        struct TabFileReader
        {
            std::ifstream& In;
            int64_t Number()
            {
                int64_t value = 0;
                In.read(reinterpret_cast<char*>(&value), sizeof value);
                if (!In)
                {
                    throw std::runtime_error("truncated tab file");
                }
                return value;
            }
            std::wstring Text()
            {
                auto size = Number();
                if (size < 0 || size > INT32_MAX)
                {
                    throw std::runtime_error("bad tab file");
                }
                std::wstring text(static_cast<size_t>(size), L'\0');
                In.read(reinterpret_cast<char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
                if (!In)
                {
                    throw std::runtime_error("truncated tab file");
                }
                return text;
            }
        };

        std::wstring NewTabFilePath()
        {
            wchar_t folder[MAX_PATH + 1]{};
            GUID guid{};
            wchar_t name[40]{};
            if (!GetTempPathW(ARRAYSIZE(folder), folder) || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, name, ARRAYSIZE(name)))
            {
                return {};
            }
            return std::wstring(folder) + L"RadPad-tab-" + name + L".tmp";
        }

        bool FileExists(std::wstring const& path)
        {
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
    }

    bool MainWindow::WriteTabFile(std::wstring const& path, std::shared_ptr<Document> const& doc, RECT const& window)
    {
        // Everything about the tab: its file, text, unsaved changes, undo history and selection, and where a
        // new window for it goes.
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            TabFileWriter writer{ out };
            writer.Number(TabFileVersion);
            writer.Text(doc->Path);
            writer.Text(doc->Url);
            writer.Text(EditorText(doc->Editor));
            writer.Text(doc->SavedText);
            writer.Number(static_cast<int64_t>(doc->Encoding));
            writer.Number(static_cast<int64_t>(doc->SavedEncoding));
            writer.Number(static_cast<int64_t>(doc->Eol));
            writer.Number(static_cast<int64_t>(doc->SavedEol));
            writer.Text(syntax::LanguageName(doc->Language));
            writer.Number(doc->DeletedOnDisk);
            writer.Number(doc->DiskStamp.has_value());
            writer.Number(doc->DiskStamp ? static_cast<int64_t>(doc->DiskStamp->first) : 0);
            writer.Number(doc->DiskStamp ? static_cast<int64_t>(doc->DiskStamp->second) : 0);
            writer.Number(doc->SelectionStart);
            writer.Number(doc->SelectionEnd);
            writer.Number(doc->Settings.WordWrap);
            writer.Number(doc->Settings.Zoom);
            writer.Number(doc->Settings.InsertSpaces);
            writer.Number(doc->Settings.IndentSize);
            writer.Number(doc->Settings.TabWidth);
            writer.Number(doc->Settings.ShowWhitespace);
            writer.Number(doc->Settings.HighlightTrailingWhitespace);
            writer.Number(doc->Settings.RainbowBrackets);
            for (auto const* stack : { &doc->UndoStack, &doc->RedoStack })
            {
                writer.Number(static_cast<int64_t>(stack->size()));
                for (auto const& record : *stack)
                {
                    writer.Number(static_cast<int64_t>(record.Start));
                    writer.Text(record.Removed);
                    writer.Text(record.Inserted);
                }
            }
            writer.Number(window.left);
            writer.Number(window.top);
            writer.Number(window.right);
            writer.Number(window.bottom);
            out.close();
            if (out)
            {
                return true;
            }
        }
        DeleteFileW(path.c_str());
        return false;
    }

    std::shared_ptr<Document> MainWindow::ReadTabFile(std::wstring const& path, std::optional<uint32_t> index, RECT* window)
    {
        // Read in full first, so a bad file doesn't leave a half-made tab.
        std::wstring filePath, url, text, savedText, language;
        int64_t encoding = 0, savedEncoding = 0, eol = 0, savedEol = 0, deleted = 0, hasStamp = 0, stamp1 = 0, stamp2 = 0, selectionStart = 0, selectionEnd = 0;
        std::vector<UndoRecord> stacks[2];
        DocumentSettings settings;
        RECT position{};
        try
        {
            std::ifstream in(path, std::ios::binary);
            TabFileReader reader{ in };
            if (!in || reader.Number() != TabFileVersion)
            {
                return nullptr;
            }
            filePath = reader.Text();
            url = reader.Text();
            text = reader.Text();
            savedText = reader.Text();
            encoding = reader.Number();
            savedEncoding = reader.Number();
            eol = reader.Number();
            savedEol = reader.Number();
            language = reader.Text();
            deleted = reader.Number();
            hasStamp = reader.Number();
            stamp1 = reader.Number();
            stamp2 = reader.Number();
            selectionStart = reader.Number();
            selectionEnd = reader.Number();
            settings.WordWrap = reader.Number() != 0;
            settings.Zoom = std::clamp(static_cast<int>(reader.Number()), MinZoom, MaxZoom);
            settings.InsertSpaces = reader.Number() != 0;
            settings.IndentSize = std::clamp(static_cast<int>(reader.Number()), 1, 8);
            settings.TabWidth = std::clamp(static_cast<int>(reader.Number()), 1, 8);
            settings.ShowWhitespace = reader.Number() != 0;
            settings.HighlightTrailingWhitespace = reader.Number() != 0;
            settings.RainbowBrackets = reader.Number() != 0;
            for (auto& stack : stacks)
            {
                auto count = reader.Number();
                for (int64_t i = 0; i < count; ++i)
                {
                    UndoRecord record;
                    record.Start = static_cast<size_t>(reader.Number());
                    record.Removed = reader.Text();
                    record.Inserted = reader.Text();
                    stack.push_back(std::move(record));
                }
            }
            position.left = static_cast<LONG>(reader.Number());
            position.top = static_cast<LONG>(reader.Number());
            position.right = static_cast<LONG>(reader.Number());
            position.bottom = static_cast<LONG>(reader.Number());
        }
        catch (std::exception const&)
        {
            return nullptr;
        }

        auto doc = CreateDocument();
        doc->Settings = settings;
        ApplyEditorSettings(doc);
        doc->Path = filePath;
        doc->Url = url;
        doc->SavedText = savedText;
        doc->Encoding = static_cast<TextEncoding>(encoding);
        doc->SavedEncoding = static_cast<TextEncoding>(savedEncoding);
        doc->Eol = static_cast<textfile::LineEnding>(eol);
        doc->SavedEol = static_cast<textfile::LineEnding>(savedEol);
        doc->Language = syntax::FindLanguage(language);
        doc->DeletedOnDisk = deleted != 0;
        if (hasStamp)
        {
            doc->DiskStamp = std::pair{ static_cast<uint64_t>(stamp1), static_cast<uint64_t>(stamp2) };
        }
        doc->Editor.Document().SetText(TextSetOptions::None, hstring{ text });
        UpdateHighlighting(doc, true);
        doc->UndoStack = std::move(stacks[0]); // after highlighting, which records setting the text as an edit
        doc->RedoStack = std::move(stacks[1]);
        doc->StartNewUndoRecord = true;
        Select(doc->Editor, static_cast<int>(selectionStart), static_cast<int>(selectionEnd - selectionStart));
        UpdateReadOnly(doc);
        UpdateDocumentState(doc);

        // CreateDocument added the tab at the end; put it where it was dropped.
        auto items = Tabs().TabItems();
        uint32_t at = 0;
        if (index && items.IndexOf(doc->Tab, at) && *index < at)
        {
            items.RemoveAt(at);
            items.InsertAt(*index, doc->Tab);
        }
        Tabs().SelectedItem(doc->Tab);
        UpdateStatusBar();
        if (window)
        {
            *window = position;
        }
        return doc;
    }

    void MainWindow::OpenStartupTab(std::wstring const& file)
    {
        // Started by another RadPad window for a tab dragged out of it. Deleting the file tells that window
        // the tab has been taken, so it can close its copy.
        RECT window{};
        if (!ReadTabFile(file, std::nullopt, &window))
        {
            ShowMessageAsync(hstring{ AppName }, L"Couldn't open the tab dragged out of another RadPad window. It's still in that window.");
            return;
        }
        DeleteFileW(file.c_str());
        if (window.right > window.left && window.bottom > window.top)
        {
            AppWindow().MoveAndResize({ window.left, window.top, window.right - window.left, window.bottom - window.top });
        }
    }

    void MainWindow::Tabs_TabDragStarting(TabView const&, TabViewTabDragStartingEventArgs const& e)
    {
        // The tab under the pointer. Tab and Item in the event arguments aren't reliably the dragged tab in
        // Windows App SDK 1.7, so they're only a fallback.
        std::shared_ptr<Document> doc;
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(WindowHandle(), &cursor))
        {
            auto scale = Tabs().XamlRoot().RasterizationScale();
            float x = static_cast<float>(cursor.x / scale);
            float y = static_cast<float>(cursor.y / scale);
            for (auto const& d : m_documents)
            {
                auto bounds = d->Tab.TransformToVisual(nullptr).TransformBounds({ 0, 0, static_cast<float>(d->Tab.ActualWidth()), static_cast<float>(d->Tab.ActualHeight()) });
                if (x >= bounds.X && x < bounds.X + bounds.Width && y >= bounds.Y - 20 && y < bounds.Y + bounds.Height + 20)
                {
                    doc = d;
                    break;
                }
            }
        }
        if (!doc)
        {
            doc = FindDocument(e.Item());
        }
        if (!doc)
        {
            doc = FindDocument(e.Tab());
        }
        if (doc && (doc->IsClosing || doc->AskingToReload))
        {
            e.Cancel(true);
            return;
        }

        // The tab goes into a file now, as the window it's dropped on may be in another process. Without one
        // the drag still reorders tabs in this window.
        auto file = doc ? NewTabFilePath() : std::wstring{};
        if (file.empty() || !WriteTabFile(file, doc, RECT{}))
        {
            return;
        }
        m_draggedDocument = doc;
        m_draggedTabFile = file;
        e.Data().SetData(TabFormat, box_value(hstring{ file }));
        e.Data().RequestedOperation(DataPackageOperation::Move);
    }

    void MainWindow::Tabs_TabStripDragOver(IInspectable const&, DragEventArgs const& e)
    {
        // A tab from another RadPad window. A tab from this one is left to TabView, which reorders it.
        if (e.DataView().Contains(TabFormat) && !m_draggedDocument.lock())
        {
            e.AcceptedOperation(DataPackageOperation::Move);
            e.DragUIOverride().Caption(L"Move tab here");
        }
    }

    fire_and_forget MainWindow::Tabs_TabStripDrop(IInspectable const&, DragEventArgs e)
    {
        if (!e.DataView().Contains(TabFormat) || m_draggedDocument.lock())
        {
            co_return;
        }
        auto strong = get_strong();

        // Before the first tab whose middle is right of the pointer.
        auto items = Tabs().TabItems();
        uint32_t index = items.Size();
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            auto item = items.GetAt(i).try_as<TabViewItem>();
            if (item && e.GetPosition(item).X < item.ActualWidth() / 2)
            {
                index = i;
                break;
            }
        }

        // The other window closes its tab when it sees the file has gone, so it's deleted only once the
        // tab is open here.
        auto deferral = e.GetDeferral();
        e.AcceptedOperation(DataPackageOperation::None);
        std::wstring file;
        try
        {
            auto data = co_await e.DataView().GetDataAsync(TabFormat);
            if (auto text = data.try_as<IReference<hstring>>())
            {
                file = text.Value();
            }
            else if (auto stream = data.try_as<Windows::Storage::Streams::IRandomAccessStream>())
            {
                Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
                auto size = static_cast<uint32_t>(stream.Size());
                co_await reader.LoadAsync(size);
                reader.UnicodeEncoding(Windows::Storage::Streams::UnicodeEncoding::Utf16LE);
                file = reader.ReadString(size / sizeof(wchar_t));
                file.erase(std::find(file.begin(), file.end(), L'\0'), file.end());
            }
        }
        catch (hresult_error const&)
        {
        }
        if (!file.empty() && ReadTabFile(file, index, nullptr))
        {
            DeleteFileW(file.c_str());
            e.AcceptedOperation(DataPackageOperation::Move);
            Activate();
        }
        deferral.Complete();
    }

    void MainWindow::Tabs_TabDroppedOutside(TabView const&, TabViewTabDroppedOutsideEventArgs const&)
    {
        // The tab noted when the drag started, as the one in the event arguments can be another one.
        auto doc = m_draggedDocument.lock();
        if (!doc || m_draggedTabFile.empty())
        {
            return;
        }

        // Dropped on this window (its editor, say) also opens the tab in a new window, unless it's the only
        // tab, when that window would just replace this one.
        POINT cursor{};
        RECT own{};
        GetCursorPos(&cursor);
        GetWindowRect(WindowHandle(), &own);
        if (PtInRect(&own, cursor) && m_documents.size() == 1)
        {
            return;
        }

        // A new RadPad process for the tab, in a window the size of this one with the tab under the pointer.
        auto file = std::exchange(m_draggedTabFile, {});
        LONG width = own.right - own.left;
        LONG height = own.bottom - own.top;
        RECT window{ cursor.x - 80, cursor.y - 20, cursor.x - 80 + width, cursor.y - 20 + height };
        if (WriteTabFile(file, doc, window))
        {
            MoveTabToNewProcessAsync(doc, file);
        }
    }

    void MainWindow::Tabs_TabDragCompleted(TabView const&, TabViewTabDragCompletedEventArgs const&)
    {
        // Afterwards, as TabDroppedOutside may come after this and take the file.
        DispatcherQueue().TryEnqueue([this, strong = get_strong()]
        {
            auto doc = std::exchange(m_draggedDocument, {}).lock();
            auto file = std::exchange(m_draggedTabFile, {});
            if (file.empty())
            {
                return; // a new process is taking it
            }
            if (!FileExists(file) && doc)
            {
                RemoveDocument(doc); // another window took it
            }
            else
            {
                DeleteFileW(file.c_str()); // dropped back here, or nowhere that took it
            }
        });
    }

    fire_and_forget MainWindow::MoveTabToNewProcessAsync(std::shared_ptr<Document> doc, std::wstring file)
    {
        // The tab stays here, read-only so no edit is lost, until the new process has opened it and deleted
        // the file.
        auto strong = get_strong();
        apartment_context ui;
        doc->IsClosing = true; // no closing or reload questions for it meanwhile
        doc->Editor.IsReadOnly(true);

        std::wstring exe = instance::IsPackaged() ? ExecutionAliasPath() : ExePath();
        std::wstring parameters = std::wstring(instance::TabArgument) + L" \"" + file + L"\"";
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.fMask = SEE_MASK_NOASYNC;
        info.lpFile = exe.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        bool started = ShellExecuteExW(&info);

        bool taken = false;
        for (int i = 0; started && i < 300 && !taken; ++i) // up to 30 seconds
        {
            co_await resume_after(std::chrono::milliseconds(100));
            taken = !FileExists(file);
        }
        co_await ui;

        doc->IsClosing = false;
        UpdateReadOnly(doc);
        if (taken)
        {
            RemoveDocument(doc);
        }
        else
        {
            DeleteFileW(file.c_str());
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"Couldn't open \"{}\" in a new window.", doc->DisplayName()) });
        }
    }
}
