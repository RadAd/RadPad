#include "pch.h"
#include "MainWindowHelpers.h"

// Syntax highlighting, the language menus, and colour themes.
namespace winrt::RadPad::implementation
{
    void MainWindow::ScheduleRefresh(std::shared_ptr<Document> const& doc)
    {
        // Several edits in a row (fast typing, IME input) are handled together once the dispatcher is idle.
        if (doc->RefreshPending)
        {
            return;
        }
        doc->RefreshPending = true;
        DispatcherQueue().TryEnqueue([this, strong = get_strong(), weakDoc = std::weak_ptr<Document>(doc)]
        {
            auto d = weakDoc.lock();
            if (!d || std::find(m_documents.begin(), m_documents.end(), d) == m_documents.end())
            {
                return;
            }
            d->RefreshPending = false;
            UpdateHighlighting(d, false);
            UpdateDocumentState(d);
            if (d == CurrentDocument())
            {
                UpdateCaretPosition();
            }
            ScheduleRepaint(d); // updates visible whitespace once the edit is laid out
        });
    }

    void MainWindow::UpdateHighlighting(std::shared_ptr<Document> const& doc, bool recolorAll)
    {
        std::wstring text = EditorText(doc->Editor);
        auto& applied = doc->AppliedKinds;
        auto const& old = doc->HighlightedText;

        if (text != old)
        {
            // The edit is the part between the common prefix and suffix of the old and new text.
            size_t prefix = std::mismatch(old.begin(), old.end(), text.begin(), text.end()).first - old.begin();
            size_t suffix = 0;
            size_t maxSuffix = std::min(old.size(), text.size()) - prefix;
            while (suffix < maxSuffix && old[old.size() - 1 - suffix] == text[text.size() - 1 - suffix])
            {
                ++suffix;
            }
            if (!doc->ApplyingUndo)
            {
                RecordEdit(doc, prefix, old.substr(prefix, old.size() - prefix - suffix), text.substr(prefix, text.size() - prefix - suffix));
            }
            if (recolorAll)
            {
                // Don't bother working out which colours survived.
                suffix = prefix = 0;
            }

            // Formatting moves with the text, so the colours before and after the edit are still in place.
            // The edited span is widened to cover the replaced selection, because replaced text all takes
            // on one format, even where the new text happens to match the old.
            auto delta = static_cast<ptrdiff_t>(text.size()) - static_cast<ptrdiff_t>(old.size());
            auto editStart = static_cast<size_t>(std::clamp<ptrdiff_t>(std::min(doc->EditStart, doc->EditEnd), 0, static_cast<ptrdiff_t>(text.size())));
            auto editEnd = static_cast<size_t>(std::clamp<ptrdiff_t>(std::max(doc->EditStart, doc->EditEnd) + delta, 0, static_cast<ptrdiff_t>(text.size())));
            size_t changedStart = std::min(prefix, editStart);
            size_t changedEnd = std::max(text.size() - suffix, editEnd);
            size_t keptBefore = changedStart;            // leading characters whose colour is known
            size_t keptAfter = text.size() - changedEnd; // trailing characters whose colour is known

            std::vector<TokenKind> rebased(text.size(), TokenKind::Unknown);
            std::copy_n(applied.begin(), keptBefore, rebased.begin());
            std::copy(applied.end() - keptAfter, applied.end(), rebased.end() - keptAfter);
            applied = std::move(rebased);
        }
        else if (recolorAll)
        {
            applied.assign(text.size(), TokenKind::Unknown);
        }

        syntax::Classify(doc->Language, text, doc->DesiredKinds, doc->Settings.RainbowBrackets);
        doc->HighlightedText = std::move(text);

        // New text gets the editor font and the plain colour, removing any formatting it came with.
        // This is one call per span of new text, so it's cheap even for a whole file.
        auto document = doc->Editor.Document();
        std::optional<FormattingScope> scope; // only opened if something needs painting
        for (size_t i = 0; i < applied.size();)
        {
            if (applied[i] != TokenKind::Unknown)
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < applied.size() && applied[j] == TokenKind::Unknown)
            {
                ++j;
            }
            if (!scope)
            {
                scope.emplace(doc->Editor);
            }
            auto format = document.GetRange(static_cast<int>(i), static_cast<int>(j)).CharacterFormat();
            ApplyFont(format, doc->Settings.Zoom);
            format.ForegroundColor(TokenColor(TokenKind::Plain));
            std::fill(applied.begin() + i, applied.begin() + j, TokenKind::Plain);
            i = j;
        }
        scope.reset();

