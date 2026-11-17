#pragma once

// For the MainWindow*.cpp files only: the namespaces they use, and the constants and helper functions they
// share. MainWindow is split across files by area; see MainWindow.xaml.cpp.
#include "MainWindow.xaml.h"
#include "Instance.h"
#include "Version.h"
#include <filesystem>
#include <mutex>

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

namespace mainwindow
{
    constexpr std::wstring_view AppName = L"RadPad";
    constexpr int DefaultZoom = 100;
    constexpr int MinZoom = 10;
    constexpr int MaxZoom = 500;
    constexpr int ZoomStep = 10;
    constexpr double DefaultFontPoints = 11.0;
    constexpr double EditorPadding = 12;     // around the text
    constexpr double WrapMarkerWidth = 14;   // extra right padding for the wrap markers, left of the scrollbar

    inline bool IsKeyDown(int virtualKey)
    {
        return (GetKeyState(virtualKey) & 0x8000) != 0;
    }

    inline bool PathsEqual(std::wstring const& a, std::wstring const& b)
    {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    inline void CopyTextToClipboard(std::wstring const& text)
    {
        DataPackage package;
        package.SetText(text);
        Clipboard::SetContent(package);
        Clipboard::Flush(); // keep the text after the app closes
    }

    // co_await ResumeOn(queue) continues on the queue's thread, after anything already queued there.
    inline auto ResumeOn(Microsoft::UI::Dispatching::DispatcherQueue const& queue)
    {
        struct Awaiter
        {
            Microsoft::UI::Dispatching::DispatcherQueue Queue;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> resume) const { Queue.TryEnqueue([resume] { resume(); }); }
            void await_resume() const noexcept {}
        };
        return Awaiter{ queue };
    }

    inline std::wstring FullPath(std::wstring const& path)
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
    // Menu icons for items built in code, from the Segoe Fluent Icons font like those in MainWindow.xaml.
    constexpr wchar_t FolderGlyph = L'\xE838';
    constexpr wchar_t RefreshGlyph = L'\xE72C';
    constexpr wchar_t EditGlyph = L'\xE70F';

    inline FontIcon MenuIcon(wchar_t glyph)
    {
        FontIcon icon;
        icon.Glyph(std::wstring(1, glyph));
        return icon;
    }

    inline std::wstring ToLower(std::wstring text)
    {
        if (!text.empty())
        {
            CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
        }
        return text;
    }

    // Regular expression search works on a copy of the editor text with '\n' line breaks (the editor
    // uses '\r'), so \n in a pattern matches a line break. The copy has the same length, so positions match.
    inline std::wstring ForRegex(std::wstring text)
    {
        std::replace(text.begin(), text.end(), L'\r', L'\n');
        return text;
    }

