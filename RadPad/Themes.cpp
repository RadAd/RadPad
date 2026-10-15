#include "pch.h"
#include "Themes.h"
#include "TextFile.h"
#include <filesystem>
#include <winrt/Windows.Data.Json.h>

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::UI::Color;

namespace themes
{
    namespace
    {
        struct Theme
        {
            std::wstring Name;
            std::optional<Palette> Light;
            std::optional<Palette> Dark;
        };

        // JSON keys for the colours, in TokenKind order.
        constexpr std::wstring_view ColorKeys[] = { L"text", L"comment", L"keyword", L"type", L"string", L"number", L"preprocessor", L"tag", L"attribute", L"variable", L"property", L"macro" };
        static_assert(std::size(ColorKeys) == ColorCount);

        Color Rgb(uint32_t rgb)
        {
            return { 0xFF, static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb) };
        }

        Palette MakePalette(bool dark, std::array<uint32_t, ColorCount> const& colors)
        {
            Palette palette;
            palette.Dark = dark;
            for (size_t i = 0; i < ColorCount; ++i)
            {
                palette.Colors[i] = Rgb(colors[i]);
            }
            return palette;
        }

        // Built in, so there's always a theme even if no files load. Themes\visual-studio.json has the same colours.
        Theme VisualStudio()
        {
            return {
                DefaultThemeName,
                MakePalette(false, { 0x000000, 0x008000, 0x0000FF, 0x2B91AF, 0xA31515, 0x098658, 0x808080, 0x800000, 0xE50000, 0x001080, 0x0451A5, 0x6F008A }),
                MakePalette(true, { 0xFFFFFF, 0x6A9955, 0x569CD6, 0x4EC9B0, 0xCE9178, 0xB5CEA8, 0x9B9B9B, 0x569CD6, 0x9CDCFE, 0x9CDCFE, 0x9CDCFE, 0xBEB7FF }),
            };
        }

        std::vector<Theme>& Themes()
        {
            static std::vector<Theme> themes{ VisualStudio() };
            return themes;
        }

        bool NamesEqual(std::wstring_view a, std::wstring_view b)
        {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        [[noreturn]] void Invalid(std::wstring const& message)
        {
            throw winrt::hresult_invalid_argument(message);
        }

        // "#RRGGBB"
        Color ParseColor(JsonObject const& json, std::wstring_view key)
        {
            auto value = json.GetNamedValue(key);
            std::wstring text = value.ValueType() == JsonValueType::String ? std::wstring{ value.GetString() } : std::wstring{};
            wchar_t* end = nullptr;
            unsigned long rgb = text.size() == 7 && text[0] == L'#' ? wcstoul(text.c_str() + 1, &end, 16) : 0;
            if (text.size() != 7 || text[0] != L'#' || end != text.c_str() + 7)
            {
                Invalid(L"\"" + std::wstring{ key } + L"\" must be a colour like \"#1E90FF\"");
            }
            return Rgb(static_cast<uint32_t>(rgb));
        }

        Palette ParsePalette(JsonObject const& json, bool dark)
        {
            // Colours that aren't given are the text colour, except macros, which fall back to the
            // preprocessor colour (themes written before macros had a colour of their own).
            Palette palette;
            palette.Dark = dark;
            Color text = json.HasKey(L"text") ? ParseColor(json, L"text") : Rgb(dark ? 0xD4D4D4 : 0x000000);
            for (size_t i = 0; i < ColorCount; ++i)
            {
                palette.Colors[i] = json.HasKey(ColorKeys[i]) ? ParseColor(json, ColorKeys[i]) : text;
            }
            auto macro = static_cast<size_t>(syntax::TokenKind::Macro);
            if (!json.HasKey(ColorKeys[macro]))
            {
                palette.Colors[macro] = palette.Colors[static_cast<size_t>(syntax::TokenKind::Preprocessor)];
            }
            if (json.HasKey(L"background"))
            {
                palette.Background = ParseColor(json, L"background");
            }
            return palette;
        }

        Theme ParseTheme(JsonObject const& json)
        {
            Theme theme;
            if (!json.HasKey(L"name") || json.GetNamedValue(L"name").ValueType() != JsonValueType::String || json.GetNamedString(L"name").empty())
            {
                Invalid(L"\"name\" is required");
            }
            theme.Name = json.GetNamedString(L"name");
            for (bool dark : { false, true })
            {
                wchar_t const* key = dark ? L"dark" : L"light";
                if (!json.HasKey(key))
                {
                    continue;
                }
                if (json.GetNamedValue(key).ValueType() != JsonValueType::Object)
                {
                    Invalid(L"\"" + std::wstring{ key } + L"\" must be an object of colours");
                }
                (dark ? theme.Dark : theme.Light) = ParsePalette(json.GetNamedObject(key), dark);
            }
            if (!theme.Light && !theme.Dark)
            {
                Invalid(L"a \"light\" or \"dark\" set of colours is required");
            }
            return theme;
        }
    }

    std::vector<std::wstring> LoadThemes(std::vector<std::wstring> const& folders)
    {
        std::vector<std::wstring> errors;
        std::vector<Theme> loaded{ VisualStudio() };
        for (auto const& folder : folders)
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

            for (auto const& file : files)
            {
                try
                {
                    auto theme = ParseTheme(JsonObject::Parse(textfile::Load(file.native()).Text));
                    auto existing = std::find_if(loaded.begin(), loaded.end(), [&](auto const& t) { return NamesEqual(t.Name, theme.Name); });
                    if (existing != loaded.end())
                    {
                        *existing = std::move(theme);
                    }
                    else
                    {
                        loaded.push_back(std::move(theme));
                    }
                }
                catch (winrt::hresult_error const& e)
                {
                    errors.push_back(file.native() + L": " + std::wstring{ e.message() });
                }
            }
        }

        // The default first, then by name.
        std::sort(loaded.begin() + 1, loaded.end(), [](auto const& a, auto const& b)
        {
            return CompareStringOrdinal(a.Name.c_str(), -1, b.Name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        Themes() = std::move(loaded);
        return errors;
    }

    int ThemeCount()
    {
        return static_cast<int>(Themes().size());
    }

    std::wstring_view ThemeName(int theme)
    {
        return Themes()[std::clamp(theme, 0, ThemeCount() - 1)].Name;
    }

    int FindTheme(std::wstring_view name)
    {
        auto const& themes = Themes();
        auto it = std::find_if(themes.begin(), themes.end(), [&](auto const& t) { return NamesEqual(t.Name, name); });
        return it == themes.end() ? 0 : static_cast<int>(it - themes.begin());
    }

    Palette ThemePalette(int theme, bool appIsDark)
    {
        auto const& t = Themes()[std::clamp(theme, 0, ThemeCount() - 1)];
        if (auto const& matching = appIsDark ? t.Dark : t.Light)
        {
            return *matching;
        }
        Palette palette = appIsDark ? *t.Light : *t.Dark;
        if (!palette.Background)
        {
            palette.Background = Rgb(palette.Dark ? 0x1E1E1E : 0xFFFFFF);
        }
        return palette;
    }
}
