#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::System;
using textfile::LineEnding;
using textfile::TextEncoding;

namespace
{
    constexpr std::wstring_view AppName = L"MyNotepad";
    constexpr int DefaultZoom = 100;
    constexpr int MinZoom = 10;
    constexpr int MaxZoom = 500;
    constexpr int ZoomStep = 10;
    constexpr double DefaultFontPoints = 11.0;

    bool IsKeyDown(int virtualKey)
    {
        return (GetKeyState(virtualKey) & 0x8000) != 0;
    }

    bool PathsEqual(std::wstring const& a, std::wstring const& b)
    {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    std::wstring FullPath(std::wstring const& path)
    {
        DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (length == 0)
        {
            return path;
        }
        std::wstring result(length, L'\0');
        length = GetFullPathNameW(path.c_str(), length, result.data(), nullptr);
        result.resize(length);
        return result;
    }

    // Lowercases in place; CharLowerBuff never changes the length, so indices stay aligned with the original.
    std::wstring ToLower(std::wstring text)
    {
        if (!text.empty())
        {
            CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        }
        return text;
    }

    int TagIndex(IInspectable const& sender)
    {
        return _wtoi(unbox_value<hstring>(sender.as<FrameworkElement>().Tag()).c_str());
    }

    bool IsChecked(Primitives::ToggleButton const& toggle)
    {
        auto value = toggle.IsChecked();
        return value && value.Value();
    }

    std::wstring CurrentTimeDate()
    {
        wchar_t time[64]{};
        wchar_t date[64]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, ARRAYSIZE(time));
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, ARRAYSIZE(date), nullptr);
        return std::wstring(time) + L" " + date;
    }

    // Line and column (both 1-based) of a character index. TextBox text uses '\r' for every line break.
    std::pair<int, int> LineColumnAt(std::wstring_view text, int index)
    {
        int line = 1;
        int column = 1;
        int end = std::min(index, static_cast<int>(text.size()));
        for (int i = 0; i < end; ++i)
        {
            if (text[i] == L'\r')
            {
                ++line;
                column = 1;
            }
            else
            {
                ++column;
            }
        }
        return { line, column };
    }

    int LineCount(std::wstring_view text)
    {
        return 1 + static_cast<int>(std::count(text.begin(), text.end(), L'\r'));
    }

    int IndexOfLine(std::wstring_view text, int line)
    {
        int current = 1;
        for (size_t i = 0; i < text.size() && current < line; ++i)
        {
            if (text[i] == L'\r' && ++current == line)
            {
                return static_cast<int>(i + 1);
            }
        }
        return line <= 1 ? 0 : static_cast<int>(text.size());
    }

    std::wstring SettingsFilePath()
    {
        PWSTR folder = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        {
            return {};
        }
        std::wstring directory = std::wstring(folder) + L"\\MyNotepad";
        CoTaskMemFree(folder);
        CreateDirectoryW(directory.c_str(), nullptr);
        return directory + L"\\settings.ini";
    }

    std::vector<std::wstring> GetSystemFontFamilies()
    {
        std::vector<std::wstring> families;

        com_ptr<IDWriteFactory> factory;
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown**>(factory.put())));
        com_ptr<IDWriteFontCollection> collection;
        check_hresult(factory->GetSystemFontCollection(collection.put()));

        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

        for (UINT32 i = 0, count = collection->GetFontFamilyCount(); i < count; ++i)
        {
            com_ptr<IDWriteFontFamily> family;
            com_ptr<IDWriteLocalizedStrings> names;
            if (FAILED(collection->GetFontFamily(i, family.put())) || FAILED(family->GetFamilyNames(names.put())))
            {
                continue;
            }

            UINT32 index = 0;
            BOOL exists = FALSE;
            names->FindLocaleName(locale, &index, &exists);
            if (!exists)
            {
                names->FindLocaleName(L"en-us", &index, &exists);
            }
            if (!exists)
            {
                index = 0;
            }

            UINT32 length = 0;
            if (FAILED(names->GetStringLength(index, &length)))
            {
                continue;
            }
            std::wstring name(length + 1, L'\0');
            if (SUCCEEDED(names->GetString(index, name.data(), length + 1)))
            {
                name.resize(length);
                families.push_back(std::move(name));
            }
        }

        std::sort(families.begin(), families.end(), [](auto const& a, auto const& b)
        {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        families.erase(std::unique(families.begin(), families.end()), families.end());
        return families;
    }

    const COMDLG_FILTERSPEC FileTypes[] =
    {
        { L"Text Documents (*.txt)", L"*.txt" },
        { L"All Files (*.*)", L"*.*" },
    };

    std::vector<std::wstring> ShowOpenFileDialog(HWND owner)
    {
        std::vector<std::wstring> paths;

        auto dialog = create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
        check_hresult(dialog->SetFileTypes(ARRAYSIZE(FileTypes), FileTypes));
        FILEOPENDIALOGOPTIONS options{};
        check_hresult(dialog->GetOptions(&options));
        check_hresult(dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM));

        HRESULT hr = dialog->Show(owner);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return paths;
        }
        check_hresult(hr);

        com_ptr<IShellItemArray> items;
        check_hresult(dialog->GetResults(items.put()));
        DWORD count = 0;
        check_hresult(items->GetCount(&count));
        for (DWORD i = 0; i < count; ++i)
        {
            com_ptr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                paths.emplace_back(path);
                CoTaskMemFree(path);
            }
        }
        return paths;
    }

    struct SaveDialogResult
    {
        std::wstring Path;
        TextEncoding Encoding;
    };

    // Shows the Save As dialog with an Encoding drop-down, like Notepad's.
    std::optional<SaveDialogResult> ShowSaveFileDialog(HWND owner, std::wstring const& currentPath, std::wstring const& suggestedName, TextEncoding encoding)
    {
        constexpr DWORD EncodingGroupId = 100;
        constexpr DWORD EncodingComboId = 101;

        auto dialog = create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
        check_hresult(dialog->SetFileTypes(ARRAYSIZE(FileTypes), FileTypes));
        check_hresult(dialog->SetDefaultExtension(L"txt"));
        check_hresult(dialog->SetFileName(suggestedName.c_str()));

        if (auto slash = currentPath.find_last_of(L"\\/"); slash != std::wstring::npos)
        {
            com_ptr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(currentPath.substr(0, slash).c_str(), nullptr, __uuidof(IShellItem), folder.put_void())))
            {
                dialog->SetFolder(folder.get());
            }
        }

        auto customize = dialog.as<IFileDialogCustomize>();
        check_hresult(customize->StartVisualGroup(EncodingGroupId, L"Encoding:"));
        check_hresult(customize->AddComboBox(EncodingComboId));
        for (int i = 0; i < textfile::EncodingCount; ++i)
        {
            check_hresult(customize->AddControlItem(EncodingComboId, i, textfile::EncodingName(static_cast<TextEncoding>(i)).data()));
        }
        check_hresult(customize->SetSelectedControlItem(EncodingComboId, static_cast<DWORD>(encoding)));
        check_hresult(customize->EndVisualGroup());

        HRESULT hr = dialog->Show(owner);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return std::nullopt;
        }
        check_hresult(hr);

        com_ptr<IShellItem> item;
        check_hresult(dialog->GetResult(item.put()));
        PWSTR path = nullptr;
        check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        SaveDialogResult result{ path, encoding };
        CoTaskMemFree(path);

        DWORD selected = 0;
        if (SUCCEEDED(customize->GetSelectedControlItem(EncodingComboId, &selected)) && selected < textfile::EncodingCount)
        {
            result.Encoding = static_cast<TextEncoding>(selected);
        }
        return result;
    }
}

