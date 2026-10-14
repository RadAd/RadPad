#include "pch.h"
#include "MainWindow.xaml.h"
#include "Instance.h"
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
using winrt::Microsoft::UI::Text::ITextCharacterFormat;
using winrt::Microsoft::UI::Text::PointOptions;
using winrt::Microsoft::UI::Text::RichEditTextDocument;
using winrt::Microsoft::UI::Text::TextGetOptions;
using winrt::Microsoft::UI::Text::TextRangeUnit;
using winrt::Microsoft::UI::Text::TextSetOptions;
using syntax::TokenKind;
using textfile::LineEnding;
using textfile::TextEncoding;

namespace
{
    constexpr std::wstring_view AppName = L"RadPad";
    constexpr int DefaultZoom = 100;
    constexpr int MinZoom = 10;
    constexpr int MaxZoom = 500;
    constexpr int ZoomStep = 10;
    constexpr double DefaultFontPoints = 11.0;
    constexpr double EditorPadding = 12;     // around the text
    constexpr double WrapMarkerWidth = 14;   // extra right padding for the wrap markers, left of the scrollbar

    bool IsKeyDown(int virtualKey)
    {
        return (GetKeyState(virtualKey) & 0x8000) != 0;
    }

    bool PathsEqual(std::wstring const& a, std::wstring const& b)
    {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    void CopyTextToClipboard(std::wstring const& text)
    {
        DataPackage package;
        package.SetText(text);
        Clipboard::SetContent(package);
        Clipboard::Flush(); // keep the text after the app closes
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

    // The editor's text. RichEditBox always ends its text with a paragraph mark that isn't part of the document.
    std::wstring EditorText(RichEditBox const& editor)
    {
        hstring text;
        editor.Document().GetText(TextGetOptions::None, text);
        std::wstring result{ text };
        if (!result.empty() && result.back() == L'\r')
        {
            result.pop_back();
        }
        return result;
    }

    int SelectionStart(RichEditBox const& editor)
    {
        return editor.Document().Selection().StartPosition();
    }

    int SelectionLength(RichEditBox const& editor)
    {
        auto selection = editor.Document().Selection();
        return selection.EndPosition() - selection.StartPosition();
    }

    std::wstring SelectedText(RichEditBox const& editor)
    {
        hstring text;
        editor.Document().Selection().GetText(TextGetOptions::None, text);
        return std::wstring{ text };
    }

    void Select(RichEditBox const& editor, int start, int length)
    {
        editor.Document().Selection().SetRange(start, start + length);
    }

    // Copies as plain text with CRLF line breaks, whatever the selection's formatting.
    void CopySelection(RichEditBox const& editor)
    {
        hstring text;
        editor.Document().Selection().GetText(TextGetOptions::UseCrlf, text);
        if (!text.empty())
        {
            DataPackage package;
            package.SetText(text);
            Clipboard::SetContent(package);
        }
    }

    // Batches formatting changes so the editor redraws once at the end.
    class FormattingScope
    {
    public:
        explicit FormattingScope(RichEditTextDocument const& document) : m_document(document)
        {
            m_document.BatchDisplayUpdates();
        }

        ~FormattingScope()
        {
            m_document.ApplyDisplayUpdates();
        }

        FormattingScope(FormattingScope const&) = delete;
        FormattingScope& operator=(FormattingScope const&) = delete;

    private:
        RichEditTextDocument m_document;
    };

    // The first descendant of type T in the visual tree, or nullptr.
    template <typename T>
    T FindDescendant(DependencyObject const& root)
    {
        for (int i = 0, count = VisualTreeHelper::GetChildrenCount(root); i < count; ++i)
        {
            auto child = VisualTreeHelper::GetChild(root, i);
            if (auto match = child.try_as<T>())
            {
                return match;
            }
            if (auto match = FindDescendant<T>(child))
            {
                return match;
            }
        }
        return nullptr;
    }

    Primitives::ScrollBar FindVerticalScrollBar(DependencyObject const& root)
    {
        for (int i = 0, count = VisualTreeHelper::GetChildrenCount(root); i < count; ++i)
        {
            auto child = VisualTreeHelper::GetChild(root, i);
            if (auto bar = child.try_as<Primitives::ScrollBar>(); bar && bar.Orientation() == Orientation::Vertical)
            {
                return bar;
            }
            if (auto bar = FindVerticalScrollBar(child))
            {
                return bar;
            }
        }
        return nullptr;
    }

    // The rectangle of the character at position, relative to the top-left of the view. RichEdit measures
    // from the top-left of the document, as the ScrollViewer around it does the scrolling.
    Windows::Foundation::Rect CharRectInView(RichEditTextDocument const& document, int position, int length, ScrollViewer const& viewer)
    {
        Windows::Foundation::Rect rect{};
        int hit = 0;
        document.GetRange(position, std::min(position + 1, length)).GetRect(PointOptions::ClientCoordinates, rect, hit);
        rect.X -= static_cast<float>(viewer.HorizontalOffset());
        rect.Y -= static_cast<float>(viewer.VerticalOffset());
        return rect;
    }

    // The first display row at least partly in view, as an empty range at its start, or null.
    // Only rows near the view are asked about: asking about rows far outside it makes RichEdit lay out the
    // whole document at once, which on a large file with word wrap stops it drawing.
    Microsoft::UI::Text::ITextRange FirstVisibleRow(RichEditTextDocument const& document, int length, ScrollViewer const& viewer)
    {
        if (length == 0 || viewer.ExtentHeight() <= 0)
        {
            return nullptr;
        }

        // Start from the same fraction of the text as the scroll position, then move by rows to the first
        // row in view. Rows are all the same height, so the distance says how many rows to move.
        auto guess = static_cast<int>(viewer.VerticalOffset() / viewer.ExtentHeight() * length);
        auto row = document.GetRange(guess, guess);
        row.StartOf(TextRangeUnit::Line, false);
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            auto rect = CharRectInView(document, row.StartPosition(), length, viewer);
            if (rect.Height <= 0)
            {
                return nullptr;
            }
            int rows = 0;
            if (rect.Y > 0.5)
            {
                rows = -std::max(1, static_cast<int>(std::ceil(rect.Y / rect.Height - 0.01))); // below the top: back up
            }
            else if (rect.Y + rect.Height <= 0.5)
            {
                rows = std::max(1, static_cast<int>(-rect.Y / rect.Height)); // above the top: go forward
            }
            if (rows == 0 || row.Move(TextRangeUnit::Line, rows) == 0)
            {
                break;
            }
        }
        return row;
    }

    // Whether the character at position is out of view, vertically and horizontally.
    std::pair<bool, bool> OutOfView(RichEditTextDocument const& document, int position, ScrollViewer const& viewer)
    {
        if (!viewer || viewer.ViewportHeight() <= 0)
        {
            return { false, false };
        }
        auto rect = CharRectInView(document, position, position + 1, viewer);
        return { rect.Y < 0 || rect.Y + rect.Height > viewer.ViewportHeight(), rect.X < 0 || rect.X + rect.Width > viewer.ViewportWidth() };
    }

    // Centres the character at position in the directions it was out of view (see OutOfView), once the
    // editor has finished its own scrolling: changing the selection makes it scroll the caret just into
    // view, which leaves text that was out of view at the very edge, where it's easy to miss.
    void CentreInView(RichEditBox const& editor, ScrollViewer const& viewer, int position, std::pair<bool, bool> outOfView)
    {
        auto [vertically, horizontally] = outOfView;
        if (!viewer || (!vertically && !horizontally))
        {
            return;
        }
        editor.DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [editor, viewer, position, vertically, horizontally]
        {
            auto rect = CharRectInView(editor.Document(), position, position + 1, viewer);
            Windows::Foundation::IReference<double> horizontal;
            Windows::Foundation::IReference<double> vertical;
            if (vertically)
            {
                vertical = std::max(0.0, viewer.VerticalOffset() + rect.Y - (viewer.ViewportHeight() - rect.Height) / 2);
            }
            if (horizontally)
            {
                // Back to the left edge if the text fits from there, otherwise a third of the way in.
                double x = viewer.HorizontalOffset() + rect.X;
                horizontal = x + rect.Width <= viewer.ViewportWidth() ? 0.0 : std::max(0.0, x - viewer.ViewportWidth() / 3);
            }
            viewer.ChangeView(horizontal, vertical, nullptr, true);
        });
    }

