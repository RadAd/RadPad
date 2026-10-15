#include "pch.h"
#include "Syntax.h"
#include "TextFile.h"

#include <cwctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <winrt/Windows.Data.Json.h>

namespace syntax
{
    namespace
    {
        constexpr size_t npos = std::wstring_view::npos;

        // A set of words that can be searched with a wstring_view without allocating.
        struct WordHash
        {
            using is_transparent = void;
            size_t operator()(std::wstring_view word) const noexcept { return std::hash<std::wstring_view>{}(word); }
        };
        using WordSet = std::unordered_set<std::wstring, WordHash, std::equal_to<>>;

        // Which tokenizer a language uses. Code is driven entirely by the definition's rules; the others
        // are hand-written for formats that rules can't describe.
        enum class Tokenizer { None, Code, Markup, Css, Batch, Ini };

        // One language, as read from a JSON file in a Languages folder. See README.md for the format.
        struct LanguageDefinition
        {
            std::wstring Name;
            std::vector<std::wstring> Extensions; // lower case, without the dot
            std::vector<std::wstring> FileNames;  // whole file names, lower case
            Tokenizer Kind = Tokenizer::Code;
            WordSet Keywords;                     // lower case when IgnoreCase
            WordSet Types;
            WordSet TypeDeclarations;             // words that declare a type named by the next word, such as class
            WordSet Macros;                       // well-known macros, coloured as macros
            bool UpperCaseMacros = false;         // ALL_CAPS words are macros
            bool BangMacros = false;              // Rust name! macros
            std::vector<std::wstring> LineComments;
            std::vector<std::wstring> LineStartComments; // comment markers only at the start of a line, e.g. ; in Inno Setup
            std::wstring BlockStart;
            std::wstring BlockEnd;
            std::wstring Quotes = L"\"";          // delimiters of single-line strings
            std::wstring MultilineQuotes;         // delimiters of strings that may span lines
            bool CharLiterals = false;            // ' delimits short character literals
            wchar_t Escape = L'\\';               // L'\0' for none
            bool IgnoreCase = false;
            bool HashDirectives = false;          // # at the start of a line begins a preprocessor directive
            bool RawStrings = false;              // C++ R"delim(...)delim"
            bool VerbatimStrings = false;         // C# @"..." with "" as the escape
            bool TripleQuotes = false;            // Python """...""" and '''...'''
            bool Annotations = false;             // @name decorators and annotations
            bool PowerShellSyntax = false;        // $variables, -operators and -Parameters, Verb-Noun commands
            bool KeysAsProperties = false;        // JSON object keys
            std::wstring ScriptLanguage;          // markup: language of <script> contents
            std::wstring StyleLanguage;           // markup: language of <style> contents
        };

        struct Registry
        {
            std::vector<LanguageDefinition> Languages;
            std::unordered_map<std::wstring, LanguageId> Extensions;
            std::unordered_map<std::wstring, LanguageId> FileNames;
        };

        LanguageDefinition MakePlainText()
        {
            LanguageDefinition plain;
            plain.Name = L"Plain text";
            plain.Kind = Tokenizer::None;
            return plain;
        }

        Registry& Languages()
        {
            static Registry registry{ { MakePlainText() } };
            return registry;
        }

