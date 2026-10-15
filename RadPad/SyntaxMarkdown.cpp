#include "pch.h"
#include "SyntaxInternal.h"

#include <array>

// Markdown (CommonMark and GitHub's extensions), simplified: block structure line by line, then inline
// markup within each line. Fenced code is coloured as the fence's language.
namespace syntax::detail
{
    namespace
    {
        bool IsBlankLine(Scanner const& s, size_t from, size_t lineEnd)
        {
            for (size_t k = from; k < lineEnd; ++k)
            {
                if (!IsBlank(s.Text[k]))
                {
                    return false;
                }
            }
            return true;
        }

        // The end of the paragraph containing i: the line break before the next blank line, or the end of the text.
        size_t ParagraphEnd(Scanner const& s, size_t i)
        {
            for (size_t lineEnd = s.LineEnd(i); lineEnd < s.Size(); lineEnd = s.LineEnd(lineEnd + 1))
            {
                if (IsBlankLine(s, lineEnd + 1, s.LineEnd(lineEnd + 1)))
                {
                    return lineEnd;
                }
            }
            return s.Size();
        }

        // The language named after a code fence (```cpp), by language name, file extension or a common alias.
        LanguageId FenceLanguage(std::wstring_view info)
        {
            if (info.empty())
            {
                return PlainText;
            }
            if (auto id = FindLanguage(info); id != PlainText)
            {
                return id;
            }
            static constexpr std::pair<std::wstring_view, std::wstring_view> aliases[] = {
                { L"c++", L"cpp" }, { L"csharp", L"cs" }, { L"c#", L"cs" }, { L"pwsh", L"ps1" }, { L"posh", L"ps1" },
                { L"jsonc", L"json" }, { L"jsx", L"js" }, { L"tsx", L"ts" }, { L"golang", L"go" }, { L"dos", L"bat" },
                { L"markdown", L"md" }, { L"svg", L"xml" }, { L"xaml", L"xml" },
            };
            std::wstring extension = Lower(info);
            for (auto const& [alias, target] : aliases)
            {
                if (extension == alias)
                {
                    extension = target;
                }
            }
            return LanguageFromPath(L"x." + extension);
        }

        bool IsAsciiPunctuation(wchar_t c) { return c < 128 && std::iswpunct(c); }

        // What ClassifyMarkdownInline remembers between lines, so a long paragraph isn't searched again and again.
        struct MarkdownInlineState
        {
            size_t ParagraphStart = npos;
            size_t ParagraphEnd = npos;
            std::array<size_t, 8> NoCloserBefore{}; // by backtick run length: no closing run before this position
        };

