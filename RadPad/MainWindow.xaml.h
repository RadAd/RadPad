#pragma once

#include "MainWindow.g.h"
#include "Syntax.h"
#include "Themes.h"
#include "Tools.h"
#include <regex>
#include "TextFile.h"

namespace winrt::RadPad::implementation
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
        // The file on disk as last loaded or saved (write time and size), to notice other programs changing it.
        std::optional<std::pair<uint64_t, uint64_t>> DiskStamp;
        std::optional<std::optional<std::pair<uint64_t, uint64_t>>> PendingDiskStamp; // a change seen, not yet acted on
        bool DeletedOnDisk{ false };                 // the file was deleted while open; saving recreates it
        bool ReadOnly{ false };                      // the file has the read-only attribute, so the editor allows no changes
        bool AskingToReload{ false };
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
        void RootGrid_PreviewKeyUp(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void Tabs_AddTabButtonClick(Microsoft::UI::Xaml::Controls::TabView const& sender, Windows::Foundation::IInspectable const& e);
        void Tabs_TabCloseRequested(Microsoft::UI::Xaml::Controls::TabView const& sender, Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& e);
        void Tabs_SelectionChanged(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void Tabs_TabDragStarting(Microsoft::UI::Xaml::Controls::TabView const& sender, Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs const& e);
        void Tabs_TabDragCompleted(Microsoft::UI::Xaml::Controls::TabView const& sender, Microsoft::UI::Xaml::Controls::TabViewTabDragCompletedEventArgs const& e);
        void Tabs_TabDroppedOutside(Microsoft::UI::Xaml::Controls::TabView const& sender, Microsoft::UI::Xaml::Controls::TabViewTabDroppedOutsideEventArgs const& e);
        void Tabs_TabStripDragOver(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& e);
        fire_and_forget Tabs_TabStripDrop(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs e);
        void FindBox_KeyDown(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void ReplaceBox_KeyDown(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void TabMenu_Opening(Windows::Foundation::IInspectable const& sender, Windows::Foundation::IInspectable const& e);

        // Menu and button commands, one handler per item
        using Sender = Windows::Foundation::IInspectable;
        using ClickArgs = Microsoft::UI::Xaml::RoutedEventArgs;
        void NewTab_Click(Sender const&, ClickArgs const&);
        void Open_Click(Sender const&, ClickArgs const&);
        void Save_Click(Sender const&, ClickArgs const&);
        void SaveAs_Click(Sender const&, ClickArgs const&);
        void SaveAll_Click(Sender const&, ClickArgs const&);
        void ReadOnly_Click(Sender const&, ClickArgs const&);
        void CloseTab_Click(Sender const&, ClickArgs const&);
        void Exit_Click(Sender const&, ClickArgs const&);
        void Undo_Click(Sender const&, ClickArgs const&);
        void Redo_Click(Sender const&, ClickArgs const&);
        void Cut_Click(Sender const&, ClickArgs const&);
        void Copy_Click(Sender const&, ClickArgs const&);
        void Paste_Click(Sender const&, ClickArgs const&);
        void Delete_Click(Sender const&, ClickArgs const&);
        void Find_Click(Sender const&, ClickArgs const&);
        void FindNext_Click(Sender const&, ClickArgs const&);
        void FindPrevious_Click(Sender const&, ClickArgs const&);
        void Replace_Click(Sender const&, ClickArgs const&);
        void ReplaceOne_Click(Sender const&, ClickArgs const&);
        void ReplaceAll_Click(Sender const&, ClickArgs const&);
        void CloseFind_Click(Sender const&, ClickArgs const&);
        void GoTo_Click(Sender const&, ClickArgs const&);
        void SelectAll_Click(Sender const&, ClickArgs const&);
        void TimeDate_Click(Sender const&, ClickArgs const&);
        void WordWrap_Click(Sender const&, ClickArgs const&);
        void Font_Click(Sender const&, ClickArgs const&);
        void ZoomIn_Click(Sender const&, ClickArgs const&);
        void ZoomOut_Click(Sender const&, ClickArgs const&);
        void ZoomReset_Click(Sender const&, ClickArgs const&);
        void StatusBar_Click(Sender const&, ClickArgs const&);
        void ShowWhitespace_Click(Sender const&, ClickArgs const&);
        void TrailingWhitespace_Click(Sender const&, ClickArgs const&);
        void RainbowBrackets_Click(Sender const&, ClickArgs const&);
        void MatchingBracket_Click(Sender const&, ClickArgs const&);
        void About_Click(Sender const&, ClickArgs const&);
        void Crlf_Click(Sender const&, ClickArgs const&);
        void Lf_Click(Sender const&, ClickArgs const&);
        void Cr_Click(Sender const&, ClickArgs const&);
        void Utf8_Click(Sender const&, ClickArgs const&);
        void Utf8Bom_Click(Sender const&, ClickArgs const&);
        void Utf16LE_Click(Sender const&, ClickArgs const&);
        void Utf16BE_Click(Sender const&, ClickArgs const&);
        void Ansi_Click(Sender const&, ClickArgs const&);
        void TabCopyName_Click(Sender const&, ClickArgs const&);
        void TabCopyPath_Click(Sender const&, ClickArgs const&);
        void TabOpenFolder_Click(Sender const&, ClickArgs const&);
        void TabClose_Click(Sender const&, ClickArgs const&);
        void TabCloseOthers_Click(Sender const&, ClickArgs const&);
        void TabCloseRight_Click(Sender const&, ClickArgs const&);
        void OutputPanel_Click(Sender const&, ClickArgs const&);
        void OutputStop_Click(Sender const&, ClickArgs const&);
        void OutputClear_Click(Sender const&, ClickArgs const&);
        void OutputClose_Click(Sender const&, ClickArgs const&);
        void OutputSplitter_PointerPressed(Sender const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OutputSplitter_PointerMoved(Sender const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OutputSplitter_PointerReleased(Sender const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OutputSplitter_PointerCaptureLost(Sender const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

    private:
        // Documents and tabs
        std::shared_ptr<Document> CreateDocument();
        std::shared_ptr<Document> CurrentDocument();
        std::shared_ptr<Document> FindDocument(Windows::Foundation::IInspectable const& tab);
        std::shared_ptr<Document> FindDocumentByPath(std::wstring const& path);
        void OpenFile(std::wstring const& path);
        void OpenWithDialog();

        // Recent files
        void AddRecentFile(std::wstring const& path);
        void OpenRecentFile(std::wstring path);
        void BuildRecentFilesMenu();
        void UpdateJumpList();
        bool DropRemovedJumpListFiles(IObjectArray* removed);
        void SyncWithJumpList();
        void OnRecentFilesChanged();
        bool SaveDocument(std::shared_ptr<Document> const& doc, bool saveAs);
        void SaveAll();
        Windows::Foundation::IAsyncOperation<bool> ConfirmCloseDocumentAsync(std::shared_ptr<Document> doc);
        fire_and_forget CloseDocumentAsync(std::shared_ptr<Document> doc);
        fire_and_forget CloseDocumentsAsync(std::vector<std::shared_ptr<Document>> docs, std::shared_ptr<Document> keep);
        void RemoveDocument(std::shared_ptr<Document> const& doc);
        void OpenContainingFolder(std::wstring const& path);
        void CycleRecentTabs(bool backwards);
        void EndTabCycle();
        void NoteRecentTab(std::shared_ptr<Document> const& doc);
        void SelectTabAt(int index);
        // Dragging a tab out of the window into a new RadPad process, or onto another RadPad window's tabs
        // (MainWindow.Tabs.cpp). The tab's document is written to a temporary file, which the window that
        // takes it reads and deletes; this window closes the tab only once the file has gone.
        bool WriteTabFile(std::wstring const& path, std::shared_ptr<Document> const& doc, RECT const& window);
        std::shared_ptr<Document> ReadTabFile(std::wstring const& path, std::optional<uint32_t> index, RECT* window);
        fire_and_forget MoveTabToNewProcessAsync(std::shared_ptr<Document> doc, std::wstring file);
        void OpenStartupTab(std::wstring const& file);
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
        void ShowLoadErrors();

        // External tools
        void ReloadTools();
        void CheckFilesOnDisk();
        void CheckDocumentFile(std::shared_ptr<Document> const& doc);
        bool ReloadDocument(std::shared_ptr<Document> const& doc);
        void UpdateReadOnly(std::shared_ptr<Document> const& doc);
        fire_and_forget AskToReloadAsync(std::shared_ptr<Document> doc, std::optional<std::pair<uint64_t, uint64_t>> stamp);
        void BuildToolsMenu();
        void AddToolItems(Windows::Foundation::Collections::IVector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const& items, std::vector<tools::Tool> const& list, int iconSize);
        fire_and_forget RunToolAsync(tools::Tool tool);
        Windows::Foundation::IAsyncOperation<bool> AskToolInputsAsync(tools::Tool tool, std::vector<std::wstring> labels, std::shared_ptr<Document> doc,
                                                                      std::shared_ptr<std::map<std::wstring, std::wstring>> inputs);
        void EditTools();

        // Output panel: tools run with their output captured
        enum class OutputColor { Normal, Error, Info };
        fire_and_forget RunToolInPanelAsync(tools::Tool tool, tools::FileContext file);
        void OnToolFinished(std::optional<DWORD> exitCode);
        void ShowOutputPanel(bool show);
        void HighlightOutputSplitter(bool highlight);
        void AppendOutput(std::wstring_view text, OutputColor color);
        bool OpenOutputLocation(std::wstring const& line);
        bool OpenOutputLocationAtCaret();

        // Colour themes
        void BuildColorThemeMenu();
        void SetColorTheme(std::wstring const& name);
        void ApplyColorTheme();
        void ReloadThemes();
        void OpenThemeFolder();
        Windows::UI::Color TokenColor(syntax::TokenKind kind);

        // Overlay: visible whitespace, or wrapped-line markers
        void UpdateEditorOverlay();
        void DrawTrailingWhitespace(std::shared_ptr<Document> const& doc, Microsoft::UI::Xaml::Controls::ScrollContentPresenter const& presenter);
        void ScheduleOverlayUpdate();
        void DrawWrapMarkers(std::shared_ptr<Document> const& doc, Microsoft::UI::Xaml::Controls::ScrollContentPresenter const& presenter);
        void UpdateWhitespaceAvailability();

        // Tabs and indentation
        void UpdateFontMetrics();
        void InsertTab(Microsoft::UI::Xaml::Controls::RichEditBox const& editor);
        bool SmartHome(Microsoft::UI::Xaml::Controls::RichEditBox const& editor, bool extend);
        void JumpToMatchingBracket(std::shared_ptr<Document> const& doc, bool select);
        bool SelectionSpansLines(Microsoft::UI::Xaml::Controls::RichEditBox const& editor);
        void ChangeIndent(std::shared_ptr<Document> const& doc, bool outdent);
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
        void EditCurrentDocument(std::function<void(std::shared_ptr<Document> const&)> const& edit);
        void SetEncoding(textfile::TextEncoding encoding);
        void SetLineEnding(textfile::LineEnding eol);
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
        std::optional<std::wregex> FindPattern();
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
        std::vector<std::weak_ptr<Document>> m_recentTabs;    // most recently used first, for Ctrl+Tab
        std::vector<std::shared_ptr<Document>> m_tabCycle;    // the order being stepped through while Ctrl is held
        int m_tabCycleIndex{ 0 };
        std::weak_ptr<Document> m_tabMenuDocument; // the tab whose right-click menu is open
        std::vector<std::pair<Microsoft::UI::Xaml::Controls::RadioMenuFlyoutItem, syntax::LanguageId>> m_languageItems; // both language menus
        std::wstring m_fontFamily{ L"Consolas" };
        double m_fontPoints{ 11.0 };
        bool m_fontBold{ false };
        bool m_fontItalic{ false };
        bool m_wordWrap{ false };
        bool m_statusBarVisible{ true };
        bool m_showWhitespace{ false };
        bool m_highlightTrailingWhitespace{ true };
        bool m_rainbowBrackets{ true };
        bool m_overlayUpdatePending{ false };
        std::vector<std::wstring> m_recentFiles;  // most recent first
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_jumpListSyncTimer{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_fileWatchTimer{ nullptr }; // checks open files for changes made by other programs
        std::vector<std::wstring> m_languageErrors; // from loading the language definitions
        std::vector<std::wstring> m_themeErrors;    // from loading the colour themes
        std::vector<tools::Tool> m_tools;           // the Tools menu
        std::vector<std::wstring> m_toolErrors;     // from loading the tools file
        std::map<std::pair<std::wstring, std::wstring>, std::wstring> m_toolInputs; // last text typed for each tool's $(Prompt) labels
        std::shared_ptr<tools::RunningTool> m_runningTool; // the tool running in the output panel, if any
        int m_runId{ 0 };                           // counts runs, so output from an earlier one is ignored
        std::wstring m_runName;                     // the tool last run in the output panel
        std::wstring m_runDirectory;                // and the folder it ran in, for relative paths in its output
        std::wstring m_runErrorPattern;
        std::chrono::steady_clock::time_point m_runStart;
        bool m_outputPanelVisible{ false };
        double m_outputPanelHeight{ 200 };
        double m_splitterDragStart{ -1 };           // pointer position when dragging the splitter began, or -1
        double m_splitterStartHeight{ 0 };
        std::wstring m_colorTheme{ themes::DefaultThemeName };
        themes::Palette m_palette;                  // the colour theme's colours for the app's light or dark mode
        std::vector<std::pair<Microsoft::UI::Xaml::Controls::RadioMenuFlyoutItem, int>> m_themeItems; // in the Color theme menu
        bool m_fontMonospaced{ false };    // whether the editor font is monospaced
        double m_spaceAdvance{ 0.5 };      // width of a space in the editor font, as a fraction of the font size
        bool m_insertSpaces{ false };      // Tab key inserts spaces rather than a tab
        int m_indentSize{ 4 };             // spaces per indent level when inserting spaces
        int m_tabWidth{ 8 };               // width of a tab character, in spaces
        int m_zoom{ 100 };
        bool m_loaded{ false };
        std::weak_ptr<Document> m_draggedDocument; // the tab being dragged from this window, if any
        std::wstring m_draggedTabFile;             // and the file holding it for another window to read
        bool m_dialogOpen{ false };
        bool m_closePending{ false };
        bool m_forceClose{ false };
        bool m_closed{ false };
    };
}

namespace winrt::RadPad::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