    // A replacement for a regular expression match: \n and \t become a line break and a tab, \\ a
    // backslash; $1, $& and $$ are left for std::regex to fill in.
    inline std::wstring ReplacementWithEscapes(std::wstring_view text)
    {
        std::wstring result;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'\\' && i + 1 < text.size())
            {
                wchar_t next = text[i + 1];
                if (next == L'n' || next == L't' || next == L'\\')
                {
                    result += next == L'n' ? L'\n' : next == L't' ? L'\t' : L'\\';
                    ++i;
                    continue;
                }
            }
            result += text[i];
        }
        return result;
    }

    // Text with the characters that have a meaning in a regular expression escaped, so it matches itself.
    inline std::wstring EscapeRegex(std::wstring_view text)
    {
        std::wstring result;
        for (wchar_t c : text)
        {
            if (std::wstring_view{ L"\\^$.|?*+()[]{}/" }.find(c) != std::wstring_view::npos)
            {
                result += L'\\';
            }
            result += c;
        }
        return result;
    }

    // A readable explanation of a regex error, without the "regex_error(error_paren): " prefix.
    inline std::wstring RegexErrorText(std::regex_error const& e)
    {
        if (e.code() == std::regex_constants::error_stack || e.code() == std::regex_constants::error_complexity)
        {
            return L"the pattern is too complex to search this text with";
        }
        std::string what = e.what();
        size_t colon = what.find("): ");
        what = colon == std::string::npos ? what : what.substr(colon + 3);
        return std::wstring(what.begin(), what.end());
    }

    // The first match at or after from, as (position, length); the text before from still counts for ^ and \b.
    inline std::optional<std::pair<size_t, size_t>> RegexSearch(std::wstring const& text, std::wregex const& pattern, size_t from)
    {
        if (from > text.size())
        {
            return std::nullopt;
        }
        std::wsmatch match;
        auto flags = from > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
        if (!std::regex_search(text.cbegin() + from, text.cend(), match, pattern, flags))
        {
            return std::nullopt;
        }
        return std::pair{ from + static_cast<size_t>(match.position(0)), static_cast<size_t>(match.length(0)) };
    }

    inline bool IsChecked(Primitives::ToggleButton const& toggle)
    {
        auto value = toggle.IsChecked();
        return value && value.Value();
    }

    inline std::wstring CurrentTimeDate()
    {
        wchar_t time[64]{};
        wchar_t date[64]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, time, ARRAYSIZE(time));
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, date, ARRAYSIZE(date), nullptr);
        return std::wstring(time) + L" " + date;
    }

    // The editor's text. RichEditBox always ends its text with a paragraph mark that isn't part of the document.
    inline std::wstring EditorText(RichEditBox const& editor)
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

    inline int SelectionStart(RichEditBox const& editor)
    {
        return editor.Document().Selection().StartPosition();
    }

    inline int SelectionLength(RichEditBox const& editor)
    {
        auto selection = editor.Document().Selection();
        return selection.EndPosition() - selection.StartPosition();
    }

    inline std::wstring SelectedText(RichEditBox const& editor)
    {
        hstring text;
        editor.Document().Selection().GetText(TextGetOptions::None, text);
        return std::wstring{ text };
    }

    inline void Select(RichEditBox const& editor, int start, int length)
    {
        editor.Document().Selection().SetRange(start, start + length);
    }

    // Copies as plain text with CRLF line breaks, whatever the selection's formatting.
    inline void CopySelection(RichEditBox const& editor)
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

    // Lets code change a read-only editor's text or formatting, which RichEditBox otherwise blocks.
    class WritableScope
    {
    public:
        explicit WritableScope(RichEditBox const& editor) : m_editor(editor), m_readOnly(editor.IsReadOnly())
        {
            if (m_readOnly)
            {
                m_editor.IsReadOnly(false);
            }
        }

        ~WritableScope()
        {
            if (m_readOnly)
            {
                m_editor.IsReadOnly(true);
            }
        }

        WritableScope(WritableScope const&) = delete;
        WritableScope& operator=(WritableScope const&) = delete;

    private:
        RichEditBox m_editor;
        bool m_readOnly;
    };

    // Batches formatting changes so the editor redraws once at the end.
    class FormattingScope
    {
    public:
        explicit FormattingScope(RichEditBox const& editor) : m_writable(editor), m_document(editor.Document())
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
        WritableScope m_writable;
        RichEditTextDocument m_document;
    };

    // The first descendant of type T in the visual tree, or nullptr.
    template <typename T>
    inline T FindDescendant(DependencyObject const& root)
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

    inline Primitives::ScrollBar FindVerticalScrollBar(DependencyObject const& root)
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
    inline Windows::Foundation::Rect CharRectInView(RichEditTextDocument const& document, int position, int length, ScrollViewer const& viewer)
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
    inline Microsoft::UI::Text::ITextRange FirstVisibleRow(RichEditTextDocument const& document, int length, ScrollViewer const& viewer)
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
    inline std::pair<bool, bool> OutOfView(RichEditTextDocument const& document, int position, ScrollViewer const& viewer)
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
    inline void CentreInView(RichEditBox const& editor, ScrollViewer const& viewer, int position, std::pair<bool, bool> outOfView)
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
    inline void SelectAndReveal(RichEditBox const& editor, ScrollViewer const& viewer, int start, int length)
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
    inline std::optional<FontMetrics> GetFontMetrics(std::wstring const& family, bool bold, bool italic)
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

    inline bool IsSpace(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r';
    }

    // Line and column (both 1-based) of a character index. Editor text uses '\r' for every line break.
    inline std::pair<int, int> LineColumnAt(std::wstring_view text, int index)
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

    inline int LineCount(std::wstring_view text)
    {
        return 1 + static_cast<int>(std::count(text.begin(), text.end(), L'\r'));
    }

    inline int IndexOfLine(std::wstring_view text, int line)
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

    // %LOCALAPPDATA%\RadPad, where settings and the user's language definitions are kept. When installed
    // from the Store, the package's own LocalState folder instead (%LOCALAPPDATA%\Packages\<package>\LocalState),
    // which Windows deletes on uninstall; writes to %LOCALAPPDATA% itself would be silently redirected.
    inline std::wstring AppDataFolder()
    {
        if (instance::IsPackaged())
        {
            try
            {
                return std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
            }
            catch (winrt::hresult_error const&)
            {
                return {};
            }
        }
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

    inline std::wstring SettingsFilePath()
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
    inline std::wstring UserLanguageFolder()
    {
        std::wstring directory = AppDataFolder();
        return directory.empty() ? std::wstring{} : directory + L"\\Languages";
    }

    // %LOCALAPPDATA%\RadPad\tools.json: the user's external tools (Tools menu).
    inline std::wstring ToolsFilePath()
    {
        std::wstring directory = AppDataFolder();
        return directory.empty() ? std::wstring{} : directory + L"\\tools.json";
    }

    // A file's last write time and size, to tell when it changes; nothing if it doesn't exist.
    inline std::optional<std::pair<uint64_t, uint64_t>> FileStamp(std::wstring const& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            return std::nullopt;
        }
        return std::pair{ (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
                          (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow };
    }

    // Whether a file has the read-only attribute; false if it doesn't exist.
    inline bool IsReadOnlyFile(std::wstring const& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    }

    // Whether the user's permissions allow writing to a file, going by its security rather than its read-only
    // attribute. Assumed so where that can't be told, as on a drive without permissions.
    inline bool CanWriteFile(std::wstring const& path)
    {
        constexpr SECURITY_INFORMATION parts = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
        DWORD size = 0;
        GetFileSecurityW(path.c_str(), parts, nullptr, 0, &size);
        std::vector<BYTE> descriptor(size);
        if (size == 0 || !GetFileSecurityW(path.c_str(), parts, descriptor.data(), size, &size))
        {
            return true;
        }

        // AccessCheck needs an impersonation token.
        winrt::handle processToken;
        winrt::handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, processToken.put())
            || !DuplicateToken(processToken.get(), SecurityImpersonation, token.put()))
        {
            return true;
        }
        GENERIC_MAPPING mapping{ FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
        DWORD access = GENERIC_WRITE;
        MapGenericMask(&access, &mapping);
        PRIVILEGE_SET privileges{};
        DWORD privilegesSize = sizeof(privileges);
        DWORD granted = 0;
        BOOL allowed = FALSE;
        if (!AccessCheck(descriptor.data(), token.get(), access, &mapping, &privileges, &privilegesSize, &granted, &allowed))
        {
            return true;
        }
        return allowed != FALSE;
    }

    // %LOCALAPPDATA%\RadPad\Themes: colour themes added or customised by the user.
    inline std::wstring UserThemeFolder()
    {
        std::wstring directory = AppDataFolder();
        return directory.empty() ? std::wstring{} : directory + L"\\Themes";
    }

    // Where language definitions are loaded from: the Languages folder installed beside the exe, then
    // the user's folder, whose definitions override installed ones with the same name.
    inline std::wstring ExePath()
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

    // %LOCALAPPDATA%\Microsoft\WindowsApps\radpad.exe: the command the MSIX package declares (see
    // installer\msix\AppxManifest.xml), which starts RadPad with the package's identity.
    inline std::wstring ExecutionAliasPath()
    {
        PWSTR folder = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        {
            return {};
        }
        std::wstring path = std::wstring(folder) + L"\\Microsoft\\WindowsApps\\radpad.exe";
        CoTaskMemFree(folder);
        return path;
    }

    inline std::vector<std::wstring> LanguageFolders()
    {
        auto exe = ExePath();
        std::wstring appFolder = exe.substr(0, exe.find_last_of(L'\\'));
        return { appFolder + L"\\Languages", UserLanguageFolder() };
    }

    // Where colour themes are loaded from, as for languages: beside the exe, then the user's folder.
    inline std::vector<std::wstring> ThemeFolders()
    {
        auto exe = ExePath();
        std::wstring appFolder = exe.substr(0, exe.find_last_of(L'\\'));
        return { appFolder + L"\\Themes", UserThemeFolder() };
    }

    inline std::vector<std::wstring> GetSystemFontFamilies()
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

    inline std::vector<std::wstring> ShowOpenFileDialog(HWND owner)
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
    inline std::optional<SaveDialogResult> ShowSaveFileDialog(HWND owner, std::wstring const& currentPath, std::wstring const& suggestedName, TextEncoding encoding, syntax::LanguageId language)
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

using namespace mainwindow;
