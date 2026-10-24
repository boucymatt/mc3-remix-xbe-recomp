// Opt-in: --mc3-car-cache=N (a multiple of 16, up to 256) grows the game's car resource cache from 16 entries.
//
// The cache ([0x50A08C], built by 0x103B40 in an 0x100-byte operator new) is 16 entries of 0x10 bytes: +0 vehicle
// type index (-1 free), +4 resource, +8 variant (_o, _g...), +9 references, +0xA load pending, +0xC its request
// object, one of a static table of 16 at 0x509F88 (bss, used by nothing but the constructor). Five methods walk it
// with a fixed bound of 16 and touch nothing else of the object: acquire 0x105B80, async preload 0x105CA0, release
// by name 0x103B80, release by resource 0x1057B0, unload all 0x105720. So the cache becomes N entries elsewhere, seen
// as N/16 windows of 16, and each method runs the game's own code on the right window: the one holding the entry it
// looks for, else the first with a free entry, else (unload) all of them. With 16 entries a scene holds at most 16
// distinct car models (the player's and the police's included); the 17th dereferences null in its Init. The same flag
// scales the per-car lists at [0x53890C] and [0x536704] by N/16 (see sub_0021F120 below) and opens windows on the one
// at [0x4D6308] (see sub_00033A00).

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <vector>

X86_EXTERN_FUNC(__imp__sub_00103B40);  // car resource cache constructor
X86_EXTERN_FUNC(__imp__sub_00105B80);  // acquire(name, ..., variant)
X86_EXTERN_FUNC(__imp__sub_00105CA0);  // async preload(name, ..., variant)
X86_EXTERN_FUNC(__imp__sub_00103B80);  // release by name(name, variant) -> bool
X86_EXTERN_FUNC(__imp__sub_001057B0);  // release by resource(resource)
X86_EXTERN_FUNC(__imp__sub_00105720);  // unload all
X86_EXTERN_FUNC(__imp__sub_0021F120);  // per-frame list init(capacity)
X86_EXTERN_FUNC(__imp__sub_0021E0B0);  // its neighbour's init(capacity)
X86_EXTERN_FUNC(sub_0026A6A0);         // vehicle-name registry index

namespace {

constexpr uint32_t kEntry = 0x10;
constexpr uint32_t kWindow = 16;  // the game's own bound

uint32_t GuestCall(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t ecx,
                   std::initializer_list<uint32_t> args) {
    uint32_t eax = ctx.eax, ecx0 = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0);  // return address: ret only pops it
    ctx.ecx = ecx;
    fn(ctx, base);
    uint32_t r = ctx.eax;
    ctx.eax = eax, ctx.ecx = ecx0, ctx.edx = edx, ctx.esp = esp;
    return r;
}

uint32_t Entries() {
    static const uint32_t n = [] {
        const wchar_t* v = Mc3DebugArg(L"--mc3-car-cache");
        const uint32_t parsed = v && *v ? uint32_t(std::wcstoul(v, nullptr, 10)) : 0;
        return parsed > kWindow ? std::min<uint32_t>((parsed + kWindow - 1) / kWindow * kWindow, 256) : 0u;
    }();
    return n;
}

// the cache object the game holds, and the larger table standing in for its 16 entries
struct Shadow {
    uint32_t object = 0;
    uint32_t table = 0;
};
Shadow g_shadows[8];

uint32_t TableOf(uint32_t object) {
    for (const Shadow& s : g_shadows)
        if (s.object == object && object) return s.table;
    return 0;
}

uint32_t Windows() { return Entries() / kWindow; }

// the first window whose entries satisfy test(entry), or UINT32_MAX
template <class Test>
uint32_t FindWindow(uint8_t* base, uint32_t table, Test&& test) {
    for (uint32_t i = 0; i < Entries(); ++i)
        if (test(table + i * kEntry)) return i / kWindow;
    return UINT32_MAX;
}

}  // namespace

