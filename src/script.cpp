// A native command is a DataFunc: DataRegisterFunc (0x2b2c80) takes the
// name's Symbol and a guest function pointer, and a call jalr's there with
// a0 = the returned DataNode's slot and a1 = the DataArray of the call
// (DataPrint, 0x2b2fd8). The pointer is an address in .vutext
// (0x3d10e0-0x3d47f0): VU1 microcode, inside the function table's range but
// never entered by EE code, so its slots are free. Every command shares one
// native function there and is told apart by the pc it was called at.

#include "script.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstring>
#include <iostream>
#include <map>
#include <vector>

namespace gh2::script
{
    namespace
    {
        constexpr uint32_t kFirstSlot = 0x3d10e0u; // .vutext
        constexpr uint32_t kEndSlot = 0x3d47f0u;
        // Hmx::Object's vtable entry for SetTypeDef: a this adjustment (s16),
        // then the function (Hmx::Object::SetType, 0x3c8c80).
        constexpr uint32_t kSetTypeDefSlot = 0x58u;

        const Addresses *s_addresses = nullptr;

        struct Pending
        {
            std::string name;
            Command command;
        };
        std::vector<Pending> s_pending;
        std::map<uint32_t, Command> s_bySlot;
        struct UiScript
        {
            std::string text;
            bool again; // after each campaign switch too
            uint32_t root = 0u; // parsed, when run again
        };
        std::vector<UiScript> s_uiScripts;
        bool s_uiRan = false;
        bool s_registered = false;

        // A guest copy of `text`, from the game's heap.
        uint32_t guestString(const Call &call, const std::string &text, uint32_t prefix = 0u)
        {
            const uint32_t block = static_cast<uint32_t>(call.runtime->callGuestFunction(
                call.rdram, call.ctx, s_addresses->builtinNew, {prefix + static_cast<uint32_t>(text.size()) + 1u}));
            std::memcpy(getMemPtr(call.rdram, block + prefix), text.c_str(), text.size() + 1u);
            return block;
        }

        void dispatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t result = GPR_U32(ctx, 4);
            const auto it = s_bySlot.find(ctx->pc);
            Node node;
            if (it != s_bySlot.end())
                node = it->second(Call{rdram, ctx, runtime, GPR_U32(ctx, 5)});
            else
                std::cerr << "[script] no command at 0x" << std::hex << ctx->pc << std::dec << std::endl;
            store<uint32_t>(rdram, result, node.value);
            store<uint32_t>(rdram, result + 4u, node.type);
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = returnTo;
        }

