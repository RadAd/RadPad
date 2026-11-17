#include "pch.h"
#include "MainWindowHelpers.h"
#include "DefaultSettingsDialog.xaml.h"
#include "FontDialog.xaml.h"

// Dialogs: messages, Go to line, font, default settings, and About. The font and default settings dialogs are
// laid out in their own XAML files (FontDialog.xaml, DefaultSettingsDialog.xaml).
namespace winrt::RadPad::implementation
{
    IAsyncOperation<ContentDialogResult> MainWindow::ShowDialogAsync(ContentDialog dialog)
    {
        // Only one ContentDialog can be open per window.
        if (m_dialogOpen)
        {
            co_return ContentDialogResult::None;
        }

        auto strong = get_strong();
        m_dialogOpen = true;
        dialog.XamlRoot(Content().XamlRoot());
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Microsoft::UI::Xaml::Style>());
        dialog.RequestedTheme(RootGrid().ActualTheme());

        auto result = ContentDialogResult::None;
        try
        {
            result = co_await dialog.ShowAsync();
        }
        catch (hresult_error const&)
        {
        }
        m_dialogOpen = false;
        co_return result;
    }

    fire_and_forget MainWindow::ShowMessageAsync(hstring title, hstring message)
    {
        auto strong = get_strong();
        ContentDialog dialog;
        dialog.Title(box_value(title));
        TextBlock content;
        content.Text(message);
        content.TextWrapping(TextWrapping::Wrap);
        dialog.Content(content);
        dialog.CloseButtonText(L"OK");
        dialog.DefaultButton(ContentDialogButton::Close);
        co_await ShowDialogAsync(dialog);
    }

    fire_and_forget MainWindow::GoToLineAsync()
    {
        auto doc = CurrentDocument();
        if (!doc)
        {
            co_return;
        }

        auto strong = get_strong();
        auto text = EditorText(doc->Editor);
        int lineCount = LineCount(text);
        int currentLine = LineColumnAt(text, SelectionStart(doc->Editor)).first;

        NumberBox lineBox;
        lineBox.Header(box_value(L"Line number"));
        lineBox.Minimum(1);
        lineBox.Maximum(lineCount);
        lineBox.Value(currentLine);
        lineBox.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
        lineBox.Loaded([weakBox = make_weak(lineBox)](auto&&, auto&&)
        {
            if (auto box = weakBox.get())
            {
                box.Focus(FocusState::Programmatic);
            }
        });

        ContentDialog dialog;
        dialog.Title(box_value(L"Go to line"));
        dialog.Content(lineBox);
        dialog.PrimaryButtonText(L"Go to");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);

        if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary || std::isnan(lineBox.Value()))
        {
            co_return;
        }

        int line = std::clamp(static_cast<int>(lineBox.Value()), 1, lineCount);
        SelectAndReveal(doc->Editor, doc->ScrollViewer, IndexOfLine(EditorText(doc->Editor), line), 0);
        FocusEditor(doc);
    }

    fire_and_forget MainWindow::ChooseFontAsync()
    {
        // The dialog is laid out in FontDialog.xaml.
        auto strong = get_strong();
        FontDialog::Font current{ m_fontFamily, m_fontBold, m_fontItalic, m_fontPoints };
        auto dialog = make_self<FontDialog>();
        dialog->Show(current, GetSystemFontFamilies());
        if (co_await ShowDialogAsync(dialog.as<ContentDialog>()) != ContentDialogResult::Primary)
        {
            co_return;
        }

        auto font = dialog->Read(current);
        m_fontFamily = font.Family;
        m_fontBold = font.Bold;
        m_fontItalic = font.Italic;
        m_fontPoints = font.Points;
        UpdateFontMetrics();
        ApplyEditorSettingsToAll();
        UpdateWhitespaceAvailability();
        SaveSettings();
    }

    fire_and_forget MainWindow::EditDefaultSettingsAsync()
    {
        // The dialog is laid out in DefaultSettingsDialog.xaml.
        auto strong = get_strong();
        DefaultSettingsDialog::Values current{ m_defaults, m_defaultEncoding, m_defaultEol };
        auto dialog = make_self<DefaultSettingsDialog>();
        dialog->Show(current);
        if (auto doc = CurrentDocument())
        {
            dialog->SetCurrentTab({ doc->Settings, doc->Encoding, doc->Eol });
        }
        if (co_await ShowDialogAsync(dialog.as<ContentDialog>()) != ContentDialogResult::Primary)
        {
            co_return;
        }

        auto values = dialog->Read(current);
        m_defaults = values.Settings;
        m_defaultEncoding = values.Encoding;
        m_defaultEol = values.Eol;
        SaveSettings();
    }

    fire_and_forget MainWindow::ShowAboutAsync()
    {
        auto strong = get_strong();
        ContentDialog dialog;
        dialog.Title(box_value(L"About RadPad"));
        dialog.Content(box_value(L"Version " RADPAD_VERSION_STRING L"\n\nA tabbed Notepad clone built with WinUI 3 and C++/WinRT."));
        dialog.CloseButtonText(L"OK");
        dialog.DefaultButton(ContentDialogButton::Close);
        co_await ShowDialogAsync(dialog);
    }
}
