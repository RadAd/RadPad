#include "pch.h"
#include "Tools.h"
#include "TextFile.h"
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
#include <wincodec.h>
#include <atomic>
#include <regex>
#include <thread>

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

namespace tools
{
    namespace
    {
        [[noreturn]] void Invalid(std::wstring const& message)
        {
            throw winrt::hresult_invalid_argument(message);
        }

        std::wstring ReadString(JsonObject const& json, wchar_t const* key, bool required)
        {
            if (!json.HasKey(key))
            {
                if (required)
                {
                    Invalid(std::wstring(L"\"") + key + L"\" is required");
                }
                return {};
            }
            auto value = json.GetNamedValue(key);
            if (value.ValueType() != JsonValueType::String || (required && value.GetString().empty()))
            {
                Invalid(std::wstring(L"\"") + key + L"\" must be " + (required ? L"a non-empty string" : L"a string"));
            }
            return std::wstring{ value.GetString() };
        }

        // "E756", "U+E756" or "0xE756" (a character code in hex), or the character itself.
        std::wstring ParseGlyph(std::wstring const& text)
        {
            if (text.empty() || text.size() == 1 || (text.size() == 2 && IS_HIGH_SURROGATE(text[0])))
            {
                return text;
            }
            std::wstring_view digits{ text };
            for (std::wstring_view prefix : { L"U+", L"u+", L"0x", L"0X" })
            {
                if (digits.starts_with(prefix))
                {
                    digits.remove_prefix(prefix.size());
                }
            }
            std::wstring hex{ digits };
            wchar_t* end = nullptr;
            unsigned long code = wcstoul(hex.c_str(), &end, 16);
            if (hex.empty() || end != hex.c_str() + hex.size() || code == 0 || code > 0x10FFFF)
            {
                Invalid(L"\"glyph\" must be a character code like \"E756\"");
            }
            if (code < 0x10000)
            {
                return std::wstring(1, static_cast<wchar_t>(code));
            }
            code -= 0x10000;
            return { static_cast<wchar_t>(0xD800 + (code >> 10)), static_cast<wchar_t>(0xDC00 + (code & 0x3FF)) };
        }

        std::wstring ExpandEnvironment(std::wstring const& text)
        {
            std::wstring result(32768, L'\0');
            DWORD length = ExpandEnvironmentStringsW(text.c_str(), result.data(), static_cast<DWORD>(result.size()));
            if (length == 0 || length > result.size())
            {
                return text;
            }
            result.resize(length - 1);
            return result;
        }

        bool NamesEqual(std::wstring_view a, std::wstring_view b)
        {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }

        // The file a tool's command runs, found as Windows would: a path, or a name on the PATH or
        // registered under App Paths. Empty if it can't be found, or depends on the current file.
        std::wstring FindProgram(std::wstring const& command)
        {
            if (command.empty() || command.find(L"$(") != std::wstring::npos)
            {
                return {};
            }
            std::wstring name = ExpandEnvironment(command);
            std::error_code error;
            std::wstring found;
            wchar_t buffer[MAX_PATH]{};
            bool hasPath = name.find_first_of(L"\\/:") != std::wstring::npos;
            bool hasExtension = std::filesystem::path{ name }.has_extension();
            if (hasPath)
            {
                if (!std::filesystem::exists(name, error))
                {
                    return {};
                }
                found = name;
            }
            for (wchar_t const* extension : { L"", L".exe", L".com", L".cmd", L".bat" })
            {
                if (hasPath || (*extension == 0) != hasExtension)
                {
                    continue;
                }
                if (SearchPathW(nullptr, name.c_str(), *extension ? extension : nullptr, MAX_PATH, buffer, nullptr))
                {
                    found = buffer;
                    break;
                }
            }
            if (found.empty())
            {
                std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + name + (hasExtension ? L"" : L".exe");
                DWORD size = sizeof(buffer);
                for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
                {
                    size = sizeof(buffer);
                    if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
                    {
                        found = buffer;
                        break;
                    }
                }
            }

            // Command-line launchers are often a .cmd in a bin folder beside the app (VS Code's code.cmd
            // beside Code.exe); show the app's icon rather than the batch file's.
            std::filesystem::path path{ found };
            if (NamesEqual(path.extension().native(), L".cmd") || NamesEqual(path.extension().native(), L".bat"))
            {
                auto folder = path.parent_path();
                if (NamesEqual(folder.filename().native(), L"bin"))
                {
                    for (std::filesystem::directory_iterator it{ folder.parent_path(), error }, end; !error && it != end; it.increment(error))
                    {
                        if (NamesEqual(it->path().stem().native(), path.stem().native()) && NamesEqual(it->path().extension().native(), L".exe"))
                        {
                            return it->path().wstring();
                        }
                    }
                }
            }
            return found;
        }

