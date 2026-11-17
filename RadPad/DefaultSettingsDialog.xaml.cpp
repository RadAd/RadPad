#include "pch.h"
#include "DefaultSettingsDialog.xaml.h"
#if __has_include("DefaultSettingsDialog.g.cpp")
#include "DefaultSettingsDialog.g.cpp"
#endif

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using textfile::LineEnding;
using textfile::TextEncoding;

namespace winrt::RadPad::implementation
{
    namespace
    {
        // A number box's value, or old if it's been left empty.
        int NumberOr(NumberBox const& box, int old)
        {
            return std::isnan(box.Value()) ? old : static_cast<int>(box.Value());
        }

        bool IsChecked(CheckBox const& box)
        {
            return box.IsChecked() && box.IsChecked().Value();
        }
    }

    void DefaultSettingsDialog::InitializeComponent()
    {
        DefaultSettingsDialogT::InitializeComponent();

        // The names come from TextFile, so they match the status bar.
        for (int i = 0; i < textfile::EncodingCount; ++i)
        {
            EncodingBox().Items().Append(box_value(hstring{ textfile::EncodingName(static_cast<TextEncoding>(i)) }));
        }
        for (auto eol : { LineEnding::CRLF, LineEnding::LF, LineEnding::CR })
        {
            LineEndingBox().Items().Append(box_value(hstring{ textfile::LineEndingName(eol) }));
        }
    }

    void DefaultSettingsDialog::Show(Values const& values)
    {
        auto const& settings = values.Settings;
        WordWrapBox().IsChecked(settings.WordWrap);
        ZoomBox().Value(settings.Zoom);
        IndentKindBox().SelectedIndex(settings.InsertSpaces ? 1 : 0);
        IndentSizeBox().Value(settings.IndentSize);
        TabWidthBox().Value(settings.TabWidth);
        EncodingBox().SelectedIndex(static_cast<int>(values.Encoding));
        LineEndingBox().SelectedIndex(static_cast<int>(values.Eol));
        ShowWhitespaceBox().IsChecked(settings.ShowWhitespace);
        TrailingWhitespaceBox().IsChecked(settings.HighlightTrailingWhitespace);
        RainbowBracketsBox().IsChecked(settings.RainbowBrackets);
    }

    DefaultSettingsDialog::Values DefaultSettingsDialog::Read(Values const& old)
    {
        Values values;
        auto& settings = values.Settings;
        settings.WordWrap = IsChecked(WordWrapBox());
        settings.Zoom = std::clamp(NumberOr(ZoomBox(), old.Settings.Zoom), static_cast<int>(ZoomBox().Minimum()), static_cast<int>(ZoomBox().Maximum()));
        settings.InsertSpaces = IndentKindBox().SelectedIndex() == 1;
        settings.IndentSize = std::clamp(NumberOr(IndentSizeBox(), old.Settings.IndentSize), 1, 8);
        settings.TabWidth = std::clamp(NumberOr(TabWidthBox(), old.Settings.TabWidth), 1, 8);
        settings.ShowWhitespace = IsChecked(ShowWhitespaceBox());
        settings.HighlightTrailingWhitespace = IsChecked(TrailingWhitespaceBox());
        settings.RainbowBrackets = IsChecked(RainbowBracketsBox());
        values.Encoding = static_cast<TextEncoding>(std::max(EncodingBox().SelectedIndex(), 0));
        values.Eol = static_cast<LineEnding>(std::max(LineEndingBox().SelectedIndex(), 0));
        return values;
    }

    void DefaultSettingsDialog::SetCurrentTab(Values const& values)
    {
        m_currentTab = values;
        FromTabButton().IsEnabled(true);
    }

    void DefaultSettingsDialog::FromTab_Click(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        Show(m_currentTab);
    }
}
