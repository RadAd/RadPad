#include "pch.h"
#include "Syntax.h"

#include <cwctype>
#include <unordered_set>

namespace syntax
{
    namespace
    {
        using WordSet = std::unordered_set<std::wstring_view>;
        constexpr size_t npos = std::wstring_view::npos;

        // Builds a set from space-separated word lists. The views point into the string literals.
        WordSet Words(std::initializer_list<std::wstring_view> lists)
        {
            WordSet words;
            for (auto list : lists)
            {
                size_t start = 0;
                while (start < list.size())
                {
                    size_t end = list.find(L' ', start);
                    if (end == npos)
                    {
                        end = list.size();
                    }
                    if (end > start)
                    {
                        words.insert(list.substr(start, end - start));
                    }
                    start = end + 1;
                }
            }
            return words;
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

        void ClassifyInto(Language language, std::wstring_view text, TokenKind* kinds);

        // -----------------------------------------------------------------------------------------
        // C-like and scripting languages, driven by a table of language rules
        // -----------------------------------------------------------------------------------------

        struct CodeLanguage
        {
            WordSet Keywords;
            WordSet Types;
            std::wstring_view LineComment;
            std::wstring_view LineComment2;
            std::wstring_view BlockStart;
            std::wstring_view BlockEnd;
            std::wstring_view Quotes;          // delimiters of single-line strings
            std::wstring_view MultilineQuotes; // delimiters of strings that may span lines
            bool CharLiterals = false;         // ' delimits short character literals
            wchar_t Escape = L'\\';            // L'\0' for none
            bool IgnoreCase = false;           // keyword and type lists are lower case
            bool HashDirectives = false;       // # at the start of a line begins a preprocessor directive
            bool RawStrings = false;           // C++ R"delim(...)delim"
            bool VerbatimStrings = false;      // C# @"..." with "" as the escape
            bool TripleQuotes = false;         // Python """...""" and '''...'''
            bool Annotations = false;          // @name decorators and annotations
            bool PowerShellSyntax = false;     // $variables, -operators and -Parameters, Verb-Noun commands
            bool KeysAsProperties = false;     // JSON object keys
        };

        constexpr std::wstring_view CKeywords = L"auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Bool _Complex _Generic _Noreturn _Static_assert _Thread_local true false NULL";
        constexpr std::wstring_view CTypes = L"bool char double float int long short signed unsigned void size_t ssize_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t wchar_t FILE";
        constexpr std::wstring_view CppKeywords = L"alignas alignof and and_eq asm bitand bitor catch class compl concept consteval constexpr constinit const_cast co_await co_return co_yield decltype delete dynamic_cast explicit export final friend import module mutable namespace new noexcept not not_eq nullptr operator or or_eq override private protected public reinterpret_cast requires static_assert static_cast template this thread_local throw try typeid typename using virtual xor xor_eq";
        constexpr std::wstring_view CppTypes = L"char8_t char16_t char32_t";
        constexpr std::wstring_view CSharpKeywords = L"abstract as base break case catch checked class const continue default delegate do else enum event explicit extern false finally fixed for foreach goto if implicit in interface internal is lock namespace new null operator out override params private protected public readonly ref return sealed sizeof stackalloc static struct switch this throw true try typeof unchecked unsafe using virtual volatile while add and async await by descending equals from get global group init into join let managed nameof not on or orderby partial record remove required select set unmanaged value var when where with yield";
        constexpr std::wstring_view CSharpTypes = L"bool byte char decimal double dynamic float int long nint nuint object sbyte short string uint ulong ushort void";
        constexpr std::wstring_view JavaKeywords = L"abstract assert break case catch class const continue default do else enum extends final finally for goto if implements import instanceof interface native new package permits private protected public record return sealed static strictfp super switch synchronized this throw throws transient try var volatile while yield true false null";
        constexpr std::wstring_view JavaTypes = L"boolean byte char double float int long short void Boolean Byte Character Double Float Integer Long Object Short String";
        constexpr std::wstring_view JavaScriptKeywords = L"async await break case catch class const continue debugger default delete do else export extends finally for from function get if import in instanceof let new of return set static super switch this throw try typeof var void while with yield true false null undefined NaN Infinity";
        constexpr std::wstring_view JavaScriptTypes = L"Array ArrayBuffer BigInt Boolean Date Error Function JSON Map Math Number Object Promise Proxy Reflect RegExp Set String Symbol WeakMap WeakSet console document globalThis window";
        constexpr std::wstring_view TypeScriptKeywords = L"abstract as asserts declare enum implements infer interface is keyof namespace never override private protected public readonly satisfies type unique";
        constexpr std::wstring_view TypeScriptTypes = L"any bigint boolean number object string symbol unknown void";
        constexpr std::wstring_view PythonKeywords = L"False None True and as assert async await break case class continue def del elif else except finally for from global if import in is lambda match nonlocal not or pass raise return try while with yield self cls";
        constexpr std::wstring_view PythonTypes = L"bool bytearray bytes complex dict float frozenset int list object range set str tuple type Exception abs all any enumerate filter isinstance len map max min open print sorted sum super zip";
        constexpr std::wstring_view RustKeywords = L"as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type union unsafe use where while macro_rules";
        constexpr std::wstring_view RustTypes = L"bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize Box Err None Ok Option Result Some String Vec";
        constexpr std::wstring_view GoKeywords = L"break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var true false nil iota";
        constexpr std::wstring_view GoTypes = L"any bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr append cap close copy delete len make new panic print println recover";
        constexpr std::wstring_view SqlKeywords = L"add all alter and any as asc avg begin between by cascade case cast check coalesce column commit constraint convert count create cross database declare default delete desc distinct drop else end exec execute exists fetch foreign from full function go grant group having if in index inner insert intersect into is join key left like limit max merge min not null offset on or order outer over partition primary procedure references return returns revoke right rollback rows select set sum table then top transaction trigger truncate union unique update use values view when where while with";
        constexpr std::wstring_view SqlTypes = L"bigint binary bit blob boolean char date datetime datetime2 datetimeoffset decimal double float int integer money nchar ntext numeric nvarchar real serial smallint text time timestamp tinyint uniqueidentifier uuid varbinary varchar xml";
        constexpr std::wstring_view PowerShellKeywords = L"begin break catch class continue data do dynamicparam else elseif end enum exit filter finally for foreach function if in param process return switch throw trap try until using while -and -as -band -bnot -bor -bxor -contains -eq -f -ge -gt -in -is -isnot -join -le -like -lt -match -ne -not -notcontains -notin -notlike -notmatch -or -replace -shl -shr -split -xor";
        constexpr std::wstring_view PowerShellTypes = L"array bool byte char datetime decimal double hashtable int int32 int64 long object pscustomobject psobject regex scriptblock string switch void xml";
        constexpr std::wstring_view JsonKeywords = L"true false null";

        CodeLanguage const* FindCodeLanguage(Language language)
        {
            static const CodeLanguage c{ .Keywords = Words({ CKeywords }), .Types = Words({ CTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"", .CharLiterals = true, .HashDirectives = true };
            static const CodeLanguage cpp{ .Keywords = Words({ CKeywords, CppKeywords }), .Types = Words({ CTypes, CppTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"", .CharLiterals = true, .HashDirectives = true, .RawStrings = true };
            static const CodeLanguage csharp{ .Keywords = Words({ CSharpKeywords }), .Types = Words({ CSharpTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"", .CharLiterals = true, .HashDirectives = true, .VerbatimStrings = true };
            static const CodeLanguage java{ .Keywords = Words({ JavaKeywords }), .Types = Words({ JavaTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"", .CharLiterals = true, .Annotations = true };
            static const CodeLanguage javascript{ .Keywords = Words({ JavaScriptKeywords }), .Types = Words({ JavaScriptTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"'", .MultilineQuotes = L"`", .Annotations = true };
            static const CodeLanguage typescript{ .Keywords = Words({ JavaScriptKeywords, TypeScriptKeywords }), .Types = Words({ JavaScriptTypes, TypeScriptTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"'", .MultilineQuotes = L"`", .Annotations = true };
            static const CodeLanguage python{ .Keywords = Words({ PythonKeywords }), .Types = Words({ PythonTypes }), .LineComment = L"#", .Quotes = L"\"'", .TripleQuotes = true, .Annotations = true };
            static const CodeLanguage rust{ .Keywords = Words({ RustKeywords }), .Types = Words({ RustTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .MultilineQuotes = L"\"", .CharLiterals = true };
            static const CodeLanguage go{ .Keywords = Words({ GoKeywords }), .Types = Words({ GoTypes }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"", .MultilineQuotes = L"`", .CharLiterals = true };
            static const CodeLanguage sql{ .Keywords = Words({ SqlKeywords }), .Types = Words({ SqlTypes }), .LineComment = L"--", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"'\"", .Escape = L'\0', .IgnoreCase = true };
            static const CodeLanguage powershell{ .Keywords = Words({ PowerShellKeywords }), .Types = Words({ PowerShellTypes }), .LineComment = L"#", .BlockStart = L"<#", .BlockEnd = L"#>", .MultilineQuotes = L"\"'", .Escape = L'`', .IgnoreCase = true, .PowerShellSyntax = true };
            static const CodeLanguage json{ .Keywords = Words({ JsonKeywords }), .LineComment = L"//", .BlockStart = L"/*", .BlockEnd = L"*/", .Quotes = L"\"", .KeysAsProperties = true };

            switch (language)
            {
            case Language::C: return &c;
            case Language::Cpp: return &cpp;
            case Language::CSharp: return &csharp;
            case Language::Java: return &java;
            case Language::JavaScript: return &javascript;
            case Language::TypeScript: return &typescript;
            case Language::Python: return &python;
            case Language::Rust: return &rust;
            case Language::Go: return &go;
            case Language::Sql: return &sql;
            case Language::PowerShell: return &powershell;
            case Language::Json: return &json;
            default: return nullptr;
            }
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

        void ClassifyCode(CodeLanguage const& lang, Scanner const& s)
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
                if (s.StartsWith(i, lang.LineComment) || s.StartsWith(i, lang.LineComment2))
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
                    }
                    s.Fill(i, j, kind);
                    i = j;
                    continue;
                }
                ++i;
            }
        }

        // -----------------------------------------------------------------------------------------
        // XML and HTML
        // -----------------------------------------------------------------------------------------

        bool IsNameStart(wchar_t c) { return std::iswalpha(c) || c == L'_' || c == L':'; }
        bool IsNameChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L':' || c == L'-' || c == L'.'; }

        void ClassifyMarkup(Scanner const& s, bool html)
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

                // Colour the contents of HTML <script> and <style> elements as JavaScript and CSS.
                if (html && !closing && !selfClosing && (name == L"script" || name == L"style"))
                {
                    size_t end = FindIgnoreCase(text, L"</" + name, i);
                    ClassifyInto(name == L"script" ? Language::JavaScript : Language::Css, text.substr(i, end - i), s.Kinds + i);
                    i = end;
                }
            }
        }

        // -----------------------------------------------------------------------------------------
        // CSS
        // -----------------------------------------------------------------------------------------

        bool IsCssNameStart(wchar_t c) { return std::iswalpha(c) || c == L'_'; }
        bool IsCssNameChar(wchar_t c) { return std::iswalnum(c) || c == L'_' || c == L'-'; }

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

        void ClassifyBatchLine(Scanner const& s, size_t i, size_t lineEnd)
        {
            static const WordSet keywords = Words({ L"call cd chdir choice cls copy defined del do echo else endlocal enabledelayedexpansion enableextensions equ erase errorlevel exist exit for geq goto gtr if in leq lss md mkdir move neq not nul off on pause popd pushd rd ren rename rmdir set setlocal shift start title type" });

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

        void ClassifyBatch(Scanner const& s)
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
                    ClassifyBatchLine(s, j, lineEnd);
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

        void ClassifyInto(Language language, std::wstring_view text, TokenKind* kinds)
        {
            Scanner s{ text, kinds };
            switch (language)
            {
            case Language::PlainText:
                break;
            case Language::Batch:
                ClassifyBatch(s);
                break;
            case Language::Css:
                ClassifyCss(s);
                break;
            case Language::Html:
                ClassifyMarkup(s, true);
                break;
            case Language::Xml:
                ClassifyMarkup(s, false);
                break;
            case Language::Ini:
                ClassifyIni(s);
                break;
            default:
                if (auto lang = FindCodeLanguage(language))
                {
                    ClassifyCode(*lang, s);
                }
                break;
            }
        }
    }

    std::wstring_view LanguageName(Language language)
    {
        static constexpr std::wstring_view names[LanguageCount] =
        {
            L"Plain text", L"Batch", L"C", L"C++", L"C#", L"CSS", L"Go", L"HTML", L"INI",
            L"Java", L"JavaScript", L"JSON", L"PowerShell", L"Python", L"Rust", L"SQL", L"TypeScript", L"XML",
        };
        auto index = static_cast<size_t>(language);
        return index < LanguageCount ? names[index] : names[0];
    }

    Language LanguageFromPath(std::wstring_view path)
    {
        static constexpr std::pair<std::wstring_view, Language> extensions[] =
        {
            { L"bat", Language::Batch }, { L"cmd", Language::Batch },
            { L"c", Language::C },
            { L"cpp", Language::Cpp }, { L"cc", Language::Cpp }, { L"cxx", Language::Cpp }, { L"c++", Language::Cpp },
            { L"h", Language::Cpp }, { L"hpp", Language::Cpp }, { L"hh", Language::Cpp }, { L"hxx", Language::Cpp },
            { L"inl", Language::Cpp }, { L"ipp", Language::Cpp }, { L"ixx", Language::Cpp }, { L"idl", Language::Cpp },
            { L"cs", Language::CSharp }, { L"csx", Language::CSharp },
            { L"css", Language::Css }, { L"scss", Language::Css }, { L"less", Language::Css },
            { L"go", Language::Go },
            { L"html", Language::Html }, { L"htm", Language::Html }, { L"xhtml", Language::Html },
            { L"ini", Language::Ini }, { L"cfg", Language::Ini }, { L"conf", Language::Ini }, { L"inf", Language::Ini },
            { L"reg", Language::Ini }, { L"properties", Language::Ini }, { L"editorconfig", Language::Ini }, { L"gitconfig", Language::Ini },
            { L"java", Language::Java },
            { L"js", Language::JavaScript }, { L"mjs", Language::JavaScript }, { L"cjs", Language::JavaScript }, { L"jsx", Language::JavaScript },
            { L"json", Language::Json }, { L"jsonc", Language::Json }, { L"webmanifest", Language::Json },
            { L"ps1", Language::PowerShell }, { L"psm1", Language::PowerShell }, { L"psd1", Language::PowerShell },
            { L"py", Language::Python }, { L"pyw", Language::Python }, { L"pyi", Language::Python },
            { L"rs", Language::Rust },
            { L"sql", Language::Sql },
            { L"ts", Language::TypeScript }, { L"tsx", Language::TypeScript }, { L"mts", Language::TypeScript }, { L"cts", Language::TypeScript },
            { L"xml", Language::Xml }, { L"xaml", Language::Xml }, { L"xsd", Language::Xml }, { L"xsl", Language::Xml }, { L"xslt", Language::Xml },
            { L"svg", Language::Xml }, { L"csproj", Language::Xml }, { L"vbproj", Language::Xml }, { L"vcxproj", Language::Xml },
            { L"props", Language::Xml }, { L"targets", Language::Xml }, { L"filters", Language::Xml }, { L"config", Language::Xml },
            { L"manifest", Language::Xml }, { L"resx", Language::Xml }, { L"nuspec", Language::Xml }, { L"plist", Language::Xml },
        };

        size_t slash = path.find_last_of(L"\\/");
        std::wstring name = Lower(path.substr(slash == npos ? 0 : slash + 1));
        size_t dot = name.find_last_of(L'.');
        if (dot == npos)
        {
            return Language::PlainText;
        }
        auto extension = std::wstring_view{ name }.substr(dot + 1);
        for (auto const& [candidate, language] : extensions)
        {
            if (candidate == extension)
            {
                return language;
            }
        }
        return Language::PlainText;
    }

    void Classify(Language language, std::wstring_view text, std::vector<TokenKind>& kinds)
    {
        kinds.assign(text.size(), TokenKind::Plain);
        if (text.size() <= MaxHighlightLength)
        {
            ClassifyInto(language, text, kinds.data());
        }
    }
}