        // Replaces $(Name) placeholders with details of the file.
        std::wstring Expand(std::wstring text, FileContext const& file)
        {
            std::filesystem::path path{ file.Path };
            std::pair<std::wstring_view, std::wstring> values[] = {
                { L"$(FilePath)", path.wstring() },
                { L"$(FileDir)", path.parent_path().wstring() },
                { L"$(FileName)", path.filename().wstring() },
                { L"$(FileNameNoExt)", path.stem().wstring() },
                { L"$(Line)", std::to_wstring(file.Line) },
                { L"$(Column)", std::to_wstring(file.Column) },
            };
            for (auto const& [name, value] : values)
            {
                for (size_t at = text.find(name); at != std::wstring::npos; at = text.find(name, at + value.size()))
                {
                    text.replace(at, name.size(), value);
                }
            }
            return text;
        }

        // The entries of a "tools" list: tools, and folders with their own "tools" list. label is the
        // position of the list for error messages ("3 › 2" for the second entry in the third). A bad entry
        // is reported and skipped; the others still load.
        std::vector<Tool> ParseToolList(winrt::Windows::Data::Json::JsonArray const& list, std::wstring const& path, std::wstring const& label, std::vector<std::wstring>& errors)
        {
            std::vector<Tool> result;
            for (uint32_t i = 0; i < list.Size(); ++i)
            {
                auto position = label.empty() ? std::to_wstring(i + 1) : label + L" › " + std::to_wstring(i + 1);
                try
                {
                    if (list.GetAt(i).ValueType() != JsonValueType::Object)
                    {
                        Invalid(L"each tool must be an object");
                    }
                    auto entry = list.GetObjectAt(i);
                    Tool tool;
                    tool.Name = ReadString(entry, L"name", true);
                    tool.Icon = ReadString(entry, L"icon", false);
                    tool.Glyph = ParseGlyph(ReadString(entry, L"glyph", false));
                    if (entry.HasKey(L"tools"))
                    {
                        // A folder: a submenu of the tools it lists.
                        if (entry.GetNamedValue(L"tools").ValueType() != JsonValueType::Array)
                        {
                            Invalid(L"a folder's \"tools\" must be a list of tools");
                        }
                        if (entry.HasKey(L"command"))
                        {
                            Invalid(L"an entry is either a tool, with a \"command\", or a folder, with \"tools\", not both");
                        }
                        tool.IsFolder = true;
                        tool.Children = ParseToolList(entry.GetNamedArray(L"tools"), path, position, errors);
                        result.push_back(std::move(tool));
                        continue;
                    }

                    tool.Command = ReadString(entry, L"command", true);
                    tool.Arguments = entry.HasKey(L"arguments") ? ReadString(entry, L"arguments", false) : L"\"$(FilePath)\"";
                    tool.WorkingDirectory = ReadString(entry, L"workingDirectory", false);
                    if (entry.HasKey(L"saveFirst"))
                    {
                        if (entry.GetNamedValue(L"saveFirst").ValueType() != JsonValueType::Boolean)
                        {
                            Invalid(L"\"saveFirst\" must be true or false");
                        }
                        tool.SaveFirst = entry.GetNamedBoolean(L"saveFirst");
                    }
                    auto output = ReadString(entry, L"output", false);
                    if (!output.empty() && output != L"panel" && output != L"window")
                    {
                        Invalid(L"\"output\" must be \"panel\" or \"window\"");
                    }
                    tool.CaptureOutput = output == L"panel";
                    tool.ErrorPattern = ReadString(entry, L"errorPattern", false);
                    if (!tool.ErrorPattern.empty())
                    {
                        try
                        {
                            std::wregex check{ tool.ErrorPattern };
                        }
                        catch (std::regex_error const&)
                        {
                            Invalid(L"\"errorPattern\" isn't a valid regular expression");
                        }
                    }
                    tool.OutputEncoding = ReadString(entry, L"outputEncoding", false);
                    std::wstring_view encodings[] = { L"", L"auto", L"utf-8", L"oem", L"ansi" };
                    if (std::find(std::begin(encodings), std::end(encodings), tool.OutputEncoding) == std::end(encodings))
                    {
                        Invalid(L"\"outputEncoding\" must be \"utf-8\", \"oem\", \"ansi\" or \"auto\"");
                    }
                    result.push_back(std::move(tool));
                }
                catch (winrt::hresult_error const& e)
                {
                    errors.push_back(std::format(L"{}: tool {}: {}", path, position, std::wstring_view{ e.message() }));
                }
            }
            return result;
        }
    }