    // Selects text; if it was out of view, centres it. Text already in view doesn't move.
    void SelectAndReveal(RichEditBox const& editor, ScrollViewer const& viewer, int start, int length)
    {
        auto outOfView = OutOfView(editor.Document(), start, viewer); // before selecting, which scrolls
        Select(editor, start, length);
        CentreInView(editor, viewer, start, outOfView);
    }

    struct FontMetrics
    {
        bool Monospaced{ false };
        double SpaceAdvance{ 0.5 }; // width of a space as a fraction of the font size
    };

    // Metrics of an installed font, or nullopt if it can't be found.
    std::optional<FontMetrics> GetFontMetrics(std::wstring const& family, bool bold, bool italic)
    {
        com_ptr<IDWriteFactory> factory;
        com_ptr<IDWriteFontCollection> collection;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown**>(factory.put())))
            || FAILED(factory->GetSystemFontCollection(collection.put())))
        {
            return std::nullopt;
        }

        UINT32 index = 0;
        BOOL exists = FALSE;
        com_ptr<IDWriteFontFamily> fontFamily;
        com_ptr<IDWriteFont> font;
        com_ptr<IDWriteFontFace> face;
        if (FAILED(collection->FindFamilyName(family.c_str(), &index, &exists)) || !exists
            || FAILED(collection->GetFontFamily(index, fontFamily.put()))
            || FAILED(fontFamily->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, font.put()))
            || FAILED(font->CreateFontFace(face.put())))
        {
            return std::nullopt;
        }
        FontMetrics result;
        auto font1 = font.try_as<IDWriteFont1>();
        result.Monospaced = font1 && font1->IsMonospacedFont();

        DWRITE_FONT_METRICS metrics{};
        face->GetMetrics(&metrics);
        UINT32 codePoint = L' ';
        UINT16 glyph = 0;
        DWRITE_GLYPH_METRICS glyphMetrics{};
        if (SUCCEEDED(face->GetGlyphIndices(&codePoint, 1, &glyph)) && SUCCEEDED(face->GetDesignGlyphMetrics(&glyph, 1, &glyphMetrics, FALSE)) && metrics.designUnitsPerEm != 0)
        {
            result.SpaceAdvance = static_cast<double>(glyphMetrics.advanceWidth) / metrics.designUnitsPerEm;
        }
        return result;
    }

    bool IsSpace(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r';
    }

    // Line and column (both 1-based) of a character index. Editor text uses '\r' for every line break.
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

    // %LOCALAPPDATA%\RadPad, where settings and the user's language definitions are kept.
    std::wstring AppDataFolder()
    {
        PWSTR folder = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        {
            return {};
        }
        std::wstring directory = std::wstring(folder) + L"\\RadPad";
        CoTaskMemFree(folder);
        CreateDirectoryW(directory.c_str(), nullptr);
        return directory;
    }

    std::wstring SettingsFilePath()
    {
        std::wstring directory = AppDataFolder();
        if (directory.empty())
        {
            return {};
        }
        std::wstring path = directory + L"\\settings.ini";

        // The profile functions only write Unicode to a file that starts with a UTF-16 byte order mark;
        // otherwise characters outside the ANSI code page (in recent file paths, say) are lost. Create
        // the file that way, and convert one written by an older version.
        std::string bytes;
        if (FILE* file = nullptr; _wfopen_s(&file, path.c_str(), L"rb") == 0 && file)
        {
            char buffer[4096];
            for (size_t read; (read = fread(buffer, 1, sizeof(buffer), file)) > 0;)
            {
                bytes.append(buffer, read);
            }
            fclose(file);
        }
        if (!bytes.starts_with("\xFF\xFE"))
        {
            std::wstring text(static_cast<size_t>(MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0)), L'\0');
            MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), static_cast<int>(text.size()));
            if (FILE* file = nullptr; _wfopen_s(&file, path.c_str(), L"wb") == 0 && file)
            {
                fwrite("\xFF\xFE", 1, 2, file);
                fwrite(text.data(), sizeof(wchar_t), text.size(), file);
                fclose(file);
            }
        }
        return path;
    }

    // %LOCALAPPDATA%\RadPad\Languages: language definitions added or customised by the user.
    std::wstring UserLanguageFolder()
    {
        std::wstring directory = AppDataFolder();
        return directory.empty() ? std::wstring{} : directory + L"\\Languages";
    }

    // %LOCALAPPDATA%\RadPad\Themes: colour themes added or customised by the user.
    std::wstring UserThemeFolder()
    {
        std::wstring directory = AppDataFolder();
        return directory.empty() ? std::wstring{} : directory + L"\\Themes";
    }

    // Where language definitions are loaded from: the Languages folder installed beside the exe, then
    // the user's folder, whose definitions override installed ones with the same name.
    std::wstring ExePath()
    {
        std::wstring exe(MAX_PATH, L'\0');
        DWORD length = 0;
        while ((length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()))) == exe.size())
        {
            exe.resize(exe.size() * 2);
        }
        exe.resize(length);
        return exe;
    }

    std::vector<std::wstring> LanguageFolders()
    {
        auto exe = ExePath();
        std::wstring appFolder = exe.substr(0, exe.find_last_of(L'\\'));
        return { appFolder + L"\\Languages", UserLanguageFolder() };
    }

    // Where colour themes are loaded from, as for languages: beside the exe, then the user's folder.
    std::vector<std::wstring> ThemeFolders()
    {
        auto exe = ExePath();
        std::wstring appFolder = exe.substr(0, exe.find_last_of(L'\\'));
        return { appFolder + L"\\Themes", UserThemeFolder() };
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

    // The "Save as type" / file type list for the Open and Save As dialogs: text documents and all files,
    // then one entry per loaded language that lists extensions or file names.
    class FileTypeFilters
    {
    public:
        FileTypeFilters()
        {
            Add(L"Text Documents", L"*.txt", syntax::PlainText);
            Add(L"All Files", L"*.*", syntax::PlainText);
            for (syntax::LanguageId language = syntax::PlainText + 1; language < syntax::LanguageCount(); ++language)
            {
                std::wstring spec;
                for (auto const& pattern : syntax::LanguagePatterns(language))
                {
                    spec += (spec.empty() ? L"" : L";") + pattern;
                }
                if (!spec.empty())
                {
                    Add(std::wstring{ syntax::LanguageName(language) }, spec, language);
                }
            }
            for (size_t i = 0; i < m_names.size(); ++i)
            {
                m_specs.push_back({ m_names[i].c_str(), m_patterns[i].c_str() });
            }
        }

        void Apply(IFileDialog* dialog) const
        {
            check_hresult(dialog->SetFileTypes(static_cast<UINT>(m_specs.size()), m_specs.data()));
        }

        // 1-based index of the entry for a language, as IFileDialog::SetFileTypeIndex expects;
        // Text Documents for plain text or a language without an entry.
        UINT IndexOf(syntax::LanguageId language) const
        {
            auto it = std::find(m_languages.begin() + 2, m_languages.end(), language);
            return it == m_languages.end() ? 1 : static_cast<UINT>(it - m_languages.begin()) + 1;
        }

    private:
        void Add(std::wstring const& name, std::wstring const& patterns, syntax::LanguageId language)
        {
            m_names.push_back(name + L" (" + patterns + L")");
            m_patterns.push_back(patterns);
            m_languages.push_back(language);
        }

        std::vector<std::wstring> m_names;
        std::vector<std::wstring> m_patterns;
        std::vector<syntax::LanguageId> m_languages;
        std::vector<COMDLG_FILTERSPEC> m_specs; // points into m_names and m_patterns
    };

    std::vector<std::wstring> ShowOpenFileDialog(HWND owner)
    {
        std::vector<std::wstring> paths;

        FileTypeFilters filters;
        auto dialog = create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
        filters.Apply(dialog.get());
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

    // Shows the Save As dialog with an Encoding drop-down, like Notepad's. The file type starts on the
    // document's language, and choosing another type changes the extension the dialog adds.
    std::optional<SaveDialogResult> ShowSaveFileDialog(HWND owner, std::wstring const& currentPath, std::wstring const& suggestedName, TextEncoding encoding, syntax::LanguageId language)
    {
        constexpr DWORD EncodingGroupId = 100;
        constexpr DWORD EncodingComboId = 101;

        FileTypeFilters filters;
        auto dialog = create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
        filters.Apply(dialog.get());
        check_hresult(dialog->SetFileTypeIndex(filters.IndexOf(language)));
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

namespace winrt::RadPad::implementation
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

        // Files opened from the command line (e.g. the jump list or Explorer) while this window is open.
        instance::AcceptForwardedFiles(WindowHandle(), [this](std::vector<std::wstring> const& files)
        {
            for (auto const& file : files)
            {
                OpenFile(file);
            }
        });

        appWindow.Closing([this](auto&&, AppWindowClosingEventArgs const& args) { OnAppWindowClosing(args); });
        Closed([this](auto&&, auto&&) { SaveSettings(); });
        // Pick up files removed from the taskbar jump list. Closing the jump list can reactivate the window
        // before Windows has saved the removal, so activation checks again a moment later, and the menu
        // also checks just before it opens.
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
        AddZoomAccelerators();
        m_languageErrors = syntax::LoadLanguages(LanguageFolders());
        BuildLanguageMenus();
        m_themeErrors = themes::LoadThemes(ThemeFolders());
        BuildColorThemeMenu();
        ApplyColorTheme();
        BuildIndentMenus();
        ShowWhitespaceItem().IsChecked(m_showWhitespace);
        TrailingWhitespaceItem().IsChecked(m_highlightTrailingWhitespace);
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

        // Open any files passed on the command line, e.g. "RadPad.exe a.txt b.txt".
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
        if (!m_languageErrors.empty() || !m_themeErrors.empty())
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

    // ---------------------------------------------------------------------------------------------
    // Documents and tabs
    // ---------------------------------------------------------------------------------------------

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
            case 'E': case 'J': case 'L': case 'R': case VK_OEM_4: case VK_OEM_6: case VK_OEM_COMMA: case VK_OEM_PERIOD:
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
        if (!doc || !doc->Path.empty() || doc->IsDirty || !EditorText(doc->Editor).empty())
        {
            doc = CreateDocument();
        }

        doc->Path = fullPath;
        doc->Encoding = doc->SavedEncoding = loaded.Encoding;
        doc->Eol = doc->SavedEol = loaded.Eol;
        doc->Language = syntax::LanguageFromPath(fullPath);
        doc->Editor.Document().SetText(TextSetOptions::None, loaded.Text);
        doc->SavedText = EditorText(doc->Editor);
        Select(doc->Editor, 0, 0);
        UpdateHighlighting(doc, true);
        ClearUndoHistory(doc);
        UpdateDocumentState(doc);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
        SHAddToRecentDocs(SHARD_PATHW, fullPath.c_str());
        AddRecentFile(fullPath);
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

    // ---------------------------------------------------------------------------------------------
    // Recent files
    // ---------------------------------------------------------------------------------------------

    void MainWindow::AddRecentFile(std::wstring const& path)
    {
        constexpr size_t MaxRecentFiles = 10;

        std::erase_if(m_recentFiles, [&](auto const& existing) { return PathsEqual(existing, path); });
        m_recentFiles.insert(m_recentFiles.begin(), path);
        if (m_recentFiles.size() > MaxRecentFiles)
        {
            m_recentFiles.resize(MaxRecentFiles);
        }
        OnRecentFilesChanged();
    }

    void MainWindow::OnRecentFilesChanged()
    {
        UpdateJumpList(); // first, as it drops files the user removed from the jump list
        BuildRecentFilesMenu();
        SaveSettings();
    }

    void MainWindow::UpdateJumpList()
    {
        // The list shown when right-clicking the app on the taskbar. Windows' own Recent category only
        // shows file types registered to an app, and this unpackaged app registers none, so the recent
        // files are added as a custom category of shortcuts that start RadPad with the file.
        try
        {
            auto list = create_instance<ICustomDestinationList>(CLSID_DestinationList);
            UINT maxSlots = 0;
            com_ptr<IObjectArray> removed;
            check_hresult(list->BeginList(&maxSlots, IID_PPV_ARGS(removed.put())));

            // Files the user removed from the jump list can't be added back, so drop them from ours too.
            DropRemovedJumpListFiles(removed.get());

            auto exe = ExePath();
            auto items = create_instance<IObjectCollection>(CLSID_EnumerableObjectCollection);
            for (auto const& path : m_recentFiles)
            {
                auto link = create_instance<IShellLinkW>(CLSID_ShellLink);
                check_hresult(link->SetPath(exe.c_str()));
                check_hresult(link->SetArguments((L"\"" + path + L"\"").c_str()));
                check_hresult(link->SetIconLocation(exe.c_str(), 0));
                link->SetDescription(path.c_str()); // the tooltip; fails harmlessly for very long paths

                // The text shown in the list is the file name.
                auto slash = path.find_last_of(L"\\/");
                PROPVARIANT title{};
                check_hresult(InitPropVariantFromString(slash == std::wstring::npos ? path.c_str() : path.c_str() + slash + 1, &title));
                auto store = link.as<IPropertyStore>();
                HRESULT hr = store->SetValue(PKEY_Title, title);
                PropVariantClear(&title);
                check_hresult(hr);
                check_hresult(store->Commit());
                check_hresult(items->AddObject(link.get()));
            }
            if (!m_recentFiles.empty())
            {
                check_hresult(list->AppendCategory(L"Recent", items.as<IObjectArray>().get()));
            }
            check_hresult(list->CommitList());
        }
        catch (hresult_error const&)
        {
            // The jump list is a convenience; the File menu still has the list.
        }
    }

    bool MainWindow::DropRemovedJumpListFiles(IObjectArray* removed)
    {
        // The removed items are the shortcuts made by UpdateJumpList, whose arguments are the quoted path.
        auto oldCount = m_recentFiles.size();
        UINT removedCount = 0;
        if (FAILED(removed->GetCount(&removedCount)))
        {
            return false;
        }
        std::wstring arguments(32768, L'\0');
        for (UINT i = 0; i < removedCount; ++i)
        {
            com_ptr<IShellLinkW> link;
            if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(link.put())))
                && SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))))
            {
                std::wstring path{ arguments.c_str() };
                if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
                {
                    path = path.substr(1, path.size() - 2);
                }
                std::erase_if(m_recentFiles, [&](auto const& existing) { return PathsEqual(existing, path); });
            }
        }
        return m_recentFiles.size() != oldCount;
    }

    void MainWindow::SyncWithJumpList()
    {
        // Windows doesn't say when the user removes an item from the jump list, so this is called when the
        // window is activated and before the Recent files menu opens.
        try
        {
            auto list = create_instance<ICustomDestinationList>(CLSID_DestinationList);
            UINT maxSlots = 0;
            com_ptr<IObjectArray> removed;
            check_hresult(list->BeginList(&maxSlots, IID_PPV_ARGS(removed.put())));
            bool changed = DropRemovedJumpListFiles(removed.get());
            list->AbortList();
            if (changed)
            {
                OnRecentFilesChanged();
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    void MainWindow::OpenRecentFile(std::wstring path)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::erase_if(m_recentFiles, [&](auto const& existing) { return PathsEqual(existing, path); });
            OnRecentFilesChanged();
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"\"{}\" can't be found, so it has been removed from Recent files.", path) });
            return;
        }
        OpenFile(path);
    }

    void MainWindow::BuildRecentFilesMenu()
    {
        auto items = RecentFilesMenu().Items();
        items.Clear();
        if (m_recentFiles.empty())
        {
            MenuFlyoutItem none;
            none.Text(L"No recent files");
            none.IsEnabled(false);
            items.Append(none);
            return;
        }

        for (size_t i = 0; i < m_recentFiles.size(); ++i)
        {
            // "1  name.txt", with the full path in the tooltip and as the accessible name.
            auto const& path = m_recentFiles[i];
            auto slash = path.find_last_of(L"\\/");
            auto name = slash == std::wstring::npos ? path : path.substr(slash + 1);
            auto folder = slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);

            // Long folders are shortened in the middle ("C:\Users\...\Project") to keep the menu narrow.
            wchar_t shortFolder[64]{};
            if (!PathCompactPathExW(shortFolder, folder.c_str(), ARRAYSIZE(shortFolder), 0))
            {
                wcsncpy_s(shortFolder, folder.c_str(), _TRUNCATE);
            }

            MenuFlyoutItem item;
            item.Text(std::format(L"{}  {}", i + 1, name));
            item.KeyboardAcceleratorTextOverride(shortFolder); // shown dimmed on the right
            ToolTipService::SetToolTip(item, box_value(hstring{ path }));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(item, path);
            item.Click([this, path](auto&&, auto&&) { OpenRecentFile(path); });
            items.Append(item);
        }

        items.Append(MenuFlyoutSeparator{});
        MenuFlyoutItem clear;
        clear.Text(L"Clear recent files");
        clear.Click([this](auto&&, auto&&)
        {
            m_recentFiles.clear();
            OnRecentFilesChanged();
        });
        items.Append(clear);
    }

    bool MainWindow::SaveDocument(std::shared_ptr<Document> const& doc, bool saveAs)
    {
        std::wstring path = doc->Path;
        TextEncoding encoding = doc->Encoding;

        if (saveAs || path.empty())
        {
            try
            {
                // A new document is suggested the first extension of its language, or .txt.
                std::wstring suggested = doc->DisplayName();
                if (doc->Path.empty())
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
        doc->Encoding = doc->SavedEncoding = encoding;
        doc->SavedEol = doc->Eol;
        doc->SavedText = std::move(text);
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

    void MainWindow::TabMenu_Opening(IInspectable const& sender, IInspectable const&)
    {
        // The menu is shared by all tabs; Target is the tab that was right-clicked.
        auto doc = FindDocument(sender.as<MenuFlyout>().Target());
        m_tabMenuDocument = doc;
        if (!doc)
        {
            return;
        }

        // An untitled document has no file name or folder yet.
        bool hasPath = !doc->Path.empty();
        TabCopyNameItem().IsEnabled(hasPath);
        TabCopyPathItem().IsEnabled(hasPath);
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
            CopyTextToClipboard(doc->Path);
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

        // Like Notepad, closing the last tab closes the window.
        if (m_documents.empty())
        {
            m_forceClose = true;
            Close();
        }
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

    void MainWindow::RootGrid_PreviewKeyUp(IInspectable const&, KeyRoutedEventArgs const& e)
    {
        auto key = e.Key();
        if (key == VirtualKey::Control || key == VirtualKey::LeftControl || key == VirtualKey::RightControl)
        {
            EndTabCycle();
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
        UpdateEditorOverlay();
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

    // Menu and button commands. Each XAML item names its own handler, so a misspelt or missing one fails the build.

    void MainWindow::NewTab_Click(IInspectable const&, RoutedEventArgs const&) { CreateDocument(); }
    void MainWindow::Open_Click(IInspectable const&, RoutedEventArgs const&) { OpenWithDialog(); }
    void MainWindow::SaveAll_Click(IInspectable const&, RoutedEventArgs const&) { SaveAll(); }
    void MainWindow::Exit_Click(IInspectable const&, RoutedEventArgs const&) { TryCloseWindowAsync(); }
    void MainWindow::Find_Click(IInspectable const&, RoutedEventArgs const&) { ShowFindPanel(false); }
    void MainWindow::Replace_Click(IInspectable const&, RoutedEventArgs const&) { ShowFindPanel(true); }
    void MainWindow::CloseFind_Click(IInspectable const&, RoutedEventArgs const&) { HideFindPanel(); }
    void MainWindow::FindNext_Click(IInspectable const&, RoutedEventArgs const&) { FindNext(true); }
    void MainWindow::FindPrevious_Click(IInspectable const&, RoutedEventArgs const&) { FindNext(false); }
    void MainWindow::ReplaceOne_Click(IInspectable const&, RoutedEventArgs const&) { ReplaceOne(); }
    void MainWindow::ReplaceAll_Click(IInspectable const&, RoutedEventArgs const&) { ReplaceAll(); }
    void MainWindow::GoTo_Click(IInspectable const&, RoutedEventArgs const&) { GoToLineAsync(); }
    void MainWindow::Font_Click(IInspectable const&, RoutedEventArgs const&) { ChooseFontAsync(); }
    void MainWindow::About_Click(IInspectable const&, RoutedEventArgs const&) { ShowAboutAsync(); }
    void MainWindow::ZoomIn_Click(IInspectable const&, RoutedEventArgs const&) { SetZoom(m_zoom + ZoomStep); }
    void MainWindow::ZoomOut_Click(IInspectable const&, RoutedEventArgs const&) { SetZoom(m_zoom - ZoomStep); }
    void MainWindow::ZoomReset_Click(IInspectable const&, RoutedEventArgs const&) { SetZoom(DefaultZoom); }

    void MainWindow::WordWrap_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_wordWrap = WordWrapItem().IsChecked();
        ApplyEditorSettingsToAll();
        UpdateWhitespaceAvailability();
    }

    void MainWindow::StatusBar_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_statusBarVisible = StatusBarItem().IsChecked();
        StatusBar().Visibility(m_statusBarVisible ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::TrailingWhitespace_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_highlightTrailingWhitespace = TrailingWhitespaceItem().IsChecked();
        UpdateEditorOverlay();
    }

    void MainWindow::ShowWhitespace_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_showWhitespace = ShowWhitespaceItem().IsChecked();
        UpdateEditorOverlay();
    }

    void MainWindow::Save_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            SaveDocument(doc, false);
        }
    }

    void MainWindow::SaveAs_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            SaveDocument(doc, true);
        }
    }

    void MainWindow::CloseTab_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            CloseDocumentAsync(doc);
        }
    }

    void MainWindow::EditCurrentDocument(std::function<void(std::shared_ptr<Document> const&)> const& edit)
    {
        // For Edit menu commands: act on the current document, then return focus to its editor.
        if (auto doc = CurrentDocument())
        {
            edit(doc);
            FocusEditor(doc);
        }
    }

    void MainWindow::Undo_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { UndoOrRedo(doc, false); }); }
    void MainWindow::Redo_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { UndoOrRedo(doc, true); }); }
    void MainWindow::Cut_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { CopySelection(doc->Editor); InsertText(doc->Editor, L""); }); }
    void MainWindow::Copy_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { CopySelection(doc->Editor); }); }
    void MainWindow::Paste_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { PasteAsync(doc); }); }
    void MainWindow::Delete_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { InsertText(doc->Editor, L""); }); }
    void MainWindow::SelectAll_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { Select(doc->Editor, 0, static_cast<int>(EditorText(doc->Editor).size())); }); }
    void MainWindow::TimeDate_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { InsertText(doc->Editor, CurrentTimeDate()); }); }

    void MainWindow::SetEncoding(TextEncoding encoding)
    {
        if (auto doc = CurrentDocument())
        {
            doc->Encoding = encoding;
            UpdateDocumentState(doc);
            UpdateStatusBar();
        }
    }

    void MainWindow::Utf8_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf8); }
    void MainWindow::Utf8Bom_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf8Bom); }
    void MainWindow::Utf16LE_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf16LE); }
    void MainWindow::Utf16BE_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf16BE); }
    void MainWindow::Ansi_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Ansi); }

    void MainWindow::SetLineEnding(LineEnding eol)
    {
        if (auto doc = CurrentDocument())
        {
            doc->Eol = eol;
            UpdateDocumentState(doc);
            UpdateStatusBar();
        }
    }

    void MainWindow::Crlf_Click(IInspectable const&, RoutedEventArgs const&) { SetLineEnding(LineEnding::CRLF); }
    void MainWindow::Lf_Click(IInspectable const&, RoutedEventArgs const&) { SetLineEnding(LineEnding::LF); }
    void MainWindow::Cr_Click(IInspectable const&, RoutedEventArgs const&) { SetLineEnding(LineEnding::CR); }

    void MainWindow::InsertText(RichEditBox const& editor, std::wstring_view text)
    {
        // Replaces the selection and leaves the caret after the new text.
        auto selection = editor.Document().Selection();
        if (selection.Length() != 0 || !text.empty())
        {
            selection.SetText(TextSetOptions::None, text);
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

    // ---------------------------------------------------------------------------------------------
    // UI state
    // ---------------------------------------------------------------------------------------------

    void MainWindow::UpdateDocumentState(std::shared_ptr<Document> const& doc)
    {
        doc->IsDirty = EditorText(doc->Editor) != doc->SavedText || doc->Encoding != doc->SavedEncoding || doc->Eol != doc->SavedEol;

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
        FormattingScope scope{ document };
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

    // ---------------------------------------------------------------------------------------------
    // Syntax highlighting
    // ---------------------------------------------------------------------------------------------

    void MainWindow::ScheduleRefresh(std::shared_ptr<Document> const& doc)
    {
        // Several edits in a row (fast typing, IME input) are handled together once the dispatcher is idle.
        if (doc->RefreshPending)
        {
            return;
        }
        doc->RefreshPending = true;
        DispatcherQueue().TryEnqueue([this, strong = get_strong(), weakDoc = std::weak_ptr<Document>(doc)]
        {
            auto d = weakDoc.lock();
            if (!d || std::find(m_documents.begin(), m_documents.end(), d) == m_documents.end())
            {
                return;
            }
            d->RefreshPending = false;
            UpdateHighlighting(d, false);
            UpdateDocumentState(d);
            if (d == CurrentDocument())
            {
                UpdateCaretPosition();
            }
            ScheduleRepaint(d); // updates visible whitespace once the edit is laid out
        });
    }

    void MainWindow::UpdateHighlighting(std::shared_ptr<Document> const& doc, bool recolorAll)
    {
        std::wstring text = EditorText(doc->Editor);
        auto& applied = doc->AppliedKinds;
        auto const& old = doc->HighlightedText;

        if (text != old)
        {
            // The edit is the part between the common prefix and suffix of the old and new text.
            size_t prefix = std::mismatch(old.begin(), old.end(), text.begin(), text.end()).first - old.begin();
            size_t suffix = 0;
            size_t maxSuffix = std::min(old.size(), text.size()) - prefix;
            while (suffix < maxSuffix && old[old.size() - 1 - suffix] == text[text.size() - 1 - suffix])
            {
                ++suffix;
            }
            if (!doc->ApplyingUndo)
            {
                RecordEdit(doc, prefix, old.substr(prefix, old.size() - prefix - suffix), text.substr(prefix, text.size() - prefix - suffix));
            }
            if (recolorAll)
            {
                // Don't bother working out which colours survived.
                suffix = prefix = 0;
            }

            // Formatting moves with the text, so the colours before and after the edit are still in place.
            // The edited span is widened to cover the replaced selection, because replaced text all takes
            // on one format, even where the new text happens to match the old.
            auto delta = static_cast<ptrdiff_t>(text.size()) - static_cast<ptrdiff_t>(old.size());
            auto editStart = static_cast<size_t>(std::clamp<ptrdiff_t>(std::min(doc->EditStart, doc->EditEnd), 0, static_cast<ptrdiff_t>(text.size())));
            auto editEnd = static_cast<size_t>(std::clamp<ptrdiff_t>(std::max(doc->EditStart, doc->EditEnd) + delta, 0, static_cast<ptrdiff_t>(text.size())));
            size_t changedStart = std::min(prefix, editStart);
            size_t changedEnd = std::max(text.size() - suffix, editEnd);
            size_t keptBefore = changedStart;            // leading characters whose colour is known
            size_t keptAfter = text.size() - changedEnd; // trailing characters whose colour is known

            std::vector<TokenKind> rebased(text.size(), TokenKind::Unknown);
            std::copy_n(applied.begin(), keptBefore, rebased.begin());
            std::copy(applied.end() - keptAfter, applied.end(), rebased.end() - keptAfter);
            applied = std::move(rebased);
        }
        else if (recolorAll)
        {
            applied.assign(text.size(), TokenKind::Unknown);
        }

        syntax::Classify(doc->Language, text, doc->DesiredKinds);
        doc->HighlightedText = std::move(text);

        // New text gets the editor font and the plain colour, removing any formatting it came with.
        // This is one call per span of new text, so it's cheap even for a whole file.
        auto document = doc->Editor.Document();
        std::optional<FormattingScope> scope; // only opened if something needs painting
        for (size_t i = 0; i < applied.size();)
        {
            if (applied[i] != TokenKind::Unknown)
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < applied.size() && applied[j] == TokenKind::Unknown)
            {
                ++j;
            }
            if (!scope)
            {
                scope.emplace(document);
            }
            auto format = document.GetRange(static_cast<int>(i), static_cast<int>(j)).CharacterFormat();
            ApplyFont(format);
            format.ForegroundColor(TokenColor(TokenKind::Plain));
            std::fill(applied.begin() + i, applied.begin() + j, TokenKind::Plain);
            i = j;
        }
        scope.reset();

        PaintVisibleText(doc);
    }

    void MainWindow::PaintVisibleText(std::shared_ptr<Document> const& doc)
    {
        // Colouring takes one call per token, which is slow for big files, so only the visible text
        // (plus a margin) is coloured. More is coloured as the editor scrolls.
        constexpr size_t Margin = 3000;

        auto editor = doc->Editor;
        auto& applied = doc->AppliedKinds;
        auto const& desired = doc->DesiredKinds;
        if (editor.Visibility() != Visibility::Visible || editor.ActualHeight() <= 0 || applied.size() != desired.size())
        {
            return; // painted once the editor is shown
        }

        // Work out the visible text from the scroll position. (RichEdit's GetRangeFromPoint can't be used:
        // it ignores the scroll position.) Without the ScrollViewer, everything is coloured.
        auto document = editor.Document();
        auto const& text = doc->HighlightedText;
        size_t from = 0;
        size_t to = desired.size();
        auto viewer = doc->ScrollViewer;
        if (viewer && viewer.ExtentHeight() > 0)
        {
            // The visible part as fractions of the document's height, widened by half a screen each way.
            double extent = viewer.ExtentHeight();
            double top = viewer.VerticalOffset() / extent;
            double bottom = (viewer.VerticalOffset() + viewer.ViewportHeight()) / extent;
            double pad = (bottom - top) / 2;
            top = std::clamp(top - pad, 0.0, 1.0);
            bottom = std::clamp(bottom + pad, 0.0, 1.0);

            // Lines are all the same height, so that fraction of the height is that fraction of the lines.
            // With word wrap a long line is taller than a short one, so also take the same fraction of the
            // characters, and colour everything either estimate covers.
            int lines = LineCount(text);
            auto lineFrom = static_cast<size_t>(IndexOfLine(text, static_cast<int>(top * lines) + 1));
            auto lineTo = static_cast<size_t>(IndexOfLine(text, static_cast<int>(std::ceil(bottom * lines)) + 2));
            auto charFrom = static_cast<size_t>(top * text.size());
            auto charTo = static_cast<size_t>(std::ceil(bottom * text.size()));
            from = std::min(lineFrom, charFrom);
            from = from > Margin ? from - Margin : 0;
            to = std::min(desired.size(), std::max(lineTo, charTo) + Margin);
        }

        std::optional<FormattingScope> scope;
        for (size_t i = from; i < to;)
        {
            if (applied[i] == desired[i])
            {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < desired.size() && desired[j] == desired[i] && applied[j] != desired[j])
            {
                ++j;
            }
            if (!scope)
            {
                scope.emplace(document);
            }
            document.GetRange(static_cast<int>(i), static_cast<int>(j)).CharacterFormat().ForegroundColor(TokenColor(desired[i]));
            std::fill(applied.begin() + i, applied.begin() + j, desired[i]);
            i = j;
        }
    }

    void MainWindow::ScheduleRepaint(std::shared_ptr<Document> const& doc)
    {
        if (doc->RepaintPending)
        {
            return;
        }
        doc->RepaintPending = true;
        // Low priority so that layout has caught up with the scroll or resize that triggered this.
        DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this, strong = get_strong(), weakDoc = std::weak_ptr<Document>(doc)]
        {
            if (auto d = weakDoc.lock())
            {
                d->RepaintPending = false;
                if (!d->RefreshPending) // otherwise the refresh repaints
                {
                    PaintVisibleText(d);
                }
                if (d == CurrentDocument())
                {
                    UpdateEditorOverlay(); // after layout, so it matches the text
                }
            }
        });
    }

    // ---------------------------------------------------------------------------------------------
    // Visible whitespace
    // ---------------------------------------------------------------------------------------------

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

    // ---------------------------------------------------------------------------------------------
    // Tabs and indentation
    // ---------------------------------------------------------------------------------------------

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

    // ---------------------------------------------------------------------------------------------
    // Undo
    // ---------------------------------------------------------------------------------------------

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

    void MainWindow::SetLanguage(std::shared_ptr<Document> const& doc, syntax::LanguageId language)
    {
        doc->Language = language;
        UpdateHighlighting(doc, false);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
    }

    void MainWindow::BuildLanguageMenus()
    {
        m_languageItems.clear();
        auto build = [this](Collections::IVector<MenuFlyoutItemBase> const& items, hstring const& group)
        {
            items.Clear();
            for (syntax::LanguageId language = 0; language < syntax::LanguageCount(); ++language)
            {
                RadioMenuFlyoutItem item;
                item.Text(syntax::LanguageName(language));
                item.GroupName(group);
                item.Click([this, language](auto&&, auto&&)
                {
                    if (auto doc = CurrentDocument())
                    {
                        SetLanguage(doc, language);
                        FocusEditor(doc);
                    }
                });
                items.Append(item);
                m_languageItems.emplace_back(item, language);
                if (language == syntax::PlainText)
                {
                    items.Append(MenuFlyoutSeparator{}); // plain text, then the languages
                }
            }
        };
        build(LanguageMenu().Items(), L"LanguageMenu");
        build(LanguageFlyout().Items(), L"LanguageStatus");

        // Commands for people editing the definitions.
        auto command = [this](wchar_t const* text, std::function<void()> action)
        {
            MenuFlyoutItem item;
            item.Text(text);
            item.Click([action](auto&&, auto&&) { action(); });
            LanguageMenu().Items().Append(item);
        };
        LanguageMenu().Items().Append(MenuFlyoutSeparator{});
        command(L"Open language folder", [this] { OpenLanguageFolder(); });
        command(L"Reload languages", [this] { ReloadLanguages(); });
    }

    void MainWindow::ReloadLanguages()
    {
        // Ids can change, so remember each document's language by name.
        std::vector<std::wstring> names;
        for (auto const& doc : m_documents)
        {
            names.emplace_back(syntax::LanguageName(doc->Language));
        }

        m_languageErrors = syntax::LoadLanguages(LanguageFolders());
        BuildLanguageMenus();
        for (size_t i = 0; i < m_documents.size(); ++i)
        {
            m_documents[i]->Language = syntax::FindLanguage(names[i]);
            UpdateHighlighting(m_documents[i], false);
        }
        UpdateStatusBar();

        if (!m_languageErrors.empty())
        {
            ShowLoadErrors();
        }
        else
        {
            StatusMessageText().Text(std::format(L"Loaded {} languages", syntax::LanguageCount() - 1));
        }
    }

    void MainWindow::OpenLanguageFolder()
    {
        // The per-user folder, where added and customised definitions go.
        auto folder = UserLanguageFolder();
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        ShellExecuteW(WindowHandle(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::ShowLoadErrors()
    {
        // Language and theme files together, as only one message can be shown at a time.
        std::wstring message;
        auto list = [&](wchar_t const* heading, std::vector<std::wstring> const& errors)
        {
            if (errors.empty())
            {
                return;
            }
            message += message.empty() ? heading : std::wstring(L"\n\n") + heading;
            for (auto const& error : errors)
            {
                message += L"\n\x2022 " + error;
            }
        };
        list(L"These language definitions couldn't be loaded:\n", m_languageErrors);
        list(L"These colour themes couldn't be loaded:\n", m_themeErrors);
        if (!message.empty())
        {
            ShowMessageAsync(L"Language and theme files", hstring{ message });
        }
    }

    void MainWindow::BuildColorThemeMenu()
    {
        auto items = ColorThemeMenu().Items();
        items.Clear();
        m_themeItems.clear();
        for (int theme = 0; theme < themes::ThemeCount(); ++theme)
        {
            RadioMenuFlyoutItem item;
            item.Text(themes::ThemeName(theme));
            item.GroupName(L"ColorTheme");
            item.Click([this, theme](auto&&, auto&&) { SetColorTheme(std::wstring{ themes::ThemeName(theme) }); });
            items.Append(item);
            m_themeItems.emplace_back(item, theme);
        }

        // Commands for people editing the themes.
        auto command = [&](wchar_t const* text, std::function<void()> action)
        {
            MenuFlyoutItem item;
            item.Text(text);
            item.Click([action](auto&&, auto&&) { action(); });
            items.Append(item);
        };
        items.Append(MenuFlyoutSeparator{});
        command(L"Open themes folder", [this] { OpenThemeFolder(); });
        command(L"Reload themes", [this] { ReloadThemes(); });
    }

    void MainWindow::SetColorTheme(std::wstring const& name)
    {
        m_colorTheme = name;
        ApplyColorTheme();
        SaveSettings();
        if (auto doc = CurrentDocument())
        {
            FocusEditor(doc);
        }
    }

    void MainWindow::ApplyColorTheme()
    {
        // The palette depends on whether the app is light or dark, so this also runs when that changes.
        int theme = themes::FindTheme(m_colorTheme);
        m_palette = themes::ThemePalette(theme, IsDarkTheme());
        if (m_palette.Background)
        {
            EditorHost().Background(SolidColorBrush{ *m_palette.Background });
        }
        else
        {
            EditorHost().ClearValue(Panel::BackgroundProperty()); // the app's own background
        }
        for (auto const& [item, index] : m_themeItems)
        {
            item.IsChecked(index == theme);
        }
        for (auto const& doc : m_documents)
        {
            ApplyEditorSettings(doc);
            UpdateHighlighting(doc, true);
        }
    }

    Windows::UI::Color MainWindow::TokenColor(TokenKind kind)
    {
        auto index = static_cast<size_t>(kind);
        return m_palette.Colors[index < m_palette.Colors.size() ? index : 0];
    }

    void MainWindow::ReloadThemes()
    {
        m_themeErrors = themes::LoadThemes(ThemeFolders());
        BuildColorThemeMenu();
        ApplyColorTheme();
        if (!m_themeErrors.empty())
        {
            ShowLoadErrors();
        }
        else
        {
            StatusMessageText().Text(std::format(L"Loaded {} colour themes", themes::ThemeCount()));
        }
    }

    void MainWindow::OpenThemeFolder()
    {
        // The per-user folder, where added and customised themes go.
        auto folder = UserThemeFolder();
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        ShellExecuteW(WindowHandle(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
            std::wstring selected = SelectedText(doc->Editor);
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

        std::wstring haystack = EditorText(doc->Editor);
        if (!IsChecked(MatchCaseCheck()))
        {
            haystack = ToLower(std::move(haystack));
            needle = ToLower(std::move(needle));
        }

        bool wrap = IsChecked(WrapAroundCheck());
        size_t start = static_cast<size_t>(SelectionStart(doc->Editor));
        size_t length = static_cast<size_t>(std::max(SelectionLength(doc->Editor), 0));
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
        SelectAndReveal(doc->Editor, doc->ScrollViewer, static_cast<int>(pos), static_cast<int>(needle.size()));
        doc->Editor.Focus(FocusState::Programmatic);
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

        std::wstring selected = SelectedText(doc->Editor);
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
        std::wstring text = EditorText(doc->Editor);
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
        auto text = EditorText(doc->Editor);
        int lineCount = LineCount(text);
        int currentLine = LineColumnAt(text, SelectionStart(doc->Editor)).first;

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
        SelectAndReveal(doc->Editor, doc->ScrollViewer, IndexOfLine(EditorText(doc->Editor), line), 0);
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
        UpdateFontMetrics();
        ApplyEditorSettingsToAll();
        UpdateWhitespaceAvailability();
        SaveSettings();
    }

    fire_and_forget MainWindow::ShowAboutAsync()
    {
        auto strong = get_strong();
        ContentDialog dialog;
        dialog.Title(box_value(L"About RadPad"));
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
    // Settings (%LOCALAPPDATA%\RadPad\settings.ini)
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

        WritePrivateProfileStringW(L"RecentFiles", nullptr, nullptr, path.c_str()); // remove the old list
        for (size_t i = 0; i < m_recentFiles.size(); ++i)
        {
            WritePrivateProfileStringW(L"RecentFiles", std::format(L"File{}", i + 1).c_str(), m_recentFiles[i].c_str(), path.c_str());
        }
    }
}
