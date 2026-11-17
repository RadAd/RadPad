#include "pch.h"
#include "SyntaxInternal.h"

// C-like and scripting languages, driven by the rules in their definitions.
namespace syntax::detail
{
    namespace
    {
        // Colours the lines after an #if 0 line (ending at lineEnd) as a comment, up to the line with the
        // matching #else, #elif or #endif, allowing for nested #if blocks. Returns the line break before
        // that line, so the scanner sees it as the start of a line and colours the directive normally.
        size_t SkipInactiveBlock(Scanner const& s, size_t lineEnd)
        {
            int depth = 0;
            for (size_t line = lineEnd + 1; line < s.Size(); line = s.LineEnd(line) + 1)
            {
                size_t k = line;
                while (IsBlank(s.At(k)))
                {
                    ++k;
                }
                if (s.At(k) != L'#')
                {
                    continue;
                }
                ++k;
                while (IsBlank(s.At(k)))
                {
                    ++k;
                }
                size_t wordEnd = k;
                while (IsIdentChar(s.At(wordEnd)))
                {
                    ++wordEnd;
                }
                auto word = s.Text.substr(k, wordEnd - k);
                if (word.starts_with(L"if"))
                {
                    ++depth; // #if, #ifdef, #ifndef
                }
                else if (word == L"endif" && depth > 0)
                {
                    --depth;
                }
                else if (depth == 0 && (word == L"endif" || word == L"else" || word.starts_with(L"elif")))
                {
                    s.Fill(lineEnd + 1, line - 1, TokenKind::Comment);
                    return line - 1;
                }
            }
            s.Fill(lineEnd + 1, s.Size(), TokenKind::Comment); // no #endif: to the end of the file
            return s.Size();
        }

        // A #directive line: colours the directive and an #include <file> name; returns where scanning resumes.
        size_t ScanDirective(Scanner const& s, size_t i)
        {
            size_t j = i + 1;
            while (IsBlank(s.At(j)))
            {
                ++j;
            }
            size_t wordStart = j;
            while (IsIdentChar(s.At(j)))
            {
                ++j;
            }
            s.Fill(i, j, TokenKind::Preprocessor);

            auto word = s.Text.substr(wordStart, j - wordStart);
            if (word == L"include" || word == L"import")
            {
                while (IsBlank(s.At(j)))
                {
                    ++j;
                }
                if (s.At(j) == L'<')
                {
                    size_t close = s.Text.find(L'>', j);
                    size_t end = std::min(close == npos ? s.Size() : close + 1, s.LineEnd(j));
                    s.Fill(j, end, TokenKind::String);
                    j = end;
                }
            }
            else if (word == L"define" || word == L"undef")
            {
                // The macro's name. MarkDeclaredNames colours its uses.
                while (IsBlank(s.At(j)))
                {
                    ++j;
                }
                size_t nameStart = j;
                while (IsIdentChar(s.At(j)))
                {
                    ++j;
                }
                s.Fill(nameStart, j, TokenKind::Macro);
            }
            else if (word == L"if")
            {
                // #if 0 switches code off: show it as a comment, up to the matching #else, #elif or #endif.
                size_t k = j;
                while (IsBlank(s.At(k)))
                {
                    ++k;
                }
                if (s.At(k) == L'0' && !IsIdentChar(s.At(k + 1)))
                {
                    size_t afterZero = k + 1;
                    while (IsBlank(s.At(afterZero)))
                    {
                        ++afterZero;
                    }
                    bool restIsComment = afterZero >= s.LineEnd(k) || s.StartsWith(afterZero, L"//") || s.StartsWith(afterZero, L"/*");
                    if (restIsComment)
                    {
                        s.Fill(k, k + 1, TokenKind::Number);
                        s.Fill(afterZero, s.LineEnd(k), TokenKind::Comment);
                        return SkipInactiveBlock(s, s.LineEnd(k));
                    }
                }
            }
            return j;
        }

        // A character literal starting at i, or npos. Only short literals closed on the same line count,
        // so that Rust lifetimes ('a) stay plain.
        size_t ScanCharLiteral(Scanner const& s, size_t i)
        {
            if (s.At(i + 1) == L'\\')
            {
                size_t limit = std::min(s.LineEnd(i), i + 12);
                for (size_t j = i + 3; j < limit; ++j)
                {
                    if (s.Text[j] == L'\'')
                    {
                        return j + 1;
                    }
                }
                return npos;
            }
            if (s.At(i + 1) != L'\r' && s.At(i + 2) == L'\'')
            {
                return i + 3;
            }
            if (IS_HIGH_SURROGATE(s.At(i + 1)) && s.At(i + 3) == L'\'')
            {
                return i + 4;
            }
            return npos;
        }