X86_FUNC(sub_00103B40) {
    const uint32_t object = ctx.ecx;
    __imp__sub_00103B40(ctx, base);  // the object's own 16 entries, as before
    if (!Entries() || !object) return;
    uint32_t table = TableOf(object);
    Shadow* slot = nullptr;
    for (Shadow& s : g_shadows)
        if (s.object == object) slot = &s;
    if (!table) {
        for (Shadow& s : g_shadows)
            if (!s.object && !slot) slot = &s;
        if (!slot) slot = &g_shadows[0];  // a ninth live cache: reuse the oldest slot
        table = mem::AllocSystem(Entries() * kEntry, 16);
        const uint32_t requests = mem::AllocSystem((Entries() - kWindow) * kEntry, 16);
        if (!table || !requests) return;
        std::memset(base + requests, 0, (Entries() - kWindow) * kEntry);
        // the extra entries: free, each with a zeroed request object like the static ones
        for (uint32_t i = kWindow; i < Entries(); ++i) {
            const uint32_t e = table + i * kEntry;
            std::memset(base + e, 0, kEntry);
            X86_MEM32(e + 0) = 0xFFFFFFFFu;
            X86_MEM32(e + 0xC) = requests + (i - kWindow) * kEntry;
        }
        slot->object = object;
        slot->table = table;
    } else {
        for (uint32_t i = kWindow; i < Entries(); ++i) {  // a cache built again at the same address: all free
            const uint32_t e = table + i * kEntry;
            const uint32_t request = X86_MEM32(e + 0xC);
            std::memset(base + e, 0, kEntry);
            X86_MEM32(e + 0) = 0xFFFFFFFFu;
            X86_MEM32(e + 0xC) = request;
        }
    }
    std::memcpy(base + table, base + object, kWindow * kEntry);  // the first window: the game's own entries
    LOGE("MC3CARCACHE cache=%08X entries %u -> %u table=%08X", object, kWindow, Entries(), table);
}

namespace {

// acquire and async preload share their lookup: the entry for this model and variant if one is usable, else the
// first free entry. Arguments: name at [esp+4], variant byte at [esp+0x10]
uint32_t AcquireWindow(X86Context& ctx, uint8_t* base, uint32_t table) {
    const uint32_t name = X86_MEM32(ctx.esp + 4);
    const uint8_t variant = X86_MEM8(ctx.esp + 0x10);
    const uint32_t type = GuestCall(ctx, base, sub_0026A6A0, 0, {name});
    uint32_t w = FindWindow(base, table, [&](uint32_t e) {
        return X86_MEM32(e) == type && X86_MEM8(e + 8) == variant && (X86_MEM8(e + 0xA) || variant);
    });
    if (w == UINT32_MAX) w = FindWindow(base, table, [&](uint32_t e) { return X86_MEM32(e) == 0xFFFFFFFFu; });
    return w == UINT32_MAX ? 0 : w;
}

uint32_t Used(uint8_t* base, uint32_t table) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < Entries(); ++i) used += X86_MEM32(table + i * kEntry) != 0xFFFFFFFFu;
    return used;
}

}  // namespace

// The window is chosen before the game's code runs, and that code can take long enough for other cars to fill it: the
// player's car looks its name up (0x26A6A0) and the cruise traffic is built inside that call, before its own entry is
// taken. A null result then only means that window filled up, and nothing was changed: look again and retry; ret 0x18
// (and a load during the race gets the car shader factory: car_late_load.cpp)
bool Mc3CarLateLoadBegin(X86Context& ctx, uint8_t* base);
void Mc3CarLateLoadEnd(X86Context& ctx, uint8_t* base);

static void Acquire(X86Context& ctx, uint8_t* base);

X86_FUNC(sub_00105B80) {
    const bool bracket = Mc3CarLateLoadBegin(ctx, base);
    Acquire(ctx, base);
    if (bracket) Mc3CarLateLoadEnd(ctx, base);
}

static void Acquire(X86Context& ctx, uint8_t* base) {
    const uint32_t table = TableOf(ctx.ecx);
    if (!table) {
        __imp__sub_00105B80(ctx, base);
        return;
    }
    uint32_t args[6];
    for (uint32_t i = 0; i < 6; ++i) args[i] = X86_MEM32(ctx.esp + 4 + i * 4);
    uint32_t result = 0, w = 0;
    for (uint32_t attempt = 0; attempt < Windows() && !result; ++attempt) {
        w = AcquireWindow(ctx, base, table);
        result = GuestCall(ctx, base, __imp__sub_00105B80, table + w * kWindow * kEntry,
                           {args[0], args[1], args[2], args[3], args[4], args[5]});
        if (!result || attempt)
            LOGE("MC3CARCACHE acquire '%s' variant=%u window=%u attempt=%u used=%u/%u -> %08X", base + args[0],
                 args[3] & 0xFF, w, attempt, Used(base, table), Entries(), result);
    }
    ctx.eax = result;
    ctx.esp += 4 + 0x18;
}

