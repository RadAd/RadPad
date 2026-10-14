#pragma once

// Opening files in a window that's already open, rather than starting another one.
namespace instance
{
    // If the command line names files and a MyNotepad window is open on the current virtual desktop,
    // sends the files to that window, brings it to the front and returns true. The caller should then exit.
    bool ForwardCommandLineToExistingWindow();

    // Marks hwnd as a MyNotepad window that accepts files from ForwardCommandLineToExistingWindow.
    // openFiles is called on the window's thread with full paths.
    void AcceptForwardedFiles(HWND hwnd, std::function<void(std::vector<std::wstring>)> openFiles);
}
