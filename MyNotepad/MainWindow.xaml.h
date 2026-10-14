#pragma once

#include "MainWindow.g.h"
#include "Syntax.h"
#include "TextFile.h"

namespace winrt::MyNotepad::implementation
{
    // One undoable edit: at Start, Removed was replaced by Inserted.
    struct UndoRecord
    {
        size_t Start{ 0 };
        std::wstring Removed;
        std::wstring Inserted;
    };

    // State for one open tab. The editor stores line breaks as '\r'.
    struct Document
    {
        Microsoft::UI::Xaml::Controls::TabViewItem Tab{ nullptr };
        Microsoft::UI::Xaml::Controls::RichEditBox Editor{ nullptr };
        std::wstring Path;      // empty for an untitled document
        std::wstring SavedText; // editor text as of the last load/save, used for dirty tracking
        textfile::TextEncoding Encoding{ textfile::TextEncoding::Utf8 };
        textfile::TextEncoding SavedEncoding{ textfile::TextEncoding::Utf8 };
        textfile::LineEnding Eol{ textfile::LineEnding::CRLF };
        textfile::LineEnding SavedEol{ textfile::LineEnding::CRLF };
        syntax::LanguageId Language{ syntax::PlainText };
        std::wstring HighlightedText;               // editor text when highlighting was last updated
        std::vector<syntax::TokenKind> AppliedKinds; // colour currently applied to each character of HighlightedText
        std::vector<syntax::TokenKind> DesiredKinds; // the tokenizer's colour for each character of HighlightedText
        int SelectionStart{ 0 };                     // last known selection
        int SelectionEnd{ 0 };
        int EditStart{ 0 };                          // selection when the pending edit began, i.e. the text it replaced
        int EditEnd{ 0 };
        // Undo history of text edits. RichEditBox's own undo is turned off because it also records
        // formatting changes, so Ctrl+Z would undo the highlighting.
        std::vector<UndoRecord> UndoStack;
        std::vector<UndoRecord> RedoStack;
        bool ApplyingUndo{ false };
        bool StartNewUndoRecord{ true };
        std::wstring Header;
        bool IsDirty{ false };
        bool IsClosing{ false };
        bool RefreshPending{ false };
        bool RepaintPending{ false };
        Microsoft::UI::Xaml::Controls::ScrollViewer ScrollViewer{ nullptr }; // inside the editor's template

        std::wstring DisplayName() const;
    };

    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;

        void InitializeComponent();