X86_FUNC(sub_00105CA0) {
    if (const uint32_t table = TableOf(ctx.ecx)) ctx.ecx = table + AcquireWindow(ctx, base, table) * kWindow * kEntry;
    __imp__sub_00105CA0(ctx, base);
}

// The other per-car limit past the cache: the list at [0x53890C] ({array, capacity word, count word}, built by
// 0x21F120 with 188 slots from 0x44398) takes about four objects per car, appended by 0x21F2C0 with no bound, so the
// 48th car of a Cruise pool wrote past its 0x2F0-byte block into the next heap header. It grows with the cache; ret 4
X86_FUNC(sub_0021F120) {
    if (Entries() && X86_MEM32(ctx.esp + 4) == 0xBC) {
        X86_MEM32(ctx.esp + 4) = 0xBC * Windows();
        LOGE("MC3CARCACHE list=%08X capacity 188 -> %u", ctx.ecx, 0xBC * Windows());
    }
    __imp__sub_0021F120(ctx, base);
}

// Its neighbour [0x536704], same layout, built by 0x21E0B0 with 100 slots from 0x44381 and appended to by 0x21E190
// with no bound: about one per car, it overran past the 100th car of a Cruise pool. Grown the same way; ret 4
X86_FUNC(sub_0021E0B0) {
    if (Entries() && X86_MEM32(ctx.esp + 4) == 0x64) {
        X86_MEM32(ctx.esp + 4) = 0x64 * Windows();
        LOGE("MC3CARCACHE list=%08X capacity 100 -> %u", ctx.ecx, 0x64 * Windows());
    }
    __imp__sub_0021E0B0(ctx, base);
}

// release by name: the game's code on each window until one releases (al = 1); ret 8
X86_FUNC(sub_00103B80) {
    const uint32_t table = TableOf(ctx.ecx);
    if (!table) {
        __imp__sub_00103B80(ctx, base);
        return;
    }
    const uint32_t name = X86_MEM32(ctx.esp + 4), variant = X86_MEM32(ctx.esp + 8);
    uint32_t result = 0;
    for (uint32_t w = 0; w < Windows() && !(result & 0xFF); ++w)
        result = GuestCall(ctx, base, __imp__sub_00103B80, table + w * kWindow * kEntry, {name, variant});
    ctx.eax = (ctx.eax & 0xFFFFFF00u) | (result & 0xFF);
    ctx.esp += 12;
}

// release by resource: the window holding it; ret 4
X86_FUNC(sub_001057B0) {
    if (const uint32_t table = TableOf(ctx.ecx)) {
        const uint32_t resource = X86_MEM32(ctx.esp + 4);
        const uint32_t w = resource ? FindWindow(base, table, [&](uint32_t e) { return X86_MEM32(e + 4) == resource; })
                                    : UINT32_MAX;
        ctx.ecx = table + (w == UINT32_MAX ? 0 : w) * kWindow * kEntry;
    }
    __imp__sub_001057B0(ctx, base);
}

// unload all: every window; plain ret
X86_FUNC(sub_00105720) {
    const uint32_t table = TableOf(ctx.ecx);
    if (!table) {
        __imp__sub_00105720(ctx, base);
        return;
    }
    for (uint32_t w = 0; w < Windows(); ++w)
        GuestCall(ctx, base, __imp__sub_00105720, table + w * kWindow * kEntry, {});
    ctx.esp += 4;
}

// The third per-car limit: the list at [0x4D6308] (0x408 bytes from 0x337A0: +0 frame flag, +4 256 item pointers,
// +0x404 count), about four 0x38-byte items per car added by 0x33A00 / 0x339A0 / 0x33980 with no bound. The 60th
// car of a Cruise pool wrote its count over. Its methods touch only their own object, so with the cache flag it
// becomes windows of 256 like the cache: [0x4D6308] is window 0, the others are made as they are needed. Adds go to
// the first window with room, a remove to the window holding the item, the draw (0x337D0) runs on every window, and
// the update (0x34340) runs once on window 0, which advances its frame counter, then calls the items of the others.
X86_EXTERN_FUNC(__imp__sub_00033A00);  // add(a, b), ret 8
X86_EXTERN_FUNC(__imp__sub_000339A0);  // add(a, b), ret 8
X86_EXTERN_FUNC(__imp__sub_00033980);  // add(item), ret 4
X86_EXTERN_FUNC(__imp__sub_00033910);  // remove and delete(item), ret 4
X86_EXTERN_FUNC(__imp__sub_000337D0);  // draw
X86_EXTERN_FUNC(__imp__sub_00034340);  // update
X86_EXTERN_FUNC(__imp__sub_000342F0);  // delete all and the list

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target);