        // Colours the escape sequences in the string from start to end: the escape character and what it
        // escapes, with the digits of \xHH, \uHHHH, \u{H…}, \UHHHHHHHH and octal \ooo.
        void MarkEscapes(Scanner const& s, size_t start, size_t end, wchar_t escape)
        {
            if (!escape)
            {
                return;
            }
            auto isOctal = [](wchar_t c) { return c >= L'0' && c <= L'7'; };
            for (size_t i = start; i < end; ++i)
            {
                if (s.Text[i] != escape)
                {
                    continue;
                }
                wchar_t c = s.At(i + 1);
                size_t j = i + 2;
                auto digits = [&](size_t count, auto isDigit)
                {
                    for (size_t k = 0; k < count && j < end && isDigit(s.Text[j]); ++k)
                    {
                        ++j;
                    }
                };
                if (c == L'x')
                {
                    digits(2, std::iswxdigit);
                }
                else if (c == L'u' && s.At(j) == L'{')
                {
                    size_t close = s.Text.find(L'}', j);
                    j = close != npos && close < end ? close + 1 : j;
                }
                else if (c == L'u')
                {
                    digits(4, std::iswxdigit);
                }
                else if (c == L'U')
                {
                    digits(8, std::iswxdigit);
                }
                else if (isOctal(c))
                {
                    digits(2, isOctal);
                }
                else if (c == L'\r' || c == L'\0')
                {
                    j = i + 1; // a line continuation: just the escape character
                }
                j = std::min(j, end);
                s.Fill(i, j, TokenKind::Escape);
                i = j - 1;
            }
        }

        // Whether the string whose quote is at i has a raw-string prefix, as in Python's r"\d" or
        // rb'...': letters from rRbBuUfF just before the quote, including an r.
        bool HasRawPrefix(Scanner const& s, size_t i)
        {
            size_t j = i;
            bool raw = false;
            while (j > 0 && std::wstring_view{ L"rRbBuUfF" }.find(s.Text[j - 1]) != npos)
            {
                raw = raw || s.Text[j - 1] == L'r' || s.Text[j - 1] == L'R';
                --j;
            }
            return raw && i - j <= 2 && (j == 0 || !IsIdentChar(s.Text[j - 1]));
        }

        bool StartsLineComment(LanguageDefinition const& lang, Scanner const& s, size_t i, bool atLineStart)
        {
            auto starts = [&](auto const& marker) { return s.StartsWith(i, marker); };
            return std::any_of(lang.LineComments.begin(), lang.LineComments.end(), starts)
                || (atLineStart && std::any_of(lang.LineStartComments.begin(), lang.LineStartComments.end(), starts));
        }

        // Colours the names of types and macros declared in the text, wherever they're used.
        // A type's name is the first plain word after one of the language's type declaration words
        // (class Foo, struct Foo), skipping keywords in between (enum class Foo). A macro's name is the
        // word after #define, which ScanDirective has already coloured. Only this text is seen, so names
        // declared in other files aren't recognised.
        void MarkDeclaredNames(LanguageDefinition const& lang, Scanner const& s)
        {
            if (lang.TypeDeclarations.empty() && !lang.HashDirectives)
            {
                return;
            }
            auto const text = s.Text;
            size_t const n = text.size();
            auto wordEnd = [&](size_t i)
            {
                while (i < n && IsIdentChar(text[i]))
                {
                    ++i;
                }
                return i;
            };
            auto key = [&](std::wstring_view word) { return lang.IgnoreCase ? Lower(word) : std::wstring{ word }; };

            // Find the declared names. Whitespace and comments may separate a type declaration word from the
            // name; anything else (punctuation, strings) ends the search.
            WordSet types;
            WordSet macros;
            bool expectName = false;
            for (size_t i = 0; i < n;)
            {
                auto kind = s.Kinds[i];
                if (kind == TokenKind::Comment || std::iswspace(text[i]))
                {
                    ++i;
                    continue;
                }
                bool wordStart = IsIdentStart(text[i]) && (i == 0 || !IsIdentChar(text[i - 1]));
                if (wordStart && kind == TokenKind::Macro && lang.HashDirectives)
                {
                    size_t j = wordEnd(i);
                    macros.insert(key(text.substr(i, j - i)));
                    expectName = false;
                    i = j;
                    continue;
                }
                if (!wordStart || (kind != TokenKind::Plain && kind != TokenKind::Keyword && kind != TokenKind::Type))
                {
                    expectName = false;
                    ++i;
                    continue;
                }
                size_t j = wordEnd(i);
                auto word = key(text.substr(i, j - i));
                if (lang.TypeDeclarations.contains(word))
                {
                    expectName = true;
                }
                else if (expectName && kind == TokenKind::Plain)
                {
                    // Not a qualified name such as typename std::vector<T>::iterator, whose first part is a namespace.
                    if (!s.StartsWith(j, L"::"))
                    {
                        types.insert(std::move(word));
                    }
                    expectName = false;
                }
                else if (kind != TokenKind::Keyword)
                {
                    expectName = false;
                }
                i = j;
            }
            if (types.empty() && macros.empty())
            {
                return;
            }

            // Colour every plain use of them.
            for (size_t i = 0; i < n;)
            {
                if (s.Kinds[i] == TokenKind::Plain && IsIdentStart(text[i]) && (i == 0 || !IsIdentChar(text[i - 1])))
                {
                    size_t j = wordEnd(i);
                    auto word = key(text.substr(i, j - i));
                    if (macros.contains(word))
                    {
                        s.Fill(i, j, TokenKind::Macro);
                    }
                    else if (types.contains(word))
                    {
                        s.Fill(i, j, TokenKind::Type);
                    }
                    i = j;
                    continue;
                }
                ++i;
            }
        }
    }

