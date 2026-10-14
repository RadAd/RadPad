#pragma once

// Syntax highlighting: language definitions loaded from JSON files, language detection, and a
// lightweight tokenizer that assigns a token kind to every character. Text uses '\r' as its only
// line break, as in the editor.
namespace syntax
{
    // Index into the loaded languages. Plain text is always 0.
    using LanguageId = int;
    constexpr LanguageId PlainText = 0;

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

    // Replaces the loaded languages with the *.json definitions in each folder, in order. A definition
    // with the same name as an earlier one replaces it, so later folders override earlier ones.
    // Returns a message for each file that couldn't be loaded.
    std::vector<std::wstring> LoadLanguages(std::vector<std::wstring> const& folders);

    int LanguageCount();
    std::wstring_view LanguageName(LanguageId language);
    LanguageId FindLanguage(std::wstring_view name); // PlainText if not found
    LanguageId LanguageFromPath(std::wstring_view path);

    // Resizes kinds to text.size() and sets the token kind of every character.
    void Classify(LanguageId language, std::wstring_view text, std::vector<TokenKind>& kinds);
}
