#pragma once

// User-defined external tools, listed in the Tools menu and run on the current file.
namespace tools
{
    struct Tool
    {
        std::wstring Name;
        std::wstring Command;          // program, script or document; may use placeholders
        std::wstring Arguments;        // may use placeholders
        std::wstring WorkingDirectory; // may use placeholders; empty is the file's folder
        bool SaveFirst{ true };        // save unsaved changes before running
        std::wstring Icon;             // optional: an image, or "file,index" for an icon in an .exe or .dll
        std::wstring Glyph;            // optional: a character from the Segoe Fluent Icons font
        bool IsFolder{ false };        // a submenu of Children rather than a tool to run
        std::vector<Tool> Children;
    };

    // What the placeholders stand for when a tool runs.
    struct FileContext
    {
        std::wstring Path;
        int Line{ 1 };
        int Column{ 1 };
    };

    // Reads the tools from a JSON file; a missing file has none. Errors are added to errors.
    std::vector<Tool> LoadTools(std::wstring const& path, std::vector<std::wstring>& errors);

    // Starts the tool. Returns an error message, or nothing if it started.
    std::optional<std::wstring> RunTool(Tool const& tool, FileContext const& file, HWND owner);

    // The tool's icon as size x size premultiplied BGRA pixels: from its icon setting, or else the
    // program its command runs. False if there's none.
    bool LoadToolIcon(Tool const& tool, int size, std::vector<uint8_t>& pixels);

    // The contents of a new tools file: a couple of examples showing the format.
    std::wstring ExampleToolsFile();
}
