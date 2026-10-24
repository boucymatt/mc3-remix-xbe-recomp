// Opt-in: --mc3-traffic-ambient-scale=X multiplies the city's pool of ambient (va_) traffic cars.
//
// aiAmbientVehicleInstances (update 0x6CC90) keeps every car of the pool in one array made up front: [+0x130]
// objects of 0x21C bytes (the count also in the dword before them), word [+0x12C] how many. The traffic director never
// drives more than that, whatever the density: San Diego's pool is 48, which with the Cruise pool's 18 spawn cars
// leaves 30 live lanes for full vp_ cars.
//
// Two ways the pool is made:
// * built at run time by 0x6C840 from the city's ambient list, which 0x6BD20 loads through 0x807A0 into +0x38 (0x68-
//   byte entries: +0 cars of this type, +4 its va_ name; no list = 300 va_civic_sh). Each entry is scaled after the load.
// * restored with the city's traffic resource (Cruise in San Diego): the array and, per car type, a list of its
//   members ({pointer, count word, capacity word} in the type) arrive already made. Before any car is active the pool
//   is rebuilt larger: the originals and, per type, copies of its own idle members, with their pointers into the old
//   array moved to the new one; then the four lists and +0x130/+0x12C are repointed. A scan of the 128 MB found no
//   other pointer to the start of a car (the rest are floats that happen to fall in range).

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "kernel/memory.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

X86_EXTERN_FUNC(__imp__sub_000807A0);  // ambient list load(arg, city table entry), ret 8
X86_EXTERN_FUNC(__imp__sub_0006CC90);  // aiAmbientVehicleInstances update

