# MyNotepad

A tabbed Notepad clone built with WinUI 3 (Windows App SDK 1.7) and C++/WinRT.

## Requirements
- Visual Studio 2022 with **Desktop development with C++** and **WinUI application development** (including *C++ WinUI app development tools*)
- Windows 10 1809 or later

## Build and run
Open `MyNotepad.sln`, choose a platform (x64 / x86 / ARM64), and press F5. NuGet packages restore automatically.
The app is **unpackaged and self-contained**: the Windows App SDK runtime ships next to `MyNotepad.exe`, so no MSIX install is needed.

## Packaging
Every **Release** build also creates `dist\MyNotepad-<platform>.zip` (e.g. `dist\MyNotepad-x64.zip`). It contains a `MyNotepad` folder with everything needed to run the app. Debug symbols and other build by-products are left out. Unzip it anywhere and run `MyNotepad.exe`.

- Target machines also need the [Microsoft Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) for the same architecture, which the zip doesn't include.
- To skip the zip, build with `/p:CreateZipPackage=false`. To get a zip from another configuration, build with `/p:CreateZipPackage=true`.
- The step is the `CreateZipPackage` target at the end of `MyNotepad.vcxproj`.

## Features
- Tabs in the title bar: new (Ctrl+N / Ctrl+T / +), close (Ctrl+W, Ctrl+F4), cycle (Ctrl+Tab), jump to a tab (Ctrl+1..9), reorder by dragging
- Right-click a tab to copy its file name or full path, open its folder in Explorer with the file selected, or close it, the other tabs, or the tabs to its right
- Open (multi-select), Save, Save As (with an Encoding picker), Save All (Ctrl+Alt+S); prompts to save when closing a tab or the window
- The Open and Save As file type lists include every loaded language, using the extensions in its definition. Save As starts on the document's language, and a new document is suggested that language's first extension.
- Detects UTF-8, UTF-8 BOM, UTF-16 LE/BE and ANSI encodings, plus CRLF/LF/CR line endings; you can change either from the status bar
- Undo/Redo, Cut/Copy/Paste, Find/Replace (match case, wrap around, F3 / Shift+F3), Go to line (Ctrl+G), Time/Date (F5)
- Tabs and indentation, under **Format > Tabs** or the status bar button: the Tab key inserts either a tab or spaces up to the next multiple of the indent size (1–8). Tab characters display at a chosen width (1–8 spaces, default 8).
- Word wrap, font picker (all system fonts), zoom (Ctrl+Plus/Minus/0, Ctrl+mouse wheel), status bar toggle
- **View > Show whitespace** marks spaces with `·` and tabs with `→`. Markers are drawn over the editor rather than inserted into the text. It's only available with a monospaced font and word wrap off, because markers are placed by line and column.
- Syntax highlighting for Batch, C, C++, C#, CSS, Go, HTML (including `<script>` and `<style>`), INI, Java, JavaScript, JSON, PowerShell, Python, Resource Script (`.rc`), Rust, SQL, TypeScript and XML. The language is picked from the file extension and can be changed from **Format > Language** or the status bar. Light and dark themes have their own colours. Files over 1,000,000 characters are shown as plain text. Languages are defined in JSON files that users can add to (see below).
- Open files from the command line or by dragging them onto the window. Files given on the command line (including from the taskbar jump list) open as tabs in the MyNotepad window already open on the current virtual desktop, if there is one, otherwise in a new window. Starting MyNotepad without files always opens a new window.
- **File > Recent files** lists the last 10 files opened or saved, newest first. Choosing a file that no longer exists removes it from the list.
- The same files appear under **Recent** when you right-click MyNotepad on the taskbar. A file removed there with **Remove from this list** is also removed from **File > Recent files**.
- Settings and the recent files list are saved to `%LOCALAPPDATA%\MyNotepad\settings.ini` (UTF-16, so paths in any language are kept)