namespace winrt::MyNotepad::implementation
{
    std::wstring Document::DisplayName() const
    {
        if (Path.empty())
        {
            return L"Untitled";
        }
        auto slash = Path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? Path : Path.substr(slash + 1);
    }

    void MainWindow::InitializeComponent()
    {
        MainWindowT::InitializeComponent();

        // Draw the tab strip in the title bar; only the empty area after the tabs drags the window.
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(TitleBarDragRegion());

        auto appWindow = AppWindow();
        auto titleBar = appWindow.TitleBar();
        titleBar.PreferredHeightOption(TitleBarHeightOption::Tall);
        titleBar.ButtonBackgroundColor(Colors::Transparent());
        titleBar.ButtonInactiveBackgroundColor(Colors::Transparent());

        UINT dpi = GetDpiForWindow(WindowHandle());
        appWindow.Resize({ MulDiv(1100, dpi, 96), MulDiv(760, dpi, 96) });

        appWindow.Closing([this](auto&&, AppWindowClosingEventArgs const& args) { OnAppWindowClosing(args); });
        Closed([this](auto&&, auto&&) { SaveSettings(); });

        LoadSettings();
        WordWrapItem().IsChecked(m_wordWrap);
        StatusBarItem().IsChecked(m_statusBarVisible);
        StatusBar().Visibility(m_statusBarVisible ? Visibility::Visible : Visibility::Collapsed);
        AddZoomAccelerators();

        // handledEventsToo: the TextBox handles drag events itself, but we still want file drops anywhere.
        RootGrid().AddHandler(UIElement::DragOverEvent(), box_value(DragEventHandler{ this, &MainWindow::OnDragOver }), true);
        RootGrid().AddHandler(UIElement::DropEvent(), box_value(DragEventHandler{ [this](IInspectable const&, DragEventArgs const& e) { OnDropAsync(e); } }), true);
    }

