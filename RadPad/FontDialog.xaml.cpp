#include "pch.h"
#include "FontDialog.xaml.h"
#if __has_include("FontDialog.g.cpp")
#include "FontDialog.g.cpp"
#endif

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::RadPad::implementation
{
    void FontDialog::Show(Font const& font, std::vector<std::wstring> const& families)
    {
        auto items = single_threaded_vector<Windows::Foundation::IInspectable>();
        int selected = -1;
        for (auto const& family : families)
        {
            if (CompareStringOrdinal(family.c_str(), -1, font.Family.c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                selected = static_cast<int>(items.Size());
            }
            items.Append(box_value(hstring{ family }));
        }
        if (selected < 0)
        {
            selected = static_cast<int>(items.Size());
            items.Append(box_value(hstring{ font.Family }));
        }
        FamilyBox().ItemsSource(items);
        FamilyBox().SelectedIndex(selected);
        StyleBox().SelectedIndex((font.Bold ? 2 : 0) + (font.Italic ? 1 : 0));
        // A size in the list is selected. Any other is typed in, once the box is loaded: before then an
        // editable ComboBox doesn't keep its text.
        hstring size{ std::format(L"{:g}", font.Points) };
        auto sizes = SizeBox().Items();
        for (uint32_t i = 0; i < sizes.Size(); ++i)
        {
            if (unbox_value_or<hstring>(sizes.GetAt(i), L"") == size)
            {
                SizeBox().SelectedIndex(static_cast<int>(i));
            }
        }
        if (SizeBox().SelectedIndex() < 0)
        {
            SizeBox().Loaded([weakBox = make_weak(SizeBox()), size](auto&&, auto&&)
            {
                if (auto box = weakBox.get())
                {
                    box.Text(size);
                }
            });
        }
        m_points = font.Points;
        UpdatePreview();
    }

    FontDialog::Font FontDialog::Read(Font const& old)
    {
        Font font = old;
        if (auto family = FamilyBox().SelectedItem())
        {
            font.Family = unbox_value<hstring>(family);
        }
        int style = StyleBox().SelectedIndex();
        font.Bold = style >= 2;
        font.Italic = style % 2 == 1;
        font.Points = SizeOr(old.Points);
        return font;
    }

    double FontDialog::SizeOr(double old)
    {
        // A size picked from the list, or else typed in.
        hstring text = SizeBox().Text();
        if (auto item = SizeBox().SelectedItem())
        {
            text = unbox_value_or<hstring>(item, text);
        }
        double points = _wtof(text.c_str());
        return points >= 1 && points <= 400 ? points : old;
    }

    void FontDialog::Font_Changed(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        UpdatePreview();
    }

    void FontDialog::UpdatePreview()
    {
        auto preview = Preview();
        if (auto family = FamilyBox().SelectedItem())
        {
            preview.FontFamily(Media::FontFamily{ unbox_value<hstring>(family) });
        }
        int style = StyleBox().SelectedIndex();
        preview.FontWeight(style >= 2 ? Microsoft::UI::Text::FontWeights::Bold() : Microsoft::UI::Text::FontWeights::Normal());
        preview.FontStyle(style % 2 ? Windows::UI::Text::FontStyle::Italic : Windows::UI::Text::FontStyle::Normal);
        preview.FontSize(std::min(SizeOr(m_points), 48.0) * 96.0 / 72.0);
    }
}