namespace {

constexpr uint32_t kCar = 0x21C;

float Scale() {
    static const float scale = [] {
        const wchar_t* v = Mc3DebugArg(L"--mc3-traffic-ambient-scale");
        const float parsed = v && *v ? std::wcstof(v, nullptr) : 1.0f;
        return parsed > 1.0f && parsed <= 16.0f ? parsed : 1.0f;
    }();
    return scale;
}

uint32_t Scaled(uint32_t n) { return uint32_t(float(n) * Scale() + 0.5f); }

// arrays this file made, so a later update does not take them for a fresh pool, and managers whose list it scaled
std::vector<uint32_t> g_rebuilt, g_scaled_managers;

struct TypeList {
    uint32_t holder = 0;  // {pointer, count word, capacity word}
    uint32_t list = 0;
    uint32_t count = 0;
};

// pointers to the start of a car, outside the pool and the manager's own +0x130, in runs: the per-type lists
std::vector<TypeList> FindTypeLists(uint8_t* base, uint32_t manager, uint32_t objects, uint32_t count) {
    const uint32_t end = objects + count * kCar;
    std::vector<TypeList> lists;
    uint32_t run_start = 0, run_next = 0;
    auto close_run = [&] {
        if (run_start) lists.push_back({0, run_start, (run_next - run_start) / 4});
        run_start = 0;
    };
    for (uint32_t page = mem::kContiguousBase; page < mem::PhysicalEnd(); page += mem::kPage) {
        if (!mem::IsCommitted(page)) continue;
        for (uint32_t va = page; va < page + mem::kPage; va += 4) {
            if ((va >= objects - 4 && va < end) || va == manager + 0x130) continue;
            const uint32_t v = X86_MEM32(va);
            if (v < objects || v >= end || (v - objects) % kCar) continue;
            if (va != run_next) close_run(), run_start = va;
            run_next = va + 4;
        }
    }
    close_run();
    for (TypeList& t : lists)
        for (uint32_t page = mem::kContiguousBase; page < mem::PhysicalEnd() && !t.holder; page += mem::kPage) {
            if (!mem::IsCommitted(page)) continue;
            for (uint32_t va = page; va < page + mem::kPage - 8; va += 4)
                if (X86_MEM32(va) == t.list && X86_MEM16(va + 4) == t.count && X86_MEM16(va + 6) == t.count) {
                    t.holder = va;
                    break;
                }
        }
    return lists;
}

// a copy of the car at `from` to `to`; pointers into the old pool follow the car they named, its own fields included
void CopyCar(uint8_t* base, uint32_t from, uint32_t to, uint32_t old_objects, uint32_t old_end, uint32_t new_objects) {
    std::memcpy(base + to, base + from, kCar);
    for (uint32_t off = 0; off < kCar; off += 4) {
        const uint32_t v = X86_MEM32(to + off);
        if (v < old_objects || v >= old_end) continue;
        X86_MEM32(to + off) = v >= from && v < from + kCar ? to + (v - from) : new_objects + (v - old_objects);
    }
}

void Rebuild(uint8_t* base, uint32_t manager) {
    const uint32_t objects = X86_MEM32(manager + 0x130), count = X86_MEM16(manager + 0x12C);
    const uint32_t end = objects + count * kCar;
    uint32_t active = 0;
    for (uint32_t i = 0; i < count; ++i) active += X86_MEM16(objects + i * kCar + 0x2C) != 0xFFFF;
    if (active) {  // a car already on the road: too late to move them
        LOGE("MC3TRAFFIC ambient pool %08X: %u of %u cars active, left as is", objects, active, count);
        return;
    }
    std::vector<TypeList> lists = FindTypeLists(base, manager, objects, count);
    // a run with no holder is a float that happens to equal a car's address
    std::erase_if(lists, [](const TypeList& t) { return !t.holder; });
    uint32_t listed = 0, total = 0;
    for (const TypeList& t : lists) listed += t.count, total += Scaled(t.count);
    if (listed != count || total > 0x7FFF) {
        LOGE("MC3TRAFFIC ambient pool %08X: lists hold %u of %u cars, left as is", objects, listed, count);
        return;
    }
    const uint32_t block = mem::AllocSystem(total * kCar + 16, 16);
    if (!block) return;
    const uint32_t fresh = block + 4;
    X86_MEM32(block) = total;
    for (uint32_t i = 0; i < count; ++i) CopyCar(base, objects + i * kCar, fresh + i * kCar, objects, end, fresh);
    uint32_t next = count;
    for (TypeList& t : lists) {
        const uint32_t grown = Scaled(t.count), list = mem::AllocSystem(grown * 4, 16);
        for (uint32_t k = 0; k < grown; ++k) {
            const uint32_t member = X86_MEM32(t.list + (k % t.count) * 4);
            uint32_t car = fresh + (member - objects);
            if (k >= t.count) {
                car = fresh + next++ * kCar;
                CopyCar(base, member, car, objects, end, fresh);
            }
            X86_MEM32(list + k * 4) = car;
        }
        X86_MEM32(t.holder) = list;
        X86_MEM16(t.holder + 4) = uint16_t(grown);
        X86_MEM16(t.holder + 6) = uint16_t(grown);
        LOGE("MC3TRAFFIC ambient pool type list holder=%08X cars %u -> %u", t.holder, t.count, grown);
    }
    X86_MEM32(manager + 0x130) = fresh;
    X86_MEM16(manager + 0x12C) = uint16_t(total);
    g_rebuilt.push_back(fresh);
    LOGE("MC3TRAFFIC ambient pool manager=%08X cars %u -> %u objects %08X -> %08X", manager, count, total, objects,
         fresh);
}

}  // namespace

X86_FUNC(sub_000807A0) {
    const uint32_t list = ctx.ecx;
    __imp__sub_000807A0(ctx, base);
    if (Scale() <= 1.0f || !list) return;
    const uint32_t entries = X86_MEM32(list + 4), count = X86_MEM16(list + 8);
    uint32_t before = 0, after = 0;
    for (uint32_t i = 0; i < count && entries; ++i) {
        const uint32_t entry = entries + i * 0x68;
        const int32_t cars = int32_t(X86_MEM32(entry));
        if (cars <= 0) continue;
        X86_MEM32(entry) = Scaled(uint32_t(cars));
        before += uint32_t(cars), after += Scaled(uint32_t(cars));
    }
    g_scaled_managers.push_back(list - 0x38);
    LOGE("MC3TRAFFIC ambient pool types=%u cars %u -> %u (x%.2f)", count, before, after, Scale());
}

X86_FUNC(sub_0006CC90) {
    static uint32_t seen = 0;
    const uint32_t manager = ctx.ecx, objects = X86_MEM32(manager + 0x130);
    if (Scale() > 1.0f && objects && objects != seen && X86_MEM16(manager + 0x12C)) {
        seen = objects;
        bool ours = false;
        for (uint32_t made : g_rebuilt) ours |= made == objects;
        for (uint32_t scaled : g_scaled_managers) ours |= scaled == manager;
        if (!ours) Rebuild(base, manager);
        seen = X86_MEM32(manager + 0x130);
    }
    __imp__sub_0006CC90(ctx, base);
}
