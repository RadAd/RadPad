#pragma once

#include <atomic>
#include <map>

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
        bool CaptureOutput{ false };   // "output": "panel": run without a window, showing its output in RadPad
        std::wstring ErrorPattern;     // optional regex finding file (group 1), line (2) and column (3) in output
        std::wstring OutputEncoding;   // "utf-8", "oem", "ansi", or empty to detect
    };

    // What the placeholders stand for when a tool runs.
    struct FileContext
    {
        std::wstring Path;
        int Line{ 1 };
        int Column{ 1 };
        std::wstring SelectedText; // the editor's selection, for $(SelectedText) and $(SelectedTextUrl)
        std::map<std::wstring, std::wstring> Inputs; // text typed for each $(Prompt:label) placeholder, by label
    };

    // Whether the tool is given the file, through a $(File...) placeholder (as its default arguments are).
    // Otherwise it can run on an untitled document without saving it first. Prompt defaults don't count.
    bool UsesFile(Tool const& tool);

    // Whether the tool uses the selected text, so needs some selected. Prompt defaults don't count: a
    // prompt whose default is the selection just starts empty without one.
    bool UsesSelection(Tool const& tool);

    // A $(Prompt) placeholder: $(Prompt), $(Prompt:label), $(Prompt=default) or $(Prompt:label=default).
    // The default can use the other placeholders, such as $(SelectedText); see ExpandPlaceholders.
    struct Prompt
    {
        std::wstring Label; // DefaultPromptLabel for $(Prompt) and $(Prompt=default)
        std::wstring Default;
        bool HasDefault{ false };
    };
    constexpr wchar_t DefaultPromptLabel[] = L"Text";

    // The tool's prompts, one per label, in the order they first appear. A label used more than once takes
    // the first default given for it. The text for them is asked for before the tool runs.
    std::vector<Prompt> Prompts(Tool const& tool);

    // Replaces the $(File...), $(Line), $(Column) and $(SelectedText...) placeholders in text, as for a
    // prompt's default.
    std::wstring ExpandPlaceholders(std::wstring const& text, FileContext const& file);

    // Reads the tools from a JSON file; a missing file has none. Errors are added to errors.
    std::vector<Tool> LoadTools(std::wstring const& path, std::vector<std::wstring>& errors);

    // Starts the tool. Returns an error message, or nothing if it started.
    std::optional<std::wstring> RunTool(Tool const& tool, FileContext const& file, HWND owner);

    // A tool running with its output captured (see StartCaptured).
    class RunningTool
    {
    public:
        // Called on background threads: text the tool wrote (line breaks are '\r'; error is true for its
        // error stream), then once when it has finished, with its exit code, or nothing if it was stopped.
        struct Events
        {
            std::function<void(std::wstring text, bool error)> Output;
            std::function<void(std::optional<DWORD> exitCode)> Finished;
        };

        ~RunningTool();
        void Stop(); // ends the tool and anything it started
        bool IsRunning() const { return m_running; }

    private:
        friend std::shared_ptr<RunningTool> StartCaptured(Tool const&, FileContext const&, Events, std::wstring&, std::wstring&, std::wstring&);
        HANDLE m_job{ nullptr };
        HANDLE m_process{ nullptr };
        std::atomic<bool> m_running{ true };
        std::atomic<bool> m_stopped{ false };
    };

    // Starts a tool whose output is captured: through cmd.exe, without a console window, with nothing on
    // its input. Sets commandLine and directory to what was run where. Returns null and sets error if it
    // couldn't start.
    std::shared_ptr<RunningTool> StartCaptured(Tool const& tool, FileContext const& file, RunningTool::Events events,
                                               std::wstring& commandLine, std::wstring& directory, std::wstring& error);

    // The tool's icon as size x size premultiplied BGRA pixels: from its icon setting, or else the
    // program its command runs. False if there's none.
    bool LoadToolIcon(Tool const& tool, int size, std::vector<uint8_t>& pixels);

    // The contents of a new tools file: a couple of examples showing the format.
    std::wstring ExampleToolsFile();
}
