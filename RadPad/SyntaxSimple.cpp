#include "pch.h"
#include "SyntaxInternal.h"

// Line-based formats: batch files, and INI and similar key = value files.
namespace syntax::detail
{
    namespace
    {
        void ClassifyBatchLine(Scanner const& s, WordSet const& keywords, size_t i, size_t lineEnd)
        {
            auto const text = s.Text;
            while (i < lineEnd)
            {
                wchar_t c = text[i];
                if (c == L'"')
                {
                    size_t close = text.find(L'"', i + 1);
                    size_t end = close == npos || close >= lineEnd ? lineEnd : close + 1;
                    s.Fill(i, end, TokenKind::String);
                    i = end;
                    continue;
                }
                if (c == L'%')
                {
                    size_t end = 0;
                    wchar_t next = s.At(i + 1);
                    if (next == L'%')
                    {
                        end = std::min(i + 3, lineEnd); // %%i loop variable
                    }
                    else if (IsDigit(next) || next == L'*')
                    {
                        end = i + 2; // %1 argument
                    }
                    else if (next == L'~')
                    {
                        size_t j = i + 2; // %~dp0
                        while (j < lineEnd && std::iswalpha(text[j]))
                        {
                            ++j;
                        }
                        if (j < lineEnd && IsDigit(text[j]))
                        {
                            end = j + 1;
                        }
                    }
                    else
                    {
                        size_t close = text.find(L'%', i + 1); // %name%
                        if (close != npos && close < lineEnd && close > i + 1)
                        {
                            end = close + 1;
                        }
                    }
                    if (end)
                    {
                        s.Fill(i, end, TokenKind::Variable);
                        i = end;
                        continue;
                    }
                }
                if (c == L'!')
                {
                    size_t close = text.find(L'!', i + 1); // !name! with delayed expansion
                    if (close != npos && close < lineEnd && close > i + 1 && text.substr(i + 1, close - i - 1).find_first_of(L" \t") == npos)
                    {
                        s.Fill(i, close + 1, TokenKind::Variable);
                        i = close + 1;
                        continue;
                    }
                }
                if (IsIdentStart(c))
                {
                    size_t j = i;
                    while (j < lineEnd && IsIdentChar(text[j]))
                    {
                        ++j;
                    }
                    if (keywords.contains(Lower(text.substr(i, j - i))))
                    {
                        s.Fill(i, j, TokenKind::Keyword);
                    }
                    i = j;
                    continue;
                }
                ++i;
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // Batch files
    // -----------------------------------------------------------------------------------------

    void ClassifyBatch(Scanner const& s, LanguageDefinition const& lang)
    {
        auto const text = s.Text;
        size_t i = 0;
        while (i < text.size())
        {
            size_t lineEnd = s.LineEnd(i);
            size_t j = i;
            while (j < lineEnd && (IsBlank(text[j]) || text[j] == L'@'))
            {
                if (text[j] == L'@')
                {
                    s.Fill(j, j + 1, TokenKind::Preprocessor);
                }
                ++j;
            }

            std::wstring first = Lower(text.substr(j, std::min<size_t>(4, lineEnd - j)));
            if (s.StartsWith(j, L"::") || (first.starts_with(L"rem") && (first.size() == 3 || IsBlank(first[3]))))
            {
                s.Fill(j, lineEnd, TokenKind::Comment);
            }
            else if (s.At(j) == L':')
            {
                s.Fill(j, lineEnd, TokenKind::Tag); // label
            }
            else
            {
                ClassifyBatchLine(s, lang.Keywords, j, lineEnd);
            }
            i = lineEnd + 1;
        }
    }

    // -----------------------------------------------------------------------------------------
    // INI and similar key = value files
    // -----------------------------------------------------------------------------------------

    void ClassifyIni(Scanner const& s)
    {
        auto const text = s.Text;
        size_t i = 0;
        while (i < text.size())
        {
            size_t lineEnd = s.LineEnd(i);
            size_t j = i;
            while (j < lineEnd && IsBlank(text[j]))
            {
                ++j;
            }
            if (j < lineEnd)
            {
                wchar_t c = text[j];
                if (c == L';' || c == L'#')
                {
                    s.Fill(j, lineEnd, TokenKind::Comment);
                }
                else if (c == L'[')
                {
                    size_t close = text.find(L']', j);
                    s.Fill(j, close != npos && close < lineEnd ? close + 1 : lineEnd, TokenKind::Tag);
                }
                else if (size_t equals = text.find(L'=', j); equals != npos && equals < lineEnd)
                {
                    size_t keyEnd = equals;
                    while (keyEnd > j && IsBlank(text[keyEnd - 1]))
                    {
                        --keyEnd;
                    }
                    s.Fill(j, keyEnd, TokenKind::Property);
                }
            }
            i = lineEnd + 1;
        }
    }
}
