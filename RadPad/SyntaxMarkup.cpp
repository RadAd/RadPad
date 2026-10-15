#include "pch.h"
#include "SyntaxInternal.h"

// XML and HTML (with embedded script and style languages), and CSS.
namespace syntax::detail
{
    namespace
    {
        bool IsNameStart(wchar_t c) { return std::iswalpha(c) || c == L'_' || c == L':'; }
        bool IsNameChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L':' || c == L'-' || c == L'.'; }
        bool IsCssNameStart(wchar_t c) { return std::iswalpha(c) || c == L'_'; }
        bool IsCssNameChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L'-'; }
    }

    // -----------------------------------------------------------------------------------------
    // XML and HTML
    // -----------------------------------------------------------------------------------------

    void ClassifyMarkup(Scanner const& s, LanguageDefinition const& lang)
    {
        auto const text = s.Text;
        size_t const n = text.size();
        size_t i = 0;
        while (i < n)
        {
            wchar_t c = text[i];
            if (c == L'&')
            {
                // Entity reference such as &amp; or &#160;
                size_t j = i + 1;
                while (j < n && j - i <= 10 && (std::iswalnum(text[j]) || text[j] == L'#'))
                {
                    ++j;
                }
                if (j > i + 1 && s.At(j) == L';')
                {
                    s.Fill(i, j + 1, TokenKind::Variable);
                    i = j + 1;
                }
                else
                {
                    ++i;
                }
                continue;
            }
            if (c != L'<')
            {
                ++i;
                continue;
            }

            if (s.StartsWith(i, L"<!--"))
            {
                size_t end = s.FindEnd(i + 4, L"-->");
                s.Fill(i, end, TokenKind::Comment);
                i = end;
                continue;
            }
            if (s.StartsWith(i, L"<![CDATA["))
            {
                size_t close = text.find(L"]]>", i + 9);
                size_t end = close == npos ? n : close;
                s.Fill(i, i + 9, TokenKind::Keyword);
                s.Fill(end, end + 3, TokenKind::Keyword);
                i = std::min(end + 3, n);
                continue;
            }
            if (s.At(i + 1) == L'?' || s.At(i + 1) == L'!')
            {
                // <?xml ... ?> and <!DOCTYPE ...>
                size_t end = s.FindEnd(i + 2, L">");
                s.Fill(i, end, TokenKind::Preprocessor);
                i = end;
                continue;
            }

            size_t j = i + 1;
            bool closing = s.At(j) == L'/';
            if (closing)
            {
                ++j;
            }
            if (!IsNameStart(s.At(j)))
            {
                ++i;
                continue;
            }
            s.Fill(i, j, TokenKind::Keyword);
            size_t nameStart = j;
            while (IsNameChar(s.At(j)))
            {
                ++j;
            }
            s.Fill(nameStart, j, TokenKind::Tag);
            std::wstring name = Lower(text.substr(nameStart, j - nameStart));

            bool selfClosing = false;
            while (j < n)
            {
                wchar_t d = text[j];
                if (d == L'>')
                {
                    s.Fill(j, j + 1, TokenKind::Keyword);
                    ++j;
                    break;
                }
                if (d == L'/' && s.At(j + 1) == L'>')
                {
                    s.Fill(j, j + 2, TokenKind::Keyword);
                    j += 2;
                    selfClosing = true;
                    break;
                }
                if (d == L'<')
                {
                    break; // unterminated tag
                }
                if (d == L'"' || d == L'\'')
                {
                    size_t end = s.FindEnd(j + 1, text.substr(j, 1));
                    s.Fill(j, end, TokenKind::String);
                    j = end;
                    continue;
                }
                if (IsNameStart(d))
                {
                    size_t start = j;
                    while (IsNameChar(s.At(j)))
                    {
                        ++j;
                    }
                    s.Fill(start, j, TokenKind::Attribute);
                    continue;
                }
                ++j;
            }
            i = j;

            // Colour the contents of <script> and <style> elements in the languages the definition names.
            auto const& embedded = name == L"script" ? lang.ScriptLanguage : name == L"style" ? lang.StyleLanguage : std::wstring{};
            if (!closing && !selfClosing && !embedded.empty())
            {
                size_t end = FindIgnoreCase(text, L"</" + name, i);
                ClassifyInto(Languages().Languages[FindLanguage(embedded)], text.substr(i, end - i), s.Kinds + i);
                i = end;
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // CSS
    // -----------------------------------------------------------------------------------------

    void ClassifyCss(Scanner const& s)
    {
        auto const text = s.Text;
        size_t const n = text.size();
        int depth = 0; // brace depth; 0 means we're in a selector
        size_t i = 0;
        auto nameEnd = [&](size_t j)
        {
            while (IsCssNameChar(s.At(j)))
            {
                ++j;
            }
            return j;
        };

        while (i < n)
        {
            wchar_t c = text[i];
            wchar_t next = s.At(i + 1);
            if (s.StartsWith(i, L"/*"))
            {
                size_t end = s.FindEnd(i + 2, L"*/");
                s.Fill(i, end, TokenKind::Comment);
                i = end;
                continue;
            }
            if (c == L'"' || c == L'\'')
            {
                size_t end = s.ScanString(i, c, L'\\', false);
                s.Fill(i, end, TokenKind::String);
                i = end;
                continue;
            }
            if (c == L'{' || c == L'}')
            {
                depth = c == L'{' ? depth + 1 : std::max(0, depth - 1);
                ++i;
                continue;
            }
            if (c == L'@' && IsCssNameStart(next))
            {
                size_t end = nameEnd(i + 1);
                s.Fill(i, end, TokenKind::Keyword);
                i = end;
                continue;
            }
            if (c == L'!' && s.StartsWith(i, L"!important"))
            {
                s.Fill(i, i + 10, TokenKind::Keyword);
                i += 10;
                continue;
            }

            if (depth == 0)
            {
                if ((c == L'.' || c == L'#') && (IsCssNameStart(next) || next == L'-'))
                {
                    size_t end = nameEnd(i + 1);
                    s.Fill(i, end, TokenKind::Attribute);
                    i = end;
                }
                else if (c == L':')
                {
                    size_t end = nameEnd(next == L':' ? i + 2 : i + 1);
                    s.Fill(i, end, TokenKind::Keyword);
                    i = std::max(end, i + 1);
                }
                else if (IsCssNameStart(c))
                {
                    size_t end = nameEnd(i);
                    s.Fill(i, end, TokenKind::Tag);
                    i = end;
                }
                else
                {
                    ++i;
                }
                continue;
            }

            if (c == L'#' && std::iswxdigit(next))
            {
                size_t end = nameEnd(i + 1);
                s.Fill(i, end, TokenKind::Number);
                i = end;
                continue;
            }
            if (IsDigit(c) || (c == L'.' && IsDigit(next)) || (c == L'-' && (IsDigit(next) || next == L'.')))
            {
                size_t j = i + 1;
                while (std::iswalnum(s.At(j)) || s.At(j) == L'.' || s.At(j) == L'%')
                {
                    ++j;
                }
                s.Fill(i, j, TokenKind::Number);
                i = j;
                continue;
            }
            if (IsCssNameStart(c) || (c == L'-' && (IsCssNameStart(next) || next == L'-')))
            {
                size_t end = nameEnd(i + 1);
                size_t j = end;
                while (IsBlank(s.At(j)))
                {
                    ++j;
                }
                if (s.At(j) == L':')
                {
                    s.Fill(i, end, TokenKind::Property);
                }
                i = end;
                continue;
            }
            ++i;
        }
    }
}