        bool NamesEqual(std::wstring_view a, std::wstring_view b)
        {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        std::wstring Lower(std::wstring_view text)
        {
            std::wstring result{ text };
            for (auto& c : result)
            {
                c = static_cast<wchar_t>(std::towlower(c));
            }
            return result;
        }

        bool IsBlank(wchar_t c) { return c == L' ' || c == L'\t'; }
        bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
        bool IsIdentStart(wchar_t c) { return std::iswalpha(c) || c == L'_'; }
        bool IsIdentChar(wchar_t c) { return std::iswalnum(c) || c == L'_'; }

        // MAX_PATH, S_OK, D3D11: at least two characters, with capital letters and no small ones.
        bool IsMacroCase(std::wstring_view word)
        {
            bool upper = false;
            for (wchar_t c : word)
            {
                if (std::iswlower(c))
                {
                    return false;
                }
                upper = upper || std::iswupper(c);
            }
            return upper && word.size() >= 2;
        }

        // Returns the index of lowerNeedle in text at or after from, ignoring case, or text.size().
        size_t FindIgnoreCase(std::wstring_view text, std::wstring_view lowerNeedle, size_t from)
        {
            for (size_t i = from; i + lowerNeedle.size() <= text.size(); ++i)
            {
                size_t k = 0;
                while (k < lowerNeedle.size() && static_cast<wchar_t>(std::towlower(text[i + k])) == lowerNeedle[k])
                {
                    ++k;
                }
                if (k == lowerNeedle.size())
                {
                    return i;
                }
            }
            return text.size();
        }

        // The text being classified and the output array, with helpers shared by the tokenizers.
        struct Scanner
        {
            std::wstring_view Text;
            TokenKind* Kinds;

            size_t Size() const { return Text.size(); }
            wchar_t At(size_t i) const { return i < Text.size() ? Text[i] : L'\0'; }
            bool StartsWith(size_t i, std::wstring_view s) const { return !s.empty() && i <= Text.size() && Text.substr(i).starts_with(s); }

            size_t LineEnd(size_t i) const
            {
                size_t p = Text.find(L'\r', i);
                return p == npos ? Text.size() : p;
            }

            void Fill(size_t from, size_t to, TokenKind kind) const
            {
                to = std::min(to, Text.size());
                if (from < to)
                {
                    std::fill(Kinds + from, Kinds + to, kind);
                }
            }

            // Index just past the next occurrence of close at or after from, or the end of the text.
            size_t FindEnd(size_t from, std::wstring_view close) const
            {
                size_t p = Text.find(close, from);
                return p == npos ? Text.size() : p + close.size();
            }

            // Scans a string that opens with the quote at i and returns the index just past its closing quote.
            // A single-line string that isn't closed ends at the line break.
            size_t ScanString(size_t i, wchar_t quote, wchar_t escape, bool multiline) const
            {
                size_t j = i + 1;
                while (j < Text.size())
                {
                    wchar_t c = Text[j];
                    if (escape && c == escape)
                    {
                        j += 2;
                        continue;
                    }
                    if (c == quote)
                    {
                        return j + 1;
                    }
                    if (c == L'\r' && !multiline)
                    {
                        return j;
                    }
                    ++j;
                }
                return Text.size();
            }
        };

        void ClassifyInto(LanguageDefinition const& language, std::wstring_view text, TokenKind* kinds);

        // -----------------------------------------------------------------------------------------
        // C-like and scripting languages, driven by the definition's rules
        // -----------------------------------------------------------------------------------------

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
                    i = end;
                    continue;
                }
                if (lang.CharLiterals && c == L'\'')
                {
                    size_t end = ScanCharLiteral(s, i);
                    if (end != npos)
                    {
                        s.Fill(i, end, TokenKind::String);
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

        // -----------------------------------------------------------------------------------------
        // XML and HTML
        // -----------------------------------------------------------------------------------------

        bool IsNameStart(wchar_t c) { return std::iswalpha(c) || c == L'_' || c == L':'; }
        bool IsNameChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L':' || c == L'-' || c == L'.'; }

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

        bool IsCssNameStart(wchar_t c) { return std::iswalpha(c) || c == L'_'; }
        bool IsCssNameChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L'-'; }

        bool& RainbowBrackets()
        {
            static bool on = true;
            return on;
        }

        // Colours (), [] and {} by how deeply they're nested, cycling through three colours, as VS Code
        // does. Brackets in strings and comments aren't plain, so they're left alone, as is a closing
        // bracket with nothing open.
        void MarkBrackets(Scanner const& s)
        {
            constexpr TokenKind colours[] = { TokenKind::Bracket1, TokenKind::Bracket2, TokenKind::Bracket3 };
            size_t depth = 0;
            for (size_t i = 0; i < s.Size(); ++i)
            {
                if (s.Kinds[i] != TokenKind::Plain)
                {
                    continue;
                }
                wchar_t c = s.Text[i];
                if (c == L'(' || c == L'[' || c == L'{')
                {
                    s.Kinds[i] = colours[depth % 3];
                    ++depth;
                }
                else if ((c == L')' || c == L']' || c == L'}') && depth > 0)
                {
                    --depth;
                    s.Kinds[i] = colours[depth % 3];
                }
            }
        }

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

