#include "pch.h"
#include "MainWindowHelpers.h"

// The Tools menu: external programs run on the current file.
namespace winrt::RadPad::implementation
{
    void MainWindow::ReloadTools()
    {
        auto path = ToolsFilePath();
        m_toolErrors.clear();
        m_tools = tools::LoadTools(path, m_toolErrors);
        BuildToolsMenu();
        if (m_loaded && !m_toolErrors.empty())
        {
            ShowLoadErrors();
        }
    }

    void MainWindow::BuildToolsMenu()
    {
        // One at a time: once a menu bar menu has been opened, Clear() doesn't reach the open menu, while
        // removing single items does.
        auto items = ToolsMenu().Items();
        while (items.Size() > 0)
        {
            items.RemoveAtEnd();
        }
        // Menu icons are 16 pixels at 100% scaling; load them at the screen's size so they're sharp.
        int iconSize = MulDiv(16, static_cast<int>(GetDpiForWindow(WindowHandle())), 96);
        AddToolItems(items, m_tools, iconSize);
        if (m_tools.empty())
        {
            items.GetAt(0).as<MenuFlyoutItem>().Text(L"No tools");
        }
        items.Append(MenuFlyoutSeparator{});
        MenuFlyoutItem edit;
        edit.Text(L"Edit tools...");
        edit.Icon(MenuIcon(EditGlyph));
        edit.Click([this](auto&&, auto&&) { EditTools(); });
        items.Append(edit);
    }

    void MainWindow::AddToolItems(Collections::IVector<MenuFlyoutItemBase> const& items, std::vector<tools::Tool> const& list, int iconSize)
    {
        // Tools as menu items and folders as submenus, each with its icon.
        auto iconFor = [iconSize](tools::Tool const& tool) -> IconElement
        {
            if (!tool.Glyph.empty())
            {
                FontIcon icon;
                icon.Glyph(tool.Glyph);
                return icon;
            }
            std::vector<uint8_t> pixels;
            if (!tools::LoadToolIcon(tool, iconSize, pixels))
            {
                return nullptr;
            }
            Media::Imaging::WriteableBitmap bitmap{ iconSize, iconSize };
            memcpy(bitmap.PixelBuffer().data(), pixels.data(), pixels.size());
            bitmap.Invalidate();
            ImageIcon icon;
            icon.Source(bitmap);
            return icon;
        };

        for (auto const& tool : list)
        {
            if (tool.IsFolder)
            {
                MenuFlyoutSubItem folder;
                folder.Text(tool.Name);
                folder.Icon(iconFor(tool));
                AddToolItems(folder.Items(), tool.Children, iconSize);
                items.Append(folder);
                continue;
            }
            MenuFlyoutItem item;
            item.Text(tool.Name);
            item.Icon(iconFor(tool));
            item.Click([this, tool](auto&&, auto&&) { RunToolAsync(tool); });
            items.Append(item);
        }
        if (list.empty())
        {
            MenuFlyoutItem empty;
            empty.Text(L"Empty");
            empty.IsEnabled(false);
            items.Append(empty);
        }
    }

