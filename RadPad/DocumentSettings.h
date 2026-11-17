#pragma once

namespace winrt::RadPad::implementation
{
    // Settings each tab has its own copy of. New tabs start with the defaults (Format > Default settings).
    struct DocumentSettings
    {
        bool WordWrap{ false };
        int Zoom{ 100 };            // percent
        bool InsertSpaces{ false }; // Tab key inserts spaces rather than a tab
        int IndentSize{ 4 };        // spaces per indent level when inserting spaces
        int TabWidth{ 8 };          // width of a tab character, in spaces
        bool ShowWhitespace{ false };
        bool HighlightTrailingWhitespace{ true };
        bool RainbowBrackets{ true };
    };
}
