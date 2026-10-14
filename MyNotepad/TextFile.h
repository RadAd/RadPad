#pragma once

// Loading and saving of plain-text files with encoding and line-ending detection.
// Text handed to and returned from these functions uses '\r' as its only line break,
// which is how the XAML text controls store text internally.
namespace textfile
{
    enum class TextEncoding
    {
        Utf8,
        Utf8Bom,
        Utf16LE,
        Utf16BE,
        Ansi,
    };

    enum class LineEnding
    {
        CRLF,
        LF,
        CR,
    };

    constexpr int EncodingCount = 5;

    struct LoadedText
    {
        std::wstring Text;
        TextEncoding Encoding{ TextEncoding::Utf8 };
        LineEnding Eol{ LineEnding::CRLF };
    };

    std::wstring_view EncodingName(TextEncoding encoding);
    std::wstring_view LineEndingName(LineEnding eol);

    // Converts every CRLF, LF and CR in the text to a single '\r'.
    std::wstring NormalizeLineBreaks(std::wstring_view text);

    // Throws winrt::hresult_error on failure.
    LoadedText Load(std::wstring const& path);
    void Save(std::wstring const& path, std::wstring_view text, TextEncoding encoding, LineEnding eol);
}