        // XAML event handlers
        void RootGrid_Loaded(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void RootGrid_PreviewKeyDown(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void Tabs_AddTabButtonClick(Microsoft::UI::Xaml::Controls::TabView const& sender, Windows::Foundation::IInspectable const& e);
        void Tabs_TabCloseRequested(Microsoft::UI::Xaml::Controls::TabView const& sender, Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& e);
        void Tabs_SelectionChanged(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void Command_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void Encoding_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void LineEnding_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void FindBox_KeyDown(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void ReplaceBox_KeyDown(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);

    private:
        // Documents and tabs
        std::shared_ptr<Document> CreateDocument();
        std::shared_ptr<Document> CurrentDocument();
        std::shared_ptr<Document> FindDocument(Windows::Foundation::IInspectable const& tab);
        std::shared_ptr<Document> FindDocumentByPath(std::wstring const& path);
        void OpenFile(std::wstring const& path);
        void OpenWithDialog();
        bool SaveDocument(std::shared_ptr<Document> const& doc, bool saveAs);
        void SaveAll();
        Windows::Foundation::IAsyncOperation<bool> ConfirmCloseDocumentAsync(std::shared_ptr<Document> doc);
        fire_and_forget CloseDocumentAsync(std::shared_ptr<Document> doc);
        void RemoveDocument(std::shared_ptr<Document> const& doc);
        void SelectAdjacentTab(int delta);
        void SelectTabAt(int index);
        void OnCurrentDocumentChanged();
        void FocusEditor(std::shared_ptr<Document> const& doc);
        void ScheduleRefresh(std::shared_ptr<Document> const& doc);

        // Syntax highlighting
        void UpdateHighlighting(std::shared_ptr<Document> const& doc, bool recolorAll);
        void PaintVisibleText(std::shared_ptr<Document> const& doc);
        void ScheduleRepaint(std::shared_ptr<Document> const& doc);

        // Undo
        void RecordEdit(std::shared_ptr<Document> const& doc, size_t start, std::wstring removed, std::wstring inserted);
        void UndoOrRedo(std::shared_ptr<Document> const& doc, bool redo);
        void ClearUndoHistory(std::shared_ptr<Document> const& doc);
        void SetLanguage(std::shared_ptr<Document> const& doc, syntax::LanguageId language);
        void ReloadLanguages();
        void OpenLanguageFolder();
        void ShowLanguageErrors();

        // Visible whitespace
        void UpdateWhitespaceOverlay();
        void UpdateWhitespaceAvailability();

        // Tabs and indentation
        void UpdateFontMetrics();
        void InsertTab(Microsoft::UI::Xaml::Controls::RichEditBox const& editor);
        void BuildIndentMenus();
        void UpdateIndentUI();
        void SetIndentation(bool insertSpaces, int indentSize, int tabWidth);
        std::vector<std::function<void()>> m_indentMenuUpdaters; // refresh the check marks in the indentation menus
        void BuildLanguageMenus();

        // Window lifetime
        void OnAppWindowClosing(Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args);
        fire_and_forget TryCloseWindowAsync();
        HWND WindowHandle();

        // UI state
        void ExecuteCommand(std::wstring_view command);
        void UpdateDocumentState(std::shared_ptr<Document> const& doc);
        void UpdateTitle();
        void UpdateStatusBar();
        void UpdateCaretPosition();
        void ApplyEditorSettings(std::shared_ptr<Document> const& doc);
        void ApplyFont(Microsoft::UI::Text::ITextCharacterFormat const& format);
        bool IsDarkTheme();
        void ApplyEditorSettingsToAll();
        void SetZoom(int zoom);
        void AddZoomAccelerators();
        void InsertText(Microsoft::UI::Xaml::Controls::RichEditBox const& editor, std::wstring_view text);
        fire_and_forget PasteAsync(std::weak_ptr<Document> weakDoc);

        // Find and replace
        void ShowFindPanel(bool showReplace);
        void HideFindPanel();
        bool FindNext(bool forward);
        void ReplaceOne();
        void ReplaceAll();
        bool IsFocusWithin(Microsoft::UI::Xaml::UIElement const& element);

        // Dialogs
        Windows::Foundation::IAsyncOperation<Microsoft::UI::Xaml::Controls::ContentDialogResult> ShowDialogAsync(Microsoft::UI::Xaml::Controls::ContentDialog dialog);
        fire_and_forget ShowMessageAsync(hstring title, hstring message);
        fire_and_forget GoToLineAsync();
        fire_and_forget ChooseFontAsync();
        fire_and_forget ShowAboutAsync();

        // Drag and drop
        void OnDragOver(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& e);
        fire_and_forget OnDropAsync(Microsoft::UI::Xaml::DragEventArgs e);

        // Settings
        void LoadSettings();
        void SaveSettings();

        std::vector<std::shared_ptr<Document>> m_documents;
        std::wstring m_fontFamily{ L"Consolas" };
        double m_fontPoints{ 11.0 };
        bool m_fontBold{ false };
        bool m_fontItalic{ false };
        bool m_wordWrap{ false };
        bool m_statusBarVisible{ true };
        bool m_showWhitespace{ false };
        std::vector<std::wstring> m_languageErrors; // from loading the language definitions
        bool m_fontMonospaced{ false };    // whether the editor font is monospaced
        double m_spaceAdvance{ 0.5 };      // width of a space in the editor font, as a fraction of the font size
        bool m_insertSpaces{ false };      // Tab key inserts spaces rather than a tab
        int m_indentSize{ 4 };             // spaces per indent level when inserting spaces
        int m_tabWidth{ 8 };               // width of a tab character, in spaces
        int m_zoom{ 100 };
        bool m_loaded{ false };
        bool m_dialogOpen{ false };
        bool m_closePending{ false };
        bool m_forceClose{ false };
    };
}

namespace winrt::MyNotepad::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
