#pragma once

#include "MainWindow.g.h"
#include "TextFile.h"

namespace winrt::MyNotepad::implementation
{
    // State for one open tab. The editor TextBox stores line breaks as '\r'.
    struct Document
    {
        Microsoft::UI::Xaml::Controls::TabViewItem Tab{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBox Editor{ nullptr };
        std::wstring Path;      // empty for an untitled document
        std::wstring SavedText; // editor text as of the last load/save, used for dirty tracking
        textfile::TextEncoding Encoding{ textfile::TextEncoding::Utf8 };
        textfile::TextEncoding SavedEncoding{ textfile::TextEncoding::Utf8 };
        textfile::LineEnding Eol{ textfile::LineEnding::CRLF };
        textfile::LineEnding SavedEol{ textfile::LineEnding::CRLF };
        std::wstring Header;
        bool IsDirty{ false };
        bool IsClosing{ false };

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
        void ApplyEditorSettings(Microsoft::UI::Xaml::Controls::TextBox const& editor);
        void ApplyEditorSettingsToAll();
        void SetZoom(int zoom);
        void AddZoomAccelerators();
        void InsertText(Microsoft::UI::Xaml::Controls::TextBox const& editor, std::wstring_view text);

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