    HWND MainWindow::WindowHandle()
    {
        HWND hwnd{};
        check_hresult(m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
        return hwnd;
    }

    void MainWindow::RootGrid_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded)
        {
            return;
        }
        m_loaded = true;

        // Open any files passed on the command line, e.g. "MyNotepad.exe a.txt b.txt".
        int argc = 0;
        if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; ++i)
            {
                OpenFile(argv[i]);
            }
            LocalFree(argv);
        }

        if (m_documents.empty())
        {
            CreateDocument();
        }
    }

    void MainWindow::RootGrid_PreviewKeyDown(IInspectable const&, KeyRoutedEventArgs const& e)
    {
        auto key = e.Key();
        bool ctrl = IsKeyDown(VK_CONTROL);
        bool shift = IsKeyDown(VK_SHIFT);
        bool alt = IsKeyDown(VK_MENU);

        if (key == VirtualKey::Escape && !ctrl && !alt && FindPanel().Visibility() == Visibility::Visible)
        {
            HideFindPanel();
            e.Handled(true);
            return;
        }

        if (!ctrl || alt)
        {
            return;
        }

        if (key == VirtualKey::Tab)
        {
            SelectAdjacentTab(shift ? -1 : 1);
            e.Handled(true);
        }
        else if ((key == VirtualKey::W || key == VirtualKey::F4) && !shift)
        {
            if (auto doc = CurrentDocument())
            {
                CloseDocumentAsync(doc);
            }
            e.Handled(true);
        }
        else if (key >= VirtualKey::Number1 && key <= VirtualKey::Number9 && !shift)
        {
            // Ctrl+1..8 select that tab, Ctrl+9 selects the last tab.
            int index = static_cast<int>(key) - static_cast<int>(VirtualKey::Number1);
            SelectTabAt(key == VirtualKey::Number9 ? static_cast<int>(m_documents.size()) - 1 : index);
            e.Handled(true);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Documents and tabs
    // ---------------------------------------------------------------------------------------------

    std::shared_ptr<Document> MainWindow::CreateDocument()
    {
        auto doc = std::make_shared<Document>();
        std::weak_ptr<Document> weakDoc = doc;

        TextBox editor;
        editor.AcceptsReturn(true);
        editor.IsSpellCheckEnabled(false);
        editor.IsTextPredictionEnabled(false);
        editor.BorderThickness(Thickness{ 0, 0, 0, 0 });
        editor.CornerRadius(Microsoft::UI::Xaml::CornerRadius{ 0, 0, 0, 0 });
        editor.Padding(Thickness{ 12, 8, 12, 8 });
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
        ApplyEditorSettings(editor);

        editor.TextChanged([this, weakDoc](auto&&, auto&&)
        {
            if (auto d = weakDoc.lock())
            {
                UpdateDocumentState(d);
                if (d == CurrentDocument())
                {
                    UpdateCaretPosition();
                }
            }
        });
        editor.SelectionChanged([this, weakDoc](auto&&, auto&&)
        {
            if (auto d = weakDoc.lock(); d && d == CurrentDocument())
            {
                UpdateCaretPosition();
            }
        });
        editor.PreviewKeyDown([this](IInspectable const& sender, KeyRoutedEventArgs const& e)
        {
            // TextBox has no AcceptsTab; insert tabs ourselves instead of moving focus.
            if (e.Key() == VirtualKey::Tab && !IsKeyDown(VK_CONTROL) && !IsKeyDown(VK_SHIFT) && !IsKeyDown(VK_MENU))
            {
                InsertText(sender.as<TextBox>(), L"\t");
                e.Handled(true);
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

        TabViewItem tab;
        FontIconSource icon;
        icon.Glyph(L"");
        tab.IconSource(icon);

        doc->Editor = editor;
        doc->Tab = tab;
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

        // Reuse the current tab if it's an untouched blank document, as Notepad does.
        auto doc = CurrentDocument();
        if (!doc || !doc->Path.empty() || doc->IsDirty || !doc->Editor.Text().empty())
        {
            doc = CreateDocument();
        }

        doc->Path = fullPath;
        doc->Encoding = doc->SavedEncoding = loaded.Encoding;
        doc->Eol = doc->SavedEol = loaded.Eol;
        doc->Editor.Text(loaded.Text);
        doc->SavedText = doc->Editor.Text();
        doc->Editor.Select(0, 0);
        doc->Editor.ClearUndoRedoHistory();
        UpdateDocumentState(doc);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
        SHAddToRecentDocs(SHARD_PATHW, fullPath.c_str());
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
                auto suggested = doc->Path.empty() ? std::wstring{ L"Untitled.txt" } : doc->DisplayName();
                auto result = ShowSaveFileDialog(WindowHandle(), doc->Path, suggested, doc->Encoding);
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

        hstring text = doc->Editor.Text();
        try
        {
            textfile::Save(path, text, encoding, doc->Eol);
        }
        catch (hresult_error const& e)
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"Couldn't save \"{}\".\n\n{}", path, std::wstring_view{ e.message() }) });
            return false;
        }

        doc->Path = path;
        doc->Encoding = doc->SavedEncoding = encoding;
        doc->SavedEol = doc->Eol;
        doc->SavedText = text;
        UpdateDocumentState(doc);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
        SHAddToRecentDocs(SHARD_PATHW, path.c_str());
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

        // Like Notepad, closing the last tab closes the window.
        if (m_documents.empty())
        {
            m_forceClose = true;
            Close();
        }
    }

    void MainWindow::SelectAdjacentTab(int delta)
    {
        int count = static_cast<int>(Tabs().TabItems().Size());
        if (count > 1)
        {
            SelectTabAt((Tabs().SelectedIndex() + delta + count) % count);
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
        for (auto const& doc : m_documents)
        {
            doc->Editor.Visibility(doc == current ? Visibility::Visible : Visibility::Collapsed);
        }
        StatusMessageText().Text(L"");
        UpdateTitle();
        UpdateStatusBar();
        if (current)
        {
            FocusEditor(current);
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

    // ---------------------------------------------------------------------------------------------
    // Window lifetime
    // ---------------------------------------------------------------------------------------------

    void MainWindow::OnAppWindowClosing(AppWindowClosingEventArgs const& args)
    {
        if (m_forceClose || std::none_of(m_documents.begin(), m_documents.end(), [](auto const& d) { return d->IsDirty; }))
        {
            return;
        }
        args.Cancel(true);
        TryCloseWindowAsync();
    }

    fire_and_forget MainWindow::TryCloseWindowAsync()
    {
        if (m_closePending)
        {
            co_return;
        }

        auto strong = get_strong();
        m_closePending = true;
        auto documents = m_documents;
        for (auto const& doc : documents)
        {
            if (!co_await ConfirmCloseDocumentAsync(doc))
            {
                m_closePending = false;
                co_return;
            }
        }
        m_forceClose = true;
        Close();
    }

    // ---------------------------------------------------------------------------------------------
    // Commands
    // ---------------------------------------------------------------------------------------------

    void MainWindow::Command_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        ExecuteCommand(unbox_value<hstring>(sender.as<FrameworkElement>().Tag()));
    }

    void MainWindow::ExecuteCommand(std::wstring_view command)
    {
        auto doc = CurrentDocument();

        if (command == L"NewTab") { CreateDocument(); return; }
        if (command == L"Open") { OpenWithDialog(); return; }
        if (command == L"SaveAll") { SaveAll(); return; }
        if (command == L"Exit") { TryCloseWindowAsync(); return; }
        if (command == L"Find") { ShowFindPanel(false); return; }
        if (command == L"Replace") { ShowFindPanel(true); return; }
        if (command == L"CloseFind") { HideFindPanel(); return; }
        if (command == L"FindNext") { FindNext(true); return; }
        if (command == L"FindPrevious") { FindNext(false); return; }
        if (command == L"ReplaceOne") { ReplaceOne(); return; }
        if (command == L"ReplaceAll") { ReplaceAll(); return; }
        if (command == L"GoTo") { GoToLineAsync(); return; }
        if (command == L"Font") { ChooseFontAsync(); return; }
        if (command == L"About") { ShowAboutAsync(); return; }
        if (command == L"ZoomIn") { SetZoom(m_zoom + ZoomStep); return; }
        if (command == L"ZoomOut") { SetZoom(m_zoom - ZoomStep); return; }
        if (command == L"ZoomReset") { SetZoom(DefaultZoom); return; }

        if (command == L"WordWrap")
        {
            m_wordWrap = WordWrapItem().IsChecked();
            ApplyEditorSettingsToAll();
            return;
        }
        if (command == L"StatusBar")
        {
            m_statusBarVisible = StatusBarItem().IsChecked();
            StatusBar().Visibility(m_statusBarVisible ? Visibility::Visible : Visibility::Collapsed);
            return;
        }

        if (!doc)
        {
            return;
        }

        auto editor = doc->Editor;
        if (command == L"Save") { SaveDocument(doc, false); return; }
        if (command == L"SaveAs") { SaveDocument(doc, true); return; }
        if (command == L"CloseTab") { CloseDocumentAsync(doc); return; }

        if (command == L"Undo") { editor.Undo(); }
        else if (command == L"Redo") { editor.Redo(); }
        else if (command == L"Cut") { editor.CutSelectionToClipboard(); }
        else if (command == L"Copy") { editor.CopySelectionToClipboard(); }
        else if (command == L"Paste") { editor.PasteFromClipboard(); }
        else if (command == L"Delete") { if (editor.SelectionLength() > 0) { editor.SelectedText(L""); } }
        else if (command == L"SelectAll") { editor.SelectAll(); }
        else if (command == L"TimeDate") { InsertText(editor, CurrentTimeDate()); }
        FocusEditor(doc);
    }

    void MainWindow::Encoding_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            doc->Encoding = static_cast<TextEncoding>(TagIndex(sender));
            UpdateDocumentState(doc);
            UpdateStatusBar();
        }
    }

    void MainWindow::LineEnding_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            doc->Eol = static_cast<LineEnding>(TagIndex(sender));
            UpdateDocumentState(doc);
            UpdateStatusBar();
        }
    }

    void MainWindow::InsertText(TextBox const& editor, std::wstring_view text)
    {
        // Replacing SelectedText keeps the change on the TextBox undo stack.
        int start = editor.SelectionStart();
        editor.SelectedText(text);
        editor.Select(start + static_cast<int>(text.size()), 0);
    }

    // ---------------------------------------------------------------------------------------------
    // UI state
    // ---------------------------------------------------------------------------------------------

    void MainWindow::UpdateDocumentState(std::shared_ptr<Document> const& doc)
    {
        hstring text = doc->Editor.Text();
        doc->IsDirty = std::wstring_view{ text } != doc->SavedText || doc->Encoding != doc->SavedEncoding || doc->Eol != doc->SavedEol;

        auto name = doc->DisplayName();
        auto header = doc->IsDirty ? name + L" •" : name;
        if (header != doc->Header)
        {
            doc->Header = header;
            doc->Tab.Header(box_value(hstring{ header }));
            ToolTipService::SetToolTip(doc->Tab, box_value(hstring{ doc->Path.empty() ? name : doc->Path }));
            if (doc == CurrentDocument())
            {
                UpdateTitle();
            }
        }
    }

    void MainWindow::UpdateTitle()
    {
        std::wstring title{ AppName };
        if (auto doc = CurrentDocument())
        {
            title = (doc->IsDirty ? L"*" : L"") + doc->DisplayName() + L" - " + title;
        }
        Title(title);
    }

    void MainWindow::UpdateStatusBar()
    {
        ZoomText().Text(std::format(L"{}%", m_zoom));

        auto doc = CurrentDocument();
        if (!doc)
        {
            return;
        }

        LineEndingButton().Content(box_value(hstring{ textfile::LineEndingName(doc->Eol) }));
        EncodingButton().Content(box_value(hstring{ textfile::EncodingName(doc->Encoding) }));
        for (auto const& item : LineEndingFlyout().Items())
        {
            if (auto radio = item.try_as<RadioMenuFlyoutItem>())
            {
                radio.IsChecked(TagIndex(radio) == static_cast<int>(doc->Eol));
            }
        }
        for (auto const& item : EncodingFlyout().Items())
        {
            if (auto radio = item.try_as<RadioMenuFlyoutItem>())
            {
                radio.IsChecked(TagIndex(radio) == static_cast<int>(doc->Encoding));
            }
        }
        UpdateCaretPosition();
    }

    void MainWindow::UpdateCaretPosition()
    {
        auto doc = CurrentDocument();
        if (!doc)
        {
            return;
        }

        hstring text = doc->Editor.Text();
        auto [line, column] = LineColumnAt(text, doc->Editor.SelectionStart());
        CaretPositionText().Text(std::format(L"Ln {}, Col {}", line, column));

        int selected = doc->Editor.SelectionLength();
        CharacterCountText().Text(selected > 0
            ? std::format(L"{} of {} characters", selected, text.size())
            : std::format(L"{} characters", text.size()));
    }

    void MainWindow::ApplyEditorSettings(TextBox const& editor)
    {
        editor.FontFamily(Media::FontFamily{ m_fontFamily });
        editor.FontSize(m_fontPoints * 96.0 / 72.0 * m_zoom / 100.0);
        editor.FontWeight(m_fontBold ? Microsoft::UI::Text::FontWeights::Bold() : Microsoft::UI::Text::FontWeights::Normal());
        editor.FontStyle(m_fontItalic ? Windows::UI::Text::FontStyle::Italic : Windows::UI::Text::FontStyle::Normal);
        editor.TextWrapping(m_wordWrap ? TextWrapping::Wrap : TextWrapping::NoWrap);
    }

    void MainWindow::ApplyEditorSettingsToAll()
    {
        for (auto const& doc : m_documents)
        {
            ApplyEditorSettings(doc->Editor);
        }
    }

    void MainWindow::SetZoom(int zoom)
    {
        m_zoom = std::clamp(zoom, MinZoom, MaxZoom);
        ApplyEditorSettingsToAll();
        ZoomText().Text(std::format(L"{}%", m_zoom));
    }

    void MainWindow::AddZoomAccelerators()
    {
        auto add = [](MenuFlyoutItem const& item, VirtualKey key)
        {
            KeyboardAccelerator accelerator;
            accelerator.Key(key);
            accelerator.Modifiers(VirtualKeyModifiers::Control);
            item.KeyboardAccelerators().Append(accelerator);
        };
        add(ZoomInItem(), static_cast<VirtualKey>(VK_OEM_PLUS));
        add(ZoomInItem(), VirtualKey::Add);
        add(ZoomOutItem(), static_cast<VirtualKey>(VK_OEM_MINUS));
        add(ZoomOutItem(), VirtualKey::Subtract);
        add(ZoomResetItem(), VirtualKey::Number0);
        add(ZoomResetItem(), VirtualKey::NumberPad0);
    }

    // ---------------------------------------------------------------------------------------------
    // Find and replace
    // ---------------------------------------------------------------------------------------------

    void MainWindow::ShowFindPanel(bool showReplace)
    {
        FindPanel().Visibility(Visibility::Visible);
        ReplaceRow().Visibility(showReplace ? Visibility::Visible : Visibility::Collapsed);
        StatusMessageText().Text(L"");

        if (auto doc = CurrentDocument())
        {
            hstring selected = doc->Editor.SelectedText();
            if (!selected.empty() && std::wstring_view{ selected }.find(L'\r') == std::wstring_view::npos)
            {
                FindBox().Text(selected);
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

        std::wstring haystack{ doc->Editor.Text() };
        if (!IsChecked(MatchCaseCheck()))
        {
            haystack = ToLower(std::move(haystack));
            needle = ToLower(std::move(needle));
        }

        bool wrap = IsChecked(WrapAroundCheck());
        size_t start = static_cast<size_t>(doc->Editor.SelectionStart());
        size_t length = static_cast<size_t>(doc->Editor.SelectionLength());
        size_t pos;
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

        if (pos == std::wstring::npos)
        {
            StatusMessageText().Text(std::format(L"Cannot find \"{}\"", std::wstring_view{ FindBox().Text() }));
            return false;
        }

        StatusMessageText().Text(L"");
        bool returnFocusToFindBox = IsFocusWithin(FindPanel());
        doc->Editor.Select(static_cast<int>(pos), static_cast<int>(needle.size()));
        doc->Editor.Focus(FocusState::Programmatic); // scrolls the match into view
        if (returnFocusToFindBox)
        {
            DispatcherQueue().TryEnqueue([this, strong = get_strong()] { FindBox().Focus(FocusState::Programmatic); });
        }
        return true;
    }

    void MainWindow::ReplaceOne()
    {
        auto doc = CurrentDocument();
        std::wstring needle{ FindBox().Text() };
        if (!doc || needle.empty())
        {
            return;
        }

        std::wstring selected{ doc->Editor.SelectedText() };
        bool matches = IsChecked(MatchCaseCheck()) ? selected == needle : ToLower(selected) == ToLower(needle);
        if (matches)
        {
            InsertText(doc->Editor, ReplaceBox().Text());
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

        bool matchCase = IsChecked(MatchCaseCheck());
        std::wstring text{ doc->Editor.Text() };
        std::wstring searchText = matchCase ? text : ToLower(text);
        std::wstring searchKey = matchCase ? needle : ToLower(needle);
        std::wstring replacement{ ReplaceBox().Text() };

        std::wstring result;
        size_t count = 0;
        size_t last = 0;
        for (size_t pos; (pos = searchText.find(searchKey, last)) != std::wstring::npos; last = pos + searchKey.size())
        {
            result.append(text, last, pos - last);
            result += replacement;
            ++count;
        }

        if (count == 0)
        {
            StatusMessageText().Text(std::format(L"Cannot find \"{}\"", needle));
            return;
        }
        result.append(text, last);

        // Replace via the selection so the whole operation is a single undo step.
        doc->Editor.SelectAll();
        doc->Editor.SelectedText(result);
        doc->Editor.Select(0, 0);
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

    // ---------------------------------------------------------------------------------------------
    // Dialogs
    // ---------------------------------------------------------------------------------------------

    IAsyncOperation<ContentDialogResult> MainWindow::ShowDialogAsync(ContentDialog dialog)
    {
        // Only one ContentDialog can be open per window.
        if (m_dialogOpen)
        {
            co_return ContentDialogResult::None;
        }

        auto strong = get_strong();
        m_dialogOpen = true;
        dialog.XamlRoot(Content().XamlRoot());
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Microsoft::UI::Xaml::Style>());
        dialog.RequestedTheme(RootGrid().ActualTheme());

        auto result = ContentDialogResult::None;
        try
        {
            result = co_await dialog.ShowAsync();
        }
        catch (hresult_error const&)
        {
        }
        m_dialogOpen = false;
        co_return result;
    }

    fire_and_forget MainWindow::ShowMessageAsync(hstring title, hstring message)
    {
        auto strong = get_strong();
        ContentDialog dialog;
        dialog.Title(box_value(title));
        TextBlock content;
        content.Text(message);
        content.TextWrapping(TextWrapping::Wrap);
        dialog.Content(content);
        dialog.CloseButtonText(L"OK");
        dialog.DefaultButton(ContentDialogButton::Close);
        co_await ShowDialogAsync(dialog);
    }

    fire_and_forget MainWindow::GoToLineAsync()
    {
        auto doc = CurrentDocument();
        if (!doc)
        {
            co_return;
        }

        auto strong = get_strong();
        hstring text = doc->Editor.Text();
        int lineCount = LineCount(text);
        int currentLine = LineColumnAt(text, doc->Editor.SelectionStart()).first;

        NumberBox lineBox;
        lineBox.Header(box_value(L"Line number"));
        lineBox.Minimum(1);
        lineBox.Maximum(lineCount);
        lineBox.Value(currentLine);
        lineBox.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
        lineBox.Loaded([weakBox = make_weak(lineBox)](auto&&, auto&&)
        {
            if (auto box = weakBox.get())
            {
                box.Focus(FocusState::Programmatic);
            }
        });

        ContentDialog dialog;
        dialog.Title(box_value(L"Go to line"));
        dialog.Content(lineBox);
        dialog.PrimaryButtonText(L"Go to");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);

        if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary || std::isnan(lineBox.Value()))
        {
            co_return;
        }

        int line = std::clamp(static_cast<int>(lineBox.Value()), 1, lineCount);
        doc->Editor.Select(IndexOfLine(doc->Editor.Text(), line), 0);
        FocusEditor(doc);
    }

    fire_and_forget MainWindow::ChooseFontAsync()
    {
        auto strong = get_strong();

        auto families = single_threaded_vector<IInspectable>();
        int selectedFamily = -1;
        for (auto const& family : GetSystemFontFamilies())
        {
            if (PathsEqual(family, m_fontFamily))
            {
                selectedFamily = static_cast<int>(families.Size());
            }
            families.Append(box_value(hstring{ family }));
        }
        if (selectedFamily < 0)
        {
            selectedFamily = static_cast<int>(families.Size());
            families.Append(box_value(hstring{ m_fontFamily }));
        }

        ComboBox familyBox;
        familyBox.Header(box_value(L"Font"));
        familyBox.ItemsSource(families);
        familyBox.SelectedIndex(selectedFamily);
        familyBox.HorizontalAlignment(HorizontalAlignment::Stretch);

        ComboBox styleBox;
        styleBox.Header(box_value(L"Style"));
        for (auto name : { L"Regular", L"Italic", L"Bold", L"Bold Italic" })
        {
            styleBox.Items().Append(box_value(name));
        }
        styleBox.SelectedIndex((m_fontBold ? 2 : 0) + (m_fontItalic ? 1 : 0));
        styleBox.Width(160);

        ComboBox sizeBox;
        sizeBox.Header(box_value(L"Size"));
        sizeBox.IsEditable(true);
        for (int size : { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 })
        {
            sizeBox.Items().Append(box_value(to_hstring(size)));
        }
        sizeBox.Text(std::format(L"{:g}", m_fontPoints));
        sizeBox.Width(110);

        TextBlock preview;
        preview.Text(L"AaBbYyZz 0123456789");
        preview.Height(64);
        preview.Margin(Thickness{ 0, 8, 0, 0 });

        auto readSize = [sizeBox, current = m_fontPoints]
        {
            hstring text = sizeBox.Text();
            if (auto item = sizeBox.SelectedItem())
            {
                text = unbox_value_or<hstring>(item, text);
            }
            double points = _wtof(text.c_str());
            return points >= 1 && points <= 400 ? points : current;
        };
        auto updatePreview = [familyBox, styleBox, preview, readSize]
        {
            if (auto family = familyBox.SelectedItem())
            {
                preview.FontFamily(Media::FontFamily{ unbox_value<hstring>(family) });
            }
            int style = styleBox.SelectedIndex();
            preview.FontWeight(style >= 2 ? Microsoft::UI::Text::FontWeights::Bold() : Microsoft::UI::Text::FontWeights::Normal());
            preview.FontStyle(style % 2 ? Windows::UI::Text::FontStyle::Italic : Windows::UI::Text::FontStyle::Normal);
            preview.FontSize(std::min(readSize(), 48.0) * 96.0 / 72.0);
        };
        familyBox.SelectionChanged([updatePreview](auto&&, auto&&) { updatePreview(); });
        styleBox.SelectionChanged([updatePreview](auto&&, auto&&) { updatePreview(); });
        sizeBox.SelectionChanged([updatePreview](auto&&, auto&&) { updatePreview(); });
        updatePreview();

        StackPanel styleRow;
        styleRow.Orientation(Orientation::Horizontal);
        styleRow.Spacing(12);
        styleRow.Children().Append(styleBox);
        styleRow.Children().Append(sizeBox);

        StackPanel content;
        content.Spacing(12);
        content.MinWidth(320);
        content.Children().Append(familyBox);
        content.Children().Append(styleRow);
        content.Children().Append(preview);

        ContentDialog dialog;
        dialog.Title(box_value(L"Font"));
        dialog.Content(content);
        dialog.PrimaryButtonText(L"OK");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);

        if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary)
        {
            co_return;
        }

        if (auto family = familyBox.SelectedItem())
        {
            m_fontFamily = unbox_value<hstring>(family);
        }
        int style = styleBox.SelectedIndex();
        m_fontBold = style >= 2;
        m_fontItalic = style % 2 == 1;
        m_fontPoints = readSize();
        ApplyEditorSettingsToAll();
        SaveSettings();
    }

    fire_and_forget MainWindow::ShowAboutAsync()
    {
        auto strong = get_strong();
        ContentDialog dialog;
        dialog.Title(box_value(L"About MyNotepad"));
        dialog.Content(box_value(L"A tabbed Notepad clone built with WinUI 3 and C++/WinRT."));
        dialog.CloseButtonText(L"OK");
        dialog.DefaultButton(ContentDialogButton::Close);
        co_await ShowDialogAsync(dialog);
    }

    // ---------------------------------------------------------------------------------------------
    // Drag and drop
    // ---------------------------------------------------------------------------------------------

    void MainWindow::OnDragOver(IInspectable const&, DragEventArgs const& e)
    {
        if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            e.AcceptedOperation(DataPackageOperation::Copy);
            e.DragUIOverride().Caption(L"Open");
        }
    }

    fire_and_forget MainWindow::OnDropAsync(DragEventArgs e)
    {
        if (!e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            co_return;
        }

        auto strong = get_strong();
        auto deferral = e.GetDeferral();
        auto items = co_await e.DataView().GetStorageItemsAsync();
        deferral.Complete();
        for (auto const& item : items)
        {
            if (auto file = item.try_as<StorageFile>())
            {
                OpenFile(std::wstring{ file.Path() });
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Settings (%LOCALAPPDATA%\MyNotepad\settings.ini)
    // ---------------------------------------------------------------------------------------------

    void MainWindow::LoadSettings()
    {
        auto path = SettingsFilePath();
        if (path.empty())
        {
            return;
        }

        wchar_t buffer[256]{};
        GetPrivateProfileStringW(L"Editor", L"FontFamily", L"Consolas", buffer, ARRAYSIZE(buffer), path.c_str());
        m_fontFamily = buffer[0] ? buffer : L"Consolas";
        GetPrivateProfileStringW(L"Editor", L"FontSize", L"11", buffer, ARRAYSIZE(buffer), path.c_str());
        m_fontPoints = _wtof(buffer);
        if (m_fontPoints < 1 || m_fontPoints > 400)
        {
            m_fontPoints = DefaultFontPoints;
        }
        m_fontBold = GetPrivateProfileIntW(L"Editor", L"Bold", 0, path.c_str()) != 0;
        m_fontItalic = GetPrivateProfileIntW(L"Editor", L"Italic", 0, path.c_str()) != 0;
        m_wordWrap = GetPrivateProfileIntW(L"Editor", L"WordWrap", 0, path.c_str()) != 0;
        m_statusBarVisible = GetPrivateProfileIntW(L"View", L"StatusBar", 1, path.c_str()) != 0;
    }

    void MainWindow::SaveSettings()
    {
        auto path = SettingsFilePath();
        if (path.empty())
        {
            return;
        }

        WritePrivateProfileStringW(L"Editor", L"FontFamily", m_fontFamily.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Editor", L"FontSize", std::format(L"{:g}", m_fontPoints).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Editor", L"Bold", m_fontBold ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Editor", L"Italic", m_fontItalic ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Editor", L"WordWrap", m_wordWrap ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"StatusBar", m_statusBarVisible ? L"1" : L"0", path.c_str());
    }
}
