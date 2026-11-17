#include "pch.h"
#include "SyntaxInternal.h"
#include "TextFile.h"

#include <filesystem>
#include <winrt/Windows.Data.Json.h>

// The language registry, loading definitions, and the public functions. The tokenizers are in Syntax*.cpp.
namespace syntax
{
    using namespace detail;

    namespace
    {
        LanguageDefinition MakePlainText()
        {
            LanguageDefinition plain;
            plain.Name = L"Plain text";
            plain.Kind = Tokenizer::None;
            return plain;
        }

        // Whether the Classify call in progress colours brackets, including in embedded languages.
        bool& RainbowBrackets()
        {
            thread_local bool on = true;
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
                { L"batch", Tokenizer::Batch }, { L"ini", Tokenizer::Ini }, { L"markdown", Tokenizer::Markdown },
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
                Invalid(L"\"tokenizer\" must be one of: code, markup, css, batch, ini, markdown");
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

    namespace detail
    {
        Registry& Languages()
        {
            static Registry registry{ { MakePlainText() } };
            return registry;
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
            case Tokenizer::Markdown:
                ClassifyMarkdown(s);
                break;
            }
            if (RainbowBrackets() && (language.Kind == Tokenizer::Code || language.Kind == Tokenizer::Css))
            {
                MarkBrackets(s);
            }
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

    void Classify(LanguageId language, std::wstring_view text, std::vector<TokenKind>& kinds, bool rainbowBrackets)
    {
        RainbowBrackets() = rainbowBrackets;
        kinds.assign(text.size(), TokenKind::Plain);
        if (text.size() <= MaxHighlightLength && language > PlainText && language < LanguageCount())
        {
            ClassifyInto(Languages().Languages[language], text, kinds.data());
        }
    }
}