        // Colours links, code, emphasis and other inline markup from i up to the end of the line. Code spans
        // and HTML comments can run onto later lines; returns where colouring stopped, which is then past lineEnd.
        size_t ClassifyMarkdownInline(Scanner const& s, size_t i, size_t lineEnd, bool inTable, MarkdownInlineState& state)
        {
            auto const text = s.Text;
            auto closingBracket = [&](size_t open, wchar_t openChar, wchar_t closeChar)
            {
                int depth = 0;
                for (size_t k = open; k < lineEnd; ++k)
                {
                    if (text[k] == L'\\')
                    {
                        ++k;
                    }
                    else if (text[k] == openChar)
                    {
                        ++depth;
                    }
                    else if (text[k] == closeChar && --depth == 0)
                    {
                        return k;
                    }
                }
                return npos;
            };

            while (i < lineEnd)
            {
                wchar_t c = text[i];
                wchar_t previous = i > 0 ? text[i - 1] : L' ';
                if (c == L'\\' && IsAsciiPunctuation(s.At(i + 1)))
                {
                    i += 2; // an escaped character is just text
                    continue;
                }
                if (c == L'`')
                {
                    size_t run = i;
                    while (s.At(run) == L'`')
                    {
                        ++run;
                    }
                    size_t length = run - i;
                    // The span ends at the next run of exactly as many backticks, within the paragraph.
                    if (state.ParagraphStart == npos || i < state.ParagraphStart || i > state.ParagraphEnd)
                    {
                        state.ParagraphStart = i;
                        state.ParagraphEnd = ParagraphEnd(s, i);
                    }
                    size_t end = state.ParagraphEnd;
                    size_t slot = std::min<size_t>(length, state.NoCloserBefore.size() - 1);
                    if (state.NoCloserBefore[slot] >= end && state.NoCloserBefore[slot] > i)
                    {
                        i = run; // already known not to close
                        continue;
                    }
                    for (size_t k = run; k < end;)
                    {
                        if (text[k] != L'`')
                        {
                            ++k;
                            continue;
                        }
                        size_t closeRun = k;
                        while (closeRun < end && text[closeRun] == L'`')
                        {
                            ++closeRun;
                        }
                        if (closeRun - k == length)
                        {
                            s.Fill(i, closeRun, TokenKind::String);
                            if (closeRun > lineEnd)
                            {
                                return closeRun;
                            }
                            i = closeRun;
                            break;
                        }
                        k = closeRun;
                    }
                    if (i < run)
                    {
                        if (slot == length)
                        {
                            state.NoCloserBefore[slot] = end;
                        }
                        i = run; // not closed: the backticks are text
                    }
                    continue;
                }
                if (s.StartsWith(i, L"<!--"))
                {
                    size_t end = s.FindEnd(i + 4, L"-->");
                    s.Fill(i, end, TokenKind::Comment);
                    if (end > lineEnd)
                    {
                        return end;
                    }
                    i = end;
                    continue;
                }
                if (c == L'<')
                {
                    size_t close = text.find(L'>', i + 1);
                    if (close != npos && close < lineEnd)
                    {
                        auto inside = text.substr(i + 1, close - i - 1);
                        bool spaces = inside.find_first_of(L" \t") != npos;
                        if (!spaces && (inside.find(L"://") != npos || inside.find(L'@') != npos) && !inside.empty())
                        {
                            s.Fill(i, close + 1, TokenKind::Attribute); // <https://...> or <someone@example.com>
                            i = close + 1;
                            continue;
                        }
                        if (std::iswalpha(s.At(i + 1)) || (s.At(i + 1) == L'/' && std::iswalpha(s.At(i + 2))))
                        {
                            s.Fill(i, close + 1, TokenKind::Tag); // inline HTML
                            i = close + 1;
                            continue;
                        }
                    }
                }
                if (c == L'[' || (c == L'!' && s.At(i + 1) == L'['))
                {
                    size_t open = c == L'!' ? i + 1 : i;
                    size_t close = closingBracket(open, L'[', L']');
                    if (close != npos)
                    {
                        wchar_t after = s.At(close + 1);
                        size_t end = npos;
                        if (after == L'(')
                        {
                            end = closingBracket(close + 1, L'(', L')');
                        }
                        else if (after == L'[')
                        {
                            end = closingBracket(close + 1, L'[', L']');
                        }
                        if (end != npos)
                        {
                            s.Fill(i, close + 1, TokenKind::Tag);       // [text] or ![alt]
                            s.Fill(close + 1, end + 1, TokenKind::Attribute); // (url "title") or [reference]
                            i = end + 1;
                            continue;
                        }
                        if (s.At(open + 1) == L'^')
                        {
                            s.Fill(i, close + 1, TokenKind::Tag); // [^footnote]; other brackets in text are left alone
                            i = close + 1;
                            continue;
                        }
                    }
                }
                if ((s.StartsWith(i, L"http://") || s.StartsWith(i, L"https://") || s.StartsWith(i, L"www.")) && !IsIdentChar(previous))
                {
                    size_t end = i;
                    while (end < lineEnd && !IsBlank(text[end]) && text[end] != L'<')
                    {
                        ++end;
                    }
                    while (end > i && std::wstring_view{ L".,;:!?*_~'\")" }.find(text[end - 1]) != npos)
                    {
                        --end; // trailing punctuation belongs to the sentence
                    }
                    s.Fill(i, end, TokenKind::Attribute);
                    i = std::max(end, i + 1);
                    continue;
                }
                if (c == L'*' || c == L'_' || (c == L'~' && s.At(i + 1) == L'~'))
                {
                    size_t run = i;
                    while (run < lineEnd && text[run] == c)
                    {
                        ++run;
                    }
                    size_t length = run - i;
                    // Opens only before text, and an underscore inside a word (snake_case) isn't emphasis.
                    bool opens = run < lineEnd && !IsBlank(text[run]) && !(c == L'_' && IsIdentChar(previous));
                    if (opens)
                    {
                        for (size_t k = run; k < lineEnd; ++k)
                        {
                            if (text[k] == L'\\')
                            {
                                ++k;
                                continue;
                            }
                            if (text[k] != c)
                            {
                                continue;
                            }
                            size_t closeRun = k;
                            while (closeRun < lineEnd && text[closeRun] == c)
                            {
                                ++closeRun;
                            }
                            bool closes = closeRun - k == length && !IsBlank(text[k - 1]) && !(c == L'_' && closeRun < lineEnd && IsIdentChar(text[closeRun]));
                            if (closes)
                            {
                                auto kind = c == L'~' ? TokenKind::Comment : length >= 2 ? TokenKind::Type : TokenKind::Variable;
                                s.Fill(i, closeRun, kind);
                                run = closeRun;
                                break;
                            }
                            k = closeRun - 1;
                        }
                    }
                    i = run;
                    continue;
                }
                if (c == L'|' && inTable)
                {
                    s.Fill(i, i + 1, TokenKind::Comment);
                }
                ++i;
            }
            return lineEnd;
        }

