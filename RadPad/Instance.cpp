#include "pch.h"
#include "Instance.h"

using namespace winrt;

namespace
{
    // Window property that marks RadPad's main windows, and the WM_COPYDATA message id.
    constexpr wchar_t WindowProperty[] = L"RadPad.MainWindow";
    constexpr ULONG_PTR OpenFilesMessage = 0x4D4E4F46; // 'MNOF'

    // The app ID shared by RadPad's process, windows and jump list (Company.Product form).
    constexpr wchar_t AppId[] = L"RadPad.RadPad";

    std::vector<std::wstring> CommandLineFiles()
    {
        // Paths are made absolute here, as the receiving window has a different working directory.
        std::vector<std::wstring> files;
        int argc = 0;
        if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; ++i)
            {
                std::wstring full(32768, L'\0');
                DWORD length = GetFullPathNameW(argv[i], static_cast<DWORD>(full.size()), full.data(), nullptr);
                if (length > 0 && length < full.size())
                {
                    full.resize(length);
                    files.push_back(std::move(full));
                }
                else
                {
                    files.emplace_back(argv[i]);
                }
            }
            LocalFree(argv);
        }
        return files;
    }

    // RadPad windows on the current virtual desktop, frontmost first.
    std::vector<HWND> WindowsOnCurrentDesktop()
    {
        std::vector<HWND> windows;
        EnumWindows([](HWND hwnd, LPARAM param)
        {
            if (GetPropW(hwnd, WindowProperty) && IsWindowVisible(hwnd))
            {
                reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));

        try
        {
            auto desktops = create_instance<IVirtualDesktopManager>(CLSID_VirtualDesktopManager);
            std::erase_if(windows, [&](HWND hwnd)
            {
                BOOL onCurrent = TRUE;
                return SUCCEEDED(desktops->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)) && !onCurrent;
            });
        }
        catch (hresult_error const&)
        {
            // Without virtual desktop information, any open window will do.
        }
        return windows;
    }

    std::function<void(std::vector<std::wstring>)> g_openFiles;

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_NCDESTROY)
        {
            RemovePropW(hwnd, WindowProperty);
            RemoveWindowSubclass(hwnd, WindowProc, 1);
        }
        else if (message == WM_COPYDATA)
        {
            auto data = reinterpret_cast<COPYDATASTRUCT const*>(lParam);
            if (data->dwData == OpenFilesMessage)
            {
                // The paths are separated by nulls.
                std::wstring_view text{ static_cast<wchar_t const*>(data->lpData), data->cbData / sizeof(wchar_t) };
                std::vector<std::wstring> files;
                for (size_t start = 0; start < text.size();)
                {
                    auto end = text.find(L'\0', start);
                    if (end == std::wstring_view::npos)
                    {
                        end = text.size();
                    }
                    if (end > start)
                    {
                        files.emplace_back(text.substr(start, end - start));
                    }
                    start = end + 1;
                }

                if (IsIconic(hwnd))
                {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                SetForegroundWindow(hwnd);

                // Open the files after returning, so the sending process isn't kept waiting.
                Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([files = std::move(files)]
                {
                    if (g_openFiles)
                    {
                        g_openFiles(files);
                    }
                });
                return TRUE;
            }
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
}

namespace instance
{
    bool ForwardCommandLineToExistingWindow()
    {
        auto files = CommandLineFiles();
        if (files.empty())
        {
            return false; // starting without files opens a new window, as Notepad does
        }

        std::wstring text;
        for (auto const& file : files)
        {
            text += file;
            text += L'\0';
        }
        COPYDATASTRUCT data{ OpenFilesMessage, static_cast<DWORD>(text.size() * sizeof(wchar_t)), text.data() };

        for (HWND hwnd : WindowsOnCurrentDesktop())
        {
            // This process was just started by the user, so it may bring another window to the front;
            // pass that on to the window receiving the files.
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            AllowSetForegroundWindow(processId);

            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000, &result) && result)
            {
                return true;
            }
        }
        return false;
    }

    void SetProcessAppId()
    {
        SetCurrentProcessExplicitAppUserModelID(AppId);
    }

    void SetTaskbarRelaunch(HWND hwnd)
    {
        std::wstring exe(32768, L'\0');
        exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
        try
        {
            com_ptr<IPropertyStore> store;
            check_hresult(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(store.put())));
            auto set = [&](PROPERTYKEY const& key, std::wstring const& value)
            {
                PROPVARIANT variant;
                check_hresult(InitPropVariantFromString(value.c_str(), &variant));
                HRESULT hr = store->SetValue(key, variant);
                PropVariantClear(&variant);
                check_hresult(hr);
            };
            // The relaunch properties only count when all three are set, with the ID set first.
            set(PKEY_AppUserModel_ID, AppId);
            set(PKEY_AppUserModel_RelaunchCommand, L"\"" + exe + L"\"");
            set(PKEY_AppUserModel_RelaunchDisplayNameResource, L"RadPad");
            set(PKEY_AppUserModel_RelaunchIconResource, exe + L",0");
            check_hresult(store->Commit());
        }
        catch (hresult_error const&)
        {
            // The taskbar falls back to its own guess.
        }
    }

    void AcceptForwardedFiles(HWND hwnd, std::function<void(std::vector<std::wstring>)> openFiles)
    {
        g_openFiles = std::move(openFiles);
        SetWindowSubclass(hwnd, WindowProc, 1, 0);
        SetPropW(hwnd, WindowProperty, reinterpret_cast<HANDLE>(1));
    }
}
