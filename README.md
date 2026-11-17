# RadPad

[![Build](https://img.shields.io/github/actions/workflow/status/RadAd/RadPad/build.yml?branch=master&label=build)](https://github.com/RadAd/RadPad/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/RadAd/RadPad)](https://github.com/RadAd/RadPad/releases/latest)

A tabbed Notepad clone built with WinUI 3 (Windows App SDK 1.7) and C++/WinRT.

## Requirements
- Visual Studio 2022 with **Desktop development with C++** and **WinUI application development** (including *C++ WinUI app development tools*)
- Windows 10 1809 or later

## Build and run
Open `RadPad.sln`, choose a platform (x64 / x86 / ARM64), and press F5. NuGet packages restore automatically.
The app is **unpackaged and self-contained**: the Windows App SDK runtime ships next to `RadPad.exe`, so no MSIX install is needed.

## Packaging
Every **Release** build also creates `dist\RadPad-<platform>.zip` (e.g. `dist\RadPad-x64.zip`). It contains a `RadPad` folder with everything needed to run the app, including the Visual C++ runtime DLLs it uses, so it runs on any Windows 10 or 11 PC without installing anything else. Debug symbols and other build by-products are left out. Unzip it anywhere and run `RadPad.exe`.

GitHub builds Release x64 and ARM64 on every push and pull request (`.github/workflows/build.yml`). Each run's page has the contents of `dist` (zip, installer and MSIX) for each platform as artifacts. To publish a release, set the version in `RadPad/Version.h`, commit, then tag and push the tag (`git tag v1.2.3`, `git push origin v1.2.3`): the build then creates a GitHub release with notes from the changes since the last tag and the zips and installers attached. It fails if the tag doesn't match `Version.h`.

If [Inno Setup 6](https://jrsoftware.org/isinfo.php) is installed, a Release build also creates an installer, `dist\RadPad-<platform>-setup.exe`, from `installer\RadPad.iss`. It:

- installs for the current user without administrator rights, into `%LOCALAPPDATA%\Programs\RadPad` (or for all users, if chosen);
- adds a Start menu shortcut, and optionally a desktop shortcut;
- adds RadPad to **Open with** for text files and the file types of the built-in languages, without becoming their default program;
- optionally adds **Edit with RadPad** to every file's right-click menu;
- installs over an earlier version, closing RadPad first, and has an uninstaller (settings in `%LOCALAPPDATA%\RadPad` are kept).

Build options:

- `/p:CreateZipPackage=false` skips the zip, and `/p:CreateZipPackage=true` makes one from another configuration.
- `/p:CreateInstaller=false` skips the installer, and `/p:InnoSetupCompiler=<path to ISCC.exe>` uses an Inno Setup installed somewhere unusual.
- `/p:CreateMsix=false` skips the MSIX.
- The steps are the `CopyVCRuntime`, `CreateZipPackage`, `CreateInstaller` and `CreateMsix` targets at the end of `RadPad.vcxproj`.

For the Microsoft Store, a Release build also packages the app as `dist\RadPad-<platform>.msix`, using `tools\MakeMsix.ps1` and the manifest in `installer\msix\AppxManifest.xml`. It needs the identity values from the app's Product identity page in Partner Center, set as `MsixIdentityName`, `MsixPublisher` (`CN=...`) and `MsixPublisherDisplayName` in `RadPad.vcxproj` or passed with `/p:`; until then no MSIX is built. The script can also be run by hand after a Release build: `powershell -File tools\MakeMsix.ps1 -IdentityName <name> -Publisher "CN=..." -PublisherDisplayName <name> [-Platform x64]`. The Store signs the package, so it needn't be signed. When installed from the Store, RadPad keeps its settings in its package's data folder (`%LOCALAPPDATA%\Packages\<package>\LocalState`) rather than `%LOCALAPPDATA%\RadPad`, and Windows removes them on uninstall. The package turns off Windows' redirection of writes to AppData, so files RadPad creates there (other programs' configuration, say) go to the real location; this uses the restricted `unvirtualizedResources` capability, which Partner Center asks you to justify when submitting.

The version is set in `RadPad/Version.h`, and only there. It's used for the exe's version (shown in Explorer under Properties > Details), the About box and the installer.

## Features
- Tabs in the title bar: new (Ctrl+N / Ctrl+T / +), close (Ctrl+W, Ctrl+F4), switch to recently used tabs (Ctrl+Tab goes to the previous tab; hold Ctrl and press Tab again to go further back, Ctrl+Shift+Tab to go the other way), jump to a tab (Ctrl+1..9), reorder by dragging
- Right-click a tab to copy its file name or full path, open its folder in Explorer with the file selected, or close it, the other tabs, or the tabs to its right
- **Tools** menu: your own external programs, run on the current file (see below). Tools such as compilers and linters can show their output in a panel below the editor, where double-clicking an error opens the file at that line.
- Open (multi-select), Save, Save As (with an Encoding picker), Save All (Ctrl+Alt+S); prompts to save when closing a tab or the window
- Files changed by another program while open are reloaded automatically, and the reload can be undone. If the tab has unsaved changes, RadPad asks first. A file deleted while open stays in its tab, marked unsaved, so saving puts it back.
- The Open and Save As file type lists include every loaded language, using the extensions in its definition. Save As starts on the document's language, and a new document is suggested that language's first extension.
- Detects UTF-8, UTF-8 BOM, UTF-16 LE/BE and ANSI encodings, plus CRLF/LF/CR line endings; you can change either from the status bar
- Undo/Redo, Cut/Copy/Paste, Find/Replace (match case, regular expressions, wrap around, F3 / Shift+F3), Go to line (Ctrl+G), Time/Date (F5)
- **Regular expression** in the Find panel uses JavaScript-style patterns. `^` and `$` match at the start and end of each line and `\n` matches a line break. In the replacement, `$1` (or `$&` for the whole match) inserts what was matched and `\n` and `\t` insert a line break or tab.
- Home goes to the first non-whitespace character of the line; pressing it again goes to the very start. Shift+Home selects the same way. On a wrapped row, Home first goes to the start of that row.
- **Ctrl+]** goes to the matching bracket (also **Edit > Go to matching bracket**): from next to a bracket to its partner, or from inside brackets to the closing one. **Ctrl+Shift+]** selects from one bracket to the other. Brackets in strings and comments are skipped.
- Tabs and indentation, under **Format > Tabs** or the status bar button: the Tab key inserts either a tab or spaces up to the next multiple of the indent size (1–8). Tab characters display at a chosen width (1–8 spaces, default 8). With more than one line selected, Tab indents the selected lines; Shift+Tab outdents them, or the current line if nothing is selected.
- Word wrap, font picker (all system fonts), zoom (Ctrl+Plus/Minus/0, Ctrl+mouse wheel), status bar toggle
- **View > Show whitespace** marks spaces with `·` and tabs with `→`. Markers are drawn over the editor rather than inserted into the text. It's only available with a monospaced font and word wrap off, because markers are placed by line and column.
- **View > Highlight trailing whitespace** (on by default) shades spaces and tabs at the ends of lines, including lines that are only whitespace. Whitespace ending at the cursor isn't shaded, so it doesn't flash while you type.
- **View > Rainbow brackets** (on by default) colours `()`, `[]` and `{}` in code by how deeply they're nested, cycling through three colours.
- With word wrap on, a `↵` marker in the right margin shows each row that continues on the next row, so wrapped lines can be told apart from separate lines.
- Syntax highlighting for Batch, C, C++, C#, CSS, Go, HTML (including `<script>` and `<style>`), INI, Inno Setup (`.iss`), Java, JavaScript, JSON, Markdown, PowerShell, Python, Resource Script (`.rc`), Rust, SQL, TypeScript and XML. In Markdown, headings, bold and italic text, code, links, lists, quotes and tables are coloured, and a fenced code block that names its language (` ```cpp `, ` ```python ` …) is coloured as that language. The language is picked from the file extension and can be changed from **Format > Language** or the status bar. Colours come from the colour theme chosen under **View > Color theme**: Visual Studio (the default), GitHub, Solarized, Monokai or One Dark, or your own (see below). Files over 1,000,000 characters are shown as plain text. Languages are defined in JSON files that users can add to (see below).
- Open files from the command line or by dragging them onto the window. Files given on the command line (including from the taskbar jump list) open as tabs in the RadPad window already open on the current virtual desktop, if there is one, otherwise in a new window. Starting RadPad without files always opens a new window, as does middle-clicking (or Shift+clicking) its taskbar button.
- **File > Recent files** lists the last 10 files opened or saved, newest first. Choosing a file that no longer exists removes it from the list.
- The same files appear under **Recent** when you right-click RadPad on the taskbar. A file removed there with **Remove from this list** is also removed from **File > Recent files**.
- Settings and the recent files list are saved to `%LOCALAPPDATA%\RadPad\settings.ini` (UTF-16, so paths in any language are kept)

## Source layout
| File | Purpose |
|---|---|
| `MainWindow.xaml` / `.h` / `.cpp` | The main window's layout and class; `MainWindow.xaml.cpp` has start-up, keyboard handling, the title and status bar, editor settings, zoom, drag and drop and settings |
| `MainWindow.<area>.cpp` | The rest of `MainWindow`, one file per area: `Documents`, `Tabs`, `RecentFiles`, `Commands`, `Editing` (indentation, brackets, paste, undo), `Find`, `Highlighting` (also languages and colour themes), `Overlay` (whitespace and wrap markers), `Tools`, `Output` (the output panel) and `Dialogs` |
| `MainWindowHelpers.h` | Constants and helper functions shared by the `MainWindow*.cpp` files |
| `TextFile.h` / `.cpp` | File loading and saving, encoding and line-ending handling |
| `Instance.h` / `.cpp` | Sends command-line files to a window already open on the current virtual desktop |
| `Syntax.h` / `.cpp` | Loads the language definitions, detects a file's language and tokenizes text for highlighting |
| `SyntaxCode.cpp`, `SyntaxMarkup.cpp`, `SyntaxSimple.cpp`, `SyntaxMarkdown.cpp` | The tokenizers: code (driven by the definitions), XML/HTML and CSS, batch and INI, and Markdown. `SyntaxInternal.h` has what they share |
| `Languages/*.json` | The built-in language definitions, copied to the output's `Languages` folder |
| `Themes.h` / `.cpp` | Loads the colour themes and picks the colours for the app's light or dark mode |
| `Themes/*.json` | The built-in colour themes, copied to the output's `Themes` folder |
| `Tools.h` / `.cpp` | Loads the user's external tools and runs them on the current file |
| `App.xaml` / `.cpp` | Application entry point |
| `RadPad.ico` / `RadPad.rc` | App icon (16–256 px), compiled into the exe and set on the window |
| `tools/MakeIcon.ps1` | Draws the icon. Run `powershell -File tools/MakeIcon.ps1 -OutIco RadPad/RadPad.ico` after changing it |
| `Version.h` | RadPad's version number, used by the exe's version resource, the About box and the installer |
| `installer/RadPad.iss` | The Inno Setup installer script, built after a Release build if Inno Setup is installed |

## How highlighting works
Each tab's editor is a `RichEditBox` used as a plain-text editor. After each edit, `Syntax.cpp` tokenizes the whole document, then `MainWindow` recolours only the characters whose colour changed, and only in and near the visible part of the document. More is coloured as you scroll. Colouring a range is slow in RichEdit, so this keeps typing and opening large files responsive.

RichEditBox's own undo is turned off because it also records formatting changes, which would make Ctrl+Z undo colours. `MainWindow` keeps its own undo history of text edits instead, grouping typing and deletions by word. For the same reason, paste always inserts plain text.

## Adding or changing a language
Each language is a JSON file. RadPad loads them at startup from two folders:

1. `Languages` next to `RadPad.exe`: the built-in definitions, replaced when the app is updated.
2. `%LOCALAPPDATA%\RadPad\Languages`: your own. **Format > Language > Open language folder** opens it.

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
| `tokenizer` | `code` (default) for languages described by the fields below. `markup`, `css`, `batch`, `ini` and `markdown` select the built-in XML/HTML, CSS, batch file, INI and Markdown tokenizers, which only use `keywords` (batch) and `scriptLanguage` / `styleLanguage` (markup). |
| `keywords` | Words coloured as keywords. |
| `types` | Words coloured as types, such as built-in types and functions. |
| `typeDeclarations` | Words that declare a type, such as `class` and `struct`. The name after one (skipping keywords, as in `enum class Color`) is coloured as a type everywhere it's used in the file. Types declared in other files aren't recognised. |
| `macros` | Well-known macros, coloured as macros, such as `NULL`, `assert` and `__FILE__`. |
| `upperCaseMacros` | `true` to colour every ALL_CAPS word (such as `MAX_PATH`) as a macro, for macros defined in headers. This also catches all-caps constants. Off for every built-in language. |
| `bangMacros` | `true` for Rust-style `name!` macros. |
| `ignoreCase` | `true` to match keywords and types regardless of case. |
| `lineComments` | Markers that start a comment running to the end of the line, e.g. `["//"]`. |
| `lineStartComments` | Markers that start a comment only at the start of a line (after any indentation), e.g. `[";"]` in Inno Setup scripts, where `;` also separates values within a line. |
| `blockComment` | Start and end markers of block comments, e.g. `["/*", "*/"]`. |
| `strings` | Characters that delimit single-line strings. Default `"\""`; use `""` for none. |
| `multilineStrings` | Characters that delimit strings that can span lines, e.g. `` "`" `` for JavaScript template strings. |
| `escape` | Escape character inside strings. Default `"\\"`; use `""` for none. |
| `charLiterals` | `true` for C-style character literals such as `'a'` and `'\n'`. |
| `preprocessor` | `true` to colour lines starting with `#` as preprocessor directives. The name after `#define` or `#undef` is coloured as a macro everywhere it's used in the file, and code between `#if 0` and its `#else`, `#elif` or `#endif` is shown as a comment. |
| `rawStrings` | `true` for C++ raw strings `R"(...)"`. |
| `verbatimStrings` | `true` for C# verbatim strings `@"..."`. |
| `tripleQuotedStrings` | `true` for Python `"""..."""` and `'''...'''` strings. |
| `annotations` | `true` to colour `@name` decorators and annotations. |
| `powerShellSyntax` | `true` for PowerShell `$variables`, `-Parameters` and `Verb-Noun` commands. |
| `objectKeys` | `true` to colour JSON object keys differently from other strings. |
| `scriptLanguage`, `styleLanguage` | Markup only: names of the languages used inside `<script>` and `<style>` elements. |

Colours come from the token kinds (keyword, type, string, comment and so on) and the colour theme. They can't be set per language.

## Colour themes
**View > Color theme** picks the colours used for syntax highlighting. Each theme is a JSON file, loaded at startup from:

1. `Themes` next to `RadPad.exe`: the built-in themes.
2. `%LOCALAPPDATA%\RadPad\Themes`: your own. **View > Color theme > Open themes folder** opens it.

A theme with the same `name` as a built-in one replaces it. Use **Reload themes** to apply edits without restarting. Files that can't be loaded are listed in a message, with the reason.

A theme has a `light` set of colours, a `dark` set, or both. With both, the editor follows Windows' light or dark mode. With only one, that set is used in either mode, and if the app's mode doesn't match it, the editor switches to match so the cursor and selection stay visible.

```json
{
  "name": "Paper",
  "light": {
    "background": "#FFFFFF",
    "text": "#333333",
    "comment": "#999999",
    "keyword": "#0055AA",
    "string": "#AA5500"
  }
}
```

Colours are written `#RRGGBB`. The colour names are `text`, `comment`, `keyword`, `type`, `string`, `number`, `preprocessor`, `tag`, `attribute`, `variable`, `property`, `macro` and `bracket1` to `bracket3` (the rainbow bracket colours); any left out use the `text` colour, except `macro`, which uses the `preprocessor` colour, and the bracket colours, which use VS Code's. `background` is optional: without it the editor keeps the app's background, unless the set is used in the other mode, when it's white for a light set and dark grey for a dark one.

## External tools
The **Tools** menu runs other programs on the current file. They're listed in `%LOCALAPPDATA%\RadPad\tools.json`; **Tools > Edit tools** opens it in a tab, creating it with two examples the first time. Saving it updates the menu straight away, and mistakes are reported in a message.

```json
{
  "tools": [
    { "name": "Open in VS Code", "command": "code", "arguments": "--goto \"$(FilePath):$(Line):$(Column)\"" },
    { "name": "Python", "glyph": "E943", "tools": [
      { "name": "Run script", "command": "cmd.exe", "arguments": "/k python \"$(FileName)\"" },
      { "name": "Run tests", "command": "cmd.exe", "arguments": "/k python -m pytest" }
    ] },
    { "name": "Show in Explorer", "command": "explorer.exe", "arguments": "/select,\"$(FilePath)\"", "saveFirst": false },
    { "name": "Lint", "command": "ruff", "arguments": "check \"$(FileName)\"", "output": "panel" }
  ]
}
```

| Field | Meaning |
|---|---|
| `name` | Text in the Tools menu. Required. |
| `command` | The program to run: a full path, or a name Windows can find (on the `PATH` or registered with Windows). Batch files and documents work too. Required. |
| `arguments` | The command line passed to it. Without it, the tool gets the file's path in quotes. |
| `workingDirectory` | The folder to start in. Without it, the file's folder. |
| `saveFirst` | `true` (the default) saves unsaved changes first, so the tool sees what's on screen; `false` runs on the file as last saved. |
| `icon` | Optional. An image file (`.png`, `.ico` …), a program, or `"file,index"` for one of the icons in an `.exe` or `.dll`, such as `"shell32.dll,4"`. Environment variables like `%ProgramFiles%` work. Without it, the menu shows the icon of the program `command` runs. A `.cmd` launcher in a `bin` folder beside its app (as VS Code installs) shows the app's icon. |
| `glyph` | Optional. A symbol from Windows' Segoe Fluent Icons font instead of an icon, as a character code such as `"E756"`. |
| `tools` | Makes the entry a folder: a submenu of the tools (and folders) listed here, instead of a tool with a `command`. A folder can have an `icon` or `glyph`. |
| `output` | `"panel"` runs the tool without a window and shows what it writes in the output panel (see below). Without it, the tool runs in its own window as usual. |
| `errorPattern` | Optional, with `"output": "panel"`. A regular expression that finds a file name (group 1), line (group 2) and optionally column (group 3) in a line of output, for tools whose messages the built-in patterns don't recognise. |
| `outputEncoding` | Optional, with `"output": "panel"`: `"utf-8"`, `"oem"` (the console code page, used by `cmd` and older tools) or `"ansi"`. Without it, output is read as UTF-8 until something that isn't valid UTF-8 appears, then as the console code page. |

`command`, `arguments` and `workingDirectory` can use `$(FilePath)` (the full path), `$(FileDir)` (its folder), `$(FileName)`, `$(FileNameNoExt)`, `$(Line)` and `$(Column)` (the cursor's position).

To type something in when the tool runs, use `$(Prompt)`, or `$(Prompt:label)` to choose the box's label. Before the tool starts, RadPad asks for the text in a dialog with one box per label (a label used twice gets the same text both times). Cancel doesn't run the tool. A box starts with what was typed for it last time the tool ran in this session, or otherwise the first box starts with the selected text if it's on one line. The text is put in as typed, so add quotes around the placeholder if it may contain spaces:

```json
{ "name": "Search in file", "command": "findstr", "arguments": "/n /i \"$(Prompt:Search for)\" \"$(FileName)\" nul", "output": "panel" }
```

An untitled document is always saved before a tool runs, as there's no file to give it otherwise.

### Output panel
A tool with `"output": "panel"` runs through `cmd.exe` with no window and nothing on its input, so it can't wait for typing. Its output appears in a panel above the status bar, errors (what it writes to its error stream) in red, followed by its exit code and how long it took. **View > Output panel** shows or hides the panel, and dragging its top edge or its heading resizes it; both are remembered. While a tool runs, **Stop** ends it and anything it started. Only one tool runs in the panel at a time: starting another asks whether to stop the first.

Double-clicking a line of output, or pressing Enter on it, opens the file it names at that line and column. RadPad recognises `file(line,column):` (Microsoft compilers and MSBuild), `file:line:column:` (gcc, clang, Rust, TypeScript, `grep -n` and many others) and Python's `File "file", line n`, plus the tool's own `errorPattern`. A relative file name is looked for in the folder the tool ran in, then beside the current file. If neither exists, the status bar says so. The panel keeps about the last million characters of output.