        // A run of three or more of one character (*, - or _) with only spaces between: a thematic break.
        bool IsThematicBreak(Scanner const& s, size_t j, size_t lineEnd)
        {
            wchar_t c = s.At(j);
            if (c != L'*' && c != L'-' && c != L'_')
            {
                return false;
            }
            int count = 0;
            for (size_t k = j; k < lineEnd; ++k)
            {
                if (s.Text[k] == c)
                {
                    ++count;
                }
                else if (!IsBlank(s.Text[k]))
                {
                    return false;
                }
            }
            return count >= 3;
        }

        // A table's delimiter row, such as |---|:--:| or --- | ---.
        bool IsTableDelimiterRow(Scanner const& s, size_t j, size_t lineEnd)
        {
            bool pipe = false, dash = false;
            for (size_t k = j; k < lineEnd; ++k)
            {
                wchar_t c = s.Text[k];
                if (c == L'|')
                {
                    pipe = true;
                }
                else if (c == L'-')
                {
                    dash = true;
                }
                else if (c != L':' && !IsBlank(c))
                {
                    return false;
                }
            }
            return pipe && dash;
        }
    }

    void ClassifyMarkdown(Scanner const& s)
    {
        auto const text = s.Text;
        size_t i = 0;

        // YAML front matter between --- lines at the very start
        if (s.StartsWith(0, L"---") && IsBlankLine(s, 3, s.LineEnd(0)))
        {
            for (size_t line = s.LineEnd(0) + 1; line < s.Size(); line = s.LineEnd(line) + 1)
            {
                size_t lineEnd = s.LineEnd(line);
                if ((s.StartsWith(line, L"---") || s.StartsWith(line, L"...")) && IsBlankLine(s, line + 3, lineEnd))
                {
                    s.Fill(0, lineEnd, TokenKind::Comment);
                    i = lineEnd + 1;
                    break;
                }
            }
        }

        size_t paragraphLine = npos; // start of the previous line, if it was paragraph text (for underlined headings)
        size_t previousLine = npos;  // start of the previous line, for a table's header row
        bool inTable = false;
        MarkdownInlineState inlineState;
        while (i < text.size())
        {
            size_t lineEnd = s.LineEnd(i);
            size_t lastLine = previousLine;
            size_t lastParagraph = paragraphLine;
            previousLine = i;
            paragraphLine = npos;
            if (IsBlankLine(s, i, lineEnd))
            {
                inTable = false;
                i = lineEnd + 1;
                continue;
            }

            size_t j = i;
            while (j < lineEnd && IsBlank(text[j]))
            {
                ++j;
            }

            // Fenced code: ``` or ~~~, closed by a fence at least as long
            wchar_t fence = s.At(j);
            if ((fence == L'`' || fence == L'~') && s.At(j + 1) == fence && s.At(j + 2) == fence)
            {
                size_t run = j;
                while (s.At(run) == fence)
                {
                    ++run;
                }
                size_t length = run - j;
                auto info = text.substr(run, lineEnd - run);
                if (fence == L'~' || info.find(L'`') == npos)
                {
                    size_t nameStart = run;
                    while (nameStart < lineEnd && (IsBlank(text[nameStart]) || text[nameStart] == L'{' || text[nameStart] == L'.'))
                    {
                        ++nameStart;
                    }
                    size_t nameEnd = nameStart;
                    while (nameEnd < lineEnd && !IsBlank(text[nameEnd]) && text[nameEnd] != L'{' && text[nameEnd] != L'}' && text[nameEnd] != L',')
                    {
                        ++nameEnd;
                    }
                    s.Fill(j, lineEnd, TokenKind::Preprocessor);

                    size_t contentStart = std::min(lineEnd + 1, s.Size());
                    size_t contentEnd = s.Size();
                    size_t next = s.Size();
                    for (size_t line = contentStart; line < s.Size(); line = s.LineEnd(line) + 1)
                    {
                        size_t closeEnd = s.LineEnd(line);
                        size_t k = line;
                        while (k < closeEnd && IsBlank(text[k]))
                        {
                            ++k;
                        }
                        size_t closeRun = k;
                        while (closeRun < closeEnd && text[closeRun] == fence)
                        {
                            ++closeRun;
                        }
                        if (closeRun - k >= length && IsBlankLine(s, closeRun, closeEnd))
                        {
                            contentEnd = line > contentStart ? line - 1 : contentStart; // before the closing line's break
                            s.Fill(k, closeEnd, TokenKind::Preprocessor);
                            next = closeEnd + 1;
                            break;
                        }
                    }

                    auto language = FenceLanguage(text.substr(nameStart, nameEnd - nameStart));
                    if (contentEnd > contentStart)
                    {
                        if (language != PlainText)
                        {
                            ClassifyInto(Languages().Languages[language], text.substr(contentStart, contentEnd - contentStart), s.Kinds + contentStart);
                        }
                        else
                        {
                            s.Fill(contentStart, contentEnd, TokenKind::String);
                        }
                    }
                    i = next;
                    continue;
                }
            }

            // Block quote markers, possibly nested: > > text
            while (s.At(j) == L'>' && j < lineEnd)
            {
                s.Fill(j, j + 1, TokenKind::Keyword);
                ++j;
                while (j < lineEnd && IsBlank(text[j]))
                {
                    ++j;
                }
            }

            // # Heading
            if (s.At(j) == L'#')
            {
                size_t hashes = j;
                while (s.At(hashes) == L'#')
                {
                    ++hashes;
                }
                if (hashes - j <= 6 && (hashes == lineEnd || IsBlank(s.At(hashes))))
                {
                    s.Fill(j, lineEnd, TokenKind::Keyword);
                    i = lineEnd + 1;
                    continue;
                }
            }

            // Underlined heading: a line of = or - under paragraph text
            if (lastParagraph != npos && (s.At(j) == L'=' || s.At(j) == L'-'))
            {
                size_t k = j;
                while (k < lineEnd && text[k] == text[j])
                {
                    ++k;
                }
                if (IsBlankLine(s, k, lineEnd))
                {
                    s.Fill(lastParagraph, lineEnd, TokenKind::Keyword);
                    i = lineEnd + 1;
                    continue;
                }
            }

            if (IsThematicBreak(s, j, lineEnd))
            {
                s.Fill(j, lineEnd, TokenKind::Comment);
                i = lineEnd + 1;
                continue;
            }

            // Table: the delimiter row makes the line above the header, and pipes are coloured until a blank line.
            if (IsTableDelimiterRow(s, j, lineEnd))
            {
                s.Fill(j, lineEnd, TokenKind::Comment);
                if (lastLine != npos && !inTable)
                {
                    for (size_t k = lastLine; k < s.LineEnd(lastLine); ++k)
                    {
                        if (text[k] == L'|' && (k == 0 || text[k - 1] != L'\\') && s.Kinds[k] != TokenKind::String)
                        {
                            s.Fill(k, k + 1, TokenKind::Comment);
                        }
                    }
                }
                inTable = true;
                i = lineEnd + 1;
                continue;
            }

            // [reference]: url "title"
            if (s.At(j) == L'[' && s.At(j + 1) != L'^')
            {
                size_t close = text.find(L']', j);
                if (close != npos && close < lineEnd && s.At(close + 1) == L':')
                {
                    s.Fill(j, close + 2, TokenKind::Tag);
                    size_t url = close + 2;
                    while (url < lineEnd && IsBlank(text[url]))
                    {
                        ++url;
                    }
                    size_t urlEnd = url;
                    while (urlEnd < lineEnd && !IsBlank(text[urlEnd]))
                    {
                        ++urlEnd;
                    }
                    s.Fill(url, urlEnd, TokenKind::Attribute);
                    s.Fill(urlEnd, lineEnd, TokenKind::String);
                    i = lineEnd + 1;
                    continue;
                }
            }

            // List item: - * + or 1. 1) then a space, and an optional task box [ ] or [x]
            bool listItem = false;
            if ((s.At(j) == L'-' || s.At(j) == L'*' || s.At(j) == L'+') && (j + 1 == lineEnd || IsBlank(s.At(j + 1))))
            {
                s.Fill(j, j + 1, TokenKind::Keyword);
                j += 1;
                listItem = true;
            }
            else if (IsDigit(s.At(j)))
            {
                size_t k = j;
                while (k < lineEnd && k - j < 9 && IsDigit(text[k]))
                {
                    ++k;
                }
                if ((s.At(k) == L'.' || s.At(k) == L')') && (k + 1 == lineEnd || IsBlank(s.At(k + 1))))
                {
                    s.Fill(j, k + 1, TokenKind::Keyword);
                    j = k + 1;
                    listItem = true;
                }
            }
            if (listItem)
            {
                while (j < lineEnd && IsBlank(text[j]))
                {
                    ++j;
                }
                if (s.At(j) == L'[' && (s.At(j + 1) == L' ' || s.At(j + 1) == L'x' || s.At(j + 1) == L'X') && s.At(j + 2) == L']')
                {
                    s.Fill(j, j + 3, TokenKind::Keyword);
                    j += 3;
                }
            }

            size_t stop = ClassifyMarkdownInline(s, j, lineEnd, inTable || (!listItem && s.At(i) == L'|'), inlineState);
            if (stop > lineEnd)
            {
                // A code span or comment ran onto later lines: carry on after it, in the middle of its last line.
                while (stop > lineEnd)
                {
                    lineEnd = s.LineEnd(stop);
                    stop = ClassifyMarkdownInline(s, stop, lineEnd, inTable, inlineState);
                }
                previousLine = npos;
                i = lineEnd + 1;
                continue;
            }
            if (!listItem && j == i)
            {
                paragraphLine = i; // could be the text of an underlined heading
            }
            i = lineEnd + 1;
        }
    }
}