        // -----------------------------------------------------------------------------------------
        // Batch files
        // -----------------------------------------------------------------------------------------

        void ClassifyBatchLine(Scanner const& s, WordSet const& keywords, size_t i, size_t lineEnd)
        {
            auto const text = s.Text;
            while (i < lineEnd)
            {
                wchar_t c = text[i];
                if (c == L'"')
                {
                    size_t close = text.find(L'"', i + 1);
                    size_t end = close == npos || close >= lineEnd ? lineEnd : close + 1;
                    s.Fill(i, end, TokenKind::String);
                    i = end;
                    continue;
                }
                if (c == L'%')
                {
                    size_t end = 0;
                    wchar_t next = s.At(i + 1);
                    if (next == L'%')
                    {
                        end = std::min(i + 3, lineEnd); // %%i loop variable
                    }
                    else if (IsDigit(next) || next == L'*')
                    {
                        end = i + 2; // %1 argument
                    }
                    else if (next == L'~')
                    {
                        size_t j = i + 2; // %~dp0
                        while (j < lineEnd && std::iswalpha(text[j]))
                        {
                            ++j;
                        }
                        if (j < lineEnd && IsDigit(text[j]))
                        {
                            end = j + 1;
                        }
                    }
                    else
                    {
                        size_t close = text.find(L'%', i + 1); // %name%
                        if (close != npos && close < lineEnd && close > i + 1)
                        {
                            end = close + 1;
                        }
                    }
                    if (end)
                    {
                        s.Fill(i, end, TokenKind::Variable);
                        i = end;
                        continue;
                    }
                }
                if (c == L'!')
                {
                    size_t close = text.find(L'!', i + 1); // !name! with delayed expansion
                    if (close != npos && close < lineEnd && close > i + 1 && text.substr(i + 1, close - i - 1).find_first_of(L" \t") == npos)
                    {
                        s.Fill(i, close + 1, TokenKind::Variable);
                        i = close + 1;
                        continue;
                    }
                }
                if (IsIdentStart(c))
                {
                    size_t j = i;
                    while (j < lineEnd && IsIdentChar(text[j]))
                    {
                        ++j;
                    }
                    if (keywords.contains(Lower(text.substr(i, j - i))))
                    {
                        s.Fill(i, j, TokenKind::Keyword);
                    }
                    i = j;
                    continue;
                }
                ++i;
            }
        }

        void ClassifyBatch(Scanner const& s, LanguageDefinition const& lang)
        {
            auto const text = s.Text;
            size_t i = 0;
            while (i < text.size())
            {
                size_t lineEnd = s.LineEnd(i);
                size_t j = i;
                while (j < lineEnd && (IsBlank(text[j]) || text[j] == L'@'))
                {
                    if (text[j] == L'@')
                    {
                        s.Fill(j, j + 1, TokenKind::Preprocessor);
                    }
                    ++j;
                }

                std::wstring first = Lower(text.substr(j, std::min<size_t>(4, lineEnd - j)));
                if (s.StartsWith(j, L"::") || (first.starts_with(L"rem") && (first.size() == 3 || IsBlank(first[3]))))
                {
                    s.Fill(j, lineEnd, TokenKind::Comment);
                }
                else if (s.At(j) == L':')
                {
                    s.Fill(j, lineEnd, TokenKind::Tag); // label
                }
                else
                {
                    ClassifyBatchLine(s, lang.Keywords, j, lineEnd);
                }
                i = lineEnd + 1;
            }
        }

        // -----------------------------------------------------------------------------------------
        // INI and similar key = value files
        // -----------------------------------------------------------------------------------------

