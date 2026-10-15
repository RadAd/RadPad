#include "pch.h"
#include "Tools.h"
#include "TextFile.h"
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
#include <wincodec.h>

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
    }

    std::vector<Tool> LoadTools(std::wstring const& path, std::vector<std::wstring>& errors)
    {
        std::vector<Tool> result;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return result;
        }
        try
        {
            auto json = JsonObject::Parse(textfile::Load(path).Text);
            if (!json.HasKey(L"tools") || json.GetNamedValue(L"tools").ValueType() != JsonValueType::Array)
            {
                Invalid(L"\"tools\" must be a list of tools");
            }
            auto list = json.GetNamedArray(L"tools");
            for (uint32_t i = 0; i < list.Size(); ++i)
            {
                // A bad entry is reported and skipped; the others still load.
                try
                {
                    if (list.GetAt(i).ValueType() != JsonValueType::Object)
                    {
                        Invalid(L"each tool must be an object");
                    }
                    auto entry = list.GetObjectAt(i);
                    Tool tool;
                    tool.Name = ReadString(entry, L"name", true);
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
                    tool.Icon = ReadString(entry, L"icon", false);
                    tool.Glyph = ParseGlyph(ReadString(entry, L"glyph", false));
                    result.push_back(std::move(tool));
                }
                catch (winrt::hresult_error const& e)
                {
                    errors.push_back(std::format(L"{}: tool {}: {}", path, i + 1, std::wstring_view{ e.message() }));
                }
            }
        }
        catch (winrt::hresult_error const& e)
        {
            errors.push_back(path + L": " + std::wstring{ e.message() });
        }
        return result;
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
