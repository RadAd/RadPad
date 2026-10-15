#include "pch.h"
#include "MainWindowHelpers.h"

// The tabs: switching, the recently used order for Ctrl+Tab, and the tab's right-click menu.
namespace winrt::RadPad::implementation
{
    void MainWindow::TabMenu_Opening(IInspectable const& sender, IInspectable const&)
    {
        // The menu is shared by all tabs; Target is the tab that was right-clicked.
        auto doc = FindDocument(sender.as<MenuFlyout>().Target());
        m_tabMenuDocument = doc;
        if (!doc)
        {
            return;
        }

        // An untitled document has no file name or folder yet.
        bool hasPath = !doc->Path.empty();
        TabCopyNameItem().IsEnabled(hasPath);
        TabCopyPathItem().IsEnabled(hasPath);
        TabOpenFolderItem().IsEnabled(hasPath);

        uint32_t index = 0;
        Tabs().TabItems().IndexOf(doc->Tab, index);
        TabCloseOthersItem().IsEnabled(m_documents.size() > 1);
        TabCloseRightItem().IsEnabled(index + 1 < Tabs().TabItems().Size());
    }

    void MainWindow::TabCopyName_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            CopyTextToClipboard(doc->DisplayName());
        }
    }

    void MainWindow::TabCopyPath_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            CopyTextToClipboard(doc->Path);
        }
    }

    void MainWindow::TabOpenFolder_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            OpenContainingFolder(doc->Path);
        }
    }

    void MainWindow::TabClose_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            CloseDocumentAsync(doc);
        }
    }

    void MainWindow::TabCloseOthers_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (auto doc = m_tabMenuDocument.lock())
        {
            std::vector<std::shared_ptr<Document>> others;
            std::copy_if(m_documents.begin(), m_documents.end(), std::back_inserter(others), [&](auto const& d) { return d != doc; });
            CloseDocumentsAsync(std::move(others), doc);
        }
    }

    void MainWindow::TabCloseRight_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto doc = m_tabMenuDocument.lock();
        uint32_t index = 0;
        if (!doc || !Tabs().TabItems().IndexOf(doc->Tab, index))
        {
            return;
        }

        // In tab order, which can differ from m_documents after tabs are dragged.
        std::vector<std::shared_ptr<Document>> right;
        for (uint32_t i = index + 1; i < Tabs().TabItems().Size(); ++i)
        {
            if (auto d = FindDocument(Tabs().TabItems().GetAt(i)))
            {
                right.push_back(d);
            }
        }
        CloseDocumentsAsync(std::move(right), doc);
    }

    void MainWindow::CycleRecentTabs(bool backwards)
    {
        // Like Visual Studio: Ctrl+Tab goes to the tab used before this one, and pressing Tab again while
        // Ctrl is held goes further back. The order stays fixed until Ctrl is released (EndTabCycle), so
        // repeated presses visit every tab rather than swapping between two.
        if (m_tabCycle.empty())
        {
            for (auto const& weak : m_recentTabs)
            {
                if (auto doc = weak.lock())
                {
                    m_tabCycle.push_back(doc);
                }
            }
            for (auto const& doc : m_documents) // any not yet shown, oldest last
            {
                if (std::find(m_tabCycle.begin(), m_tabCycle.end(), doc) == m_tabCycle.end())
                {
                    m_tabCycle.push_back(doc);
                }
            }
            m_tabCycleIndex = 0;
        }

        int count = static_cast<int>(m_tabCycle.size());
        if (count < 2)
        {
            m_tabCycle.clear();
            return;
        }
        m_tabCycleIndex = (m_tabCycleIndex + (backwards ? count - 1 : 1)) % count;
        Tabs().SelectedItem(m_tabCycle[m_tabCycleIndex]->Tab);
    }

    void MainWindow::EndTabCycle()
    {
        if (!m_tabCycle.empty())
        {
            m_tabCycle.clear();
            NoteRecentTab(CurrentDocument());
        }
    }

    void MainWindow::NoteRecentTab(std::shared_ptr<Document> const& doc)
    {
        // Moves doc to the front of the most-recently-used order, dropping closed documents.
        std::erase_if(m_recentTabs, [&](auto const& weak)
        {
            auto other = weak.lock();
            return !other || other == doc;
        });
        if (doc)
        {
            m_recentTabs.insert(m_recentTabs.begin(), doc);
        }
    }

    void MainWindow::SelectTabAt(int index)
    {
        if (index >= 0 && index < static_cast<int>(Tabs().TabItems().Size()))
        {
            Tabs().SelectedIndex(index);
        }
    }

    void MainWindow::Tabs_AddTabButtonClick(TabView const&, IInspectable const&)
    {
        CreateDocument();
    }

    void MainWindow::Tabs_TabCloseRequested(TabView const&, TabViewTabCloseRequestedEventArgs const& e)
    {
        if (auto doc = FindDocument(e.Tab()))
        {
            CloseDocumentAsync(doc);
        }
    }

    void MainWindow::Tabs_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        OnCurrentDocumentChanged();
    }

    void MainWindow::OnCurrentDocumentChanged()
    {
        auto current = CurrentDocument();
        if (m_tabCycle.empty()) // while cycling with Ctrl+Tab, the order changes when Ctrl is released
        {
            NoteRecentTab(current);
        }
        for (auto const& doc : m_documents)
        {
            doc->Editor.Visibility(doc == current ? Visibility::Visible : Visibility::Collapsed);
        }
        StatusMessageText().Text(L"");
        UpdateTitle();
        UpdateStatusBar();
        UpdateEditorOverlay();
        if (current)
        {
            FocusEditor(current);
            ScheduleRepaint(current);
        }
    }

    void MainWindow::FocusEditor(std::shared_ptr<Document> const& doc)
    {
        // Deferred so a just-made-visible editor has been laid out and can take focus.
        DispatcherQueue().TryEnqueue([weakDoc = std::weak_ptr<Document>(doc)]
        {
            if (auto d = weakDoc.lock())
            {
                d->Editor.Focus(FocusState::Programmatic);
            }
        });
    }
}