    fire_and_forget MainWindow::RunToolAsync(tools::Tool tool)
    {
        auto strong = get_strong();
        auto doc = CurrentDocument();
        if (!doc)
        {
            co_return;
        }

        // Line breaks in the selection become spaces, as a command line can't hold them.
        auto file = tools::FileContext{};
        file.SelectedText = SelectedText(doc->Editor);
        std::replace(file.SelectedText.begin(), file.SelectedText.end(), L'\r', L' ');
        if (tools::UsesSelection(tool) && file.SelectedText.find_first_not_of(L" \t") == std::wstring::npos)
        {
            StatusMessageText().Text(std::format(L"Select some text for \"{}\" first", tool.Name));
            co_return;
        }

        // Text for $(Prompt) placeholders is asked for first, so Cancel leaves everything as it was. Their
        // defaults can use the file and selection.
        file.Path = doc->Path;
        std::tie(file.Line, file.Column) = LineColumnAt(EditorText(doc->Editor), SelectionStart(doc->Editor));
        if (auto prompts = tools::Prompts(tool); !prompts.empty())
        {
            auto inputs = std::make_shared<std::map<std::wstring, std::wstring>>();
            if (!co_await AskToolInputsAsync(tool, prompts, file, inputs) || doc != CurrentDocument())
            {
                co_return;
            }
            file.Inputs = std::move(*inputs);
        }

        // A tool given the file, or one that saves first and so works on files in its folder, needs an
        // untitled document saved first; one that doesn't (a web search, say) runs without. By default
        // unsaved changes are saved so the tool sees what's on screen.
        bool needsFile = tools::UsesFile(tool) || tool.SaveFirst;
        if ((doc->Path.empty() && needsFile) || (!doc->Path.empty() && tool.SaveFirst && doc->IsDirty))
        {
            if (!SaveDocument(doc, doc->Path.empty()))
            {
                co_return;
            }
        }

        file.Path = doc->Path;
        std::tie(file.Line, file.Column) = LineColumnAt(EditorText(doc->Editor), SelectionStart(doc->Editor));
        if (tool.CaptureOutput)
        {
            RunToolInPanelAsync(tool, std::move(file));
        }
        else if (auto error = tools::RunTool(tool, file, WindowHandle()))
        {
            ShowMessageAsync(hstring{ AppName }, hstring{ *error });
        }
    }

    IAsyncOperation<bool> MainWindow::AskToolInputsAsync(tools::Tool tool, std::vector<tools::Prompt> prompts, tools::FileContext file,
                                                         std::shared_ptr<std::map<std::wstring, std::wstring>> inputs)
    {
        // One box per label. Each starts with its default, if it has one that isn't empty (one that's the
        // selected text is empty with nothing selected), or else with what was typed for it last time this
        // tool ran. The selection is only used where a default asks for it.
        auto strong = get_strong();
        StackPanel panel;
        panel.Spacing(12);
        std::vector<TextBox> boxes;
        for (auto const& prompt : prompts)
        {
            TextBox box;
            box.Header(box_value(prompt.Label));
            box.MinWidth(320);
            box.IsSpellCheckEnabled(false);
            auto value = prompt.HasDefault ? tools::ExpandPlaceholders(prompt.Default, file) : std::wstring{};
            if (!value.empty())
            {
                box.Text(value);
            }
            else if (auto last = m_toolInputs.find({ tool.Name, prompt.Label }); last != m_toolInputs.end())
            {
                box.Text(last->second);
            }
            panel.Children().Append(box);
            boxes.push_back(box);
        }
        boxes.front().Loaded([weakBox = make_weak(boxes.front())](auto&&, auto&&)
        {
            if (auto box = weakBox.get())
            {
                box.Focus(FocusState::Programmatic);
                box.SelectAll();
            }
        });

        ContentDialog dialog;
        dialog.Title(box_value(hstring{ tool.Name }));
        dialog.Content(panel);
        dialog.PrimaryButtonText(L"Run");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await ShowDialogAsync(dialog) != ContentDialogResult::Primary)
        {
            co_return false;
        }

        for (size_t i = 0; i < prompts.size(); ++i)
        {
            (*inputs)[prompts[i].Label] = boxes[i].Text();
            m_toolInputs[{ tool.Name, prompts[i].Label }] = boxes[i].Text();
        }
        co_return true;
    }

    void MainWindow::EditTools()
    {
        // Opens the tools file in a tab, creating it with examples the first time. Saving it updates the menu.
        auto path = ToolsFilePath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            auto text = tools::ExampleToolsFile();
            int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
            if (FILE* file = nullptr; _wfopen_s(&file, path.c_str(), L"wb") == 0 && file)
            {
                fwrite(utf8.data(), 1, utf8.size(), file);
                fclose(file);
            }
        }
        OpenFile(path);
    }
}
