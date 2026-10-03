// A GH1 .dtb is encrypted whole after a 4-byte seed, then a version byte and
// the root array. Arrays are a u16 size, line and id, then their nodes; a
// node is a u32 type and its value. Directives are nodes too, a #define's
// body the array after it.

#include "gh1/dtb.h"

#include <array>
#include <cstring>

namespace gh2
{
    namespace
    {
        using gh1::dtb::Macros;
        using gh1::dtb::Node;
        using milo::Bytes;

        // BinStream::EnableReadEncryption (GH1 0x2407e8) seeds Rand
        // (0x265940) with the first word; Read (0x2408d0) XORs each byte
        // with Rand::Int (0x265aa0).
        Bytes decrypt(const Bytes &file)
        {
            uint32_t seed = milo::u32(file, 0u);
            std::array<uint32_t, 256> table;
            for (uint32_t &t : table)
            {
                const uint32_t a = seed * 0x41c64e6du + 0x3039u;
                const uint32_t b = a * 0x41c64e6du + 0x3039u;
                t = (a >> 16) | (b & 0x7fff0000u);
                seed = b;
            }
            Bytes out;
            out.reserve(file.size() > 4u ? file.size() - 4u : 0u);
            size_t i = 0u, j = 0x67u;
            for (size_t o = 4u; o < file.size(); ++o)
            {
                table[i] ^= table[j];
                out.push_back(static_cast<uint8_t>(file[o] ^ (table[i] & 0xffu)));
                i = i + 1u < 0xf9u ? i + 1u : 0u;
                j = j + 1u < 0xf9u ? j + 1u : 0u;
            }
            return out;
        }

        struct Reader
        {
            const Bytes &b;
            size_t o = 0u;

            uint16_t u16()
            {
                uint16_t v = 0u;
                if (o + 2u <= b.size())
                    std::memcpy(&v, b.data() + o, 2u);
                o += 2u;
                return v;
            }

            uint32_t u32()
            {
                const uint32_t v = milo::u32(b, o);
                o += 4u;
                return v;
            }

            std::optional<Node> array(gh1::dtb::Type type)
            {
                Node node{type};
                const uint16_t size = u16();
                o += 4u; // line, id
                for (uint16_t i = 0; i < size; ++i)
                {
                    auto n = this->node();
                    if (!n)
                        return std::nullopt;
                    node.nodes.push_back(std::move(*n));
                }
                return node;
            }

            std::optional<Node> node()
            {
                if (o + 4u > b.size())
                    return std::nullopt;
                using namespace gh1::dtb;
                const auto type = static_cast<Type>(u32());
                Node n{type};
                switch (type)
                {
                case kArray:
                case kCommand:
                case kProperty:
                    return array(type);
                case kInt:
                    n.integer = static_cast<int32_t>(u32());
                    break;
                case kFloat:
                {
                    const uint32_t v = u32();
                    std::memcpy(&n.real, &v, 4u);
                    break;
                }
                case kVar:
                case kSymbol:
                case kString:
                case kIfdef:
                case kDefine:
                case kInclude:
                case kMerge:
                case kIfndef:
                case kAutorun:
                case kUndef:
                    n.text = milo::str(b, o);
                    break;
                case kUnhandled:
                case kElse:
                case kEndif:
                    u32();
                    break;
                default:
                    return std::nullopt;
                }
                return n;
            }
        };

        // A script's name (charsys/hair_anims.dta) to its built file
        // (charsys/gen/hair_anims.dtb), with "dir/../" folded away.
        std::string built(const std::string &script)
        {
            std::vector<std::string> parts;
            for (size_t start = 0u; start <= script.size();)
            {
                size_t end = script.find('/', start);
                if (end == std::string::npos)
                    end = script.size();
                const std::string part = script.substr(start, end - start);
                if (part == ".." && !parts.empty() && parts.back() != "..")
                    parts.pop_back();
                else if (!part.empty() && part != ".")
                    parts.push_back(part);
                start = end + 1u;
            }
            std::string path;
            for (size_t i = 0; i + 1u < parts.size(); ++i)
                path += parts[i] + "/";
            std::string name = parts.empty() ? std::string() : parts.back();
            name = name.substr(0, name.rfind('.'));
            return path + "gen/" + name + ".dtb";
        }

