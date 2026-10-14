#include "pch.h"
#include "TextFile.h"

namespace textfile
{
    namespace
    {
        constexpr uint64_t MaxFileSize = 256ull * 1024 * 1024;

        std::optional<std::wstring> Decode(UINT codePage, std::string_view bytes, DWORD flags)
        {
            if (bytes.empty())
            {
                return std::wstring{};
            }

            int length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (length == 0)
            {
                return std::nullopt;
            }

            std::wstring text(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
            return text;
        }

        std::wstring DecodeUtf16(std::string_view bytes, bool bigEndian)
        {
            std::wstring text(bytes.size() / 2, L'\0');
            for (size_t i = 0; i < text.size(); ++i)
            {
                auto first = static_cast<unsigned char>(bytes[2 * i]);
                auto second = static_cast<unsigned char>(bytes[2 * i + 1]);
                text[i] = bigEndian ? static_cast<wchar_t>((first << 8) | second)
                                    : static_cast<wchar_t>((second << 8) | first);
            }
            return text;
        }

        std::string Encode(UINT codePage, std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }

            int length = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (length == 0)
            {
                winrt::throw_last_error();
            }

            std::string bytes(static_cast<size_t>(length), '\0');
            WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr, nullptr);
            return bytes;
        }

        void AppendUtf16(std::string& bytes, std::wstring_view text, bool bigEndian)
        {
            bytes.reserve(bytes.size() + text.size() * 2);
            for (wchar_t c : text)
            {
                char high = static_cast<char>((c >> 8) & 0xFF);
                char low = static_cast<char>(c & 0xFF);
                bytes.push_back(bigEndian ? high : low);
                bytes.push_back(bigEndian ? low : high);
            }
        }

        LineEnding DetectLineEnding(std::wstring_view text)
        {
            auto pos = text.find_first_of(L"\r\n");
            if (pos == std::wstring_view::npos)
            {
                return LineEnding::CRLF;
            }
            if (text[pos] == L'\n')
            {
                return LineEnding::LF;
            }
            return pos + 1 < text.size() && text[pos + 1] == L'\n' ? LineEnding::CRLF : LineEnding::CR;
        }

        std::wstring ConvertLineBreaks(std::wstring_view text, LineEnding eol)
        {
            std::wstring_view lineBreak = eol == LineEnding::CRLF ? L"\r\n" : eol == LineEnding::LF ? L"\n" : L"\r";
            std::wstring result;
            result.reserve(text.size() + text.size() / 16);
            for (size_t i = 0; i < text.size(); ++i)
            {
                wchar_t c = text[i];
                if (c == L'\r' || c == L'\n')
                {
                    if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
                    {
                        ++i;
                    }
                    result.append(lineBreak);
                }
                else
                {
                    result.push_back(c);
                }
            }
            return result;
        }
    }

    std::wstring_view EncodingName(TextEncoding encoding)
    {
        switch (encoding)
        {
        case TextEncoding::Utf8: return L"UTF-8";
        case TextEncoding::Utf8Bom: return L"UTF-8 with BOM";
        case TextEncoding::Utf16LE: return L"UTF-16 LE";
        case TextEncoding::Utf16BE: return L"UTF-16 BE";
        case TextEncoding::Ansi: return L"ANSI";
        }
        return L"";
    }

    std::wstring_view LineEndingName(LineEnding eol)
    {
        switch (eol)
        {
        case LineEnding::CRLF: return L"Windows (CRLF)";
        case LineEnding::LF: return L"Unix (LF)";
        case LineEnding::CR: return L"Macintosh (CR)";
        }
        return L"";
    }

    std::wstring NormalizeLineBreaks(std::wstring_view text)
    {
        std::wstring result;
        result.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            wchar_t c = text[i];
            if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
            {
                ++i;
            }
            result.push_back(c == L'\n' ? L'\r' : c);
        }
        return result;
    }

    LoadedText Load(std::wstring const& path)
    {
        winrt::file_handle file{ CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!file)
        {
            winrt::throw_last_error();
        }

        LARGE_INTEGER size{};
        winrt::check_bool(GetFileSizeEx(file.get(), &size));
        if (static_cast<uint64_t>(size.QuadPart) > MaxFileSize)
        {
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), L"The file is too large to open.");
        }

        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        size_t total = 0;
        while (total < bytes.size())
        {
            DWORD read = 0;
            winrt::check_bool(ReadFile(file.get(), bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &read, nullptr));
            if (read == 0)
            {
                break;
            }
            total += read;
        }
        bytes.resize(total);

        LoadedText result;
        std::string_view view = bytes;
        if (view.starts_with("\xEF\xBB\xBF"))
        {
            result.Encoding = TextEncoding::Utf8Bom;
            result.Text = Decode(CP_UTF8, view.substr(3), 0).value_or(L"");
        }
        else if (view.starts_with("\xFF\xFE"))
        {
            result.Encoding = TextEncoding::Utf16LE;
            result.Text = DecodeUtf16(view.substr(2), false);
        }
        else if (view.starts_with("\xFE\xFF"))
        {
            result.Encoding = TextEncoding::Utf16BE;
            result.Text = DecodeUtf16(view.substr(2), true);
        }
        else if (auto utf8 = Decode(CP_UTF8, view, MB_ERR_INVALID_CHARS))
        {
            result.Encoding = TextEncoding::Utf8;
            result.Text = std::move(*utf8);
        }
        else
        {
            result.Encoding = TextEncoding::Ansi;
            result.Text = Decode(CP_ACP, view, 0).value_or(L"");
        }

        result.Eol = DetectLineEnding(result.Text);
        result.Text = NormalizeLineBreaks(result.Text);
        return result;
    }

    void Save(std::wstring const& path, std::wstring_view text, TextEncoding encoding, LineEnding eol)
    {
        std::wstring converted = ConvertLineBreaks(text, eol);

        std::string bytes;
        switch (encoding)
        {
        case TextEncoding::Utf8Bom:
            bytes = "\xEF\xBB\xBF";
            bytes += Encode(CP_UTF8, converted);
            break;
        case TextEncoding::Utf8:
            bytes = Encode(CP_UTF8, converted);
            break;
        case TextEncoding::Utf16LE:
            bytes = "\xFF\xFE";
            AppendUtf16(bytes, converted, false);
            break;
        case TextEncoding::Utf16BE:
            bytes = "\xFE\xFF";
            AppendUtf16(bytes, converted, true);
            break;
        case TextEncoding::Ansi:
            bytes = Encode(CP_ACP, converted);
            break;
        }

        winrt::file_handle file{ CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                             FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!file)
        {
            winrt::throw_last_error();
        }

        DWORD written = 0;
        winrt::check_bool(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
    }
}
