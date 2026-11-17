#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "MainWindowHelpers.h"

// The main window: start-up, keyboard handling, closing, the title and status bar, editor settings, zoom,
// drag and drop, and saving settings. The rest of MainWindow is in MainWindow.<area>.cpp, one file per area.
namespace winrt::RadPad::implementation
{
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

        // The app icon from RadPad.rc, for Alt+Tab and the taskbar. The small icon is loaded separately
        // so the 16-24 px artwork is used rather than a shrunken large icon.
        auto loadIcon = [dpi](int metric)
        {
            int size = GetSystemMetricsForDpi(metric, dpi);
            return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_SHARED));
        };
        if (auto icon = loadIcon(SM_CXICON))
        {
            appWindow.SetIcon(GetIconIdFromIcon(icon));
        }
        if (auto smallIcon = loadIcon(SM_CXSMICON))
        {
            SendMessageW(WindowHandle(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        }

        appWindow.Resize({ MulDiv(1100, dpi, 96), MulDiv(760, dpi, 96) });

        instance::SetTaskbarRelaunch(WindowHandle());

        // Files opened from the command line (e.g. the jump list or Explorer) while this window is open.
        instance::AcceptForwardedFiles(WindowHandle(), [this](std::vector<std::wstring> const& files)
        {
            for (auto const& file : files)
            {
                OpenFile(file);
            }
        });

        appWindow.Closing([this](auto&&, AppWindowClosingEventArgs const& args) { OnAppWindowClosing(args); });
        Closed([this](auto&&, auto&&)
        {
            m_closed = true;
            SaveSettings();
        });
        // Pick up files removed from the taskbar jump list. Closing the jump list can reactivate the window
        // before Windows has saved the removal, so activation checks again a moment later, and the menu
        // also checks just before it opens.
        // Pick up changes made by other programs to open files.
        m_fileWatchTimer = DispatcherQueue().CreateTimer();
        m_fileWatchTimer.Interval(std::chrono::seconds(1));
        m_fileWatchTimer.Tick([this](auto&&, auto&&) { CheckFilesOnDisk(); });
        m_fileWatchTimer.Start();

        m_jumpListSyncTimer = DispatcherQueue().CreateTimer();
        m_jumpListSyncTimer.Interval(std::chrono::milliseconds(1500));
        m_jumpListSyncTimer.IsRepeating(false);
        m_jumpListSyncTimer.Tick([this](auto&&, auto&&) { SyncWithJumpList(); });
        Activated([this](auto&&, WindowActivatedEventArgs const& args)
        {
            if (m_loaded && args.WindowActivationState() != WindowActivationState::Deactivated)
            {
                SyncWithJumpList();
                m_jumpListSyncTimer.Start();
            }
            if (args.WindowActivationState() == WindowActivationState::Deactivated)
            {
                EndTabCycle(); // Ctrl may be released in another window
            }
        });
        RecentFilesMenu().PointerEntered([this](auto&&, auto&&) { SyncWithJumpList(); });
        RecentFilesMenu().GotFocus([this](auto&&, auto&&) { SyncWithJumpList(); });

        LoadSettings();
        UpdateFontMetrics();
        OnRecentFilesChanged();
        WordWrapItem().IsChecked(m_wordWrap);
        StatusBarItem().IsChecked(m_statusBarVisible);
        StatusBar().Visibility(m_statusBarVisible ? Visibility::Visible : Visibility::Collapsed);
        ShowOutputPanel(m_outputPanelVisible);
        // The bar above the output panel and the panel's heading resize it: they show the up-down pointer
        // (ProtectedCursor is only reachable through this interface) and highlight the bar under the pointer.
        for (UIElement handle : { OutputSplitter().as<UIElement>(), OutputHeader().as<UIElement>() })
        {
            handle.as<IUIElementProtected>().ProtectedCursor(Microsoft::UI::Input::InputSystemCursor::Create(Microsoft::UI::Input::InputSystemCursorShape::SizeNorthSouth));
            handle.PointerEntered([this](auto&&, auto&&) { HighlightOutputSplitter(true); });
            handle.PointerExited([this](auto&&, auto&&) { HighlightOutputSplitter(false); });
        }
        // The output box selects a word on double-click and ignores Enter as it's read-only, marking both
        // handled, so these handlers ask to see handled events too.
        OutputBox().AddHandler(UIElement::DoubleTappedEvent(), box_value(DoubleTappedEventHandler([this](auto&&, DoubleTappedRoutedEventArgs const& e)
        {
            e.Handled(OpenOutputLocationAtCaret());
        })), true);
        OutputBox().AddHandler(UIElement::KeyDownEvent(), box_value(KeyEventHandler([this](auto&&, KeyRoutedEventArgs const& e)
        {
            if (e.Key() == Windows::System::VirtualKey::Enter)
            {
                e.Handled(OpenOutputLocationAtCaret());
            }
        })), true);
        AddZoomAccelerators();
        m_languageErrors = syntax::LoadLanguages(LanguageFolders());
        BuildLanguageMenus();
        m_themeErrors = themes::LoadThemes(ThemeFolders());
        ReloadTools();
        BuildColorThemeMenu();
        ApplyColorTheme();
        BuildIndentMenus();
        ShowWhitespaceItem().IsChecked(m_showWhitespace);
        TrailingWhitespaceItem().IsChecked(m_highlightTrailingWhitespace);
        RainbowBracketsItem().IsChecked(m_rainbowBrackets);
        UpdateWhitespaceAvailability();

        // A colour theme can have different colours for the app's light and dark modes.
        RootGrid().ActualThemeChanged([this](auto&&, auto&&) { ApplyColorTheme(); });

        // handledEventsToo: the editor handles drag events itself, but we still want file drops anywhere.
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

        // Open any files passed on the command line, e.g. "RadPad.exe a.txt b.txt", or a tab dragged out of
        // another RadPad window ("RadPad.exe /tab <file>").
        int argc = 0;
        if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            if (argc == 3 && _wcsicmp(argv[1], instance::TabArgument) == 0)
            {
                OpenStartupTab(argv[2]);
            }
            else
            {
                for (int i = 1; i < argc; ++i)
                {
                    OpenFile(argv[i]);
                }
            }
            LocalFree(argv);
        }

        if (m_documents.empty())
        {
            CreateDocument();
        }
        if (!m_languageErrors.empty() || !m_themeErrors.empty() || !m_toolErrors.empty())
        {
            ShowLoadErrors();
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
            CycleRecentTabs(shift);
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

    void MainWindow::RootGrid_PreviewKeyUp(IInspectable const&, KeyRoutedEventArgs const& e)
    {
        auto key = e.Key();
        if (key == VirtualKey::Control || key == VirtualKey::LeftControl || key == VirtualKey::RightControl)
        {
            EndTabCycle();
        }
    }

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

    void MainWindow::UpdateDocumentState(std::shared_ptr<Document> const& doc)
    {
        doc->IsDirty = doc->DeletedOnDisk || EditorText(doc->Editor) != doc->SavedText || doc->Encoding != doc->SavedEncoding || doc->Eol != doc->SavedEol;

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
        LanguageButton().Content(box_value(hstring{ syntax::LanguageName(doc->Language) }));
        CrlfItem().IsChecked(doc->Eol == LineEnding::CRLF);
        LfItem().IsChecked(doc->Eol == LineEnding::LF);
        CrItem().IsChecked(doc->Eol == LineEnding::CR);
        Utf8Item().IsChecked(doc->Encoding == TextEncoding::Utf8);
        Utf8BomItem().IsChecked(doc->Encoding == TextEncoding::Utf8Bom);
        Utf16LEItem().IsChecked(doc->Encoding == TextEncoding::Utf16LE);
        Utf16BEItem().IsChecked(doc->Encoding == TextEncoding::Utf16BE);
        AnsiItem().IsChecked(doc->Encoding == TextEncoding::Ansi);
        UpdateReadOnlyItem(doc);
        for (auto const& [item, language] : m_languageItems)
        {
            item.IsChecked(language == doc->Language);
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

        auto text = EditorText(doc->Editor);
        auto [line, column] = LineColumnAt(text, SelectionStart(doc->Editor));
        CaretPositionText().Text(std::format(L"Ln {}, Col {}", line, column));

        // A selection can include the final paragraph mark, which isn't part of the text.
        size_t selected = std::min(static_cast<size_t>(std::max(SelectionLength(doc->Editor), 0)), text.size());
        CharacterCountText().Text(selected > 0
            ? std::format(L"{} of {} characters", selected, text.size())
            : std::format(L"{} characters", text.size()));
    }

    void MainWindow::ApplyFont(ITextCharacterFormat const& format)
    {
        // Bold and italic are left to the control's FontWeight and FontStyle: setting them on a range
        // takes RichEdit around 70µs per character, against milliseconds for the control properties.
        format.Name(m_fontFamily);
        format.Size(static_cast<float>(m_fontPoints * m_zoom / 100.0));
    }

    void MainWindow::ApplyEditorSettings(std::shared_ptr<Document> const& doc)
    {
        auto editor = doc->Editor;
        editor.FontFamily(Media::FontFamily{ m_fontFamily });
        editor.FontSize(m_fontPoints * 96.0 / 72.0 * m_zoom / 100.0);
        editor.FontWeight(m_fontBold ? Microsoft::UI::Text::FontWeights::Bold() : Microsoft::UI::Text::FontWeights::Normal());
        editor.FontStyle(m_fontItalic ? Windows::UI::Text::FontStyle::Italic : Windows::UI::Text::FontStyle::Normal);
        editor.TextWrapping(m_wordWrap ? TextWrapping::Wrap : TextWrapping::NoWrap);
        // A dark colour theme with the app in light mode, or the other way round, brings its own background;
        // match the editor to it so the caret, selection and scrollbars stay visible.
        bool appIsDark = IsDarkTheme();
        editor.RequestedTheme(m_palette.Dark == appIsDark ? ElementTheme::Default : m_palette.Dark ? ElementTheme::Dark : ElementTheme::Light);
        // With word wrap, room on the right for the wrap markers (see DrawWrapMarkers).
        editor.Padding(Thickness{ EditorPadding, 8, EditorPadding + (m_wordWrap ? WrapMarkerWidth : 0), 8 });

        // Set the font name and size on the existing text and on the default format for new text too,
        // since text that has been coloured carries its own character formatting.
        auto document = editor.Document();
        FormattingScope scope{ editor };
        auto defaultFormat = document.GetDefaultCharacterFormat();
        ApplyFont(defaultFormat);
        defaultFormat.ForegroundColor(TokenColor(TokenKind::Plain));
        document.SetDefaultCharacterFormat(defaultFormat);
        // Tab stops every m_tabWidth spaces, in points (the font size is in points too). RichEditBox
        // paragraphs also carry an explicit stop at half an inch, which would come first, so clear it.
        document.DefaultTabStop(static_cast<float>(m_tabWidth * m_spaceAdvance * m_fontPoints * m_zoom / 100.0));
        auto defaultParagraph = document.GetDefaultParagraphFormat();
        defaultParagraph.ClearAllTabs();
        document.SetDefaultParagraphFormat(defaultParagraph);
        document.GetRange(0, static_cast<int>(EditorText(editor).size()) + 1).ParagraphFormat().ClearAllTabs();
        ApplyFont(document.GetRange(0, static_cast<int>(EditorText(editor).size()) + 1).CharacterFormat());
        ScheduleRepaint(doc); // a font or zoom change can bring more text into view
    }

    void MainWindow::ApplyEditorSettingsToAll()
    {
        for (auto const& doc : m_documents)
        {
            ApplyEditorSettings(doc);
        }
    }

    bool MainWindow::IsDarkTheme()
    {
        return RootGrid().ActualTheme() == ElementTheme::Dark;
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
        GetPrivateProfileStringW(L"View", L"ColorTheme", themes::DefaultThemeName, buffer, ARRAYSIZE(buffer), path.c_str());
        m_colorTheme = buffer[0] ? buffer : themes::DefaultThemeName;
        GetPrivateProfileStringW(L"Editor", L"FontSize", L"11", buffer, ARRAYSIZE(buffer), path.c_str());
        m_fontPoints = _wtof(buffer);
        if (m_fontPoints < 1 || m_fontPoints > 400)
        {
            m_fontPoints = DefaultFontPoints;
        }
        m_fontBold = GetPrivateProfileIntW(L"Editor", L"Bold", 0, path.c_str()) != 0;
        m_fontItalic = GetPrivateProfileIntW(L"Editor", L"Italic", 0, path.c_str()) != 0;
        m_wordWrap = GetPrivateProfileIntW(L"Editor", L"WordWrap", 0, path.c_str()) != 0;
        m_insertSpaces = GetPrivateProfileIntW(L"Editor", L"InsertSpaces", 0, path.c_str()) != 0;
        m_indentSize = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Editor", L"IndentSize", 4, path.c_str())), 1, 8);
        m_tabWidth = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Editor", L"TabWidth", 8, path.c_str())), 1, 8);
        m_statusBarVisible = GetPrivateProfileIntW(L"View", L"StatusBar", 1, path.c_str()) != 0;
        m_showWhitespace = GetPrivateProfileIntW(L"View", L"ShowWhitespace", 0, path.c_str()) != 0;
        m_highlightTrailingWhitespace = GetPrivateProfileIntW(L"View", L"HighlightTrailingWhitespace", 1, path.c_str()) != 0;
        m_rainbowBrackets = GetPrivateProfileIntW(L"View", L"RainbowBrackets", 1, path.c_str()) != 0;
        syntax::SetRainbowBrackets(m_rainbowBrackets);
        m_outputPanelVisible = GetPrivateProfileIntW(L"View", L"OutputPanel", 0, path.c_str()) != 0;
        m_outputPanelHeight = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"View", L"OutputPanelHeight", 200, path.c_str())), 60, 2000);

        m_recentFiles.clear();
        std::wstring file(32768, L'\0'); // long enough for any path
        for (int i = 1; i <= 10; ++i)
        {
            DWORD length = GetPrivateProfileStringW(L"RecentFiles", std::format(L"File{}", i).c_str(), L"", file.data(), static_cast<DWORD>(file.size()), path.c_str());
            if (length > 0)
            {
                m_recentFiles.emplace_back(file.c_str(), length);
            }
        }
    }

    void MainWindow::SaveSettings()
    {
        auto path = SettingsFilePath();
        if (path.empty())
        {
            return;
        }

        WritePrivateProfileStringW(L"Editor", L"FontFamily", m_fontFamily.c_str(), path.c_str());
        WritePrivateProfileStringW(L"View", L"ColorTheme", m_colorTheme.c_str(), path.c_str());
        WritePrivateProfileStringW(L"Editor", L"FontSize", std::format(L"{:g}", m_fontPoints).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Editor", L"Bold", m_fontBold ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Editor", L"Italic", m_fontItalic ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Editor", L"WordWrap", m_wordWrap ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Editor", L"InsertSpaces", m_insertSpaces ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"Editor", L"IndentSize", std::to_wstring(m_indentSize).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Editor", L"TabWidth", std::to_wstring(m_tabWidth).c_str(), path.c_str());
        WritePrivateProfileStringW(L"View", L"StatusBar", m_statusBarVisible ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"ShowWhitespace", m_showWhitespace ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"HighlightTrailingWhitespace", m_highlightTrailingWhitespace ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"RainbowBrackets", m_rainbowBrackets ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"OutputPanel", m_outputPanelVisible ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"View", L"OutputPanelHeight", std::to_wstring(static_cast<int>(m_outputPanelHeight)).c_str(), path.c_str());

        WritePrivateProfileStringW(L"RecentFiles", nullptr, nullptr, path.c_str()); // remove the old list
        for (size_t i = 0; i < m_recentFiles.size(); ++i)
        {
            WritePrivateProfileStringW(L"RecentFiles", std::format(L"File{}", i + 1).c_str(), m_recentFiles[i].c_str(), path.c_str());
        }
    }
}