namespace {

constexpr uint32_t kListSlots = 256, kListCount = 0x404, kListSize = 0x408;

uint32_t g_list_owner = 0;  // the [0x4D6308] these windows belong to
std::vector<uint32_t> g_list_windows;

std::vector<uint32_t>& ListWindows(uint8_t* base) {
    const uint32_t owner = X86_MEM32(0x4D6308);
    if (owner != g_list_owner) {
        g_list_owner = owner;
        g_list_windows.clear();  // a new list: the old one's windows went with it (0x342F0)
    }
    return g_list_windows;
}

// window 0 if it has room, else the first other window with room, made if needed
uint32_t RoomyWindow(uint8_t* base, uint32_t list) {
    if (X86_MEM32(list + kListCount) < kListSlots) return list;
    for (uint32_t w : ListWindows(base))
        if (X86_MEM32(w + kListCount) < kListSlots) return w;
    const uint32_t w = mem::AllocSystem(kListSize, 16);
    if (!w) return list;
    std::memset(base + w, 0, kListSize);
    X86_MEM8(w) = X86_MEM8(list);
    ListWindows(base).push_back(w);
    LOGE("MC3CARCACHE list %08X full at %u: window %u at %08X", list, kListSlots, unsigned(g_list_windows.size()), w);
    return w;
}

uint32_t WindowHolding(uint8_t* base, uint32_t list, uint32_t item) {
    auto holds = [&](uint32_t w) {
        for (uint32_t i = 0; i < X86_MEM32(w + kListCount); ++i)
            if (X86_MEM32(w + 4 + i * 4) == item) return true;
        return false;
    };
    if (holds(list)) return list;
    for (uint32_t w : ListWindows(base))
        if (holds(w)) return w;
    return list;
}

void VirtualCall(X86Context& ctx, uint8_t* base, uint32_t object, uint32_t slot, std::initializer_list<uint32_t> args) {
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0);
    ctx.ecx = object;
    x86_icall(ctx, base, X86_MEM32(X86_MEM32(object) + slot * 4));
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx, ctx.esp = esp;
}

}  // namespace

X86_FUNC(sub_00033A00) {
    if (Entries() && ctx.ecx == X86_MEM32(0x4D6308)) ctx.ecx = RoomyWindow(base, ctx.ecx);
    __imp__sub_00033A00(ctx, base);
}

X86_FUNC(sub_000339A0) {
    if (Entries() && ctx.ecx == X86_MEM32(0x4D6308)) ctx.ecx = RoomyWindow(base, ctx.ecx);
    __imp__sub_000339A0(ctx, base);
}

X86_FUNC(sub_00033980) {
    if (Entries() && ctx.ecx == X86_MEM32(0x4D6308)) ctx.ecx = RoomyWindow(base, ctx.ecx);
    __imp__sub_00033980(ctx, base);
}

X86_FUNC(sub_00033910) {
    if (Entries() && ctx.ecx == X86_MEM32(0x4D6308) && !ListWindows(base).empty())
        ctx.ecx = WindowHolding(base, ctx.ecx, X86_MEM32(ctx.esp + 4));
    __imp__sub_00033910(ctx, base);
}

X86_FUNC(sub_000337D0) {
    const uint32_t list = ctx.ecx;
    __imp__sub_000337D0(ctx, base);
    if (!Entries() || list != X86_MEM32(0x4D6308)) return;
    for (uint32_t w : ListWindows(base)) GuestCall(ctx, base, __imp__sub_000337D0, w, {});
}

X86_FUNC(sub_00034340) {
    const uint32_t list = ctx.ecx;
    __imp__sub_00034340(ctx, base);
    if (!Entries() || list != X86_MEM32(0x4D6308)) return;
    for (uint32_t w : ListWindows(base)) {
        X86_MEM8(w) = X86_MEM8(list);  // this frame's flag, which the items read from their list
        for (uint32_t i = 0; i < X86_MEM32(w + kListCount); ++i) VirtualCall(ctx, base, X86_MEM32(w + 4 + i * 4), 2, {});
    }
}

X86_FUNC(sub_000342F0) {
    if (Entries()) {
        for (uint32_t w : ListWindows(base)) {
            for (uint32_t i = 0; i < X86_MEM32(w + kListCount); ++i)
                if (const uint32_t item = X86_MEM32(w + 4 + i * 4)) VirtualCall(ctx, base, item, 0, {1});
            mem::FreeSystem(w);
        }
        g_list_windows.clear();
    }
    __imp__sub_000342F0(ctx, base);
}