    void ClassifyCode(LanguageDefinition const& lang, Scanner const& s)
    {
        auto const text = s.Text;
        size_t const n = text.size();
        bool lineStart = true;
        size_t i = 0;
        while (i < n)
        {
            wchar_t c = text[i];
            if (c == L'\r')
            {
                lineStart = true;
                ++i;
                continue;
            }
            if (IsBlank(c))
            {
                ++i;
                continue;
            }
            bool atLineStart = std::exchange(lineStart, false);

            if (s.StartsWith(i, lang.BlockStart))
            {
                size_t end = s.FindEnd(i + lang.BlockStart.size(), lang.BlockEnd);
                s.Fill(i, end, TokenKind::Comment);
                i = end;
                continue;
            }
            if (StartsLineComment(lang, s, i, atLineStart))
            {
                size_t end = s.LineEnd(i);
                s.Fill(i, end, TokenKind::Comment);
                i = end;
                continue;
            }
            if (lang.HashDirectives && c == L'#' && atLineStart)
            {
                i = ScanDirective(s, i);
                continue;
            }
            if (lang.TripleQuotes && (s.StartsWith(i, L"\"\"\"") || s.StartsWith(i, L"'''")))
            {
                size_t end = s.FindEnd(i + 3, text.substr(i, 3));
                s.Fill(i, end, TokenKind::String);
                if (!HasRawPrefix(s, i))
                {
                    MarkEscapes(s, i + 3, end, lang.Escape);
                }
                i = end;
                continue;
            }
            if (lang.RawStrings && c == L'R' && s.At(i + 1) == L'"' && (i == 0 || !IsIdentChar(text[i - 1])))
            {
                size_t open = text.find(L'(', i + 2);
                if (open != npos && open - (i + 2) <= 16)
                {
                    std::wstring close = L")" + std::wstring{ text.substr(i + 2, open - (i + 2)) } + L"\"";
                    size_t end = s.FindEnd(open + 1, close);
                    s.Fill(i, end, TokenKind::String);
                    i = end;
                    continue;
                }
            }
            if (lang.VerbatimStrings && c == L'@' && s.At(i + 1) == L'"')
            {
                size_t j = i + 2;
                while (j < n && !(text[j] == L'"' && s.At(j + 1) != L'"'))
                {
                    j += text[j] == L'"' ? 2 : 1;
                }
                size_t end = std::min(j + 1, n);
                s.Fill(i, end, TokenKind::String);
                i = end;
                continue;
            }
            if (lang.Quotes.find(c) != npos || lang.MultilineQuotes.find(c) != npos)
            {
                size_t end = s.ScanString(i, c, lang.Escape, lang.MultilineQuotes.find(c) != npos);
                auto kind = TokenKind::String;
                if (lang.KeysAsProperties)
                {
                    size_t j = end;
                    while (IsBlank(s.At(j)))
                    {
                        ++j;
                    }
                    if (s.At(j) == L':')
                    {
                        kind = TokenKind::Property;
                    }
                }
                s.Fill(i, end, kind);
                // Not in raw strings, or PowerShell's single-quoted strings, which have no escapes.
                if (kind == TokenKind::String && !HasRawPrefix(s, i) && !(lang.PowerShellSyntax && c == L'\''))
                {
                    MarkEscapes(s, i + 1, end, lang.Escape);
                }
                i = end;
                continue;
            }
            if (lang.CharLiterals && c == L'\'')
            {
                size_t end = ScanCharLiteral(s, i);
                if (end != npos)
                {
                    s.Fill(i, end, TokenKind::String);
                    MarkEscapes(s, i + 1, end, lang.Escape);
                    i = end;
                }
                else
                {
                    ++i;
                }
                continue;
            }
            if (lang.Annotations && c == L'@' && IsIdentStart(s.At(i + 1)))
            {
                size_t j = i + 1;
                while (IsIdentChar(s.At(j)) || s.At(j) == L'.')
                {
                    ++j;
                }
                s.Fill(i, j, TokenKind::Preprocessor);
                i = j;
                continue;
            }
            if (lang.PowerShellSyntax)
            {
                if (c == L'$' && (IsIdentChar(s.At(i + 1)) || s.At(i + 1) == L'_'))
                {
                    size_t j = i + 1;
                    while (IsIdentChar(s.At(j)) || (s.At(j) == L':' && IsIdentStart(s.At(j + 1))))
                    {
                        ++j;
                    }
                    s.Fill(i, j, TokenKind::Variable);
                    i = j;
                    continue;
                }
                // -eq, -and and other operators are keywords; any other -Name is a parameter.
                if (c == L'-' && std::iswalpha(s.At(i + 1)) && (i == 0 || !IsIdentChar(text[i - 1])))
                {
                    size_t j = i + 1;
                    while (IsIdentChar(s.At(j)))
                    {
                        ++j;
                    }
                    s.Fill(i, j, lang.Keywords.contains(Lower(text.substr(i, j - i))) ? TokenKind::Keyword : TokenKind::Attribute);
                    i = j;
                    continue;
                }
            }
            if (IsDigit(c) || (c == L'.' && IsDigit(s.At(i + 1))))
            {
                size_t j = i + 1;
                while (j < n)
                {
                    wchar_t d = text[j];
                    bool exponentSign = (d == L'+' || d == L'-') && (text[j - 1] == L'e' || text[j - 1] == L'E') && IsDigit(s.At(j + 1));
                    if (IsIdentChar(d) || (d == L'.' && s.At(j + 1) != L'.') || exponentSign)
                    {
                        ++j;
                    }
                    else
                    {
                        break;
                    }
                }
                s.Fill(i, j, TokenKind::Number);
                i = j;
                continue;
            }
            bool dollarIdent = c == L'$' && !lang.PowerShellSyntax;
            if (IsIdentStart(c) || dollarIdent)
            {
                size_t j = i + 1;
                while (j < n)
                {
                    wchar_t d = text[j];
                    bool cmdletDash = lang.PowerShellSyntax && d == L'-' && std::iswalpha(s.At(j + 1));
                    if (IsIdentChar(d) || (d == L'$' && !lang.PowerShellSyntax) || cmdletDash)
                    {
                        ++j;
                    }
                    else
                    {
                        break;
                    }
                }

                auto word = text.substr(i, j - i);
                auto kind = TokenKind::Plain;
                if (lang.PowerShellSyntax && word.find(L'-') != npos)
                {
                    kind = TokenKind::Type; // Verb-Noun command
                }
                else
                {
                    std::wstring lowered;
                    if (lang.IgnoreCase)
                    {
                        lowered = Lower(word);
                        word = lowered;
                    }
                    if (lang.Keywords.contains(word))
                    {
                        kind = TokenKind::Keyword;
                    }
                    else if (lang.Types.contains(word))
                    {
                        kind = TokenKind::Type;
                    }
                    else if (lang.Macros.contains(word))
                    {
                        kind = TokenKind::Macro;
                    }
                    else if (lang.BangMacros && s.At(j) == L'!' && s.At(j + 1) != L'=')
                    {
                        kind = TokenKind::Macro;
                        ++j; // println!
                    }
                    else if (lang.UpperCaseMacros && IsMacroCase(text.substr(i, j - i)))
                    {
                        kind = TokenKind::Macro;
                    }
                }
                s.Fill(i, j, kind);
                i = j;
                continue;
            }
            ++i;
        }

        MarkDeclaredNames(lang, s);
    }
}
