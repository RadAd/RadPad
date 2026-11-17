#include "pch.h"
#include "MainWindowHelpers.h"

// File > Recent files, and the Recent list in the taskbar jump list.
namespace winrt::RadPad::implementation
{
    void MainWindow::AddRecentFile(std::wstring const& path)
    {
        constexpr size_t MaxRecentFiles = 10;

        std::erase_if(m_recentFiles, [&](auto const& existing) { return PathsEqual(existing, path); });
        m_recentFiles.insert(m_recentFiles.begin(), path);
        if (m_recentFiles.size() > MaxRecentFiles)
        {
            m_recentFiles.resize(MaxRecentFiles);
        }
        OnRecentFilesChanged();
    }

    void MainWindow::OnRecentFilesChanged()
    {
        UpdateJumpList(); // first, as it drops files the user removed from the jump list
        BuildRecentFilesMenu();
        SaveSettings();
    }

    void MainWindow::UpdateJumpList()
    {
        // The list shown when right-clicking the app on the taskbar. Windows' own Recent category only
        // shows file types registered to an app, and this unpackaged app registers none, so the recent
        // files are added as a custom category of shortcuts that start RadPad with the file.
        try
        {
            auto list = create_instance<ICustomDestinationList>(CLSID_DestinationList);
            UINT maxSlots = 0;
            com_ptr<IObjectArray> removed;
            check_hresult(list->BeginList(&maxSlots, IID_PPV_ARGS(removed.put())));

            // Files the user removed from the jump list can't be added back, so drop them from ours too.
            DropRemovedJumpListFiles(removed.get());

            // When installed from the Store, the shortcuts use the package's "radpad" command, as starting
            // the exe itself would run it without the package's identity (and so its settings).
            auto exe = ExePath();
            auto target = instance::IsPackaged() ? ExecutionAliasPath() : exe;
            auto items = create_instance<IObjectCollection>(CLSID_EnumerableObjectCollection);
            for (auto const& path : m_recentFiles)
            {
                auto link = create_instance<IShellLinkW>(CLSID_ShellLink);
                check_hresult(link->SetPath(target.c_str()));
                check_hresult(link->SetArguments((L"\"" + path + L"\"").c_str()));
                check_hresult(link->SetIconLocation(exe.c_str(), 0));
                link->SetDescription(path.c_str()); // the tooltip; fails harmlessly for very long paths

                // The text shown in the list is the file name.
                auto slash = path.find_last_of(L"\\/");
                PROPVARIANT title{};
                check_hresult(InitPropVariantFromString(slash == std::wstring::npos ? path.c_str() : path.c_str() + slash + 1, &title));
                auto store = link.as<IPropertyStore>();
                HRESULT hr = store->SetValue(PKEY_Title, title);
                PropVariantClear(&title);
                check_hresult(hr);
                check_hresult(store->Commit());
                check_hresult(items->AddObject(link.get()));
            }
            if (!m_recentFiles.empty())
            {
                check_hresult(list->AppendCategory(L"Recent", items.as<IObjectArray>().get()));
            }
            check_hresult(list->CommitList());
        }
        catch (hresult_error const&)
        {
            // The jump list is a convenience; the File menu still has the list.
        }
    }

    bool MainWindow::DropRemovedJumpListFiles(IObjectArray* removed)
    {
        // The removed items are the shortcuts made by UpdateJumpList, whose arguments are the quoted path.
        auto oldCount = m_recentFiles.size();
        UINT removedCount = 0;
        if (FAILED(removed->GetCount(&removedCount)))
        {
            return false;
        }
        std::wstring arguments(32768, L'\0');
        for (UINT i = 0; i < removedCount; ++i)
        {
            com_ptr<IShellLinkW> link;
            if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(link.put())))
                && SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))))
            {
                std::wstring path{ arguments.c_str() };
                if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
                {
                    path = path.substr(1, path.size() - 2);
                }
                std::erase_if(m_recentFiles, [&](auto const& existing) { return PathsEqual(existing, path); });
            }
        }
        return m_recentFiles.size() != oldCount;
    }

    void MainWindow::SyncWithJumpList()
    {
        // Windows doesn't say when the user removes an item from the jump list, so this is called when the
        // window is activated and before the Recent files menu opens.
        try
        {
            auto list = create_instance<ICustomDestinationList>(CLSID_DestinationList);
            UINT maxSlots = 0;
            com_ptr<IObjectArray> removed;
            check_hresult(list->BeginList(&maxSlots, IID_PPV_ARGS(removed.put())));
            bool changed = DropRemovedJumpListFiles(removed.get());
            list->AbortList();
            if (changed)
            {
                OnRecentFilesChanged();
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    void MainWindow::OpenRecentFile(std::wstring path)
    {
        if (!instance::IsUrl(path) && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::erase_if(m_recentFiles, [&](auto const& existing) { return PathsEqual(existing, path); });
            OnRecentFilesChanged();
            ShowMessageAsync(hstring{ AppName }, hstring{ std::format(L"\"{}\" can't be found, so it has been removed from Recent files.", path) });
            return;
        }
        OpenFile(path);
    }

    void MainWindow::BuildRecentFilesMenu()
    {
        auto items = RecentFilesMenu().Items();
        items.Clear();
        if (m_recentFiles.empty())
        {
            MenuFlyoutItem none;
            none.Text(L"No recent files");
            none.IsEnabled(false);
            items.Append(none);
            return;
        }

        for (size_t i = 0; i < m_recentFiles.size(); ++i)
        {
            // "1  name.txt", with the full path in the tooltip and as the accessible name.
            auto const& path = m_recentFiles[i];
            auto slash = path.find_last_of(L"\\/");
            auto name = slash == std::wstring::npos ? path : path.substr(slash + 1);
            auto folder = slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);

            // Long folders are shortened in the middle ("C:\Users\...\Project") to keep the menu narrow.
            wchar_t shortFolder[64]{};
            if (!PathCompactPathExW(shortFolder, folder.c_str(), ARRAYSIZE(shortFolder), 0))
            {
                wcsncpy_s(shortFolder, folder.c_str(), _TRUNCATE);
            }

            MenuFlyoutItem item;
            item.Text(std::format(L"{}  {}", i + 1, name));
            item.KeyboardAcceleratorTextOverride(shortFolder); // shown dimmed on the right
            ToolTipService::SetToolTip(item, box_value(hstring{ path }));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(item, path);
            item.Click([this, path](auto&&, auto&&) { OpenRecentFile(path); });
            items.Append(item);
        }

        items.Append(MenuFlyoutSeparator{});
        MenuFlyoutItem clear;
        clear.Text(L"Clear recent files");
        clear.Click([this](auto&&, auto&&)
        {
            m_recentFiles.clear();
            OnRecentFilesChanged();
        });
        items.Append(clear);
    }
}