        struct InitTag;
        // Guest calls from an entry hook clobber the hooked call's argument
        // registers, so both hooks put the context back.
        void onDataReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            uint32_t slot = kFirstSlot + 4u * static_cast<uint32_t>(s_bySlot.size());
            for (const Pending &p : s_pending)
            {
                if (slot >= kEndSlot)
                {
                    std::cerr << "[script] out of command slots at " << p.name << std::endl;
                    break;
                }
                const Call call{rdram, ctx, runtime, 0u};
                // Symbol::Symbol(this, const char*) interns the name into
                // *this; the block's first word is that Symbol.
                const uint32_t block = guestString(call, p.name, 4u);
                runtime->callGuestFunction(rdram, ctx, s_addresses->symbolCtor, {block, block + 4u});
                runtime->callGuestFunction(rdram, ctx, s_addresses->dataRegisterFunc,
                                           {load<uint32_t>(rdram, block), slot});
                runtime->callGuestFunction(rdram, ctx, s_addresses->builtinDelete, {block});
                runtime->registerFunction(slot, &dispatch);
                s_bySlot[slot] = p.command;
                slot += 4u;
            }
            s_pending.clear();
            if (!s_registered)
            {
                // Fader has a creator the game registers nowhere, so no
                // script could {new Fader}: NewObject (0x2c0e48) calls 0.
                const Call call{rdram, ctx, runtime, 0u};
                const uint32_t block = guestString(call, "Fader", 4u);
                runtime->callGuestFunction(rdram, ctx, s_addresses->symbolCtor, {block, block + 4u});
                runtime->callGuestFunction(rdram, ctx, s_addresses->registerFactory,
                                           {load<uint32_t>(rdram, block), s_addresses->faderNewObject});
                runtime->callGuestFunction(rdram, ctx, s_addresses->builtinDelete, {block});
            }
            s_registered = true;
            *ctx = saved;
        }

        // DebugModal(bool &fail, char *msg), the game's (0x105a88), which it
        // gives Debug as its modal callback: where a script error or failed
        // assert ends up, to wait on a button. A ship build shows it nowhere.
        struct DebugModalTag;
        void onDebugModal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t message = GPR_U32(ctx, 5);
            std::cerr << "[game] " << (load<uint8_t>(rdram, GPR_U32(ctx, 4)) ? "FAIL: " : "")
                      << (message ? reinterpret_cast<const char *>(getMemPtr(rdram, message)) : "") << std::endl;
        }

        // abort(), retail 0x307b80, which ends the thread through _Exit: its
        // caller, then words on the stack that point into code, nearest
        // first, as a rough backtrace.
        struct AbortTag;
        void onAbort(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::cerr << "[game] ABORT from 0x" << std::hex << GPR_U32(ctx, 31) << ", stack";
            const uint32_t sp = GPR_U32(ctx, 29);
            int shown = 0;
            for (uint32_t at = sp; at < sp + 0x800u && shown < 16; at += 4u)
            {
                const uint32_t word = load<uint32_t>(rdram, at);
                if (word >= 0x100000u && word < 0x3d0000u && (word & 3u) == 0u)
                {
                    std::cerr << " 0x" << word;
                    ++shown;
                }
            }
            std::cerr << std::dec << std::endl;
        }

        // DelayThread (0x2f7dd8), whose one caller is sceFsSifCallRpc
        // (0x2fb048) between tries of a file I/O call that failed: the only
        // thing to set a timer alarm. That call's number and caller are in
        // its frame (+0x14, +0xb0), its try count in $s2.
        struct DelayThreadTag;
        void onDelayThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t sp = GPR_U32(ctx, 29);
            std::cerr << "[game] file I/O call " << load<uint32_t>(rdram, sp + 0x14u) << " failed, try "
                      << GPR_U32(ctx, 18) << ", from 0x" << std::hex << load<uint32_t>(rdram, sp + 0xb0u) << std::dec
                      << std::endl;
        }

        void evaluate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t root)
        {
            const Call rootCall{rdram, ctx, runtime, root};
            for (int i = 0; i < rootCall.size(); ++i)
                rootCall.arg(i);
        }

        void runUi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, UiScript &script)
        {
            if (!script.again)
                return run(rdram, ctx, runtime, script.text);
            if (script.root == 0u)
            {
                const uint32_t parsed = parse(rdram, ctx, runtime, script.text);
                if (parsed == 0u)
                    return;
                script.root = park(rdram, runtime, parsed);
                release(rdram, ctx, runtime, parsed);
            }
            const uint32_t copy = clone(rdram, ctx, runtime, script.root);
            evaluate(rdram, ctx, runtime, copy);
            release(rdram, ctx, runtime, copy);
        }

        struct GotoScreenTag;
        void onGotoScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            readyUi(rdram, ctx, runtime);
        }

        // {retype <object> (<type def>)}: the object's handlers and
        // properties become the array's, as `{new Class name ...}` sets them.
        Node retype(const Call &call)
        {
            const uint32_t object = call.object(1);
            const Node def = call.arg(2);
            if (object == 0u || def.type != kArray)
            {
                std::cerr << "[script] retype: no such object, or no array" << std::endl;
                return {};
            }
            // Virtual, as Hmx::Object::SetType calls it, so a class's own
            // override (UIPanel's reads file and focus) runs.
            const uint32_t vtable = load<uint32_t>(call.rdram, object);
            const int16_t delta = load<int16_t>(call.rdram, vtable + kSetTypeDefSlot);
            const uint32_t function = load<uint32_t>(call.rdram, vtable + kSetTypeDefSlot + 4u);
            call.runtime->callGuestFunction(call.rdram, call.ctx, function,
                                            {object + static_cast<uint32_t>(static_cast<int32_t>(delta)), def.value});
            return {};
        }
    }

    Node floatNode(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return {bits, kFloat};
    }

    int Call::size() const
    {
        return load<int16_t>(rdram, args + 8u);
    }

    Node Call::arg(int i) const
    {
        const uint32_t node = load<uint32_t>(rdram, args) + 8u * static_cast<uint32_t>(i);
        const uint32_t evaluated =
            static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->dataNodeEvaluate, {node}));
        return {load<uint32_t>(rdram, evaluated), load<uint32_t>(rdram, evaluated + 4u)};
    }

    std::string Call::symbol(int i) const
    {
        const Node node = arg(i);
        if (node.type != kSymbol || node.value == 0u)
            return "";
        return reinterpret_cast<const char *>(getMemPtr(rdram, node.value));
    }

    float Call::number(int i) const
    {
        const Node node = arg(i);
        if (node.type == kInt)
            return static_cast<float>(static_cast<int32_t>(node.value));
        float value = 0.0f;
        if (node.type == kFloat)
            std::memcpy(&value, &node.value, sizeof(value));
        return value;
    }

    uint32_t Call::object(int i) const
    {
        const uint32_t node = load<uint32_t>(rdram, args) + 8u * static_cast<uint32_t>(i);
        return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->dataNodeGetObj, {node, args}));
    }

    void runWhenUiReady(std::string text)
    {
        s_uiScripts.push_back({std::move(text), false});
    }

    bool readyUi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // bootup_load comes up before GHUtl::Init, so wait for the
        // commands the scripts call.
        if (!s_registered)
            return false;
        if (s_uiRan)
            return true;
        s_uiRan = true;
        const R5900Context saved = *ctx;
        for (UiScript &script : s_uiScripts)
            runUi(rdram, ctx, runtime, script);
        *ctx = saved;
        return true;
    }

    void patchUi(std::string text)
    {
        s_uiScripts.push_back({std::move(text), true});
    }

    void patchUiAgain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        for (UiScript &script : s_uiScripts)
            if (script.again)
                runUi(rdram, ctx, runtime, script);
    }

    uint32_t clone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t array)
    {
        // Each node shared, then each array under it its own copy in turn.
        const uint32_t copy =
            static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->dataArrayClone, {array, 0u}));
        store<uint32_t>(rdram, copy + 4u, load<uint32_t>(rdram, array + 4u));
        store<int16_t>(rdram, copy + 0xcu, load<int16_t>(rdram, array + 0xcu));
        const int count = load<int16_t>(rdram, copy + 8u);
        for (int i = 0; i < count; ++i)
        {
            const uint32_t node = load<uint32_t>(rdram, copy) + 8u * static_cast<uint32_t>(i);
            const uint32_t type = load<uint32_t>(rdram, node + 4u);
            if (type != kArray && type != kCommand && type != kProperty)
                continue;
            const uint32_t shared = load<uint32_t>(rdram, node);
            store<uint32_t>(rdram, node, clone(rdram, ctx, runtime, shared));
            store<int16_t>(rdram, shared + 0xau, static_cast<int16_t>(load<int16_t>(rdram, shared + 0xau) - 1));
        }
        return copy;
    }

    namespace
    {
        // `array` and all under it copied to `at`, which moves past them:
        // each array, then its nodes or its text. With no `rdram` to write
        // to, only measured.
        uint32_t parkAt(uint8_t *from, uint8_t *rdram, uint32_t array, uint32_t type, uint32_t &at)
        {
            const auto take = [&at](uint32_t bytes)
            {
                const uint32_t block = at;
                at += (bytes + 7u) & ~7u;
                return block;
            };
            const uint32_t copy = take(0x10u);
            const uint32_t data = load<uint32_t>(from, array);
            const int count = load<int16_t>(from, array + 8u);
            // A string's array holds its text and counts no nodes
            // (DataArray::DataArray(const char *), 0x2b0760).
            const bool nodes = type == kArray || type == kCommand || type == kProperty;
            const uint32_t bytes = data == 0u ? 0u
                                   : nodes ? 8u * static_cast<uint32_t>(count > 0 ? count : 0)
                                   : count < 0 ? static_cast<uint32_t>(-count)
                                   : static_cast<uint32_t>(std::strlen(reinterpret_cast<const char *>(getMemPtr(from, data)))) + 1u;
            const uint32_t to = bytes ? take(bytes) : 0u;
            if (rdram)
            {
                std::memcpy(getMemPtr(rdram, copy), getMemPtr(from, array), 0x10u);
                store<int16_t>(rdram, copy + 0xau, 1);
                store<uint32_t>(rdram, copy, to);
                if (bytes)
                    std::memcpy(getMemPtr(rdram, to), getMemPtr(from, data), bytes);
            }
            for (int i = 0; nodes && bytes && i < count; ++i)
            {
                const uint32_t node = 8u * static_cast<uint32_t>(i);
                const uint32_t under = load<uint32_t>(from, data + node + 4u);
                if ((under & 0x10u) == 0u)
                    continue;
                const uint32_t kept = parkAt(from, rdram, load<uint32_t>(from, data + node), under, at);
                if (rdram)
                    store<uint32_t>(rdram, to + node, kept);
            }
            return copy;
        }
    }

    uint32_t park(uint8_t *rdram, PS2Runtime *runtime, uint32_t array, uint32_t type)
    {
        uint32_t bytes = 0u;
        parkAt(rdram, nullptr, array, type, bytes);
        uint32_t at = runtime->guestMalloc(bytes);
        if (at == 0u)
        {
            std::cerr << "[script] no memory above the game's heap for " << bytes << " bytes" << std::endl;
            std::abort();
        }
        return parkAt(rdram, rdram, array, type, at);
    }

    void release(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t array)
    {
        const int16_t held = static_cast<int16_t>(load<int16_t>(rdram, array + 0xau) - 1);
        store<int16_t>(rdram, array + 0xau, held);
        if (held == 0)
            runtime->callGuestFunction(rdram, ctx, s_addresses->dataArrayDtor, {array, 3u});
    }

    void addCommand(const char *name, Command command)
    {
        s_pending.push_back({name, command});
    }

    uint32_t parse(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text)
    {
        const Call call{rdram, ctx, runtime, 0u};
        const uint32_t source = guestString(call, text);
        const uint32_t root =
            static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->dataReadString, {source}));
        runtime->callGuestFunction(rdram, ctx, s_addresses->builtinDelete, {source});
        if (root == 0u)
            std::cerr << "[script] parse failed" << std::endl;
        return root;
    }

    void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text)
    {
        const uint32_t root = parse(rdram, ctx, runtime, text);
        if (root == 0u)
            return;
        evaluate(rdram, ctx, runtime, root);
        release(rdram, ctx, runtime, root);
    }

    uint32_t symbol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text)
    {
        const Call call{rdram, ctx, runtime, 0u};
        const uint32_t block = guestString(call, text, 4u);
        runtime->callGuestFunction(rdram, ctx, s_addresses->symbolCtor, {block, block + 4u});
        const uint32_t interned = load<uint32_t>(rdram, block);
        runtime->callGuestFunction(rdram, ctx, s_addresses->builtinDelete, {block});
        return interned;
    }

    void setVariable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *name, Node node)
    {
        const uint32_t variable = static_cast<uint32_t>(runtime->callGuestFunction(
            rdram, ctx, s_addresses->dataVariable, {symbol(rdram, ctx, runtime, name)}));
        // Raw, as a DataNode's own assignment would take a reference.
        store<uint32_t>(rdram, variable, node.value);
        store<uint32_t>(rdram, variable + 4u, node.type);
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addCommand("retype", retype);
        EntryHook<InitTag>::install(runtime, addresses.ghUtlInit, onDataReady);
        EntryHook<GotoScreenTag>::install(runtime, addresses.uiGotoScreen, onGotoScreen);
        EntryHook<DebugModalTag>::install(runtime, addresses.debugModal, onDebugModal);
        EntryHook<AbortTag>::install(runtime, addresses.abort, onAbort);
        EntryHook<DelayThreadTag>::install(runtime, addresses.delayThread, onDelayThread);
    }
}
