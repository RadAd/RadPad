#pragma once

// Opening files in a window that's already open, rather than starting another one.
namespace instance
{
    // "RadPad.exe /tab <file>" starts a window with a tab dragged out of another RadPad window, from the
    // file it wrote (see MainWindow::WriteTabFile). It always starts a new window.
    constexpr wchar_t TabArgument[] = L"/tab";

    // Whether a command-line argument (or other file name) is an http or https address, which RadPad
    // downloads rather than opening as a file.
    inline bool IsUrl(std::wstring_view text)
    {
        auto startsWith = [&](std::wstring_view prefix)
        {
            return text.size() > prefix.size() && CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
        };
        return startsWith(L"http://") || startsWith(L"https://");
    }

    // If the command line names files and a RadPad window is open on the current virtual desktop,
    // sends the files to that window, brings it to the front and returns true. The caller should then exit.
    bool ForwardCommandLineToExistingWindow();

    // True when running from an MSIX package (e.g. installed from the Store). Windows then gives the
    // process the package's app ID and knows how to relaunch it, so RadPad mustn't set its own, and
    // settings are kept in the package's own data folder.
    bool IsPackaged();

    // Gives the process an explicit app ID, which its windows, taskbar buttons and jump list share.
    // Call before any window is created.
    void SetProcessAppId();

    // Tells the taskbar how to start another RadPad from hwnd's button (middle-click or Shift+click): its app
    // ID, and a relaunch command that starts this exe without files. Without these, the taskbar can't work
    // out what to start for an unpackaged WinUI app, and does nothing.
    void SetTaskbarRelaunch(HWND hwnd);

    // Marks hwnd as a RadPad window that accepts files from ForwardCommandLineToExistingWindow.
    // openFiles is called on the window's thread with full paths (or URLs).
    void AcceptForwardedFiles(HWND hwnd, std::function<void(std::vector<std::wstring>)> openFiles);
}