    std::vector<Tool> LoadTools(std::wstring const& path, std::vector<std::wstring>& errors)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return {};
        }
        try
        {
            auto json = JsonObject::Parse(textfile::Load(path).Text);
            if (!json.HasKey(L"tools") || json.GetNamedValue(L"tools").ValueType() != JsonValueType::Array)
            {
                Invalid(L"\"tools\" must be a list of tools");
            }
            return ParseToolList(json.GetNamedArray(L"tools"), path, L"", errors);
        }
        catch (winrt::hresult_error const& e)
        {
            errors.push_back(path + L": " + std::wstring{ e.message() });
            return {};
        }
    }

    std::optional<std::wstring> RunTool(Tool const& tool, FileContext const& file, HWND owner)
    {
        auto command = Expand(tool.Command, file);
        auto arguments = Expand(tool.Arguments, file);
        auto directory = tool.WorkingDirectory.empty() ? std::filesystem::path{ file.Path }.parent_path().wstring() : Expand(tool.WorkingDirectory, file);

        // ShellExecute rather than CreateProcess, so commands on the PATH or registered with Windows,
        // batch files and documents all work.
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.hwnd = owner;
        info.lpFile = command.c_str();
        info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
        {
            auto message = winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError())).message();
            return std::format(L"Couldn't run \"{}\" ({}).\n\n{}", tool.Name, command, std::wstring_view{ message });
        }
        return std::nullopt;
    }

    bool LoadToolIcon(Tool const& tool, int size, std::vector<uint8_t>& pixels)
    {
        // The icon setting: an image file, or "file,index" for one of the icons in an .exe or .dll.
        std::wstring file;
        std::optional<int> index;
        if (!tool.Icon.empty())
        {
            file = ExpandEnvironment(tool.Icon);
            size_t comma = file.find_last_of(L',');
            wchar_t* end = nullptr;
            if (comma != std::wstring::npos && comma + 1 < file.size())
            {
                long value = wcstol(file.c_str() + comma + 1, &end, 10);
                if (*end == 0)
                {
                    index = static_cast<int>(value);
                    file.resize(comma);
                }
            }
        }
        else
        {
            file = FindProgram(tool.Command);
        }
        if (file.empty())
        {
            return false;
        }

        try
        {
            auto wic = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
            winrt::com_ptr<IWICBitmapSource> source;
            if (index)
            {
                HICON icon = nullptr;
                if (FAILED(SHDefExtractIconW(file.c_str(), *index, 0, &icon, nullptr, MAKELONG(size, 16))) || !icon)
                {
                    return false;
                }
                winrt::com_ptr<IWICBitmap> bitmap;
                HRESULT hr = wic->CreateBitmapFromHICON(icon, bitmap.put());
                DestroyIcon(icon);
                winrt::check_hresult(hr);
                source = bitmap;
            }
            else
            {
                // The shell gives a program's or document's icon, or an image file's picture.
                winrt::com_ptr<IShellItemImageFactory> factory;
                winrt::check_hresult(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(factory.put())));
                auto extension = std::filesystem::path{ file }.extension().native();
                bool image = false;
                for (wchar_t const* e : { L".png", L".ico", L".jpg", L".jpeg", L".bmp", L".gif" })
                {
                    image = image || NamesEqual(extension, e);
                }
                HBITMAP hbitmap = nullptr;
                winrt::check_hresult(factory->GetImage({ size, size }, image ? SIIGBF_RESIZETOFIT : SIIGBF_ICONONLY, &hbitmap));
                winrt::com_ptr<IWICBitmap> bitmap;
                HRESULT hr = wic->CreateBitmapFromHBITMAP(hbitmap, nullptr, WICBitmapUsePremultipliedAlpha, bitmap.put());
                DeleteObject(hbitmap);
                winrt::check_hresult(hr);
                source = bitmap;
            }

            UINT width = 0, height = 0;
            winrt::check_hresult(source->GetSize(&width, &height));
            if (width != static_cast<UINT>(size) || height != static_cast<UINT>(size))
            {
                winrt::com_ptr<IWICBitmapScaler> scaler;
                winrt::check_hresult(wic->CreateBitmapScaler(scaler.put()));
                winrt::check_hresult(scaler->Initialize(source.get(), size, size, WICBitmapInterpolationModeHighQualityCubic));
                source = scaler;
            }
            winrt::com_ptr<IWICFormatConverter> converter;
            winrt::check_hresult(wic->CreateFormatConverter(converter.put()));
            winrt::check_hresult(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            pixels.resize(static_cast<size_t>(size) * size * 4);
            winrt::check_hresult(converter->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data()));
            return true;
        }
        catch (winrt::hresult_error const&)
        {
            return false; // no icon rather than an error: the tool still works
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Running a tool with its output captured
    // ---------------------------------------------------------------------------------------------

    namespace
    {
        // Turns the bytes a tool writes into text. With no encoding given, output is read as UTF-8 until
        // something that isn't valid UTF-8 turns up, then in the console's code page (which cmd's own
        // commands use). Line breaks become '\r'; a lone '\r' (progress lines that rewrite themselves) is dropped.
        class OutputDecoder
        {
        public:
            explicit OutputDecoder(std::wstring const& encoding)
            {
                if (encoding == L"utf-8") m_codePage = CP_UTF8, m_detect = false;
                else if (encoding == L"oem") m_codePage = GetOEMCP(), m_detect = false;
                else if (encoding == L"ansi") m_codePage = GetACP(), m_detect = false;
            }

            std::wstring Decode(char const* data, size_t size, bool last)
            {
                m_bytes.append(data, size);
                // A UTF-8 character split between reads waits for the rest.
                size_t complete = m_bytes.size();
                if (m_codePage == CP_UTF8 && !last)
                {
                    for (size_t back = 1; back <= 3 && back <= m_bytes.size(); ++back)
                    {
                        auto c = static_cast<unsigned char>(m_bytes[m_bytes.size() - back]);
                        if ((c & 0xC0) == 0x80)
                        {
                            continue; // a continuation byte
                        }
                        size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
                        if (length > back)
                        {
                            complete = m_bytes.size() - back;
                        }
                        break;
                    }
                }
                std::wstring text = Convert(std::string_view{ m_bytes }.substr(0, complete));
                m_bytes.erase(0, complete);
                return Normalize(text, last);
            }

        private:
            std::wstring Convert(std::string_view bytes)
            {
                if (bytes.empty())
                {
                    return {};
                }
                auto convert = [&](UINT codePage, DWORD flags)
                {
                    int length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                    std::wstring text(static_cast<size_t>(std::max(length, 0)), L'\0');
                    if (length > 0)
                    {
                        MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
                    }
                    return std::pair{ length > 0, text };
                };
                if (m_detect && m_codePage == CP_UTF8)
                {
                    if (auto [ok, text] = convert(CP_UTF8, MB_ERR_INVALID_CHARS); ok)
                    {
                        return text;
                    }
                    m_codePage = GetOEMCP(); // not UTF-8 after all
                }
                return convert(m_codePage, 0).second;
            }

            std::wstring Normalize(std::wstring const& text, bool last)
            {
                std::wstring result;
                std::wstring input = m_pendingCR ? L"\r" + text : text;
                m_pendingCR = false;
                for (size_t i = 0; i < input.size(); ++i)
                {
                    wchar_t c = input[i];
                    if (c == L'\r')
                    {
                        if (i + 1 == input.size() && !last)
                        {
                            m_pendingCR = true; // a \r\n split between reads
                        }
                        else if (input[i + 1 < input.size() ? i + 1 : i] == L'\n' && i + 1 < input.size())
                        {
                            result += L'\r';
                            ++i;
                        }
                        continue;
                    }
                    result += c == L'\n' ? L'\r' : c;
                }
                return result;
            }

            UINT m_codePage{ CP_UTF8 };
            bool m_detect{ true };
            std::string m_bytes;
            bool m_pendingCR{ false };
        };

        void CloseIfOpen(HANDLE& handle)
        {
            if (handle && handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
            handle = nullptr;
        }
    }

    RunningTool::~RunningTool()
    {
        // Closing the job ends the tool and everything it started, if they're still running.
        CloseIfOpen(m_process);
        CloseIfOpen(m_job);
    }

    void RunningTool::Stop()
    {
        if (m_running)
        {
            m_stopped = true;
            TerminateJobObject(m_job, 1);
        }
    }

    std::shared_ptr<RunningTool> StartCaptured(Tool const& tool, FileContext const& file, RunningTool::Events events,
                                               std::wstring& commandLine, std::wstring& directory, std::wstring& error)
    {
        // Through cmd.exe, so batch files, cmd's own commands and anything on the PATH all work.
        auto command = Expand(tool.Command, file);
        auto arguments = Expand(tool.Arguments, file);
        directory = tool.WorkingDirectory.empty() ? std::filesystem::path{ file.Path }.parent_path().wstring() : Expand(tool.WorkingDirectory, file);
        bool needsQuotes = command.find_first_of(L" \t&()^") != std::wstring::npos && command.front() != L'"';
        commandLine = (needsQuotes ? L"\"" + command + L"\"" : command) + (arguments.empty() ? L"" : L" " + arguments);
        std::wstring shell(MAX_PATH, L'\0');
        shell.resize(GetEnvironmentVariableW(L"ComSpec", shell.data(), MAX_PATH));
        if (shell.empty())
        {
            shell = L"cmd.exe";
        }
        std::wstring fullCommand = L"\"" + shell + L"\" /d /s /c \"" + commandLine + L"\"";

        // Pipes for its output and errors, and the null device as its input so it can't wait for typing.
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        HANDLE outRead{}, outWrite{}, errRead{}, errWrite{};
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        auto cleanUp = [&]
        {
            CloseIfOpen(outRead); CloseIfOpen(outWrite); CloseIfOpen(errRead); CloseIfOpen(errWrite); CloseIfOpen(input);
        };
        if (input == INVALID_HANDLE_VALUE || !CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0))
        {
            error = winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError())).message();
            cleanUp();
            return nullptr;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        // Only these three handles are passed on to the tool.
        HANDLE inherited[] = { input, outWrite, errWrite };
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeBuffer(attributeSize);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
        InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize);
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = outWrite;
        startup.StartupInfo.hStdError = errWrite;
        startup.lpAttributeList = attributes;

        // In a job, so stopping it (or closing RadPad) also ends anything it started.
        auto running = std::make_shared<RunningTool>();
        running->m_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(running->m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        PROCESS_INFORMATION process{};
        BOOL started = CreateProcessW(nullptr, fullCommand.data(), nullptr, nullptr, TRUE,
                                      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                      nullptr, directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process);
        DWORD startError = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        CloseIfOpen(outWrite);
        CloseIfOpen(errWrite);
        CloseIfOpen(input);
        if (!started)
        {
            error = winrt::hresult_error(HRESULT_FROM_WIN32(startError)).message();
            cleanUp();
            return nullptr;
        }
        AssignProcessToJobObject(running->m_job, process.hProcess);
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        running->m_process = process.hProcess;

        // One thread reads each stream; another waits for the tool to finish. They keep the RunningTool alive.
        auto events_ = std::make_shared<RunningTool::Events>(std::move(events));
        auto encoding = tool.OutputEncoding;
        auto reader = [running, events_, encoding](HANDLE pipe, bool isError)
        {
            OutputDecoder decoder{ encoding };
            char buffer[8192];
            DWORD read = 0;
            while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                if (auto text = decoder.Decode(buffer, read, false); !text.empty())
                {
                    events_->Output(std::move(text), isError);
                }
            }
            if (auto text = decoder.Decode(nullptr, 0, true); !text.empty())
            {
                events_->Output(std::move(text), isError);
            }
            CloseHandle(pipe);
        };
        auto outThread = std::make_shared<std::thread>(reader, outRead, false);
        auto errThread = std::make_shared<std::thread>(reader, errRead, true);
        std::thread([running, events_, outThread, errThread]
        {
            WaitForSingleObject(running->m_process, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(running->m_process, &exitCode);
            // Output still in the pipes is read first. Something the tool started may keep a pipe open
            // after it exits, so stop reading after a moment.
            for (auto const& thread : { outThread, errThread })
            {
                auto handle = static_cast<HANDLE>(thread->native_handle());
                if (WaitForSingleObject(handle, 2000) == WAIT_TIMEOUT)
                {
                    do
                    {
                        CancelSynchronousIo(handle); // retried in case it was between reads
                    } while (WaitForSingleObject(handle, 100) == WAIT_TIMEOUT);
                }
                thread->join();
            }
            // Not running only once Finished has been called, so a caller that waits for that knows the
            // Finished event has been raised.
            events_->Finished(running->m_stopped ? std::nullopt : std::optional<DWORD>{ exitCode });
            running->m_running = false;
        }).detach();
        return running;
    }

    std::wstring ExampleToolsFile()
    {
        return LR"({
  "tools": [
    {
      "name": "Show in Explorer",
      "command": "explorer.exe",
      "arguments": "/select,\"$(FilePath)\"",
      "saveFirst": false
    },
    {
      "name": "Command prompt here",
      "command": "cmd.exe",
      "arguments": "",
      "saveFirst": false
    }
  ]
}
)";
    }
}