// Heavy traffic also reaches 0x2A5E10 (from 0x2A6060) with a contact whose object has left the physics level: its
// level index (word +8) is 0xFFFF, the level ([0x5580F0], 0x297AA0) answers null and [null+0x10] is read. Such a
// contact has nothing left to push; with the cache flag it is skipped. cdecl, plain ret
X86_EXTERN_FUNC(__imp__sub_002A5E10);

X86_FUNC(sub_002A5E10) {
    const uint32_t contact = X86_MEM32(ctx.esp + 4);
    if (Entries() && contact && X86_MEM16(contact + 8) == 0xFFFF) {
        static uint32_t logged = 0;
        if (logged++ < 8) LOGE("MC3CARCACHE contact %08X with an object out of the physics level: skipped", contact);
        ctx.esp += 4;
        return;
    }
    __imp__sub_002A5E10(ctx, base);
}

// And the level clear at Go to Menus (0xED8A0: walks each bucket's chain of 0x27C-byte object records, [level+0x40],
// next index word +0, instance +8, and deletes every instance through vtable +0x18) found, once in four 100-car
// runs, a record still chained with no instance: [null+4] read. Before the clear, with the cache flag, such
// records are unchained (their delete already happened) and logged to find who leaves them.
X86_EXTERN_FUNC(__imp__sub_000ED8A0);

X86_FUNC(sub_000ED8A0) {
    const uint32_t level = ctx.ecx;
    if (Entries() && level) {
        const uint32_t buckets = X86_MEM32(level + 0xE8), records = X86_MEM32(level + 0x40);
        const uint32_t count = X86_MEM16(level + 0xE4);
        for (uint32_t b = 1; b < count && buckets && records; ++b) {
            uint32_t link = buckets + b * 0xA;  // the word naming the next record
            for (uint32_t guard = 0; X86_MEM16(link) != 0xFFFF && guard < 0x10000; ++guard) {
                const uint32_t rec = records + X86_MEM16(link) * 0x27C;
                // the clear reads the instance at [+8], or at [+0x10] when [+8] names the record's own +0xC
                const uint32_t holder = X86_MEM32(rec + 8);
                const uint32_t instance = holder == rec + 0xC ? X86_MEM32(rec + 0x10) : holder;
                if (instance) {
                    link = rec;
                    continue;
                }
                LOGE("MC3CARCACHE level clear: bucket %u record %u has no instance, unchained (%08X %08X %08X %08X "
                     "%08X %08X)", b, X86_MEM16(link), X86_MEM32(rec), X86_MEM32(rec + 4), X86_MEM32(rec + 0xC),
                     X86_MEM32(rec + 0x10), X86_MEM32(rec + 0x14), X86_MEM32(rec + 0x18));
                X86_MEM16(link) = X86_MEM16(rec);
            }
        }
    }
    __imp__sub_000ED8A0(ctx, base);
}

// The physics level's moving objects: word +0x10 records (0x27C bytes each at +0x40; San Diego's level has 96, the
// city's own use few). From about the 92nd car of a 100-car pool a car found no record: its instance stayed out of
// the level (id 0xFFFF), the car sat at the origin and its contacts named no object. Init (0x2AC730) sums the four
// capacities (+0x2A, +0x10, +0x44, +0x54) into +0xE and allocates; with the cache flag the moving one grows by N/16
// first. thiscall, plain ret
X86_EXTERN_FUNC(__imp__sub_002AC730);

X86_FUNC(sub_002AC730) {
    const uint32_t level = ctx.ecx;
    if (Entries() && level) {
        const uint32_t moving = X86_MEM16(level + 0x10), grown = moving * Windows();
        const uint32_t total = X86_MEM16(level + 0x2A) + grown + X86_MEM16(level + 0x44) + X86_MEM16(level + 0x54);
        if (moving && total < 0x10000) X86_MEM16(level + 0x10) = uint16_t(grown);
        LOGE("MC3CARCACHE physics level=%08X moving records %u -> %u (others %u/%u/%u)", level, moving,
             X86_MEM16(level + 0x10), X86_MEM16(level + 0x2A), X86_MEM16(level + 0x44), X86_MEM16(level + 0x54));
    }
    __imp__sub_002AC730(ctx, base);
}