        void ClassifyIni(Scanner const& s)
        {
            auto const text = s.Text;
            size_t i = 0;
            while (i < text.size())
            {
                size_t lineEnd = s.LineEnd(i);
                size_t j = i;
                while (j < lineEnd && IsBlank(text[j]))
                {
                    ++j;
                }
                if (j < lineEnd)
                {
                    wchar_t c = text[j];
                    if (c == L';' || c == L'#')
                    {
                        s.Fill(j, lineEnd, TokenKind::Comment);
                    }
                    else if (c == L'[')
                    {
                        size_t close = text.find(L']', j);
                        s.Fill(j, close != npos && close < lineEnd ? close + 1 : lineEnd, TokenKind::Tag);
                    }
                    else if (size_t equals = text.find(L'=', j); equals != npos && equals < lineEnd)
                    {
                        size_t keyEnd = equals;
                        while (keyEnd > j && IsBlank(text[keyEnd - 1]))
                        {
                            --keyEnd;
                        }
                        s.Fill(j, keyEnd, TokenKind::Property);
                    }
                }
                i = lineEnd + 1;
            }
        }

        void ClassifyInto(LanguageDefinition const& language, std::wstring_view text, TokenKind* kinds)
        {
            Scanner s{ text, kinds };
            switch (language.Kind)
            {
            case Tokenizer::None:
                break;
            case Tokenizer::Code:
                ClassifyCode(language, s);
                break;
            case Tokenizer::Markup:
                ClassifyMarkup(s, language);
                break;
            case Tokenizer::Css:
                ClassifyCss(s);
                break;
            case Tokenizer::Batch:
                ClassifyBatch(s, language);
                break;
            case Tokenizer::Ini:
                ClassifyIni(s);
                break;
            }
            if (RainbowBrackets() && (language.Kind == Tokenizer::Code || language.Kind == Tokenizer::Css))
            {
                MarkBrackets(s);
            }
        }

        // -----------------------------------------------------------------------------------------
        // Loading definitions from JSON
        // -----------------------------------------------------------------------------------------

        using winrt::Windows::Data::Json::JsonObject;
        using winrt::Windows::Data::Json::JsonValueType;

        [[noreturn]] void Invalid(std::wstring const& message)
        {
            throw winrt::hresult_invalid_argument(message);
        }

        std::wstring Quoted(std::wstring_view key)
        {
            return L"\"" + std::wstring{ key } + L"\"";
        }

        std::wstring ReadString(JsonObject const& json, std::wstring_view key, std::wstring fallback = {})
        {
            if (!json.HasKey(key))
            {
                return fallback;
            }
            auto value = json.GetNamedValue(key);
            if (value.ValueType() != JsonValueType::String)
            {
                Invalid(Quoted(key) + L" must be a string");
            }
            return std::wstring{ value.GetString() };
        }

        bool ReadBool(JsonObject const& json, std::wstring_view key)
        {
            if (!json.HasKey(key))
            {
                return false;
            }
            auto value = json.GetNamedValue(key);
            if (value.ValueType() != JsonValueType::Boolean)
            {
                Invalid(Quoted(key) + L" must be true or false");
            }
            return value.GetBoolean();
        }

        // A list of words or markers: an array of strings, or one string of words separated by spaces.
        std::vector<std::wstring> ReadList(JsonObject const& json, std::wstring_view key)
        {
            std::vector<std::wstring> items;
            if (!json.HasKey(key))
            {
                return items;
            }
            auto value = json.GetNamedValue(key);
            if (value.ValueType() == JsonValueType::String)
            {
                winrt::hstring all = value.GetString();
                std::wstring_view words = all;
                size_t i = 0;
                while (i < words.size())
                {
                    while (i < words.size() && std::iswspace(words[i]))
                    {
                        ++i;
                    }
                    size_t start = i;
                    while (i < words.size() && !std::iswspace(words[i]))
                    {
                        ++i;
                    }
                    if (i > start)
                    {
                        items.emplace_back(words.substr(start, i - start));
                    }
                }
            }
            else if (value.ValueType() == JsonValueType::Array)
            {
                for (auto const& item : value.GetArray())
                {
                    if (item.ValueType() != JsonValueType::String)
                    {
                        Invalid(Quoted(key) + L" must contain only strings");
                    }
                    if (!item.GetString().empty())
                    {
                        items.emplace_back(item.GetString());
                    }
                }
            }
            else
            {
                Invalid(Quoted(key) + L" must be an array of strings, or a string of words separated by spaces");
            }
            return items;
        }

