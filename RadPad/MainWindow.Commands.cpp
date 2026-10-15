#include "pch.h"
#include "MainWindowHelpers.h"

// Menu and button commands. Each XAML item names its own handler, so a misspelt or missing one fails the build.
namespace winrt::RadPad::implementation
{
    void MainWindow::NewTab_Click(IInspectable const&, RoutedEventArgs const&) { CreateDocument(); }
    void MainWindow::Open_Click(IInspectable const&, RoutedEventArgs const&) { OpenWithDialog(); }
    void MainWindow::SaveAll_Click(IInspectable const&, RoutedEventArgs const&) { SaveAll(); }
    void MainWindow::Exit_Click(IInspectable const&, RoutedEventArgs const&) { TryCloseWindowAsync(); }
    void MainWindow::Find_Click(IInspectable const&, RoutedEventArgs const&) { ShowFindPanel(false); }
    void MainWindow::Replace_Click(IInspectable const&, RoutedEventArgs const&) { ShowFindPanel(true); }
    void MainWindow::CloseFind_Click(IInspectable const&, RoutedEventArgs const&) { HideFindPanel(); }
    void MainWindow::FindNext_Click(IInspectable const&, RoutedEventArgs const&) { FindNext(true); }
    void MainWindow::FindPrevious_Click(IInspectable const&, RoutedEventArgs const&) { FindNext(false); }
    void MainWindow::ReplaceOne_Click(IInspectable const&, RoutedEventArgs const&) { ReplaceOne(); }
    void MainWindow::ReplaceAll_Click(IInspectable const&, RoutedEventArgs const&) { ReplaceAll(); }
    void MainWindow::GoTo_Click(IInspectable const&, RoutedEventArgs const&) { GoToLineAsync(); }
    void MainWindow::Font_Click(IInspectable const&, RoutedEventArgs const&) { ChooseFontAsync(); }
    void MainWindow::About_Click(IInspectable const&, RoutedEventArgs const&) { ShowAboutAsync(); }
    void MainWindow::ZoomIn_Click(IInspectable const&, RoutedEventArgs const&) { SetZoom(m_zoom + ZoomStep); }
    void MainWindow::ZoomOut_Click(IInspectable const&, RoutedEventArgs const&) { SetZoom(m_zoom - ZoomStep); }
    void MainWindow::ZoomReset_Click(IInspectable const&, RoutedEventArgs const&) { SetZoom(DefaultZoom); }

    void MainWindow::WordWrap_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_wordWrap = WordWrapItem().IsChecked();
        ApplyEditorSettingsToAll();
        UpdateWhitespaceAvailability();
    }

    void MainWindow::StatusBar_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_statusBarVisible = StatusBarItem().IsChecked();
        StatusBar().Visibility(m_statusBarVisible ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::RainbowBrackets_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rainbowBrackets = RainbowBracketsItem().IsChecked();
        syntax::SetRainbowBrackets(m_rainbowBrackets);
        for (auto const& doc : m_documents)
        {
            UpdateHighlighting(doc, false);
        }
    }

    void MainWindow::MatchingBracket_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            JumpToMatchingBracket(doc, false);
            FocusEditor(doc);
        }
    }

    void MainWindow::TrailingWhitespace_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_highlightTrailingWhitespace = TrailingWhitespaceItem().IsChecked();
        UpdateEditorOverlay();
    }

    void MainWindow::ShowWhitespace_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_showWhitespace = ShowWhitespaceItem().IsChecked();
        UpdateEditorOverlay();
    }

    void MainWindow::Save_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            SaveDocument(doc, false);
        }
    }

    void MainWindow::SaveAs_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            SaveDocument(doc, true);
        }
    }

    void MainWindow::CloseTab_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = CurrentDocument())
        {
            CloseDocumentAsync(doc);
        }
    }

    void MainWindow::EditCurrentDocument(std::function<void(std::shared_ptr<Document> const&)> const& edit)
    {
        // For Edit menu commands: act on the current document, then return focus to its editor.
        if (auto doc = CurrentDocument())
        {
            edit(doc);
            FocusEditor(doc);
        }
    }

    void MainWindow::Undo_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { UndoOrRedo(doc, false); }); }
    void MainWindow::Redo_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { UndoOrRedo(doc, true); }); }
    void MainWindow::Cut_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { CopySelection(doc->Editor); InsertText(doc->Editor, L""); }); }
    void MainWindow::Copy_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { CopySelection(doc->Editor); }); }
    void MainWindow::Paste_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { PasteAsync(doc); }); }
    void MainWindow::Delete_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { InsertText(doc->Editor, L""); }); }
    void MainWindow::SelectAll_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { Select(doc->Editor, 0, static_cast<int>(EditorText(doc->Editor).size())); }); }
    void MainWindow::TimeDate_Click(IInspectable const&, RoutedEventArgs const&) { EditCurrentDocument([&](auto const& doc) { InsertText(doc->Editor, CurrentTimeDate()); }); }

    void MainWindow::SetEncoding(TextEncoding encoding)
    {
        if (auto doc = CurrentDocument())
        {
            doc->Encoding = encoding;
            UpdateDocumentState(doc);
            UpdateStatusBar();
        }
    }

    void MainWindow::Utf8_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf8); }
    void MainWindow::Utf8Bom_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf8Bom); }
    void MainWindow::Utf16LE_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf16LE); }
    void MainWindow::Utf16BE_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Utf16BE); }
    void MainWindow::Ansi_Click(IInspectable const&, RoutedEventArgs const&) { SetEncoding(TextEncoding::Ansi); }

    void MainWindow::SetLineEnding(LineEnding eol)
    {
        if (auto doc = CurrentDocument())
        {
            doc->Eol = eol;
            UpdateDocumentState(doc);
            UpdateStatusBar();
        }
    }

    void MainWindow::Crlf_Click(IInspectable const&, RoutedEventArgs const&) { SetLineEnding(LineEnding::CRLF); }
    void MainWindow::Lf_Click(IInspectable const&, RoutedEventArgs const&) { SetLineEnding(LineEnding::LF); }
    void MainWindow::Cr_Click(IInspectable const&, RoutedEventArgs const&) { SetLineEnding(LineEnding::CR); }
}