        PaintVisibleText(doc);
    }

    void MainWindow::PaintVisibleText(std::shared_ptr<Document> const& doc)
    {
        // Colouring takes one call per token, which is slow for big files, so only the visible text
        // (plus a margin) is coloured. More is coloured as the editor scrolls.
        constexpr size_t Margin = 3000;

        auto editor = doc->Editor;
        auto& applied = doc->AppliedKinds;
        auto const& desired = doc->DesiredKinds;
        if (editor.Visibility() != Visibility::Visible || editor.ActualHeight() <= 0 || applied.size() != desired.size())
        {
            return; // painted once the editor is shown
        }

        // Work out the visible text from the scroll position. (RichEdit's GetRangeFromPoint can't be used:
        // it ignores the scroll position.) Without the ScrollViewer, everything is coloured.
        auto document = editor.Document();
        auto const& text = doc->HighlightedText;
        size_t from = 0;
        size_t to = desired.size();
        auto viewer = doc->ScrollViewer;
        if (viewer && viewer.ExtentHeight() > 0)
        {
            // The visible part as fractions of the document's height, widened by half a screen each way.
            double extent = viewer.ExtentHeight();
            double top = viewer.VerticalOffset() / extent;
            double bottom = (viewer.VerticalOffset() + viewer.ViewportHeight()) / extent;
            double pad = (bottom - top) / 2;
            top = std::clamp(top - pad, 0.0, 1.0);
            bottom = std::clamp(bottom + pad, 0.0, 1.0);

            // Lines are all the same height, so that fraction of the height is that fraction of the lines.
            // With word wrap a long line is taller than a short one, so also take the same fraction of the
            // characters, and colour everything either estimate covers.
            int lines = LineCount(text);
            auto lineFrom = static_cast<size_t>(IndexOfLine(text, static_cast<int>(top * lines) + 1));
            auto lineTo = static_cast<size_t>(IndexOfLine(text, static_cast<int>(std::ceil(bottom * lines)) + 2));
            auto charFrom = static_cast<size_t>(top * text.size());
            auto charTo = static_cast<size_t>(std::ceil(bottom * text.size()));
            from = std::min(lineFrom, charFrom);
            from = from > Margin ? from - Margin : 0;
            to = std::min(desired.size(), std::max(lineTo, charTo) + Margin);
        }

        std::optional<FormattingScope> scope;
        for (size_t i = from; i < to;)
        {
            if (applied[i] == desired[i])
            {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < desired.size() && desired[j] == desired[i] && applied[j] != desired[j])
            {
                ++j;
            }
            if (!scope)
            {
                scope.emplace(editor);
            }
            document.GetRange(static_cast<int>(i), static_cast<int>(j)).CharacterFormat().ForegroundColor(TokenColor(desired[i]));
            std::fill(applied.begin() + i, applied.begin() + j, desired[i]);
            i = j;
        }
    }

    void MainWindow::ScheduleRepaint(std::shared_ptr<Document> const& doc)
    {
        if (doc->RepaintPending)
        {
            return;
        }
        doc->RepaintPending = true;
        // Low priority so that layout has caught up with the scroll or resize that triggered this.
        DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this, strong = get_strong(), weakDoc = std::weak_ptr<Document>(doc)]
        {
            if (auto d = weakDoc.lock())
            {
                d->RepaintPending = false;
                if (!d->RefreshPending) // otherwise the refresh repaints
                {
                    PaintVisibleText(d);
                }
                if (d == CurrentDocument())
                {
                    UpdateEditorOverlay(); // after layout, so it matches the text
                }
            }
        });
    }

    void MainWindow::SetLanguage(std::shared_ptr<Document> const& doc, syntax::LanguageId language)
    {
        doc->Language = language;
        UpdateHighlighting(doc, false);
        if (doc == CurrentDocument())
        {
            UpdateStatusBar();
        }
    }

    void MainWindow::BuildLanguageMenus()
    {
        m_languageItems.clear();
        auto build = [this](Collections::IVector<MenuFlyoutItemBase> const& items, hstring const& group)
        {
            items.Clear();
            for (syntax::LanguageId language = 0; language < syntax::LanguageCount(); ++language)
            {
                RadioMenuFlyoutItem item;
                item.Text(syntax::LanguageName(language));
                item.GroupName(group);
                item.Click([this, language](auto&&, auto&&)
                {
                    if (auto doc = CurrentDocument())
                    {
                        SetLanguage(doc, language);
                        FocusEditor(doc);
                    }
                });
                items.Append(item);
                m_languageItems.emplace_back(item, language);
                if (language == syntax::PlainText)
                {
                    items.Append(MenuFlyoutSeparator{}); // plain text, then the languages
                }
            }
        };
        build(LanguageMenu().Items(), L"LanguageMenu");
        build(LanguageFlyout().Items(), L"LanguageStatus");

        // Commands for people editing the definitions.
        auto command = [this](wchar_t const* text, wchar_t glyph, std::function<void()> action)
        {
            MenuFlyoutItem item;
            item.Text(text);
            item.Icon(MenuIcon(glyph));
            item.Click([action](auto&&, auto&&) { action(); });
            LanguageMenu().Items().Append(item);
        };
        LanguageMenu().Items().Append(MenuFlyoutSeparator{});
        command(L"Open language folder", FolderGlyph, [this] { OpenLanguageFolder(); });
        command(L"Reload languages", RefreshGlyph, [this] { ReloadLanguages(); });
    }

    void MainWindow::ReloadLanguages()
    {
        // Ids can change, so remember each document's language by name.
        std::vector<std::wstring> names;
        for (auto const& doc : m_documents)
        {
            names.emplace_back(syntax::LanguageName(doc->Language));
        }

        m_languageErrors = syntax::LoadLanguages(LanguageFolders());
        BuildLanguageMenus();
        for (size_t i = 0; i < m_documents.size(); ++i)
        {
            m_documents[i]->Language = syntax::FindLanguage(names[i]);
            UpdateHighlighting(m_documents[i], false);
        }
        UpdateStatusBar();

        if (!m_languageErrors.empty())
        {
            ShowLoadErrors();
        }
        else
        {
            StatusMessageText().Text(std::format(L"Loaded {} languages", syntax::LanguageCount() - 1));
        }
    }

    void MainWindow::OpenLanguageFolder()
    {
        // The per-user folder, where added and customised definitions go.
        auto folder = UserLanguageFolder();
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        ShellExecuteW(WindowHandle(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::ShowLoadErrors()
    {
        // Language, theme and tool files together, as only one message can be shown at a time.
        std::wstring message;
        auto list = [&](wchar_t const* heading, std::vector<std::wstring> const& errors)
        {
            if (errors.empty())
            {
                return;
            }
            message += message.empty() ? heading : std::wstring(L"\n\n") + heading;
            for (auto const& error : errors)
            {
                message += L"\n\x2022 " + error;
            }
        };
        list(L"These language definitions couldn't be loaded:\n", m_languageErrors);
        list(L"These colour themes couldn't be loaded:\n", m_themeErrors);
        list(L"These tools couldn't be loaded:\n", m_toolErrors);
        if (!message.empty())
        {
            ShowMessageAsync(L"Settings files", hstring{ message });
        }
    }

    void MainWindow::BuildColorThemeMenu()
    {
        auto items = ColorThemeMenu().Items();
        items.Clear();
        m_themeItems.clear();
        for (int theme = 0; theme < themes::ThemeCount(); ++theme)
        {
            RadioMenuFlyoutItem item;
            item.Text(themes::ThemeName(theme));
            item.GroupName(L"ColorTheme");
            item.Click([this, theme](auto&&, auto&&) { SetColorTheme(std::wstring{ themes::ThemeName(theme) }); });
            items.Append(item);
            m_themeItems.emplace_back(item, theme);
        }

        // Commands for people editing the themes.
        auto command = [&](wchar_t const* text, wchar_t glyph, std::function<void()> action)
        {
            MenuFlyoutItem item;
            item.Text(text);
            item.Icon(MenuIcon(glyph));
            item.Click([action](auto&&, auto&&) { action(); });
            items.Append(item);
        };
        items.Append(MenuFlyoutSeparator{});
        command(L"Open themes folder", FolderGlyph, [this] { OpenThemeFolder(); });
        command(L"Reload themes", RefreshGlyph, [this] { ReloadThemes(); });
    }

    void MainWindow::SetColorTheme(std::wstring const& name)
    {
        m_colorTheme = name;
        ApplyColorTheme();
        SaveSettings();
        if (auto doc = CurrentDocument())
        {
            FocusEditor(doc);
        }
    }

    void MainWindow::ApplyColorTheme()
    {
        // The palette depends on whether the app is light or dark, so this also runs when that changes.
        int theme = themes::FindTheme(m_colorTheme);
        m_palette = themes::ThemePalette(theme, IsDarkTheme());
        if (m_palette.Background)
        {
            EditorHost().Background(SolidColorBrush{ *m_palette.Background });
        }
        else
        {
            EditorHost().ClearValue(Panel::BackgroundProperty()); // the app's own background
        }
        for (auto const& [item, index] : m_themeItems)
        {
            item.IsChecked(index == theme);
        }
        for (auto const& doc : m_documents)
        {
            ApplyEditorSettings(doc);
            UpdateHighlighting(doc, true);
        }
    }

    Windows::UI::Color MainWindow::TokenColor(TokenKind kind)
    {
        auto index = static_cast<size_t>(kind);
        return m_palette.Colors[index < m_palette.Colors.size() ? index : 0];
    }

    void MainWindow::ReloadThemes()
    {
        m_themeErrors = themes::LoadThemes(ThemeFolders());
        BuildColorThemeMenu();
        ApplyColorTheme();
        if (!m_themeErrors.empty())
        {
            ShowLoadErrors();
        }
        else
        {
            StatusMessageText().Text(std::format(L"Loaded {} colour themes", themes::ThemeCount()));
        }
    }

    void MainWindow::OpenThemeFolder()
    {
        // The per-user folder, where added and customised themes go.
        auto folder = UserThemeFolder();
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        ShellExecuteW(WindowHandle(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
