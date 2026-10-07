#include "gh1/menu_scripts.h"

#include "dta.h"

#include <iostream>
#include <optional>
#include <set>
#include <vector>

namespace gh2::gh1
{
    namespace
    {
        // Every {new Class name ...} under `node` that is `name`'s.
        void definitions(dtb::Node &node, const std::string &name, std::vector<dtb::Node *> &out)
        {
            if (node.type == dtb::kCommand && node.nodes.size() > 2u && node.nodes[0].text == "new" &&
                node.nodes[2].text == name)
                out.push_back(&node);
            for (dtb::Node &n : node.nodes)
                definitions(n, name, out);
        }
    }

    bool fitScript(dtb::Node &file, const std::set<std::string> &scenes)
    {
        static const std::optional<dtb::Node> edits = dtb::parse(kGh1MenusDta);
        if (!edits)
        {
            std::cerr << "[gh1] cannot read menus.dta" << std::endl;
            return false;
        }
        bool fitted = false;
        for (const dtb::Node &edit : edits->nodes)
        {
            // An edit is for GH1's scenes: without one it needs, GH2's is
            // there, which has none of the objects the edit names.
            bool there = edit.type == dtb::kArray && !edit.nodes.empty();
            if (const dtb::Node *needs = there ? dtb::find(edit, "needs") : nullptr)
                for (size_t i = 1u; i < needs->nodes.size(); ++i)
                    there = there && scenes.count(needs->nodes[i].text) != 0u;
            std::vector<dtb::Node *> found;
            if (there)
                definitions(file, edit.nodes[0].text, found);
            for (dtb::Node *definition : found)
                for (size_t i = 1u; i < edit.nodes.size(); ++i)
                {
                    const dtb::Node &handler = edit.nodes[i];
                    if (handler.type != dtb::kArray || handler.nodes.size() < 2u || handler.nodes[0].text == "needs")
                        continue;
                    const bool whole = handler.nodes[0].text == "=";
                    const std::string &key = handler.nodes[whole ? 1u : 0u].text;
                    dtb::Node *own = nullptr;
                    for (dtb::Node &n : definition->nodes)
                        if (n.type == dtb::kArray && !n.nodes.empty() && n.nodes[0].text == key)
                            own = &n;
                    if (!own)
                    {
                        // A handler can be a macro's (SEL_GUITAR_COMMON), which
                        // other screens share: this one gets its own, ahead of
                        // the macro's, where a lookup ends.
                        dtb::Node made{dtb::kArray, 0, 0.0f, {}, {handler.nodes[whole ? 1u : 0u]}};
                        for (const dtb::Node &n : definition->nodes)
                            for (size_t d = 0; n.type == dtb::kSymbol && d + 1u < file.nodes.size(); ++d)
                                if (file.nodes[d].type == dtb::kDefine && file.nodes[d].text == n.text)
                                    if (const dtb::Node *shared = dtb::find(file.nodes[d + 1u], key))
                                        made = *shared;
                        own = &*definition->nodes.insert(definition->nodes.begin() + 3, std::move(made));
                    }
                    if (whole)
                        own->nodes.resize(1u);
                    own->nodes.insert(own->nodes.end(), handler.nodes.begin() + (whole ? 2 : 1), handler.nodes.end());
                    fitted = true;
                }
        }
        return fitted;
    }
}
