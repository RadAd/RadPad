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
- **View > Show whitespace** marks spaces with `·` and tabs with `→`. Markers are drawn over the editor rather than inserted into the text. It's only available with a monospaced font and word wrap off, because markers are placed by line and column.
- Syntax highlighting for Batch, C, C++, C#, CSS, Go, HTML (including `<script>` and `<style>`), INI, Java, JavaScript, JSON, PowerShell, Python, Rust, SQL, TypeScript and XML. The language is picked from the file extension and can be changed from **Format > Language** or the status bar. Light and dark themes have their own colours. Files over 1,000,000 characters are shown as plain text.
- Open files from the command line or by dragging them onto the window
- Settings are saved to `%LOCALAPPDATA%\MyNotepad\settings.ini`

## Source layout
| File | Purpose |
|---|---|
| `MainWindow.xaml` / `.h` / `.cpp` | UI, tabs, commands, find/replace, dialogs |
| `TextFile.h` / `.cpp` | File loading and saving, encoding and line-ending handling |
| `Syntax.h` / `.cpp` | Language detection and the tokenizer behind syntax highlighting |
| `MyNotepad.ico` / `MyNotepad.rc` | App icon (16–256 px), compiled into the exe and set on the window |
| `tools/MakeIcon.ps1` | Draws the icon. Run `powershell -File tools/MakeIcon.ps1 -OutIco MyNotepad/MyNotepad.ico` after changing it |

## How highlighting works
Each tab's editor is a `RichEditBox` used as a plain-text editor. After each edit, `Syntax.cpp` tokenizes the whole document, then `MainWindow` recolours only the characters whose colour changed, and only in and near the visible part of the document. More is coloured as you scroll. Colouring a range is slow in RichEdit, so this keeps typing and opening large files responsive.

RichEditBox's own undo is turned off because it also records formatting changes, which would make Ctrl+Z undo colours. `MainWindow` keeps its own undo history of text edits instead, grouping typing and deletions by word. For the same reason, paste always inserts plain text.
| `App.xaml` / `.cpp` | Application entry point |
