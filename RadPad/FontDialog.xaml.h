#pragma once

#include "FontDialog.g.h"

namespace winrt::RadPad::implementation
{
    // Format > Font. The caller fills it with Show, shows it, and on OK reads it back with Read.
    struct FontDialog : FontDialogT<FontDialog>
    {
        struct Font
        {
            std::wstring Family;
            bool Bold{ false };
            bool Italic{ false };
            double Points{ 11.0 };
        };

        FontDialog() = default;

        // families is the list to choose from; the font's own family is added if it isn't there.
        void Show(Font const& font, std::vector<std::wstring> const& families);
        // The font as chosen; a size that isn't a number from 1 to 400 keeps old's.
        Font Read(Font const& old);

        void Font_Changed(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

    private:
        double SizeOr(double old);
        void UpdatePreview();

        double m_points{ 11.0 }; // the size it was shown with, for the preview while the size box isn't a number
    };
}

namespace winrt::RadPad::factory_implementation
{
    struct FontDialog : FontDialogT<FontDialog, implementation::FontDialog>
    {
    };
}
