#pragma once

// Shared by the Syntax*.cpp files: language definitions as loaded from JSON, and the scanner and helpers
// that the tokenizers use. Not part of the interface in Syntax.h.
#include "Syntax.h"

#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace syntax::detail
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
    enum class Tokenizer { None, Code, Markup, Css, Batch, Ini, Markdown };

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

    // The loaded languages; the first is plain text.
    Registry& Languages();

    inline bool NamesEqual(std::wstring_view a, std::wstring_view b)
    {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    inline std::wstring Lower(std::wstring_view text)
    {
        std::wstring result{ text };
        for (auto& c : result)
        {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return result;
    }

    inline bool IsBlank(wchar_t c) { return c == L' ' || c == L'\t'; }
    inline bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
    inline bool IsIdentStart(wchar_t c) { return std::iswalpha(c) || c == L'_'; }
    inline bool IsIdentChar(wchar_t c) { return std::iswalnum(c) || c == L'_'; }

    // MAX_PATH, S_OK, D3D11: at least two characters, with capital letters and no small ones.
    inline bool IsMacroCase(std::wstring_view word)
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
    inline size_t FindIgnoreCase(std::wstring_view text, std::wstring_view lowerNeedle, size_t from)
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

    // The tokenizers, each setting the token kind of every character of s.Text in s.Kinds.
    void ClassifyCode(LanguageDefinition const& lang, Scanner const& s);   // SyntaxCode.cpp
    void ClassifyMarkup(Scanner const& s, LanguageDefinition const& lang); // SyntaxMarkup.cpp
    void ClassifyCss(Scanner const& s);                                    // SyntaxMarkup.cpp
    void ClassifyBatch(Scanner const& s, LanguageDefinition const& lang);  // SyntaxSimple.cpp
    void ClassifyIni(Scanner const& s);                                    // SyntaxSimple.cpp
    void ClassifyMarkdown(Scanner const& s);                               // SyntaxMarkdown.cpp

    // Classifies text with the language's tokenizer, then colours brackets if it uses rainbow brackets.
    // Tokenizers call it for code embedded in another language, such as <script> in HTML. In Syntax.cpp.
    void ClassifyInto(LanguageDefinition const& language, std::wstring_view text, TokenKind* kinds);
}
