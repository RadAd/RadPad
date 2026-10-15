#include "pch.h"
#include "MainWindowHelpers.h"

// Dialogs: messages, Go to line, font, and About.
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
        auto strong = get_strong();

        auto families = single_threaded_vector<IInspectable>();
        int selectedFamily = -1;
        for (auto const& family : GetSystemFontFamilies())
        {
            if (PathsEqual(family, m_fontFamily))
            {
                selectedFamily = static_cast<int>(families.Size());
            }
            families.Append(box_value(hstring{ family }));
        }
        if (selectedFamily < 0)
        {
            selectedFamily = static_cast<int>(families.Size());
            families.Append(box_value(hstring{ m_fontFamily }));
        }

        ComboBox familyBox;
        familyBox.Header(box_value(L"Font"));
        familyBox.ItemsSource(families);
        familyBox.SelectedIndex(selectedFamily);
        familyBox.HorizontalAlignment(HorizontalAlignment::Stretch);

        ComboBox styleBox;
        styleBox.Header(box_value(L"Style"));
        for (auto name : { L"Regular", L"Italic", L"Bold", L"Bold Italic" })
        {
            styleBox.Items().Append(box_value(name));
        }
        styleBox.SelectedIndex((m_fontBold ? 2 : 0) + (m_fontItalic ? 1 : 0));
        styleBox.Width(160);

        ComboBox sizeBox;
        sizeBox.Header(box_value(L"Size"));
        sizeBox.IsEditable(true);
        for (int size : { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 })
        {
            sizeBox.Items().Append(box_value(to_hstring(size)));
        }
        sizeBox.Text(std::format(L"{:g}", m_fontPoints));
        sizeBox.Width(110);

        TextBlock preview;
        preview.Text(L"AaBbYyZz 0123456789");
        preview.Height(64);
        preview.Margin(Thickness{ 0, 8, 0, 0 });

        auto readSize = [sizeBox, current = m_fontPoints]
        {
            hstring text = sizeBox.Text();
            if (auto item = sizeBox.SelectedItem())
            {
                text = unbox_value_or<hstring>(item, text);
            }
            double points = _wtof(text.c_str());
            return points >= 1 && points <= 400 ? points : current;
        };
        auto updatePreview = [familyBox, styleBox, preview, readSize]
        {
            if (auto family = familyBox.SelectedItem())
            {
                preview.FontFamily(Media::FontFamily{ unbox_value<hstring>(family) });
            }
            int style = styleBox.SelectedIndex();
            preview.FontWeight(style >= 2 ? Microsoft::UI::Text::FontWeights::Bold() : Microsoft::UI::Text::FontWeights::Normal());
            preview.FontStyle(style % 2 ? Windows::UI::Text::FontStyle::Italic : Windows::UI::Text::FontStyle::Normal);
            preview.FontSize(std::min(readSize(), 48.0) * 96.0 / 72.0);
        };
        familyBox.SelectionChanged([updatePreview](auto&&, auto&&) { updatePreview(); });
        styleBox.SelectionChanged([updatePreview](auto&&, auto&&) { updatePreview(); });
        sizeBox.SelectionChanged([updatePreview](auto&&, auto&&) { updatePreview(); });
        updatePreview();

        StackPanel styleRow;
        styleRow.Orientation(Orientation::Horizontal);
        styleRow.Spacing(12);
        styleRow.Children().Append(styleBox);
        styleRow.Children().Append(sizeBox);

        StackPanel content;
        content.Spacing(12);
        content.MinWidth(320);
        content.Children().Append(familyBox);
        content.Children().Append(styleRow);
        content.Children().Append(preview);

        ContentDialog dialog;
        dialog.Title(box_value(L"Font"));
        dialog.Content(content);
        dialog.PrimaryButtonText(L"OK");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);

        if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary)
        {
            co_return;
        }

        if (auto family = familyBox.SelectedItem())
        {
            m_fontFamily = unbox_value<hstring>(family);
        }
        int style = styleBox.SelectedIndex();
        m_fontBold = style >= 2;
        m_fontItalic = style % 2 == 1;
        m_fontPoints = readSize();
        UpdateFontMetrics();
        ApplyEditorSettingsToAll();
        UpdateWhitespaceAvailability();
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
