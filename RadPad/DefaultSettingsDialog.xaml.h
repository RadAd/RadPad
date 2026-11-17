#pragma once

#include "DefaultSettingsDialog.g.h"
#include "DocumentSettings.h"
#include "TextFile.h"

namespace winrt::RadPad::implementation
{
    // Format > Default settings. The caller fills it with Show, shows it, and on OK reads it back with Read.
    struct DefaultSettingsDialog : DefaultSettingsDialogT<DefaultSettingsDialog>
    {
        // Everything the dialog edits.
        struct Values
        {
            DocumentSettings Settings;
            textfile::TextEncoding Encoding{ textfile::TextEncoding::Utf8 };
            textfile::LineEnding Eol{ textfile::LineEnding::CRLF };
        };

        DefaultSettingsDialog() = default;
        void InitializeComponent();

        void Show(Values const& values);
        // The values as edited; a number box left empty keeps its value from old.
        Values Read(Values const& old);
        // Enables "Use the current tab's settings", which shows these.
        void SetCurrentTab(Values const& values);

        void FromTab_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        Values m_currentTab;
    };
}

namespace winrt::RadPad::factory_implementation
{
    struct DefaultSettingsDialog : DefaultSettingsDialogT<DefaultSettingsDialog, implementation::DefaultSettingsDialog>
    {
    };
}
