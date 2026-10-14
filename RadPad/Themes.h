#pragma once

#include "Syntax.h"
#include <array>

// Colour themes for syntax highlighting, loaded from JSON files.
namespace themes
{
    constexpr size_t ColorCount = static_cast<size_t>(syntax::TokenKind::Unknown);

    struct Palette
    {
        bool Dark{ false };                                     // made for a dark background
        std::array<winrt::Windows::UI::Color, ColorCount> Colors{}; // indexed by TokenKind
        std::optional<winrt::Windows::UI::Color> Background;    // the editor background; none uses the app's
    };

    // Loads the *.json theme files in each folder, in order. A theme with the same name as an earlier
    // one replaces it. "Visual Studio" is always available, built in. Returns a message per file that
    // couldn't be loaded.
    std::vector<std::wstring> LoadThemes(std::vector<std::wstring> const& folders);

    int ThemeCount();
    std::wstring_view ThemeName(int theme);
    int FindTheme(std::wstring_view name); // 0, the default theme, if not found
    constexpr wchar_t DefaultThemeName[] = L"Visual Studio";

    // The palette to use with the app in light or dark mode. A theme with only one palette uses it in both
    // modes, with a background so the colours stay readable.
    Palette ThemePalette(int theme, bool appIsDark);
}