        LanguageDefinition ParseDefinition(JsonObject const& json)
        {
            static constexpr std::pair<std::wstring_view, Tokenizer> tokenizers[] =
            {
                { L"code", Tokenizer::Code }, { L"markup", Tokenizer::Markup }, { L"css", Tokenizer::Css },
                { L"batch", Tokenizer::Batch }, { L"ini", Tokenizer::Ini },
            };

            LanguageDefinition lang;
            lang.Name = ReadString(json, L"name");
            if (lang.Name.empty())
            {
                Invalid(L"\"name\" is missing");
            }
            if (NamesEqual(lang.Name, MakePlainText().Name))
            {
                Invalid(L"\"Plain text\" is built in and can't be redefined");
            }

            auto tokenizer = Lower(ReadString(json, L"tokenizer", L"code"));
            auto match = std::find_if(std::begin(tokenizers), std::end(tokenizers), [&](auto const& t) { return t.first == tokenizer; });
            if (match == std::end(tokenizers))
            {
                Invalid(L"\"tokenizer\" must be one of: code, markup, css, batch, ini");
            }
            lang.Kind = match->second;

            for (auto const& extension : ReadList(json, L"extensions"))
            {
                auto name = Lower(extension);
                lang.Extensions.push_back(name.starts_with(L'.') ? name.substr(1) : name);
            }
            for (auto const& fileName : ReadList(json, L"fileNames"))
            {
                lang.FileNames.push_back(Lower(fileName));
            }

            lang.IgnoreCase = ReadBool(json, L"ignoreCase");
            for (auto const& word : ReadList(json, L"keywords"))
            {
                lang.Keywords.insert(lang.IgnoreCase ? Lower(word) : word);
            }
            for (auto const& word : ReadList(json, L"types"))
            {
                lang.Types.insert(lang.IgnoreCase ? Lower(word) : word);
            }
            for (auto const& word : ReadList(json, L"typeDeclarations"))
            {
                lang.TypeDeclarations.insert(lang.IgnoreCase ? Lower(word) : word);
            }
            for (auto const& word : ReadList(json, L"macros"))
            {
                lang.Macros.insert(lang.IgnoreCase ? Lower(word) : word);
            }
            lang.UpperCaseMacros = ReadBool(json, L"upperCaseMacros");
            lang.BangMacros = ReadBool(json, L"bangMacros");

            lang.LineComments = ReadList(json, L"lineComments");
            lang.LineStartComments = ReadList(json, L"lineStartComments");
            auto block = ReadList(json, L"blockComment");
            if (!block.empty())
            {
                if (block.size() != 2)
                {
                    Invalid(L"\"blockComment\" must be the start and end markers, such as [\"/*\", \"*/\"]");
                }
                lang.BlockStart = block[0];
                lang.BlockEnd = block[1];
            }

            lang.Quotes = ReadString(json, L"strings", L"\"");
            lang.MultilineQuotes = ReadString(json, L"multilineStrings");
            auto escape = ReadString(json, L"escape", L"\\");
            if (escape.size() > 1)
            {
                Invalid(L"\"escape\" must be a single character, or \"\" for none");
            }
            lang.Escape = escape.empty() ? L'\0' : escape[0];

            lang.CharLiterals = ReadBool(json, L"charLiterals");
            lang.HashDirectives = ReadBool(json, L"preprocessor");
            lang.RawStrings = ReadBool(json, L"rawStrings");
            lang.VerbatimStrings = ReadBool(json, L"verbatimStrings");
            lang.TripleQuotes = ReadBool(json, L"tripleQuotedStrings");
            lang.Annotations = ReadBool(json, L"annotations");
            lang.PowerShellSyntax = ReadBool(json, L"powerShellSyntax");
            lang.KeysAsProperties = ReadBool(json, L"objectKeys");
            lang.ScriptLanguage = ReadString(json, L"scriptLanguage");
            lang.StyleLanguage = ReadString(json, L"styleLanguage");
            return lang;
        }