## Source layout
| File | Purpose |
|---|---|
| `MainWindow.xaml` / `.h` / `.cpp` | UI, tabs, commands, find/replace, dialogs |
| `TextFile.h` / `.cpp` | File loading and saving, encoding and line-ending handling |
| `Instance.h` / `.cpp` | Sends command-line files to a window already open on the current virtual desktop |
| `Syntax.h` / `.cpp` | Loads the language definitions, detects a file's language and tokenizes text for highlighting |
| `Languages/*.json` | The built-in language definitions, copied to the output's `Languages` folder |
| `App.xaml` / `.cpp` | Application entry point |
| `MyNotepad.ico` / `MyNotepad.rc` | App icon (16–256 px), compiled into the exe and set on the window |
| `tools/MakeIcon.ps1` | Draws the icon. Run `powershell -File tools/MakeIcon.ps1 -OutIco MyNotepad/MyNotepad.ico` after changing it |

## How highlighting works
Each tab's editor is a `RichEditBox` used as a plain-text editor. After each edit, `Syntax.cpp` tokenizes the whole document, then `MainWindow` recolours only the characters whose colour changed, and only in and near the visible part of the document. More is coloured as you scroll. Colouring a range is slow in RichEdit, so this keeps typing and opening large files responsive.

RichEditBox's own undo is turned off because it also records formatting changes, which would make Ctrl+Z undo colours. `MainWindow` keeps its own undo history of text edits instead, grouping typing and deletions by word. For the same reason, paste always inserts plain text.

## Adding or changing a language
Each language is a JSON file. MyNotepad loads them at startup from two folders:

1. `Languages` next to `MyNotepad.exe`: the built-in definitions, replaced when the app is updated.
2. `%LOCALAPPDATA%\MyNotepad\Languages`: your own. **Format > Language > Open language folder** opens it.

To add a language, create a `.json` file in your folder. To change a built-in one, copy its file into your folder and edit the copy. A definition with the same `name` replaces the built-in one, so updates won't overwrite your changes. Use **Format > Language > Reload languages** to apply edits without restarting. Files that can't be loaded are listed in a message, with the reason.

Example:
```json
{
  "name": "Lua",
  "extensions": ["lua"],
  "keywords": "and break do else elseif end false for function if in local nil not or repeat return then true until while",
  "types": ["print", "pairs", "ipairs", "string", "table"],
  "lineComments": ["--"],
  "blockComment": ["--[[", "]]"],
  "strings": "\"'"
}
```

Lists such as `keywords` can be either an array of strings or a single string of words separated by spaces. All fields except `name` are optional.

| Field | Meaning |
|---|---|
| `name` | Name shown in the menus. Required, and used to match definitions across folders. |
| `extensions` | File extensions without the dot, e.g. `["py", "pyw"]`. |
| `fileNames` | Whole file names that use this language, e.g. `["Makefile"]`. |
| `tokenizer` | `code` (default) for languages described by the fields below. `markup`, `css`, `batch` and `ini` select the built-in XML/HTML, CSS, batch file and INI tokenizers, which only use `keywords` (batch) and `scriptLanguage` / `styleLanguage` (markup). |
| `keywords` | Words coloured as keywords. |
| `types` | Words coloured as types, such as built-in types and functions. |
| `ignoreCase` | `true` to match keywords and types regardless of case. |
| `lineComments` | Markers that start a comment running to the end of the line, e.g. `["//"]`. |
| `blockComment` | Start and end markers of block comments, e.g. `["/*", "*/"]`. |
| `strings` | Characters that delimit single-line strings. Default `"\""`; use `""` for none. |
| `multilineStrings` | Characters that delimit strings that can span lines, e.g. `` "`" `` for JavaScript template strings. |
| `escape` | Escape character inside strings. Default `"\\"`; use `""` for none. |
| `charLiterals` | `true` for C-style character literals such as `'a'` and `'\n'`. |
| `preprocessor` | `true` to colour lines starting with `#` as preprocessor directives. |
| `rawStrings` | `true` for C++ raw strings `R"(...)"`. |
| `verbatimStrings` | `true` for C# verbatim strings `@"..."`. |
| `tripleQuotedStrings` | `true` for Python `"""..."""` and `'''...'''` strings. |
| `annotations` | `true` to colour `@name` decorators and annotations. |
| `powerShellSyntax` | `true` for PowerShell `$variables`, `-Parameters` and `Verb-Noun` commands. |
| `objectKeys` | `true` to colour JSON object keys differently from other strings. |
| `scriptLanguage`, `styleLanguage` | Markup only: names of the languages used inside `<script>` and `<style>` elements. |

Colours come from the token kinds (keyword, type, string, comment and so on) and follow the app's light or dark theme. They can't be set per language.
