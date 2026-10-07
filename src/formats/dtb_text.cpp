// Script text, as the port's own .dta files are written.

#include "formats/dtb.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace gh2::dtb
{
    namespace
    {
        // The nodes of `text` from `o` up to `close`, or to its end for none.
        bool parseNodes(const std::string &text, size_t &o, char close, std::vector<Node> &out)
        {
            static const std::string kEnds = " \t\r\n()[]{}\";";
            for (;;)
            {
                while (o < text.size() && (std::isspace(static_cast<unsigned char>(text[o])) || text[o] == ';'))
                    if (text[o++] == ';')
                        while (o < text.size() && text[o] != '\n')
                            ++o;
                if (o >= text.size())
                    return close == 0;
                const char c = text[o];
                if (c == ')' || c == '}' || c == ']')
                {
                    ++o;
                    return c == close;
                }
                Node node;
                if (c == '(' || c == '{' || c == '[')
                {
                    node.type = c == '(' ? kArray : c == '{' ? kCommand : kProperty;
                    if (!parseNodes(text, ++o, c == '(' ? ')' : c == '{' ? '}' : ']', node.nodes))
                        return false;
                }
                else if (c == '"' || c == '\'')
                {
                    node.type = c == '"' ? kString : kSymbol;
                    for (++o; o < text.size() && text[o] != c; ++o)
                        if (text[o] == '\\' && o + 1u < text.size() && (text[o + 1u] == 'q' || text[o + 1u] == 'n'))
                            node.text += text[++o] == 'q' ? '"' : '\n';
                        else
                            node.text += text[o];
                    if (o++ >= text.size())
                        return false;
                }
                else
                {
                    const size_t end = std::min(text.find_first_of(kEnds, o), text.size());
                    const std::string word = text.substr(o, end - o);
                    o = end;
                    // A number has a digit: strtof alone takes "inf" and
                    // "nan" for one. strtoll, for a long is 32 bits on
                    // Windows.
                    const bool digits = std::any_of(word.begin(), word.end(),
                                                    [](unsigned char ch) { return std::isdigit(ch) != 0; });
                    char *rest = nullptr;
                    const long long integer = std::strtoll(word.c_str(), &rest, 0);
                    if (digits && *rest == '\0')
                        node = {kInt, static_cast<int32_t>(integer), 0.0f, {}, {}};
                    else if (const float real = std::strtof(word.c_str(), &rest); digits && *rest == '\0')
                        node = {kFloat, 0, real, {}, {}};
                    else if (word[0] == '$')
                        node = {kVar, 0, 0.0f, word.substr(1), {}};
                    else if (word == "TRUE" || word == "FALSE")
                        node = {kInt, word == "TRUE" ? 1 : 0, 0.0f, {}, {}};
                    else if (word == "kDataUnhandled")
                        node.type = kUnhandled;
                    else
                        node = {kSymbol, 0, 0.0f, word, {}};
                }
                out.push_back(std::move(node));
            }
        }
    }

    std::optional<Node> parse(const std::string &text)
    {
        Node root;
        root.type = kArray;
        size_t o = 0u;
        if (!parseNodes(text, o, 0, root.nodes))
            return std::nullopt;
        return root;
    }
}
