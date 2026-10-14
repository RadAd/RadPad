#pragma once

// Syntax highlighting: language detection and a lightweight tokenizer that assigns a token kind
// to every character. Text uses '\r' as its only line break, as in the editor.
namespace syntax
{
    enum class Language : uint8_t
    {
        PlainText,
        Batch,
        C,
        Cpp,
        CSharp,
        Css,
        Go,
        Html,
        Ini,
        Java,
        JavaScript,
        Json,
        PowerShell,
        Python,
        Rust,
        Sql,
        TypeScript,
        Xml,
    };

    constexpr int LanguageCount = 18;

    enum class TokenKind : uint8_t
    {
        Plain,
        Comment,
        Keyword,
        Type,
        String,
        Number,
        Preprocessor,
        Tag,
        Attribute,
        Variable,
        Property,
        Unknown, // never produced by Classify; marks text whose colour hasn't been applied yet
    };

    // Longer documents are shown as plain text to keep typing responsive.
    constexpr size_t MaxHighlightLength = 1'000'000;

    std::wstring_view LanguageName(Language language);
    Language LanguageFromPath(std::wstring_view path);

    // Resizes kinds to text.size() and sets the token kind of every character.
    void Classify(Language language, std::wstring_view text, std::vector<TokenKind>& kinds);
}
