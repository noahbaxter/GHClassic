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
        std::vector<const char *> s_uiScripts;
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

        struct GotoScreenTag;
        void onGotoScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // bootup_load comes up before GHUtl::Init, so wait for the
            // commands the scripts call.
            if (!s_registered || s_uiScripts.empty())
                return;
            const R5900Context saved = *ctx;
            for (const char *text : s_uiScripts)
                run(rdram, ctx, runtime, text);
            s_uiScripts.clear();
            *ctx = saved;
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

    void runWhenUiReady(const char *text)
    {
        s_uiScripts.push_back(text);
    }

    void addCommand(const char *name, Command command)
    {
        s_pending.push_back({name, command});
    }

    void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text)
    {
        const Call call{rdram, ctx, runtime, 0u};
        const uint32_t source = guestString(call, text);
        // The parsed array is kept: handlers defined in it stay referenced.
        const uint32_t root =
            static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->dataReadString, {source}));
        runtime->callGuestFunction(rdram, ctx, s_addresses->builtinDelete, {source});
        if (root == 0u)
        {
            std::cerr << "[script] parse failed" << std::endl;
            return;
        }
        const Call rootCall{rdram, ctx, runtime, root};
        for (int i = 0; i < rootCall.size(); ++i)
            rootCall.arg(i);
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addCommand("retype", retype);
        EntryHook<InitTag>::install(runtime, addresses.ghUtlInit, onDataReady);
        EntryHook<GotoScreenTag>::install(runtime, addresses.uiGotoScreen, onGotoScreen);
        EntryHook<DebugModalTag>::install(runtime, addresses.debugModal, onDebugModal);
        EntryHook<AbortTag>::install(runtime, addresses.abort, onAbort);
    }
}
