// Opt-in with --mc3-traffic-cruise-late-models: a car model loaded for the first time during the race.
//
// Car resources page in their shaders through the current shader factory, [0x5077B8]. While a city loads (state 7) it
// is the cars' own ([0x52B494]+0x18, vtable 0x468B68); during the race the standard one is current (vtable 0x45377C),
// and the car resource handler (vtable 0x4554EC) only puts the cars' back for each frame's car draws (Begin 0x109ED0 /
// End 0x109F40 from 0x92A68 / 0x92A78, which also set the draw phase [0x4B0D74] to 2). So a car the Cruise pool
// acquires during the race (0x105B80 from the live-lane build) reached the standard factory's ResourcePageIn (0xDFB40),
// read its shader records as the standard kind, found type 42 and stopped the game on "Invalid type %d in
// rmcShaderFactoryStandard::ResourcePageIn". Bracketing it with Begin/End fixed the shaders but left the draw phase at
// 2 through the load, and such a car came without its kit and wheels: its LODs never asked the part loader (0x1BD8F0)
// for a single .mesh. A city-load acquire runs at phase 3 with the cars' factory and nothing else changed (traced with
// --mc3-trace-resources), so only the factory is swapped here, for the acquire, and given back after it.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "kernel/memory.h"

#include <cstdint>

namespace {

uint32_t g_saved_factory = 0;

}  // namespace

// true when it put the cars' factory in, which Mc3CarLateLoadEnd then takes out
bool Mc3CarLateLoadBegin(X86Context& ctx, uint8_t* base) {
    static const bool on = Mc3DebugHasArg(L"--mc3-traffic-cruise-late-models");
    if (!on) return false;
    // during the race only (game state [[0x557ED0]+4] == 2): a city load's acquires, the player's among them, need none
    const uint32_t state_object = X86_MEM32(0x557ED0), handler_ctx = X86_MEM32(0x52B494);
    if (!state_object || X86_MEM32(state_object + 4) != 2 || !handler_ctx) return false;
    const uint32_t cars = X86_MEM32(handler_ctx + 0x18), current = X86_MEM32(0x5077B8);
    if (!cars || current == cars) return false;
    g_saved_factory = current;
    X86_MEM32(0x5077B8) = cars;
    static uint32_t logged = 0;
    if (logged++ < 8) LOGE("MC3LATE car load during the race: shader factory %08X -> %08X for it", current, cars);
    return true;
}

void Mc3CarLateLoadEnd(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    X86_MEM32(0x5077B8) = g_saved_factory;
}

// Diagnostic, --mc3-trace-resources: every paged resource the game loads by name (0x2105F0, its hook in mc2_city.cpp
// calls here), with the game state; shows what a car needs besides its body.
void Mc3TraceResource(const char* how, X86Context& ctx, uint8_t* base) {
    static const bool on = Mc3DebugHasArg(L"--mc3-trace-resources");
    if (!on) return;
    const uint32_t name = X86_MEM32(ctx.esp + 4), state_object = X86_MEM32(0x557ED0);
    const uint32_t state = state_object ? X86_MEM32(state_object + 4) : 0xFFFFFFFFu;
    if (name >= 0x10000 && name < 0xF0000000u)
        LOGE("MC3RES %s state=%u phase=%u factory=%08X g668=%08X g8d8=%08X from=%08X '%.120s'", how, state,
             X86_MEM32(0x4B0D74), X86_MEM32(0x5077B8), X86_MEM32(0x507668), X86_MEM32(0x5078D8), X86_MEM32(ctx.esp),
             reinterpret_cast<const char*>(base + name));
}

// The same for the car's parts. Its geometry (0x109D40: part loader [0x5319E4] given the car's folder and kit mask,
// then its LODs loaded by 0xE6D00) has each LOD ask the current mesh loader [0x507668] for its kit and wheel .mesh
// files (0xE90D0). While a city loads that is the part loader; during the race it is another one (the world's), which
// loads no car part, so a late car came without bumpers, kit and wheels. With the flag, during the race, the part
// loader is put in for the call and the other given back after it.
X86_EXTERN_FUNC(__imp__sub_00109D40);

X86_FUNC(sub_00109D40) {
    static const bool on = Mc3DebugHasArg(L"--mc3-traffic-cruise-late-models");
    static const bool trace = Mc3DebugHasArg(L"--mc3-trace-resources");
    const uint32_t state_object = X86_MEM32(0x557ED0), parts = X86_MEM32(0x5319E4), current = X86_MEM32(0x507668);
    const bool racing = state_object && X86_MEM32(state_object + 4) == 2;
    const bool swap = on && racing && parts && current != parts;
    if (trace) {
        const uint32_t path = X86_MEM32(ctx.ecx + 0xC);
        LOGE("MC3RES geometry racing=%u mesh loader %08X%s '%.60s'", unsigned(racing), current,
             swap ? " -> part loader" : "", path >= 0x10000 ? reinterpret_cast<const char*>(base + path) : "");
    }
    if (swap) X86_MEM32(0x507668) = parts;
    __imp__sub_00109D40(ctx, base);
    if (swap) X86_MEM32(0x507668) = current;
}

X86_EXTERN_FUNC(__imp__sub_001BD8F0);  // part loader LoadPart(name, index), ret 8: skipped unless the mask has the bit

X86_FUNC(sub_001BD8F0) {
    static const bool on = Mc3DebugHasArg(L"--mc3-trace-resources");
    const uint32_t loader = ctx.ecx, name = X86_MEM32(ctx.esp + 4), index = X86_MEM32(ctx.esp + 8);
    __imp__sub_001BD8F0(ctx, base);
    if (on) {
        const uint32_t mask = X86_MEM32(loader + 8), bits = mask ? X86_MEM32(mask) : 0;
        const uint32_t word = bits ? X86_MEM32(bits + (index >> 5) * 4) : 0;
        LOGE("MC3RES part index=%u bit=%u -> %08X '%.80s'", index, (word >> (index & 31)) & 1, ctx.eax,
             name >= 0x80000000u && name < mem::PhysicalEnd() ? reinterpret_cast<const char*>(base + name) : "");
    }
}

X86_EXTERN_FUNC(__imp__sub_00109ED0);
X86_EXTERN_FUNC(__imp__sub_00109F40);

X86_FUNC(sub_00109ED0) {
    static const bool on = Mc3DebugHasArg(L"--mc3-trace-resources");
    if (on) LOGE("MC3RES begin from=%08X factory=%08X", X86_MEM32(ctx.esp), X86_MEM32(0x5077B8));
    __imp__sub_00109ED0(ctx, base);
}

X86_FUNC(sub_00109F40) {
    static const bool on = Mc3DebugHasArg(L"--mc3-trace-resources");
    if (on) LOGE("MC3RES end from=%08X", X86_MEM32(ctx.esp));
    __imp__sub_00109F40(ctx, base);
}