        // The *.json files in a folder, in name order. A missing folder has none.
        std::vector<std::filesystem::path> DefinitionFiles(std::wstring const& folder)
        {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (std::filesystem::directory_iterator it{ folder, error }, end; !error && it != end; it.increment(error))
            {
                if (it->is_regular_file(error) && NamesEqual(it->path().extension().native(), L".json"))
                {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            return files;
        }
    }

    std::vector<std::wstring> LoadLanguages(std::vector<std::wstring> const& folders)
    {
        std::vector<std::wstring> errors;
        std::vector<LanguageDefinition> loaded{ MakePlainText() };
        std::vector<std::wstring> loadOrder; // names in the order they were (last) loaded

        for (auto const& folder : folders)
        {
            for (auto const& file : DefinitionFiles(folder))
            {
                try
                {
                    auto lang = ParseDefinition(JsonObject::Parse(textfile::Load(file.native()).Text));
                    std::erase_if(loadOrder, [&](auto const& name) { return NamesEqual(name, lang.Name); });
                    loadOrder.push_back(lang.Name);

                    // A definition with the same name as an earlier one replaces it.
                    auto existing = std::find_if(loaded.begin(), loaded.end(), [&](auto const& l) { return NamesEqual(l.Name, lang.Name); });
                    if (existing != loaded.end())
                    {
                        *existing = std::move(lang);
                    }
                    else
                    {
                        loaded.push_back(std::move(lang));
                    }
                }
                catch (winrt::hresult_error const& e)
                {
                    errors.push_back(file.native() + L": " + std::wstring{ e.message() });
                }
            }
        }

        // Menus list the languages by name, after Plain text.
        std::sort(loaded.begin() + 1, loaded.end(), [](auto const& a, auto const& b)
        {
            return CompareStringOrdinal(a.Name.c_str(), -1, b.Name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });

        Registry registry;
        registry.Languages = std::move(loaded);
        auto const& languages = registry.Languages;
        for (auto const& name : loadOrder)
        {
            // Later definitions win when two claim the same extension or file name.
            auto id = static_cast<LanguageId>(std::find_if(languages.begin(), languages.end(), [&](auto const& l) { return l.Name == name; }) - languages.begin());
            for (auto const& extension : languages[id].Extensions)
            {
                registry.Extensions[extension] = id;
            }
            for (auto const& fileName : languages[id].FileNames)
            {
                registry.FileNames[fileName] = id;
            }
        }
        Languages() = std::move(registry);
        return errors;
    }

    int LanguageCount()
    {
        return static_cast<int>(Languages().Languages.size());
    }

    std::wstring_view LanguageName(LanguageId language)
    {
        auto const& languages = Languages().Languages;
        return languages[language >= 0 && language < LanguageCount() ? language : PlainText].Name;
    }

    LanguageId FindLanguage(std::wstring_view name)
    {
        auto const& languages = Languages().Languages;
        auto it = std::find_if(languages.begin(), languages.end(), [&](auto const& l) { return NamesEqual(l.Name, name); });
        return it == languages.end() ? PlainText : static_cast<LanguageId>(it - languages.begin());
    }

    LanguageId LanguageFromPath(std::wstring_view path)
    {
        auto const& registry = Languages();
        size_t slash = path.find_last_of(L"\\/");
        std::wstring name = Lower(path.substr(slash == npos ? 0 : slash + 1));
        if (auto it = registry.FileNames.find(name); it != registry.FileNames.end())
        {
            return it->second;
        }
        if (size_t dot = name.find_last_of(L'.'); dot != npos)
        {
            if (auto it = registry.Extensions.find(name.substr(dot + 1)); it != registry.Extensions.end())
            {
                return it->second;
            }
        }
        return PlainText;
    }

    std::vector<std::wstring> LanguagePatterns(LanguageId language)
    {
        std::vector<std::wstring> patterns;
        if (language > PlainText && language < LanguageCount())
        {
            auto const& lang = Languages().Languages[language];
            for (auto const& extension : lang.Extensions)
            {
                patterns.push_back(L"*." + extension);
            }
            patterns.insert(patterns.end(), lang.FileNames.begin(), lang.FileNames.end());
        }
        return patterns;
    }

    void SetRainbowBrackets(bool on)
    {
        RainbowBrackets() = on;
    }

    void Classify(LanguageId language, std::wstring_view text, std::vector<TokenKind>& kinds)
    {
        kinds.assign(text.size(), TokenKind::Plain);
        if (text.size() <= MaxHighlightLength && language > PlainText && language < LanguageCount())
        {
            ClassifyInto(Languages().Languages[language], text, kinds.data());
        }
    }
}