        std::string directory(const std::string &script)
        {
            const size_t slash = script.rfind('/');
            return slash == std::string::npos ? std::string() : script.substr(0, slash + 1u);
        }

        struct Load
        {
            Macros &macros;
            const gh1::dtb::Files &files;
            std::vector<bool> conditions;
        };

        std::optional<std::vector<Node>> readFile(const std::string &script, Load &load);

        // DataArray::Load's directives over one array's nodes, in `script`.
        // The conditionals are one stack across arrays, as GH1's is global.
        std::vector<Node> apply(const std::vector<Node> &in, Load &load, const std::string &script)
        {
            using namespace gh1::dtb;
            Macros &macros = load.macros;
            std::vector<bool> &conditions = load.conditions;
            std::vector<Node> out;
            const auto active = [&] {
                for (const bool c : conditions)
                    if (!c)
                        return false;
                return true;
            };
            for (size_t i = 0; i < in.size(); ++i)
            {
                const Node &n = in[i];
                switch (n.type)
                {
                case kIfdef:
                    conditions.push_back(macros.count(n.text) != 0u);
                    continue;
                case kIfndef:
                    conditions.push_back(macros.count(n.text) == 0u);
                    continue;
                case kElse:
                    if (!conditions.empty())
                        conditions.back() = !conditions.back();
                    continue;
                case kEndif:
                    if (!conditions.empty())
                        conditions.pop_back();
                    continue;
                default:
                    break;
                }
                if (!active())
                    continue;
                if (n.type == kDefine)
                {
                    if (i + 1u < in.size())
                        macros[n.text] = apply(in[++i].nodes, load, script);
                }
                else if (n.type == kUndef)
                    macros.erase(n.text);
                else if (n.type == kInclude)
                {
                    // Spliced in, its path next to this script's
                    // (FileMakePath(FileGetPath(...)), GH1 0x245568).
                    if (auto nodes = readFile(directory(script) + n.text, load))
                        out.insert(out.end(), nodes->begin(), nodes->end());
                }
                else if (n.type == kSymbol && macros.count(n.text))
                {
                    const std::vector<Node> &body = macros.at(n.text);
                    out.insert(out.end(), body.begin(), body.end());
                }
                else if (n.type == kArray || n.type == kCommand || n.type == kProperty)
                {
                    Node array{n.type};
                    array.nodes = apply(n.nodes, load, script);
                    out.push_back(std::move(array));
                }
                else if (n.type != kMerge && n.type != kAutorun)
                    out.push_back(n);
            }
            return out;
        }

        std::optional<std::vector<Node>> readFile(const std::string &script, Load &load)
        {
            const auto file = load.files(built(script));
            if (!file)
                return std::nullopt;
            const Bytes plain = decrypt(*file);
            if (plain.empty() || plain[0] != 1u)
                return std::nullopt;
            Reader reader{plain, 1u};
            auto root = reader.array(gh1::dtb::kArray);
            if (!root)
                return std::nullopt;
            return apply(root->nodes, load, script);
        }
    }

    namespace gh1::dtb
    {
        std::optional<Node> read(const std::string &script, Macros &macros, const Files &files)
        {
            Load load{macros, files, {}};
            auto nodes = readFile(script, load);
            if (!nodes)
                return std::nullopt;
            return Node{kArray, 0, 0.0f, {}, std::move(*nodes)};
        }

        const Node *find(const Node &array, const std::string &key)
        {
            for (const Node &n : array.nodes)
                if (n.type == kArray && !n.nodes.empty() && n.nodes[0].type == kSymbol && n.nodes[0].text == key)
                    return &n;
            return nullptr;
        }

        const Node *find(const Node &array, int32_t key)
        {
            for (const Node &n : array.nodes)
                if (n.type == kArray && !n.nodes.empty() && n.nodes[0].type == kInt && n.nodes[0].integer == key)
                    return &n;
            return nullptr;
        }

        std::optional<float> number(const Node &node)
        {
            if (node.type == kInt)
                return static_cast<float>(node.integer);
            if (node.type == kFloat)
                return node.real;
            return std::nullopt;
        }
    }
}
