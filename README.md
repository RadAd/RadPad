# MyNotepad

A tabbed Notepad clone built with WinUI 3 (Windows App SDK 1.7) and C++/WinRT.

## Requirements
- Visual Studio 2022 with **Desktop development with C++** and **WinUI application development** (including *C++ WinUI app development tools*)
- Windows 10 1809 or later

## Build and run
Open `MyNotepad.sln`, choose a platform (x64 / x86 / ARM64), and press F5. NuGet packages restore automatically.
The app is **unpackaged and self-contained**: the Windows App SDK runtime ships next to `MyNotepad.exe`, so no MSIX install is needed.

## Features
- Tabs in the title bar: new (Ctrl+N / Ctrl+T / +), close (Ctrl+W, Ctrl+F4), cycle (Ctrl+Tab), jump to a tab (Ctrl+1..9), reorder by dragging
- Open (multi-select), Save, Save As (with an Encoding picker), Save All (Ctrl+Alt+S); prompts to save when closing a tab or the window
- Detects UTF-8, UTF-8 BOM, UTF-16 LE/BE and ANSI encodings, plus CRLF/LF/CR line endings; you can change either from the status bar
- Undo/Redo, Cut/Copy/Paste, Find/Replace (match case, wrap around, F3 / Shift+F3), Go to line (Ctrl+G), Time/Date (F5), Tab key inserts tabs
- Word wrap, font picker (all system fonts), zoom (Ctrl+Plus/Minus/0, Ctrl+mouse wheel), status bar toggle
- Open files from the command line or by dragging them onto the window
- Settings are saved to `%LOCALAPPDATA%\MyNotepad\settings.ini`

## Source layout
| File | Purpose |
|---|---|
| `MainWindow.xaml` / `.h` / `.cpp` | UI, tabs, commands, find/replace, dialogs |
| `TextFile.h` / `.cpp` | File loading and saving, encoding and line-ending handling |
| `App.xaml` / `.cpp` | Application entry point |
