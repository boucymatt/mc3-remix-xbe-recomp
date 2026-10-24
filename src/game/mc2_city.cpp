// Los Angeles from Midnight Club II drawn by the recomp where MC3 draws its city. Nothing of MC2 ships with the
// recomp: the original PC files are read at run time from the folder MC3_MC2_DIR names (MC2's assets_p, holding
// city/losangeles/models/*.xmod, city/losangeles/*.hood and texture_x/*.tex; the lighting, city/losangeles/<time>/
// losangeles_<time>_cpvs.dat, sits beside assets_p in the game's folder). Drawn with its own pipeline through
// gpu::Extension::HostDraw: the baked blocks (#geom, world space) and the instances the hoods place (l_inst_*, local
// space), both at LOD 0 near and LOD 1 far, coloured by MC2's per vertex lighting and fogged by MC2's fog.
//
// MC2's lighting: losangeles_<time>_cpvs.dat is a Dave archive of one .xbcpv per drawn part, named
// <block>#geom_<lod>_<part> for a block and <owner>_<type><extension>_<lod>_<part> for an instance (owner, type and
// extension as the hood gives them). A .xbcpv is "CPV1", u32 packets, then per packet of the .xmod in file order a u32
// count (the packet's adjuncts) and that many B8G8R8A8 colours, one per adjunct. A material's second texture (lit
// windows) is added with the second texture coordinates. Collision: Mc2LaBound, at the end.
//
// Settings, environment: MC3_MC2_DIR (on), MC3_MC2_TIME (cpvs and fog, default midnight_clear), MC3_MC2_LIGHT (scale of
// the lighting, default 1), MC3_MC2_LOD (metres to LOD 1, default 150), MC3_MC2_ALWAYS=1 (Los Angeles in place of any
// city), MC3_MC2_TELEPORT (F6, see Teleport), MC3_MC2_TRACE (logs every file opened).

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "gpu/ext.h"

#include <d3dcompiler.h>
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "debug_tools.h"
#include "mc2_career_state.h"
#include "mc2_career_test.h"
#include "mc2_career_bridge.h"

using mc2::kMc3Cars;

// the game's own functions the collision and MC2 Career hooks call (generated code)
X86_EXTERN_FUNC(sub_001A2280);        // Stream::Open(path, read)
X86_EXTERN_FUNC(__imp__sub_001A2280);
X86_EXTERN_FUNC(sub_001A2630);        // Stream::Close
X86_EXTERN_FUNC(sub_0020EE80);        // ASCII tokenizer init(name, stream)
X86_EXTERN_FUNC(sub_00234DF0);        // phBound::LoadAscii(tokenizer)
X86_EXTERN_FUNC(sub_00349A40);        // mcUiListMenu::AddEntry
X86_EXTERN_FUNC(__imp__sub_00349A40);
X86_EXTERN_FUNC(sub_0018C560);        // mcMenuModeSelect::OnSelectCareer
X86_EXTERN_FUNC(__imp__sub_0018C560);
X86_EXTERN_FUNC(sub_0018C580);        // mcMenuModeSelect::OnSelectTokyoChallenge
X86_EXTERN_FUNC(__imp__sub_0018C580);
X86_EXTERN_FUNC(sub_0018C530);        // mcMenuModeSelect::OnSelectArcade
X86_EXTERN_FUNC(__imp__sub_0018C530);
X86_EXTERN_FUNC(sub_00184510);        // CareerStartScreen::OnEnter
X86_EXTERN_FUNC(__imp__sub_00184510);
X86_EXTERN_FUNC(sub_00154E40);        // mcUiScreen::SetHeaderTitle
X86_EXTERN_FUNC(__imp__sub_00154E40);
X86_EXTERN_FUNC(sub_00289CE0);        // mcPgUnlockingRulesEval::IsCityUnlockedForCareer
X86_EXTERN_FUNC(__imp__sub_00289CE0);
X86_EXTERN_FUNC(sub_001848E0);        // CareerStartScreen::PopulateMenu
X86_EXTERN_FUNC(__imp__sub_001848E0);
X86_EXTERN_FUNC(sub_001558A0);        // mcUiScreen::AddMenuCommandRow
X86_EXTERN_FUNC(__imp__sub_001558A0);
X86_EXTERN_FUNC(sub_00184230);        // CareerStartScreen::StepCity
X86_EXTERN_FUNC(__imp__sub_00184230);
X86_EXTERN_FUNC(sub_00184770);        // CareerStartScreen::OnSelectStartNewOrContinue
X86_EXTERN_FUNC(__imp__sub_00184770);
X86_EXTERN_FUNC(sub_00184450);        // CareerStartScreen::LaunchCareerDirect
X86_EXTERN_FUNC(__imp__sub_00184450);
X86_EXTERN_FUNC(sub_00278270);        // mcPgCareer::SetupNextCareerEvent
X86_EXTERN_FUNC(__imp__sub_00278270);
X86_EXTERN_FUNC(sub_00277EE0);        // mcProgressManager::ReportRaceResult
X86_EXTERN_FUNC(__imp__sub_00277EE0);
X86_EXTERN_FUNC(sub_00277350);        // mcPgCareer::InitCityCruise
X86_EXTERN_FUNC(sub_0027DAD0);        // mcCareerData::SetCurrentCity
X86_EXTERN_FUNC(sub_0026B8E0);        // mcRaceConfig::InitFromCityRace
X86_EXTERN_FUNC(sub_0026B050);        // mcRaceConfig::SetCity(char const*)
X86_EXTERN_FUNC(__imp__sub_00021410);  // mcGameState::EnterStateMC3Frontend
X86_EXTERN_FUNC(sub_00272010);        // mcRaceConfigCar::SetVehicleFromRecord
X86_EXTERN_FUNC(sub_001398E0);        // audFrontEnd::StopMusic
X86_EXTERN_FUNC(sub_002105F0);        // mcResourceMgr::LoadResource
X86_EXTERN_FUNC(__imp__sub_002105F0);
X86_EXTERN_FUNC(sub_0002A2E0);        // mcCity::InitResourceObjects
X86_EXTERN_FUNC(__imp__sub_0002A2E0);
X86_EXTERN_FUNC(sub_001CD0C0);        // mcRace::CalcRaceDistances
X86_EXTERN_FUNC(__imp__sub_001CD0C0);
X86_EXTERN_FUNC(sub_002B7C60);        // aiMap::GetNode
X86_EXTERN_FUNC(__imp__sub_002B7C60);
X86_EXTERN_FUNC(sub_0027F030);        // mcPgCareer::GetEventData
X86_EXTERN_FUNC(__imp__sub_0027F030);
X86_EXTERN_FUNC(sub_001CD73A);        // mcRaceManager::SetupCareerRace
X86_EXTERN_FUNC(__imp__sub_001CD73A);
X86_EXTERN_FUNC(sub_00083ED0);        // MC3 road/path helper used during city setup
X86_EXTERN_FUNC(__imp__sub_00083ED0);
X86_EXTERN_FUNC(sub_00084090);        // MC3 road/path helper periodic update
X86_EXTERN_FUNC(__imp__sub_00084090);
X86_EXTERN_FUNC(sub_00083E60);        // MC3 road/path helper rebuild
X86_EXTERN_FUNC(__imp__sub_00083E60);

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

std::atomic<bool> g_mc2_city_first_draw_ready{false};

constexpr uint32_t kHostDrawCity = 1;  // d3d::HostDraw id

struct Mc2CityDef {
    uint32_t record;         // MC3 city record index at [0x557EE4]
    const char* name;        // MC3 internal city name
    const char* mc2_folder;  // MC2 folder name under assets_p/city/
    const char* cm_key;      // String table lookup key
    const char16_t* label;   // UI display label
    float spawn[3];          // Default cruise/teleport coordinates
};

constexpr Mc2CityDef kMc2Cities[3] = {
    {13, "losangeles", "losangeles", "CM_losangeles", u"Los Angeles (MC2)", {336.1f, 1.0f, 321.0f}},
    {10, "paris",      "paris",      "CM_paris",      u"Paris (MC2)",       {10.5f, 1.0f, -759.3f}},
    {11, "tokyo_mc2",  "tokyo",      "CM_tokyo_mc2",  u"Tokyo (MC2)",       {-687.2f, 22.0f, 475.1f}},
};

std::atomic<int> g_mc2_active_city{-1};  // -1 = off, 0 = LA, 1 = Paris, 2 = Tokyo
// the frontend was sent back to San Diego (sub_00021410): its first city file turns the MC2 city off
std::atomic<bool> g_mc2_leaving{false};
bool g_mc2_career_mode = false;
const bool g_always = [] {
    const char* v = std::getenv("MC3_MC2_ALWAYS");
    return v && *v == '1';
}();
int ActiveMc2City() {
    int c = g_mc2_active_city.load(std::memory_order_relaxed);
    return c >= 0 ? c : (g_always ? 0 : -1);
}
bool LaActive() { return ActiveMc2City() >= 0; }
int FindMc2CityByRecord(uint32_t rec) {
    for (int i = 0; i < 3; i++)
        if (kMc2Cities[i].record == rec) return i;
    return -1;
}
bool IsMc2Record(uint32_t rec) { return FindMc2CityByRecord(rec) >= 0; }
std::atomic<float> g_eye[3];          // the camera of the last draw, for the test teleport
constexpr uint32_t kWhite = 0, kBlack = 1;  // textures made here

struct Vertex {
    float pos[3];
    uint32_t color;  // B8G8R8A8
    float uv[2];
    float uv2[2];  // the second layer's, the lit windows
};
// indices [first, first + count) of one texture and its second layer (kBlack without one)
struct Batch {
    uint32_t first, count, texture, layer;
};
struct Range {
    uint32_t first = 0, count = 0;  // batches
};
// a block or an instance: its batches at LOD 0 and 1 (none at 1: LOD 0 at every distance)
struct Object {
    float center[3], radius;
    Range lod[2];
};
// a .tex: 7 x u16 header (width, height, format, mip levels, 3 more), then the levels
struct TexFile {
    uint32_t w = 0, h = 0, fmt = 0, mips = 0;
    std::vector<uint8_t> data;
};

struct City {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<Object> objects;
    std::vector<TexFile> textures;  // kWhite, kBlack, then the files
};

// MC2's fog for the time of day, tune/losangeles_<time>.parfileio
struct Fog {
    float color[3] = {0.14f, 0.14f, 0.14f};
    float start = 0, end = 470, clamp = 0.64f;
};

// --- .xmod, the Angel text model (version 1.10): the text grammar read below ---

class Tokens {
public:
    explicit Tokens(const std::string& t) : p_(t.data()), end_(t.data() + t.size()) {}
    // the next whitespace separated word, a quoted string without its quotes; empty at the end
    std::string_view Next() {
        while (p_ < end_ && std::isspace(uint8_t(*p_))) p_++;
        if (p_ >= end_) return {};
        if (*p_ == '"') {
            const char* b = ++p_;
            while (p_ < end_ && *p_ != '"') p_++;
            std::string_view w(b, size_t(p_ - b));
            if (p_ < end_) p_++;
            return w;
        }
        const char* b = p_;
        while (p_ < end_ && !std::isspace(uint8_t(*p_))) p_++;
        return {b, size_t(p_ - b)};
    }
    float Float() { return std::strtof(std::string(Next()).c_str(), nullptr); }
    int Int() { return std::atoi(std::string(Next()).c_str()); }
    bool AtNumber() {
        const char* q = p_;
        while (q < end_ && std::isspace(uint8_t(*q))) q++;
        return q < end_ && (std::isdigit(uint8_t(*q)) || *q == '-' || *q == '+' || *q == '.');
    }

private:
    const char* p_;
    const char* end_;
};

uint32_t Bgra(const float c[4]) {
    auto b = [](float v) { return uint32_t(std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f)); };
    return b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
}

std::string Lower(std::string_view s) {
    std::string o(s);
    for (auto& ch : o) ch = ch == '\\' ? '/' : char(std::tolower(uint8_t(ch)));
    return o;
}

bool ReadFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), {});
    return true;
}

// a .xmod in its own space: vertices one per adjunct, indices local to it, batches indexing its indices
struct Part {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<uint32_t> packets;  // adjuncts of each packet, in file order
    bool ok = false;
};

// --- Dave, the Angel archive (the cpvs): mc2's dave.py reads it the same way ---
class Dave {
public:
    bool Open(const fs::path& p) {
        if (!ReadFile(p, data_) || data_.size() < 0x800 || (data_.compare(0, 4, "Dave") && data_.compare(0, 4, "DAVE")))
            return false;
        bool packed = !data_.compare(0, 4, "Dave");  // 6 bit names, each sharing a prefix with the one before
        uint32_t n = U32(4), info = U32(8);
        static const char kChars[] = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~\x7F";
        std::string name;
        for (uint32_t i = 0; i < n; i++) {
            size_t e = 0x800 + size_t(i) * 16;
            if (e + 16 > data_.size()) return false;
            size_t at = size_t(U32(e)) + info + 0x800;
            uint32_t off = U32(e + 4), full = U32(e + 8), comp = U32(e + 12);
            if (packed) {
                std::vector<uint32_t> bits;
                auto more = [&] {
                    uint32_t v = at + 3 <= data_.size() ? uint32_t(uint8_t(data_[at])) | uint32_t(uint8_t(data_[at + 1])) << 8 |
                                                             uint32_t(uint8_t(data_[at + 2])) << 16
                                                       : 0;
                    at += 3;
                    for (int k = 0; k < 4; k++) bits.push_back(v >> (6 * k) & 0x3F);
                };
                more();
                std::string prev = std::move(name);
                name.clear();
                size_t b = 0;
                if (bits[0] >= 0x38) {
                    size_t keep = (bits[1] - 0x20) * 8 + bits[0] - 0x38;
                    name = prev.substr(0, std::min(keep, prev.size()));
                    b = 2;
                }
                for (;;) {
                    if (b >= bits.size()) more();
                    uint32_t c = bits[b++];
                    if (!c || c >= sizeof(kChars) - 1) break;
                    name += kChars[c];
                }
            } else {
                name.assign(data_.c_str() + std::min(at, data_.size()));
            }
            if (full != comp) {
                compressed_++;  // the cpvs are stored; nothing inflates here
                continue;
            }
            if (size_t(off) + full <= data_.size()) files_[name] = {off, full};
        }
        return true;
    }
    // the file's bytes, or null
    const uint8_t* Find(const std::string& name, uint32_t& size) const {
        auto it = files_.find(name);
        if (it == files_.end()) return nullptr;
        size = it->second.second;
        return reinterpret_cast<const uint8_t*>(data_.data()) + it->second.first;
    }
    size_t Files() const { return files_.size(); }
    uint32_t compressed_ = 0;

private:
    uint32_t U32(size_t at) const {
        uint32_t v = 0;
        std::memcpy(&v, data_.data() + at, 4);
        return v;
    }
    std::string data_;
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> files_;
};

class Loader {
public:
    Loader(const fs::path& root, City& city) : root_(root), city_(city) {
        for (int i = 0; i < 2; i++) {  // white, black
            city_.textures.emplace_back();
            auto& w = city_.textures.back();
            w.w = w.h = 4;
            w.mips = 1;
        }
    }

    uint32_t Texture(std::string_view name) {
        std::string key = Lower(name);
        auto it = by_name_.find(key);
        if (it != by_name_.end()) return it->second;
        uint32_t index = kWhite;
        std::string d;
        if (ReadFile(root_ / "texture_x" / (key + ".tex"), d)) {
            uint16_t h[7] = {};
            if (d.size() >= 14) std::memcpy(h, d.data(), 14);
            // 22 DXT1, 24 DXT3, 26 DXT5; the levels follow the header, each at least one block
            uint32_t block = h[2] == 22 ? 8 : (h[2] == 24 || h[2] == 26) ? 16 : 0;
            if (block && h[0] % 4 == 0 && h[1] % 4 == 0) {
                TexFile t;
                t.w = h[0];
                t.h = h[1];
                t.fmt = h[2];
                size_t off = 14;
                for (uint32_t m = 0, w = t.w, hh = t.h; m < std::max<uint32_t>(h[3], 1); m++) {
                    size_t bytes = size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (hh + 3) / 4) * block;
                    if (off + bytes > d.size()) break;
                    off += bytes;
                    t.mips++;
                    w = std::max(1u, w / 2);
                    hh = std::max(1u, hh / 2);
                }
                if (t.mips) {
                    t.data.assign(d.begin() + 14, d.begin() + std::ptrdiff_t(off));

                    // Patch alpha for opaque surfaces (roads, sidewalks, pavements, ground, walls, buildings)
                    // In MC2 PC/Xbox, DXT5 textures for roads/sidewalks store specular/wetness mask
                    // in alpha (values 0..50). Without patching, clip(t.a - 0.5) discards 100% of road/sidewalk pixels.
                    static const char* const kCutoutKeys[] = {
                        "tree", "palm", "bush", "weed", "leaf", "leaves", "foliage",
                        "fence", "chain", "grate", "wire", "ladder", "rail", "lightglow", "reflect"
                    };
                    static const char* const kOpaqueKeys[] = {
                        "road", "ashphalt", "sidewalk", "sdwlk", "crosswalk", "pave", "curb", "ground",
                        "concrete", "alley", "parking", "dirt", "cobble", "tunnel", "wall", "brick",
                        "roof", "window", "building", "facade", "floor", "ceiling"
                    };

                    bool is_cutout = false;
                    for (const char* k : kCutoutKeys) {
                        if (key.find(k) != std::string::npos) { is_cutout = true; break; }
                    }
                    bool is_opaque = false;
                    for (const char* k : kOpaqueKeys) {
                        if (key.find(k) != std::string::npos) { is_opaque = true; break; }
                    }

                    if (!is_cutout) {
                        if (t.fmt == 26) { // DXT5 / BC3
                            uint8_t max_a = 0;
                            for (size_t b = 0; b + 16 <= t.data.size(); b += 16) {
                                max_a = std::max(max_a, std::max(t.data[b], t.data[b + 1]));
                            }
                            if (is_opaque || max_a < 128) {
                                for (size_t b = 0; b + 16 <= t.data.size(); b += 16) {
                                    t.data[b + 0] = 0xFF; // alpha0
                                    t.data[b + 1] = 0xFF; // alpha1
                                    std::memset(&t.data[b + 2], 0, 6);
                                }
                            }
                        } else if (t.fmt == 24) { // DXT3 / BC2
                            uint8_t max_a = 0;
                            for (size_t b = 0; b + 16 <= t.data.size(); b += 16) {
                                for (int k = 0; k < 8; k++) max_a = std::max(max_a, t.data[b + k]);
                            }
                            if (is_opaque || max_a < 0x88) {
                                for (size_t b = 0; b + 16 <= t.data.size(); b += 16) {
                                    std::memset(&t.data[b], 0xFF, 8);
                                }
                            }
                        }
                    }

                    index = uint32_t(city_.textures.size());
                    city_.textures.push_back(std::move(t));
                }
            } else {
                unsupported_++;
            }
        } else {
            missing_++;
        }
        by_name_[key] = index;
        return index;
    }

    // a .xmod read once, kept for every instance of it
    const Part& Model(const fs::path& path) {
        std::string key = Lower(path.filename().string());
        auto it = parts_.find(key);
        if (it != parts_.end()) return it->second;
        Part& part = parts_[key];
        std::string text;
        if (!ReadFile(path, text)) return part;
        part.ok = Parse(text, part);
        if (!part.ok) failed_++;
        return part;
    }

    // part into the city at m (rows x, y, z, translation: world = x*r0 + y*r1 + z*r2 + t; null: as it is), its
    // colours from cpv when that matches its packets; grows lo/hi by what it adds
    void Append(const Part& part, const float* m, const uint8_t* cpv, uint32_t cpv_size, float lo[3], float hi[3]) {
        uint32_t base = uint32_t(city_.vertices.size()), ifirst = uint32_t(city_.indices.size());
        city_.vertices.insert(city_.vertices.end(), part.vertices.begin(), part.vertices.end());
        Vertex* v = city_.vertices.data() + base;
        size_t n = part.vertices.size();
        if (m) {
            for (size_t i = 0; i < n; i++) {
                float x = v[i].pos[0], y = v[i].pos[1], z = v[i].pos[2];
                for (int c = 0; c < 3; c++) v[i].pos[c] = x * m[c] + y * m[3 + c] + z * m[6 + c] + m[9 + c];
            }
        }
        if (cpv) {
            if (Colour(part, cpv, cpv_size, v)) lit_++;
            else cpv_mismatch_++;
        } else {
            unlit_++;
        }
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) lo[c] = std::fmin(lo[c], v[i].pos[c]), hi[c] = std::fmax(hi[c], v[i].pos[c]);
        for (uint32_t x : part.indices) city_.indices.push_back(base + x);
        for (Batch b : part.batches) {
            b.first += ifirst;
            Batch* last = city_.batches.empty() ? nullptr : &city_.batches.back();
            if (last && last->texture == b.texture && last->layer == b.layer && last->first + last->count == b.first &&
                uint32_t(city_.batches.size()) > open_batch_)
                last->count += b.count;
            else
                city_.batches.push_back(b);
        }
    }
    // the batches Append adds from here on form one range (never merged with the one before)
    void BeginRange() { open_batch_ = uint32_t(city_.batches.size()); }

    uint32_t missing_ = 0, unsupported_ = 0, failed_ = 0, lit_ = 0, unlit_ = 0, cpv_mismatch_ = 0;

private:
    static bool Colour(const Part& part, const uint8_t* cpv, uint32_t size, Vertex* v) {
        if (size < 8 || std::memcmp(cpv, "CPV1", 4)) return false;
        uint32_t packets = 0;
        std::memcpy(&packets, cpv + 4, 4);
        if (packets != part.packets.size()) return false;
        // check every count first: a mismatch leaves the part white
        size_t at = 8;
        for (uint32_t p = 0; p < packets; p++) {
            uint32_t c = 0;
            if (at + 4 > size) return false;
            std::memcpy(&c, cpv + at, 4);
            if (c != part.packets[p] || at + 4 + size_t(c) * 4 > size) return false;
            at += 4 + size_t(c) * 4;
        }
        at = 8;
        size_t vi = 0;
        for (uint32_t p = 0; p < packets; p++) {
            uint32_t c = part.packets[p];
            at += 4;
            for (uint32_t i = 0; i < c; i++, vi++, at += 4) std::memcpy(&v[vi].color, cpv + at, 4);
        }
        return true;
    }

    bool Parse(const std::string& text, Part& part) {
        Tokens tk(text);
        std::vector<std::array<float, 3>> pos;
        std::vector<uint32_t> col;
        std::vector<std::array<float, 2>> uv, uv2;
        struct Mat {
            uint32_t texture = kWhite, layer = kBlack;
            int packets = 0, read = 0;
        };
        std::vector<Mat> mats;
        size_t pending = 0;  // the first material still short of its packets

        for (std::string_view w = tk.Next(); !w.empty(); w = tk.Next()) {
            if (w == "v") {
                std::array<float, 3> v{tk.Float(), tk.Float(), tk.Float()};
                pos.push_back(v);
            } else if (w == "n") {
                tk.Next(), tk.Next(), tk.Next();
            } else if (w == "c") {
                float c[4] = {tk.Float(), tk.Float(), tk.Float(), tk.Float()};
                col.push_back(Bgra(c));
            } else if (w == "t1" || w == "t2") {
                std::array<float, 2> t{tk.Float(), tk.Float()};
                (w == "t1" ? uv : uv2).push_back(t);
            } else if (w == "mtl") {
                Mat m;
                tk.Next();  // name
                for (std::string_view k = tk.Next(); !k.empty() && k != "}"; k = tk.Next()) {
                    if (k == "packets:") m.packets = tk.Int();
                    else if (k == "texture:") {
                        int layer = tk.Int();
                        uint32_t t = Texture(tk.Next());
                        if (layer == 0) m.texture = t;
                        else if (layer == 1 && t != kWhite) m.layer = t;
                    }
                }
                mats.push_back(m);
            } else if (w == "packet") {
                while (tk.AtNumber()) tk.Next();
                if (tk.Next() != "{") return false;
                while (pending < mats.size() && mats[pending].read >= mats[pending].packets) pending++;
                if (pending >= mats.size()) return false;
                Mat& m = mats[pending];
                m.read++;
                uint32_t base = uint32_t(part.vertices.size());
                uint32_t first = uint32_t(part.indices.size());
                uint32_t nadj = 0;
                for (std::string_view k = tk.Next(); !k.empty() && k != "}"; k = tk.Next()) {
                    if (k == "adj") {
                        // position, normal, colour, first and second texture coordinates, matrix
                        int a[6];
                        for (int& x : a) x = tk.Int();
                        Vertex v{};
                        if (a[0] >= 0 && size_t(a[0]) < pos.size()) std::memcpy(v.pos, pos[size_t(a[0])].data(), 12);
                        v.color = a[2] >= 0 && size_t(a[2]) < col.size() ? col[size_t(a[2])] : 0xFFFFFFFFu;
                        if (a[3] >= 0 && size_t(a[3]) < uv.size()) std::memcpy(v.uv, uv[size_t(a[3])].data(), 8);
                        if (a[4] >= 0 && size_t(a[4]) < uv2.size()) std::memcpy(v.uv2, uv2[size_t(a[4])].data(), 8);
                        part.vertices.push_back(v);
                        nadj++;
                    } else if (k == "tri") {
                        uint32_t a = uint32_t(tk.Int()), b = uint32_t(tk.Int()), c = uint32_t(tk.Int());
                        if (a < nadj && b < nadj && c < nadj) part.indices.insert(part.indices.end(), {base + a, base + b, base + c});
                    } else if (k == "str" || k == "stp") {
                        // strips; stp starts on the other parity; a repeated index restarts
                        int n = tk.Int();
                        uint32_t flip = k == "stp";
                        std::vector<uint32_t> s(size_t(std::max(n, 0)));
                        for (auto& x : s) x = uint32_t(tk.Int());
                        for (size_t i = 2; i < s.size(); i++) {
                            uint32_t a = s[i - 2], b = s[i - 1], c = s[i];
                            if (a == b || b == c || a == c || a >= nadj || b >= nadj || c >= nadj) continue;
                            if ((i + flip) % 2) std::swap(a, b);
                            part.indices.insert(part.indices.end(), {base + a, base + b, base + c});
                        }
                    } else if (k == "mtx" || k == "reskin") {
                        while (tk.AtNumber()) tk.Next();
                    }
                }
                part.packets.push_back(nadj);
                uint32_t count = uint32_t(part.indices.size()) - first;
                if (count) {
                    Batch* last = part.batches.empty() ? nullptr : &part.batches.back();
                    if (last && last->texture == m.texture && last->layer == m.layer && last->first + last->count == first)
                        last->count += count;
                    else
                        part.batches.push_back({first, count, m.texture, m.layer});
                }
            } else if (w == "mtxv" || w == "mtxn") {
                tk.Next();
            }
        }
        return true;
    }

    fs::path root_;
    City& city_;
    std::unordered_map<std::string, uint32_t> by_name_;
    std::unordered_map<std::string, Part> parts_;
    uint32_t open_batch_ = 0;
};

// --- drawing ---

// b0: vp, then the camera (xyz, lighting scale), the fog colour (rgb, most fog), the fog's start and end
const char* kShader = R"(
cbuffer C : register(b0) { row_major float4x4 vp; float4 cam; float4 fog_color; float4 fog; };
Texture2D tex : register(t0);
Texture2D layer : register(t1);
SamplerState sm : register(s0);
struct V { float3 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; };
struct P { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1; float d : TEXCOORD2; };
P vs(V v) {
    P o; o.p = mul(float4(v.p, 1), vp); o.c = v.c; o.uv = v.uv; o.uv2 = v.uv2; o.d = distance(v.p, cam.xyz); return o;
}
float4 ps(P i) : SV_Target {
    float4 t = tex.Sample(sm, i.uv);
    clip(t.a - 0.5);
    // MC2's baked lighting times the texture, its lit windows added
    float3 c = t.rgb * i.c.rgb * cam.w + layer.Sample(sm, i.uv2).rgb;
    float f = min(saturate((i.d - fog.x) / max(fog.y - fog.x, 1)), fog_color.w);
    return float4(lerp(c, fog_color.rgb, f), 1);
}
)";
constexpr uint32_t kConstants = 28;

void MatMul(const float* a, const float* b, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

float EnvFloat(const char* name, float def) {
    const char* v = std::getenv(name);
    return v && *v ? std::strtof(v, nullptr) : def;
}

class Mc2City final : public gpu::Extension {
public:
    explicit Mc2City(fs::path root) : root_(std::move(root)) {
        const char* t = std::getenv("MC3_MC2_TIME");
        time_ = t && *t ? t : "midnight_clear";
        light_ = EnvFloat("MC3_MC2_LIGHT", 1.0f);
        lod_distance_ = EnvFloat("MC3_MC2_LOD", 150.0f);
    }

    void HostDraw(gpu::Context& cx, const gpu::State& s, gpu::Target* rt, gpu::Target* ds, uint32_t id) override {
        if (id != kHostDrawCity || !rt) return;
        int want = ActiveMc2City();
        if (want < 0) return;
        // the city went off (back in the frontend) and on again: announce its first draw again
        if (!g_mc2_city_first_draw_ready.load(std::memory_order_acquire)) logged_ = false;
        int st = state_.load(std::memory_order_acquire);
        if (st != 1 && loaded_city_ != want) {
            if (vb_) cx.Release(vb_), vb_.Reset();
            if (ib_) cx.Release(ib_), ib_.Reset();
            if (heap_) old_heaps_.push_back(heap_), heap_.Reset();  // may still be in flight
            for (auto& t : tex_) if (t) cx.Release(t);
            tex_.clear();
            city_ = City{};
            loaded_city_ = want;
            logged_ = false;
            state_.store(0, std::memory_order_release);
            st = 0;
        }
        if (st == 0) {
            state_.store(1, std::memory_order_release);
            int c = loaded_city_;
            std::thread([this, c] { Load(c); }).detach();
            return;
        }
        if (st == 2 && !Upload(cx)) state_ = -1;
        if (state_.load() != 3) return;
        if (!MakePipeline(cx, rt->fmt, ds != nullptr)) return;

        float k[kConstants] = {};
        float* vp = k;
        const float* view = s.transforms[2].data();
        MatMul(view, s.transforms[3].data(), vp);
        // the camera: view = [R; t] with rows of R its axes, so eye = -t R^T
        float eye[3];
        for (int j = 0; j < 3; j++) eye[j] = -(view[12] * view[j * 4] + view[13] * view[j * 4 + 1] + view[14] * view[j * 4 + 2]);
        k[16] = eye[0], k[17] = eye[1], k[18] = eye[2], k[19] = light_;
        for (int i = 0; i < 3; i++) g_eye[i].store(eye[i], std::memory_order_relaxed);
        k[20] = fog_.color[0], k[21] = fog_.color[1], k[22] = fog_.color[2], k[23] = fog_.clamp;
        k[24] = fog_.start, k[25] = fog_.end;
        if (!logged_) {
            logged_ = true;
            g_mc2_city_first_draw_ready.store(true, std::memory_order_release);
            const float* p = s.transforms[3].data();
            LOGE("MC2 city: first draw, target %ux%u of %ux%u, depth %s, proj %g %g %g %g / %g %g %g %g, eye %.1f %.1f %.1f", rt->w,
                 rt->h, rt->gw, rt->gh, ds ? "yes" : "no", p[0], p[5], p[10], p[11], p[14], p[15], s.viewport.min_z,
                 s.viewport.max_z, eye[0], eye[1], eye[2]);
        }
        // the frustum's planes from the columns of vp (row vectors: clip = v * vp)
        float planes[5][4];
        auto col = [&](int c, float sign, int base, float* o) {
            for (int r = 0; r < 4; r++) o[r] = vp[r * 4 + 3] * (base ? 1.0f : 0.0f) + sign * vp[r * 4 + c];
        };
        col(0, 1, 1, planes[0]);
        col(0, -1, 1, planes[1]);
        col(1, 1, 1, planes[2]);
        col(1, -1, 1, planes[3]);
        col(2, 1, 0, planes[4]);  // z >= 0
        for (auto& p : planes) {
            float n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (n > 0)
                for (float& x : p) x /= n;
        }

        auto* cl = cx.List();
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(root_sig_.Get());
        cl->SetPipelineState(ds ? pso_depth_.Get() : pso_.Get());
        D3D12_CPU_DESCRIPTOR_HANDLE rh = cx.View(*rt);
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? cx.DepthView(*ds) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl->OMSetRenderTargets(1, &rh, FALSE, ds ? &dh : nullptr);
        float sc = float(rt->w) / float(rt->gw);
        D3D12_VIEWPORT v{float(s.viewport.x) * sc, float(s.viewport.y) * sc, float(s.viewport.w) * sc,
                         float(s.viewport.h) * sc, s.viewport.min_z, s.viewport.max_z};
        D3D12_RECT scissor{0, 0, LONG(rt->w), LONG(rt->h)};
        cl->RSSetViewports(1, &v);
        cl->RSSetScissorRects(1, &scissor);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &vbv_);
        cl->IASetIndexBuffer(&ibv_);
        cl->SetGraphicsRoot32BitConstants(0, kConstants, k, 0);

        auto table = [&](uint32_t param, uint32_t index) {
            D3D12_GPU_DESCRIPTOR_HANDLE g = heap_->GetGPUDescriptorHandleForHeapStart();
            g.ptr += uint64_t(index) * inc_;
            cl->SetGraphicsRootDescriptorTable(param, g);
        };
        uint32_t drawn = 0, distant = 0, current = ~0u, current_layer = ~0u;
        for (const Object& o : city_.objects) {
            bool in = true;
            for (auto& p : planes)
                if (p[0] * o.center[0] + p[1] * o.center[1] + p[2] * o.center[2] + p[3] < -o.radius) {
                    in = false;
                    break;
                }
            if (!in) continue;
            float dx = o.center[0] - eye[0], dy = o.center[1] - eye[1], dz = o.center[2] - eye[2];
            bool use1 = o.lod[1].count && std::sqrt(dx * dx + dy * dy + dz * dz) - o.radius > lod_distance_;
            const Range& r = o.lod[use1 ? 1 : 0];
            drawn++;
            distant += use1;
            for (uint32_t b = r.first; b < r.first + r.count; b++) {
                const Batch& bt = city_.batches[b];
                if (bt.texture != current) table(1, current = bt.texture);
                if (bt.layer != current_layer) table(2, current_layer = bt.layer);
                cl->DrawIndexedInstanced(bt.count, 1, bt.first, 0, 0);
            }
        }
        if (++frames_ % 600 == 1)
            LOGE("MC2 city: %u of %zu objects in view (%u at LOD 1), eye %.1f %.1f %.1f", drawn, city_.objects.size(), distant, eye[0],
                 eye[1], eye[2]);
        cx.Restore();
    }

private:
    void LoadFog(const char* mc2_folder) {
        fog_ = Fog{};
        std::string text;
        if (!ReadFile(root_ / "tune" / (std::string(mc2_folder) + "_" + time_ + ".parfileio"), text)) return;
        Tokens tk(text);
        for (std::string_view w = tk.Next(); !w.empty(); w = tk.Next()) {
            if (w == "fog_color") fog_.color[0] = tk.Float(), fog_.color[1] = tk.Float(), fog_.color[2] = tk.Float();
            else if (w == "fog_start") fog_.start = tk.Float();
            else if (w == "fog_end") fog_.end = tk.Float();
            else if (w == "fog_clamp") fog_.clamp = tk.Float();
        }
    }

    // the hoods' instances: type, owner, extension and the matrix (rows x, y, z, translation)
    struct Instance {
        std::string type, owner, extension;
        float m[12];
    };
    std::vector<Instance> Instances(const char* mc2_folder) {
        std::vector<Instance> out;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(root_ / "city" / mc2_folder, ec)) {
            if (e.path().extension() != ".hood") continue;
            std::string text;
            if (!ReadFile(e.path(), text)) continue;
            Tokens tk(text);
            for (std::string_view w = tk.Next(); !w.empty(); w = tk.Next()) {
                if (w != "instance_component") continue;
                tk.Next();  // its number
                if (tk.Next() != "{") break;
                Instance in{};
                int floats = 0;
                for (std::string_view k = tk.Next(); !k.empty() && k != "}"; k = tk.Next()) {
                    if (k == "type:") in.type = Lower(tk.Next());
                    else if (k == "owner:") in.owner = Lower(tk.Next());
                    else if (k == "extension:") in.extension = std::string(tk.Next());
                    else if (k == "emin" || k == "emax") tk.Next(), tk.Next(), tk.Next();
                    else if (floats < 12) in.m[floats++] = std::strtof(std::string(k).c_str(), nullptr);
                }
                if (floats == 12 && !in.type.empty()) out.push_back(std::move(in));
            }
        }
        return out;
    }

    void Load(int city_idx) {
        const char* mc2_folder = kMc2Cities[city_idx >= 0 && city_idx < 3 ? city_idx : 0].mc2_folder;
        auto t0 = std::chrono::steady_clock::now();
        LoadFog(mc2_folder);
        // the lighting: beside assets_p in MC2's folder, or inside it
        Dave cpvs;
        std::string cpv_dir = std::string("city/") + mc2_folder + "/" + time_ + "/";
        fs::path cpv_name = fs::path("city") / mc2_folder / time_ / (std::string(mc2_folder) + "_" + time_ + "_cpvs.dat");
        bool have_cpvs = cpvs.Open(root_.parent_path() / cpv_name) || cpvs.Open(root_ / cpv_name);
        if (!have_cpvs) LOGE("MC2 city (%s): no %s beside or in %s, drawn unlit", mc2_folder, cpv_name.string().c_str(), root_.string().c_str());
        auto cpv = [&](const std::string& name, uint32_t& size) -> const uint8_t* {
            return have_cpvs ? cpvs.Find(cpv_dir + name + ".xbcpv", size) : nullptr;
        };

        Loader ld(root_, city_);
        fs::path models = root_ / "city" / mc2_folder / "models";
        // the parts present of each model: <name>_<lod>_<part>.xmod, or <block>#geom_<lod>_<part>.xmod
        std::map<std::string, std::array<std::vector<std::string>, 2>> geoms, types;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(models, ec)) {
            if (e.path().extension() != ".xmod") continue;
            std::string n = Lower(e.path().stem().string());
            size_t g = n.find("#geom_");
            if (g != std::string::npos && n.size() > g + 7 && (n[g + 6] == '0' || n[g + 6] == '1')) {
                geoms[n.substr(0, g)][size_t(n[g + 6] - '0')].push_back(n.substr(g + 8));
                continue;
            }
            if (n.size() <= 7 || n[1] != '_' || n.compare(2, 5, "inst_") != 0) continue;
            // <type>_<lod>_<part>
            size_t u2 = n.rfind('_'), u1 = u2 == std::string::npos || !u2 ? std::string::npos : n.rfind('_', u2 - 1);
            if (u1 == std::string::npos || u2 != u1 + 2 || (n[u1 + 1] != '0' && n[u1 + 1] != '1')) continue;
            types[n.substr(0, u1)][size_t(n[u1 + 1] - '0')].push_back(n.substr(u2 + 1));
        }

        auto add = [&](const std::array<std::vector<std::string>, 2>& lods, const std::string& file_prefix,
                       const std::string& cpv_prefix, const float* m) {
            Object o{};
            float lo[2][3], hi[2][3];
            for (int lod = 0; lod < 2; lod++) {
                ld.BeginRange();
                o.lod[lod].first = uint32_t(city_.batches.size());
                for (int i = 0; i < 3; i++) lo[lod][i] = 1e30f, hi[lod][i] = -1e30f;
                for (const std::string& part : lods[size_t(lod)]) {
                    std::string suffix = std::to_string(lod) + "_" + part;
                    const Part& p = ld.Model(models / (file_prefix + suffix + ".xmod"));
                    if (!p.ok) continue;
                    uint32_t size = 0;
                    const uint8_t* c = cpv(cpv_prefix + suffix, size);
                    ld.Append(p, m, c, size, lo[lod], hi[lod]);
                }
                o.lod[lod].count = uint32_t(city_.batches.size()) - o.lod[lod].first;
            }
            int shown = 0;  // the level drawn near
            if (!o.lod[0].count) {
                // only a far model (freeway walls, some houses): MC2 draws it at every distance
                if (!o.lod[1].count) return;
                o.lod[0] = o.lod[1];
                o.lod[1] = {};
                shown = 1;
            }
            const float* l0 = lo[shown];
            const float* h0 = hi[shown];
            if (l0[0] > h0[0]) return;
            for (int i = 0; i < 3; i++) o.center[i] = (l0[i] + h0[i]) * 0.5f;
            float dx = h0[0] - l0[0], dy = h0[1] - l0[1], dz = h0[2] - l0[2];
            o.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
            city_.objects.push_back(o);
        };

        for (auto& [block, lods] : geoms) add(lods, block + "#geom_", block + "#geom_", nullptr);
        size_t blocks = city_.objects.size();
        std::vector<Instance> insts = Instances(mc2_folder);
        uint32_t no_model = 0;
        for (const Instance& in : insts) {
            auto it = types.find(in.type);
            if (it == types.end()) {
                no_model++;
                continue;
            }
            add(it->second, in.type + "_", in.owner + "_" + in.type + in.extension + "_", in.m);
        }
        city_.vertices.shrink_to_fit();
        city_.indices.shrink_to_fit();
        float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        LOGE("MC2 city: %zu blocks, %zu of %zu instances (%u without a model), %zu vertices, %zu triangles, %zu batches, %zu textures "
             "(%u missing, %u unsupported), %u models failed; cpvs %s: %zu files, %u parts lit, %u unlit, %u mismatched; fog %.0f..%.0f "
             "at most %.2f; %.1f s",
             blocks, city_.objects.size() - blocks, insts.size(), no_model, city_.vertices.size(), city_.indices.size() / 3,
             city_.batches.size(), city_.textures.size(), ld.missing_, ld.unsupported_, ld.failed_, time_.c_str(), cpvs.Files(),
             ld.lit_, ld.unlit_, ld.cpv_mismatch_, fog_.start, fog_.end, fog_.clamp, secs);
        state_.store(city_.objects.empty() ? -1 : 2, std::memory_order_release);
    }


    static DXGI_FORMAT Format(uint32_t fmt) {
        return fmt == 22 ? DXGI_FORMAT_BC1_UNORM : fmt == 24 ? DXGI_FORMAT_BC2_UNORM : fmt == 26 ? DXGI_FORMAT_BC3_UNORM
                                                                                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    ComPtr<ID3D12Resource> Buffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, uint64_t size, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp{type};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = std::max<uint64_t>(size, 256);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
        return r;
    }

    // everything to the GPU once, on the render thread: buffers, then each texture through one upload buffer
    bool Upload(gpu::Context& cx) {
        ID3D12Device* dev = cx.Device();
        auto* cl = cx.List();
        uint64_t vbytes = city_.vertices.size() * sizeof(Vertex), ibytes = city_.indices.size() * 4;
        vb_ = Buffer(dev, D3D12_HEAP_TYPE_UPLOAD, vbytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        ib_ = Buffer(dev, D3D12_HEAP_TYPE_UPLOAD, ibytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!vb_ || !ib_) return false;
        void* p = nullptr;
        vb_->Map(0, nullptr, &p);
        std::memcpy(p, city_.vertices.data(), vbytes);
        vb_->Unmap(0, nullptr);
        ib_->Map(0, nullptr, &p);
        std::memcpy(p, city_.indices.data(), ibytes);
        ib_->Unmap(0, nullptr);
        vbv_ = {vb_->GetGPUVirtualAddress(), UINT(vbytes), sizeof(Vertex)};
        ibv_ = {ib_->GetGPUVirtualAddress(), UINT(ibytes), DXGI_FORMAT_R32_UINT};

        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(city_.textures.size()),
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
        inc_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // footprints of every level of every texture, packed in one upload buffer
        struct Level {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT rows;
            UINT64 row_bytes;
            size_t src;
        };
        std::vector<std::vector<Level>> levels(city_.textures.size());
        std::vector<D3D12_RESOURCE_DESC> descs(city_.textures.size());
        uint64_t total = 0;
        for (size_t i = 0; i < city_.textures.size(); i++) {
            TexFile& t = city_.textures[i];
            if (i == kWhite || i == kBlack) {  // 4 x 4
                t.data.assign(64, i == kWhite ? 0xFF : 0x00);
                for (size_t a = 3; a < 64; a += 4) t.data[a] = 0xFF;
            }
            D3D12_RESOURCE_DESC& d = descs[i];
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = t.w;
            d.Height = t.h;
            d.DepthOrArraySize = 1;
            d.MipLevels = UINT16(t.mips);
            d.Format = Format(t.fmt);
            d.SampleDesc.Count = 1;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.mips);
            std::vector<UINT> rows(t.mips);
            std::vector<UINT64> rb(t.mips);
            UINT64 bytes = 0;
            dev->GetCopyableFootprints(&d, 0, t.mips, total, fp.data(), rows.data(), rb.data(), &bytes);
            size_t src = 0;
            for (uint32_t m = 0; m < t.mips; m++) {
                levels[i].push_back({fp[m], rows[m], rb[m], src});
                src += size_t(rows[m]) * rb[m];
            }
            total = (fp[0].Offset + bytes + 511) & ~uint64_t(511);
        }
        ComPtr<ID3D12Resource> up = Buffer(dev, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up) return false;
        uint8_t* dst = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&dst));
        tex_.resize(city_.textures.size());
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        for (size_t i = 0; i < city_.textures.size(); i++) {
            TexFile& t = city_.textures[i];
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &descs[i], D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&tex_[i]))))
                return false;
            for (uint32_t m = 0; m < t.mips; m++) {
                const Level& l = levels[i][m];
                for (UINT r = 0; r < l.rows; r++)
                    std::memcpy(dst + l.fp.Offset + uint64_t(r) * l.fp.Footprint.RowPitch, t.data.data() + l.src + size_t(r) * l.row_bytes,
                                size_t(l.row_bytes));
                D3D12_TEXTURE_COPY_LOCATION to{tex_[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                to.SubresourceIndex = m;
                D3D12_TEXTURE_COPY_LOCATION from{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                from.PlacedFootprint = l.fp;
                cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
            b.Transition = {tex_[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            cl->ResourceBarrier(1, &b);
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = descs[i].Format;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = t.mips;
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * inc_;
            dev->CreateShaderResourceView(tex_[i].Get(), &sv, h);
            std::vector<uint8_t>().swap(t.data);
        }
        up->Unmap(0, nullptr);
        cx.Release(up);
        state_ = 3;
        LOGE("MC2 city: uploaded %zu textures (%.1f MB), %.1f MB of geometry", tex_.size(), total / 1048576.0,
             (vbytes + ibytes) / 1048576.0);
        return true;
    }

    bool MakePipeline(gpu::Context& cx, DXGI_FORMAT rtv, bool depth) {
        if (pso_ && rtv == pso_fmt_) return true;
        ID3D12Device* dev = cx.Device();
        if (!root_sig_) {
            D3D12_DESCRIPTOR_RANGE range[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1}};
            D3D12_ROOT_PARAMETER params[3] = {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.Num32BitValues = kConstants;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            for (int i = 0; i < 2; i++) {
                params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1 + i].DescriptorTable = {1, &range[i]};
                params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }
            D3D12_STATIC_SAMPLER_DESC smp{};
            smp.Filter = D3D12_FILTER_ANISOTROPIC;
            smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            smp.MaxAnisotropy = 8;
            smp.MaxLOD = D3D12_FLOAT32_MAX;
            smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rd{3, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
            if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_sig_))))
                return false;
        }
        ComPtr<ID3DBlob> vs, ps, err;
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), "mc2_city", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
            FAILED(D3DCompile(kShader, std::strlen(kShader), "mc2_city", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) {
            LOGE("MC2 city: shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        D3D12_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_sig_.Get();
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.InputLayout = {il, 4};
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rtv;
        d.SampleDesc.Count = 1;
        if (FAILED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_)))) return false;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        if (FAILED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_depth_)))) return false;
        pso_fmt_ = rtv;
        (void)depth;
        return true;
    }

    fs::path root_;
    std::string time_;  // MC2's time of day and weather: whose cpvs and fog
    float light_ = 1, lod_distance_ = 150;
    Fog fog_;
    std::atomic<int> state_{0};  // 0 not loaded, 1 loading, 2 loaded, 3 on the GPU, -1 failed
    City city_;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_, pso_depth_;
    DXGI_FORMAT pso_fmt_ = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> heap_;
    uint32_t inc_ = 0;
    std::vector<ComPtr<ID3D12Resource>> tex_;
    ComPtr<ID3D12Resource> vb_, ib_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW ibv_{};
    bool logged_ = false;
    uint32_t frames_ = 0;
    int loaded_city_ = -1;
    std::vector<ComPtr<ID3D12DescriptorHeap>> old_heaps_;  // retired on a city switch (at most a few)
};

// on only when MC3_MC2_DIR names MC2's assets_p, the user's own files; there is no default folder
fs::path Mc2Dir() {
    const char* d = std::getenv("MC3_MC2_DIR");
    std::error_code ec;
    if (d && *d) {
        fs::path p(d);
        if (fs::is_directory(p / "city" / "losangeles" / "models", ec)) return p;
    }
    return {};
}

const bool g_enabled = [] {
    fs::path p = Mc2Dir();
    if (p.empty()) return false;
    gpu::SetExtension(std::make_unique<Mc2City>(p));
    return true;
}();

constexpr uint32_t kCities = 0x557EE4;      // record 0 (after the new[] count)
constexpr uint32_t kRecordSize = 0x50;
constexpr uint32_t kSdRecord = 0;
constexpr uint32_t kReadDevice = 0x4B26E8;      // fiDevice* Stream::Open takes for reading: the archives once mounted
constexpr uint32_t kRawDevice = 0x4B26A0;       // the console's file system (CreateFileA)
constexpr uint32_t kAsciiTokenizer = 0x46D868;  // vtable of the ASCII token reader phBound::LoadAscii reads
constexpr uint32_t kPhInst = 0x475508, kArchetype = 0x475580;
constexpr char kCache[] = "\\Device\\Harddisk0\\Partition5\\mc2\\";

// fn(ecx; args) on the guest stack as the game calls it; eax back, the caller's registers as they were
uint32_t GuestCall(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t ecx, std::initializer_list<uint32_t> args) {
    uint32_t eax = ctx.eax, ecx0 = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    uint32_t cf = ctx.cf, zf = ctx.zf, sf = ctx.sf, of = ctx.of, pf = ctx.pf;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0);  // return address: ret only pops it
    ctx.ecx = ecx;
    fn(ctx, base);
    uint32_t r = ctx.eax;
    ctx.eax = eax, ctx.ecx = ecx0, ctx.edx = edx, ctx.esp = esp;
    ctx.cf = cf, ctx.zf = zf, ctx.sf = sf, ctx.of = of, ctx.pf = pf;
    return r;
}

thread_local bool g_stream_raw_override = false;
thread_local uint32_t g_saved_read_device = 0;

uint32_t AllocLoosePathSlot(uint8_t* base, const std::string& xbox_path) {
    static uint32_t ring_base = 0;
    static std::atomic<uint32_t> ring_idx{0};
    if (!ring_base) ring_base = mem::AllocSystem(32 * 256, 16);
    uint32_t slot = ring_base + (ring_idx.fetch_add(1) & 31u) * 256u;
    size_t n = std::min<size_t>(xbox_path.size(), 255);
    std::memcpy(base + slot, xbox_path.c_str(), n);
    base[slot + n] = 0;
    return slot;
}

}  // namespace

// Override Stream::Open (0x1A2280) so loose files redirected to hdd/Z/mc2/ open via kRawDevice and restore kReadDevice
X86_FUNC(sub_001A2280) {
    bool prev_override = g_stream_raw_override;
    uint32_t prev_dev = g_saved_read_device;
    g_stream_raw_override = false;
    __imp__sub_001A2280(ctx, base);
    if (g_stream_raw_override) {
        X86_MEM32(kReadDevice) = g_saved_read_device;
    }
    g_stream_raw_override = prev_override;
    g_saved_read_device = prev_dev;
}

// Override mcResourceMgr::LoadResource (0x2105F0) so city resource requests for MC2 cities
// ($/resources/city/losangeles_..., paris_..., tokyo_mc2_...) redirect to San Diego ("sd") resources,
// providing the base lighting, sky, and weather pack without returning NULL and crashing.
void Mc3TraceResource(const char* how, X86Context& ctx, uint8_t* base);  // car_late_load.cpp, --mc3-trace-resources

X86_FUNC(sub_002105F0) {
    Mc3TraceResource("load", ctx, base);
    if (g_enabled) {
        uint32_t name_va = X86_MEM32(ctx.esp + 4);
        if (name_va) {
            const char* p = reinterpret_cast<const char*>(base + name_va);
            for (const auto& def : kMc2Cities) {
                if (std::strstr(p, def.name)) {
                    std::string s(p);
                    size_t pos = s.find(def.name);
                    if (pos != std::string::npos) {
                        s.replace(pos, std::strlen(def.name), "sd");
                        uint32_t slot = AllocLoosePathSlot(base, s);
                        X86_MEM32(ctx.esp + 4) = slot;
                        LOGE("MC2 resource redirect: %s -> %s", p, s.c_str());
                    }
                    break;
                }
            }
        }
    }
    __imp__sub_002105F0(ctx, base);
}

// Override mcCity::InitResourceObjects (0x02A2E0) to guard against NULL cityMgr (0x50715C)
X86_FUNC(sub_0002A2E0) {
    if (!X86_MEM32(0x50715C)) {
        LOGE("MC2: sub_0002A2E0 called with null cityMgr (0x50715C == 0), safely skipping");
        ctx.esp += 4;
        return;
    }
    __imp__sub_0002A2E0(ctx, base);
}

// Override aiMap::GetNode (0x2B7C60) to guard against NULL node array (when playing on MC2 cities without MC3 AI graph)
X86_FUNC(sub_002B7C60) {
    if (!ctx.ecx || !X86_MEM32(ctx.ecx + 0x2C)) {
        static uint32_t dummy_node_va = 0;
        if (!dummy_node_va) dummy_node_va = mem::AllocSystem(0x40, 16);
        ctx.eax = dummy_node_va;
        ctx.esp += 8;
        return;
    }
    __imp__sub_002B7C60(ctx, base);
}

// Override mcRace::CalcRaceDistances (0x1CD0C0) to handle MC2 races without MC3 road network graph
X86_FUNC(sub_001CD0C0) {
    uint32_t mgr = X86_MEM32(0x5587E0);
    if (!mgr || !X86_MEM32(mgr + 0x2C)) {
        if (ctx.ecx) {
            // Set start and end road nodes to -1 so calculation skips road graph and uses checkpoints
            X86_MEM32(ctx.ecx + 0x00) = 0xFFFFFFFFu;
            X86_MEM32(ctx.ecx + 0x04) = 0xFFFFFFFFu;
        }
    }
    __imp__sub_001CD0C0(ctx, base);
}

// Override mcPgCareer::GetEventData (0x27F030) to provide safe defaults when MC2 Career event is requested
X86_FUNC(sub_0027F030) {
    __imp__sub_0027F030(ctx, base);
    if (!ctx.eax) {
        static uint32_t dummy_event_va = 0;
        if (!dummy_event_va) {
            dummy_event_va = mem::AllocSystem(0x4C, 16);
            std::memset(base + dummy_event_va, 0, 0x4C);
            base[dummy_event_va + 0x09] = 0xFF;  // -1
            base[dummy_event_va + 0x0A] = 0x00;
            *reinterpret_cast<float*>(base + dummy_event_va + 0x14) = 1.0f;
            *reinterpret_cast<float*>(base + dummy_event_va + 0x18) = 1.0f;
            base[dummy_event_va + 0x25] = 0x00;  // difficulty / vehicle class
            *reinterpret_cast<float*>(base + dummy_event_va + 0x28) = 1.0f;
        }
        ctx.eax = dummy_event_va;
    }
}

// Override mcRaceManager::SetupCareerRace (0x1CD73A) to safely bypass MC3 career event setup [edx+0x2C]
// when launching an MC2 city in Career mode.
X86_FUNC(sub_001CD73A) {
    uint32_t saved_mode = X86_MEM32(0x0053F2D4);
    if (g_mc2_active_city.load() >= 0 || g_mc2_career_mode) {
        // MC2 race records have no matching MC3 career event object. The
        // original function dereferences the missing object at 0x1CD838. Do
        // not predicate this guard on the transient mode value: loading has
        // already changed it by the time SetupCareerRace executes.
        LOGE("MC2 Career: bypassing MC3 event object in sub_001CD73A (saved mode=%u)", saved_mode);
        X86_MEM32(0x0053F2D4) = 0;
        __imp__sub_001CD73A(ctx, base);
        X86_MEM32(0x0053F2D4) = saved_mode;
    } else {
        __imp__sub_001CD73A(ctx, base);
    }
}

// 0x1CD816 is duplicated inside several overlapping recompiled entry points,
// so replacing sub_001CD73A alone does not cover every caller. Skip the exact
// MC3 event-object virtual call and resume at its original continuation.
bool Mc2CareerSkipEventObject(X86Context&, uint8_t*) {
    if (g_mc2_active_city.load() >= 0 || g_mc2_career_mode) {
        LOGE("MC2 Career: skipped missing MC3 event object at 0x1CD816 -> 0x1CD838");
        return true;
    }
    return false;
}

// MC2 road data stores compact indices where MC3's path helper expects guest
// pointers.  The original routine reaches sub_000835A0 and then dereferences
// values such as 0x981C as pointers.  The imported city is drawn and collided
// on the host, so retain the helper's harmless state initialization and skip
// the incompatible MC3 road-graph traversal while an MC2 city is active.
X86_FUNC(sub_00083ED0) {
    if (g_mc2_active_city.load(std::memory_order_acquire) >= 0) {
        static bool logged = false;
        const uint32_t self = ctx.ecx;
        const uint8_t mode = X86_MEM8(ctx.esp + 8);
        const uint32_t anchor = X86_MEM32(self + 0x10);
        X86_MEM32(self + 0x14) = anchor;
        X86_MEM8(self + 0x1C) = mode;
        X86_MEM32(self + 0x08) = 0;  // float 0.0f
        X86_MEM32(self + 0x30) = 0;  // float 0.0f
        if (X86_MEM32(self + 0x18) != 1) {
            X86_MEM32(self + 0x18) = 1;
            X86_MEM32(self + 0x14) = anchor;
        }
        if (!logged) {
            LOGE("MC2 city: bypassed incompatible MC3 road/path helper sub_00083ED0");
            logged = true;
        }
        ctx.eax = 0;
        ctx.esp += 12;  // ret 0x08
        return;
    }
    __imp__sub_00083ED0(ctx, base);
}

// Periodic updates of the same object eventually reach sub_000838C0 and use
// the compact MC2 road index as a pointer again.  Host-side MC2 rendering and
// collision do not consume this guest helper, so keep it dormant for MC2.
X86_FUNC(sub_00084090) {
    if (g_mc2_active_city.load(std::memory_order_acquire) >= 0) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_00084090(ctx, base);
}

X86_FUNC(sub_00083E60) {
    if (g_mc2_active_city.load(std::memory_order_acquire) >= 0) {
        ctx.esp += 8;  // ret 0x04
        return;
    }
    __imp__sub_00083E60(ctx, base);
}

// 0x26F001, the registry's ret: register Los Angeles (13), Paris (10), and Tokyo (11)
void Mc2LaRegister(X86Context&, uint8_t* base) {
    if (!g_enabled) return;
    uint32_t cities = X86_MEM32(kCities);
    if (!cities) return;
    uint32_t sd = cities + kSdRecord * kRecordSize;
    for (const auto& def : kMc2Cities) {
        uint32_t rec = cities + def.record * kRecordSize;
        size_t len = std::strlen(def.name) + 1;
        uint32_t name_va = mem::AllocSystem(uint32_t(len), 16);
        std::memcpy(base + name_va, def.name, len);
        X86_MEM32(rec + 0x00) = name_va;
        X86_MEM32(rec + 0x04) = X86_MEM32(sd + 0x04);  // code
        X86_MEM32(rec + 0x08) = name_va;
        X86_MEM32(rec + 0x0C) = X86_MEM32(sd + 0x0C);  // hoods
        X86_MEM32(rec + 0x10) = X86_MEM32(sd + 0x10);
        LOGE("MC2: registered %s as city %u", def.name, def.record);
    }
}

// 0x1A2280 Stream::Open(path, ...): every file the game opens.
void Mc2LaPaths(X86Context& ctx, uint8_t* base) {
    if (!g_enabled) return;
    uint32_t va = X86_MEM32(ctx.esp + 4);
    if (!va) return;
    char* p = reinterpret_cast<char*>(base + va);
    std::string path(p, strnlen(p, 260));
    std::string low(path);
    for (auto& c : low) c = c == '\\' ? '/' : char(std::tolower(uint8_t(c)));
    bool city = low.find("resources/city/") != std::string::npos || low.find("city/") != std::string::npos;
    if (std::getenv("MC3_MC2_TRACE")) LOGE("MC2 open: %s", path.c_str());

    if (low.compare(0, 33, "/device/harddisk0/partition5/mc2/") == 0) {
        g_saved_read_device = X86_MEM32(kReadDevice);
        X86_MEM32(kReadDevice) = kRawDevice;
        g_stream_raw_override = true;
        return;
    }

    // MC3_MC2_ALWAYS: San Diego's cruise starts where Los Angeles' does
    static const char kSdCruise[] = "tune/race/sd/cruise/easy/sd_cruise_easy_cruise_race01.rac";
    static const char kLaCruise[] = "tune\\race\\losangeles\\cruise\\easy\\losangeles_cruise_race01.rac";
    if (g_always && low == kSdCruise) {
        std::memcpy(p, kLaCruise, sizeof(kLaCruise));
        LOGE("MC2 LA: %s -> %s", path.c_str(), kLaCruise);
        return;
    }

    int matched_city = -1;
    size_t match_pos = std::string::npos;
    size_t match_len = 0;
    for (int i = 0; i < 3; i++) {
        size_t pos = low.find(kMc2Cities[i].name);
        if (pos != std::string::npos) {
            matched_city = i;
            match_pos = pos;
            match_len = std::strlen(kMc2Cities[i].name);
            break;
        }
    }

    if (matched_city < 0) {
        // the MC2 career's own load opens shared city files too: only outside it, or once the frontend went back to San Diego
        if ((!g_mc2_career_mode || g_mc2_leaving.load(std::memory_order_acquire)) && city &&
            g_mc2_active_city.exchange(-1) >= 0) {
            g_mc2_leaving.store(false, std::memory_order_release);
            g_mc2_city_first_draw_ready.store(false, std::memory_order_release);
            LOGE("MC2 city: off (%s)", path.c_str());
        }
        return;
    }

    // Serve loose MC2 race/career files (and optional converted _city.aib when MC3_MC2_AIB=1) from hdd/Z/mc2/
    bool want_loose = (low.find("tune/race") != std::string::npos) ||
                      (std::getenv("MC3_MC2_AIB") && low.find("_city.aib") != std::string::npos);
    if (want_loose) {
        std::string rel = path;
        for (auto& c : rel) if (c == '/') c = '\\';
        std::string xbox = std::string(kCache) + rel;
        std::string host = kernel::ResolvePath(xbox, 0);
        std::error_code ec;
        if (!host.empty() && fs::is_regular_file(host, ec)) {
            uint32_t slot = AllocLoosePathSlot(base, xbox);
            X86_MEM32(ctx.esp + 4) = slot;
            g_saved_read_device = X86_MEM32(kReadDevice);
            X86_MEM32(kReadDevice) = kRawDevice;
            g_stream_raw_override = true;
            static int loose_shown = 0;
            if (loose_shown++ < 120) LOGE("MC2 loose: %s -> %s", path.c_str(), xbox.c_str());
            return;
        }
        if (low.find("tune/race") != std::string::npos) return;
    }

    static int shown = 0;
    // Suppress redirection of files that belong exclusively to MC3's San Diego:
    // 1. .occlude: MC3 occluder system will cleanly fail (0 occluders), preventing camera culling holes
    // 2. _props / .props: prevents loading San Diego street lamps, traffic lights, trash cans into MC2
    // 3. _peds / .peds: prevents spawning San Diego pedestrians
    // 4. _traffic / .traffic: prevents spawning San Diego traffic paths
    // 5. _city.aib: prevents loading San Diego AI routes unless loose AIB exists
    bool is_sd_specific = (low.find(".occlude") != std::string::npos) ||
                          (low.find("_props.xbck") != std::string::npos) ||
                          (low.find(".props") != std::string::npos) ||
                          (low.find("_peds.xbck") != std::string::npos) ||
                          (low.find(".peds") != std::string::npos) ||
                          (low.find("_traffic.xbck") != std::string::npos) ||
                          (low.find(".traffic") != std::string::npos) ||
                          (low.find("_city.aib") != std::string::npos);
    if (is_sd_specific) {
        if (shown++ < 200) LOGE("MC2 path: suppressed SD redirection for %s", path.c_str());
        return;
    }

    std::string out = path;
    const char* cname = kMc2Cities[matched_city].name;
    for (size_t i = out.size(); (i = low.rfind(cname, i)) != std::string::npos;) {
        out.replace(i, match_len, "sd");
        if (i == 0) break;
        i--;
    }
    std::memcpy(p, out.c_str(), out.size() + 1);
    if (city) {
        int prev = g_mc2_active_city.exchange(matched_city);
        if (prev != matched_city) LOGE("MC2 city: active = %s (%d)", cname, matched_city);
    }
    if (shown++ < 200) LOGE("MC2 path: %s -> %s", path.c_str(), out.c_str());
}

namespace {
constexpr uint32_t kCarInst = 0x46C750;
void Teleport(uint8_t* base) {
    int c = ActiveMc2City();
    if (c < 0) c = 0;
    const char* v = std::getenv("MC3_MC2_TELEPORT");
    float to[3] = {kMc2Cities[c].spawn[0], kMc2Cities[c].spawn[1], kMc2Cities[c].spawn[2]};
    if (v && *v && std::strchr(v, ',')) std::sscanf(v, "%f,%f,%f", &to[0], &to[1], &to[2]);
    float eye[3] = {g_eye[0].load(), g_eye[1].load(), g_eye[2].load()};
    auto at = [&](uint32_t a) { return reinterpret_cast<float*>(base + a); };
    uint32_t car = 0;
    float best = 400;
    for (uint32_t a = 0x80000010u; a < mem::PhysicalEnd() - 48; a += 4) {
        if (X86_MEM32(a - 16) != kCarInst) continue;
        const float* f = at(a + 36);
        float d = (f[0] - eye[0]) * (f[0] - eye[0]) + (f[1] - eye[1]) * (f[1] - eye[1]) + (f[2] - eye[2]) * (f[2] - eye[2]);
        if (d < best) best = d, car = a;
    }
    if (!car) {
        LOGE("MC2: teleport: no car near the camera %.1f %.1f %.1f", eye[0], eye[1], eye[2]);
        return;
    }
    float from[3];
    std::memcpy(from, at(car + 36), 12);
    float delta[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    uint32_t moved = 0;
    for (uint32_t a = 0x80000000u; a < mem::PhysicalEnd() - 12; a += 4) {
        float* f = at(a);
        if (std::fabs(f[0] - from[0]) < 3 && std::fabs(f[1] - from[1]) < 3 && std::fabs(f[2] - from[2]) < 3) {
            for (int i = 0; i < 3; i++) f[i] += delta[i];
            moved++;
        }
    }
    LOGE("MC2: teleport: car %08X from %.1f %.1f %.1f to %.1f %.1f %.1f, %u positions moved", car, from[0], from[1], from[2], to[0], to[1],
         to[2], moved);
}
}  // namespace

bool Mc2CityDraw(X86Context& ctx, uint8_t* base) {
    if (!g_enabled || !LaActive()) return false;
    static bool f6 = false;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (down && !f6) Teleport(base);
    f6 = down;
    d3d::HostDraw(kHostDrawCity);
    ctx.esp += 8;
    return true;
}

bool Mc2CityHideList(X86Context& ctx, uint8_t*) {
    return g_enabled && LaActive() && (ctx.edi >= 2 && ctx.edi <= 8);
}

namespace {
uint32_t StageBound(uint8_t* base, const char* name) {
    std::error_code ec;
    fs::path src = Mc2Dir() / "bound" / (std::string(name) + ".bnd");
    std::string xbox = std::string(kCache) + name + ".bnd";
    std::string dst = kernel::ResolvePath(xbox, 0);
    if (dst.empty() || !fs::exists(src, ec)) {
        LOGE("MC2: no %s or no cache partition (%s)", src.string().c_str(), dst.c_str());
        return 0;
    }
    if (!fs::exists(dst, ec) || fs::file_size(dst, ec) != fs::file_size(src, ec) || fs::last_write_time(dst, ec) < fs::last_write_time(src, ec)) {
        fs::create_directories(fs::path(dst).parent_path(), ec);
        if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
            LOGE("MC2: cannot copy %s to %s: %s", src.string().c_str(), dst.c_str(), ec.message().c_str());
            return 0;
        }
    }
    uint32_t path = mem::AllocSystem(uint32_t(xbox.size() + 1), 16);
    std::memcpy(base + path, xbox.c_str(), xbox.size() + 1);
    return path;
}

uint32_t LoadBound(X86Context& ctx, uint8_t* base, uint32_t path) {
    static uint32_t tokenizer = mem::AllocSystem(0x100, 16);
    uint32_t stream = GuestCall(ctx, base, sub_001A2280, 0, {path, 1});  // Stream::Open(path, read)
    if (!stream) return 0;
    std::memset(base + tokenizer, 0, 0x100);
    X86_MEM32(tokenizer) = kAsciiTokenizer;
    GuestCall(ctx, base, sub_0020EE80, tokenizer, {path, stream});         // tokenizer(name, stream)
    uint32_t bound = GuestCall(ctx, base, sub_00234DF0, 0, {tokenizer});  // phBound::LoadAscii
    GuestCall(ctx, base, sub_001A2630, stream, {});                        // Stream::Close
    return bound;
}

uint32_t Archetype(uint8_t* base, uint32_t holder, uint32_t at, uint8_t type) {
    uint32_t inst = X86_MEM32(holder + at);
    if (!inst || X86_MEM32(inst) != kPhInst) return 0;
    uint32_t arch = X86_MEM32(inst + 4);
    if (!arch || X86_MEM32(arch) != kArchetype) return 0;
    uint32_t bound = X86_MEM32(arch + 0xC);
    return bound && X86_MEM8(bound + 4) == type ? arch : 0;
}
}  // namespace

void Mc2LaBound(X86Context& ctx, uint8_t* base) {
    int c = ActiveMc2City();
    if (!g_enabled || c < 0) return;
    uint32_t holder = X86_MEM32(ctx.esi + 0x18);
    if (!holder) return;
    uint32_t ground = Archetype(base, holder, 0x20, 0x0A), city = Archetype(base, holder, 0x24, 0x09);
    if (!city) {
        LOGE("MC2: the city's _bnd is not laid out as expected (holder %08X), its collision stays", holder);
        return;
    }
    static uint32_t main_paths[3] = {}, quad_paths[3] = {};
    const char* folder = kMc2Cities[c].mc2_folder;
    if (!main_paths[c]) {
        main_paths[c] = StageBound(base, folder);
        std::string qname = std::string(folder) + "_quad_0";
        quad_paths[c] = StageBound(base, qname.c_str());
    }
    auto t0 = std::chrono::steady_clock::now();
    uint32_t grid = main_paths[c] ? LoadBound(ctx, base, main_paths[c]) : 0;
    if (!grid || X86_MEM8(grid + 4) != 0x0B) {
        LOGE("MC2 (%s): .bnd did not load as an OTGRID (%08X), the city keeps its own collision", folder, grid);
        return;
    }
    X86_MEM32(city + 0xC) = grid;
    uint32_t quad = ground && quad_paths[c] ? LoadBound(ctx, base, quad_paths[c]) : 0;
    if (ground) {
        if (quad && X86_MEM8(quad + 4) == 0x0A) X86_MEM32(ground + 0xC) = quad;
        else LOGE("MC2 (%s): _quad_0.bnd did not load as a QUADTREE (%08X), the city's ground stays", folder, quad);
    }
    float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    LOGE("MC2 (%s): collision from MC2: OTGRID %08X (cells x %d..%d, z %d..%d) and QUADTREE %08X, %.1f s", folder, grid,
         int32_t(X86_MEM32(grid + 0x54)), int32_t(X86_MEM32(grid + 0x58)), int32_t(X86_MEM32(grid + 0x60)),
         int32_t(X86_MEM32(grid + 0x64)), quad, secs);
}

bool Mc2LaArcadeModulo(X86Context& ctx, uint8_t*) {
    if (!g_enabled) return false;
    ctx.esi = 14;
    return true;
}
void Mc2LaArcadeWrap(X86Context& ctx, uint8_t*) {
    if (g_enabled && ctx.ecx) ctx.ecx = 14;
}

bool Mc2LaUnlocked(X86Context& ctx, uint8_t* base) {
    return g_enabled && IsMc2Record(X86_MEM32(ctx.esp + 4));
}

bool Mc2LaAudioBank(X86Context& ctx, uint8_t* base) {
    if (!g_enabled || !IsMc2Record(X86_MEM32(X86_MEM32(0x53F2E0)))) return false;
    ctx.eax = X86_MEM32(0x4AFBC8);
    return true;
}
void Mc2LaAudioEntry(X86Context& ctx, uint8_t*) {
    if (g_enabled && IsMc2Record(ctx.eax)) ctx.eax = 0;
}

namespace {
uint32_t MakeStringEntry(uint8_t* base, const char16_t* str) {
    size_t bytes = (std::char_traits<char16_t>::length(str) + 1) * sizeof(char16_t);
    uint32_t text = mem::AllocSystem(uint32_t(bytes), 16);
    std::memcpy(base + text, str, bytes);
    uint32_t entry = mem::AllocSystem(0x18, 16);
    std::memset(base + entry, 0, 0x18);
    X86_MEM32(entry + 0x08) = text;
    X86_MEM32(entry + 0x0C) = X86_MEM32(0x447C1C);
    X86_MEM32(entry + 0x10) = X86_MEM32(0x447C1C);
    *reinterpret_cast<uint16_t*>(base + entry + 0x16) = 1;
    return entry;
}
}  // namespace

bool Mc2LaCityName(X86Context& ctx, uint8_t* base) {
    if (!g_enabled || !ctx.edi) return false;
    const char* key = reinterpret_cast<const char*>(base + ctx.edi);
    static uint32_t city_entries[3] = {};
    for (int i = 0; i < 3; i++) {
        if (_stricmp(key, kMc2Cities[i].cm_key) == 0) {
            if (!city_entries[i]) city_entries[i] = MakeStringEntry(base, kMc2Cities[i].label);
            ctx.esi = city_entries[i];
            return true;
        }
    }
    if (_stricmp(key, "CM_MC2Career") == 0) {
        static uint32_t career_entry = 0;
        if (!career_entry) career_entry = MakeStringEntry(base, u"MC2 CAREER");
        ctx.esi = career_entry;
        return true;
    }
    return false;
}

// ============================================================================
// MC2 CAREER MODE PROTOTYPE (Main Menu below Tokyo Challenge + CareerStartScreen)
// ============================================================================
namespace {
constexpr uint32_t kMc2MenuSelfTag  = 0x4D433201u;
constexpr uint32_t kMc2StageSelfTag = 0x4D433202u;
constexpr uint32_t kMc2CarSelfTag   = 0x4D433203u;

// The MC2 career borrows the retail career's own state: the career data's city (mcCareerData +0x50/+0x54, set through
// SetCurrentCity 0x27DAD0) and the profile garage (owned count 0x53F80C, selected slot 0x53F2FE, slot 0's 0x108-byte
// vehicle record at 0x53F810). Both belong to the loaded MC3 profile, which the game saves by itself (seen when the
// MC2 cruise reports its result on Go to Menus). So the retail values are taken aside when the MC2 career first
// touches them, put in place around the calls that may save (Retail() below), and put back for good when the MC2
// career is left: the retail career never starts in an MC2 city or with the MC2 pick in its garage
struct RetailCareerState {
    uint32_t city50 = 0, city54 = 0;
    uint32_t event2c = 0, event30 = 0;  // mcPgCareer +0x2C/+0x30, which SetupNextCareerEvent's MC2 path sets
    uint8_t owned = 0;
    uint16_t selected = 0;
    uint8_t slot0[0x108] = {};
};
bool g_retail_saved = false;
RetailCareerState g_retail;

uint32_t CareerData(uint8_t* base) {
    const uint32_t career = X86_MEM32(0x557F0C);
    return career ? X86_MEM32(career + 0x0C) : 0;
}

RetailCareerState ReadCareerState(uint8_t* base) {
    RetailCareerState s;
    if (const uint32_t cdata = CareerData(base)) s.city50 = X86_MEM32(cdata + 0x50), s.city54 = X86_MEM32(cdata + 0x54);
    if (const uint32_t career = X86_MEM32(0x557F0C)) s.event2c = X86_MEM32(career + 0x2C), s.event30 = X86_MEM32(career + 0x30);
    s.owned = X86_MEM8(0x53F80C);
    s.selected = X86_MEM16(0x53F2FE);
    std::memcpy(s.slot0, base + 0x53F810, sizeof(s.slot0));
    return s;
}

void WriteCareerState(X86Context& ctx, uint8_t* base, const RetailCareerState& s, bool events = true) {
    if (const uint32_t cdata = CareerData(base)) {
        if (X86_MEM32(cdata + 0x50) != s.city50) GuestCall(ctx, base, sub_0027DAD0, cdata, {s.city50});
        X86_MEM32(cdata + 0x50) = s.city50;
        X86_MEM32(cdata + 0x54) = s.city54;
    }
    const uint32_t career = X86_MEM32(0x557F0C);
    if (events && career) X86_MEM32(career + 0x2C) = s.event2c, X86_MEM32(career + 0x30) = s.event30;
    X86_MEM8(0x53F80C) = s.owned;
    X86_MEM16(0x53F2FE) = s.selected;
    std::memcpy(base + 0x53F810, s.slot0, sizeof(s.slot0));
}

// The retail career cannot be in an MC2 city (they are locked there). A profile saved before this guard existed can
// still hold one (13, 10, 11 at mcCareerData +0x50/+0x54), and its retail career would load Los Angeles without the
// MC2 renderer: such a city goes back to San Diego
void HealRetailCareerCity(X86Context& ctx, uint8_t* base) {
    const uint32_t cdata = CareerData(base);
    if (!cdata || (!IsMc2Record(X86_MEM32(cdata + 0x50)) && !IsMc2Record(X86_MEM32(cdata + 0x54)))) return;
    LOGE("MC2 Career: retail career was in MC2 city %u/%u, back to San Diego", X86_MEM32(cdata + 0x50), X86_MEM32(cdata + 0x54));
    GuestCall(ctx, base, sub_0027DAD0, cdata, {kSdRecord});
    X86_MEM32(cdata + 0x50) = kSdRecord;
    X86_MEM32(cdata + 0x54) = kSdRecord;
}

// before the MC2 career's first write
void SaveRetailCareer(uint8_t* base) {
    if (g_retail_saved) return;
    g_retail = ReadCareerState(base);
    if (IsMc2Record(g_retail.city50) || IsMc2Record(g_retail.city54)) g_retail.city50 = g_retail.city54 = kSdRecord;
    g_retail_saved = true;
    LOGE("MC2 Career: retail career state set aside (city %u/%u, %u owned, slot %u)", g_retail.city50, g_retail.city54,
         unsigned(g_retail.owned), unsigned(g_retail.selected));
}

// the MC2 career is left: the retail state goes back for good
void RestoreRetailCareer(X86Context& ctx, uint8_t* base) {
    if (!g_retail_saved) return;
    WriteCareerState(ctx, base, g_retail);
    g_retail_saved = false;
    LOGE("MC2 Career: retail career state restored (city %u)", g_retail.city50);
}

// a call that may save the profile while the MC2 career runs: it sees the retail city and garage, and the MC2 ones come
// back. events=false leaves the career's event fields as the MC2 path set them: ReportRaceResult must not report an
// MC2 result against the retail career's own current event
template <class F>
void Retail(X86Context& ctx, uint8_t* base, bool events, F&& call) {
    if (!g_retail_saved) {
        call();
        return;
    }
    const RetailCareerState mc2 = ReadCareerState(base);
    const RetailCareerState retail = g_retail;
    WriteCareerState(ctx, base, retail, events);
    call();
    g_retail = ReadCareerState(base);  // what the call changed (its own progress, a reward) is the retail state now
    if (!events) g_retail.event2c = retail.event2c, g_retail.event30 = retail.event30;
    WriteCareerState(ctx, base, mc2);
}

int g_mc2_career_city = 0;               // 0 = Los Angeles, 1 = Paris, 2 = Tokyo
int g_mc2_career_stage[3] = {0, 0, 0};   // Default to Stage 0 (Cruise Roam; 1..N = Hookman races)
uint32_t g_career_screen_self = 0;
int g_mc2_career_car = 0;

uint32_t CurrentCarLabelVa(uint8_t* base) {
    static uint32_t car_buf_va = 0;
    if (!car_buf_va) car_buf_va = mem::AllocSystem(256, 16);
    int n = int(std::size(mc2::kMc3Cars));
    if (g_mc2_career_car < 0 || g_mc2_career_car >= n) g_mc2_career_car = 0;
    const char16_t* s = mc2::kMc3Cars[g_mc2_career_car].label;
    size_t bytes = (std::char_traits<char16_t>::length(s) + 1) * sizeof(char16_t);
    std::memcpy(base + car_buf_va, s, bytes);
    return car_buf_va;
}

struct Mc2StageDesc {
    const char16_t* label;
};

constexpr Mc2StageDesc kLaStages[] = {
    {u"CRUISE: LOS ANGELES ROAM"},
    {u"MOSES #1: CHECKPOINT ONE (1/19)"},
    {u"MOSES #2: CHECKPOINT TWO (2/19)"},
    {u"STEVEN #1: CHECKPOINT ONE (3/19)"},
    {u"STEVEN #2: CHECKPOINT TWO (4/19)"},
    {u"MARIA #1: CHECKPOINT ONE (5/19)"},
    {u"MARIA #2: CHECKPOINT TWO (6/19)"},
    {u"ANGEL #1: CHECKPOINT ONE (7/19)"},
    {u"ANGEL #2: SIDERACE (8/19)"},
    {u"ANGEL #3: CHECKPOINT TWO (9/19)"},
    {u"GINA #1: CHECKPOINT ONE (10/19)"},
    {u"GINA #2: SIDERACE (11/19)"},
    {u"GINA #3: BIKE TUTORIAL (12/19)"},
    {u"GINA #4: CHECKPOINT TWO (13/19)"},
    {u"HECTOR #1: CHECKPOINT TWO (14/19)"},
    {u"HECTOR #2: CHECKPOINT THREE (15/19)"},
    {u"DICE #1: CHECKPOINT ONE (16/19)"},
    {u"DICE #2: CHECKPOINT TWO (17/19)"},
    {u"SAVO (LA) #1: CHECKPOINT ONE (18/19)"},
    {u"SAVO (LA) #2: CHECKPOINT TWO (19/19)"},
};

constexpr Mc2StageDesc kParisStages[] = {
    {u"CRUISE: PARIS ROAM"},
    {u"BLOG #1: CHECKPOINT ONE (1/16)"},
    {u"BLOG #2: CHECKPOINT TWO (2/16)"},
    {u"JULIE #1: CHECKPOINT ONE (3/16)"},
    {u"JULIE #2: CHECKPOINT TWO (4/16)"},
    {u"PRIMO #1: CHECKPOINT TWO (5/16)"},
    {u"STEPHANE #1: CHECKPOINT ONE (6/16)"},
    {u"STEPHANE #2: CHECKPOINT TWO (7/16)"},
    {u"IAN #1: CHECKPOINT ONE (8/16)"},
    {u"IAN #2: CHECKPOINT TWO (9/16)"},
    {u"IAN #3: SIDERACE (10/16)"},
    {u"FARID #1: CHECKPOINT ONE (11/16)"},
    {u"FARID #2: SIDERACE (12/16)"},
    {u"PARFAIT #1: CHECKPOINT ONE (13/16)"},
    {u"PARFAIT #2: CHECKPOINT TWO (14/16)"},
    {u"SAVO (PARIS) #1: CHECKPOINT ONE (15/16)"},
    {u"SAVO (PARIS) #2: CHECKPOINT TWO (16/16)"},
};

constexpr Mc2StageDesc kTokyoStages[] = {
    {u"CRUISE: TOKYO ROAM"},
    {u"RICKY #1: CHECKPOINT ONE (1/17)"},
    {u"RICKY #2: CHECKPOINT TWO (2/17)"},
    {u"SHING #1: CHECKPOINT ONE (3/17)"},
    {u"SHING #2: SIDERACE (4/17)"},
    {u"SHING #3: CHECKPOINT TWO (5/17)"},
    {u"HALEY #1: CHECKPOINT ONE (6/17)"},
    {u"HALEY #2: CHECKPOINT TWO (7/17)"},
    {u"NIKKO #1: CHECKPOINT ONE (8/17)"},
    {u"NIKKO #2: CHECKPOINT TWO (9/17)"},
    {u"ZEN #1: CHECKPOINT ONE (10/17)"},
    {u"ZEN #2: CHECKPOINT TWO (11/17)"},
    {u"KENICHI #1: CHECKPOINT ONE (12/17)"},
    {u"KENICHI #2: CHECKPOINT THREE (13/17)"},
    {u"MAKOTO #1: CHECKPOINT ONE (14/17)"},
    {u"MAKOTO #2: CHECKPOINT TWO (15/17)"},
    {u"SAVO (TOKYO) #1: CHECKPOINT ONE (16/17)"},
    {u"SAVO (TOKYO) #2: CHECKPOINT TWO (17/17)"},
};

int StageCount(int city_idx) {
    return city_idx == 1 ? int(std::size(kParisStages))
         : city_idx == 2 ? int(std::size(kTokyoStages))
                         : int(std::size(kLaStages));
}

[[maybe_unused]] const char16_t* StageLabel(int city_idx, int stage_idx) {
    int count = StageCount(city_idx);
    if (stage_idx < 0 || stage_idx >= count) stage_idx = 0;
    return city_idx == 1 ? kParisStages[stage_idx].label
         : city_idx == 2 ? kTokyoStages[stage_idx].label
                         : kLaStages[stage_idx].label;
}

[[maybe_unused]] uint32_t CurrentStageLabelVa(uint8_t* base) {
    static uint32_t buf_va = 0;
    if (!buf_va) buf_va = mem::AllocSystem(256, 16);
    const wchar_t* stage_arg = Mc3DebugArg(L"--mc3-mc2-stage");
    const char16_t* s = nullptr;
    if (stage_arg) {
        s = StageLabel(g_mc2_career_city, g_mc2_career_stage[g_mc2_career_city]);
    } else {
        s = mc2::GetCurrentStageLabel();
    }
    size_t bytes = (std::char_traits<char16_t>::length(s) + 1) * sizeof(char16_t);
    std::memcpy(base + buf_va, s, bytes);
    return buf_va;
}
}  // namespace

namespace mc2 {
int GetSelectedMenuStage(int city_idx) {
    if (city_idx < 0 || city_idx >= 3) return 0;
    return g_mc2_career_stage[city_idx];
}
}

void Mc2ApplyDebugBootArgs() {
    const wchar_t* city = Mc3DebugArg(L"--mc3-mc2-city");
    if (city) {
        if (!_wcsicmp(city, L"la") || !_wcsicmp(city, L"losangeles")) g_mc2_career_city = 0;
        else if (!_wcsicmp(city, L"paris")) g_mc2_career_city = 1;
        else if (!_wcsicmp(city, L"tokyo")) g_mc2_career_city = 2;
        else g_mc2_career_city = std::clamp(_wtoi(city), 0, 2);
    }
    if (const wchar_t* stage = Mc3DebugArg(L"--mc3-mc2-stage"))
        g_mc2_career_stage[g_mc2_career_city] = std::clamp(_wtoi(stage), 0, StageCount(g_mc2_career_city) - 1);
    if (const wchar_t* car = Mc3DebugArg(L"--mc3-mc2-car"))
        g_mc2_career_car = std::clamp(_wtoi(car), 0, int(std::size(kMc3Cars)) - 1);
    LOGE("MC3 boot args: MC2 city=%s(%d) stage=%d car=%s(%d)",
         kMc2Cities[g_mc2_career_city].name, g_mc2_career_city,
         g_mc2_career_stage[g_mc2_career_city], kMc3Cars[g_mc2_career_car].name, g_mc2_career_car);
    if (Mc3DebugArg(L"--mc2-test-state")) {
        int rc = mc2::RunCareerStateMachineTests();
        std::exit(rc);
    }
}

// 1. MC2 CAREER is a main menu row of its own, below Tokyo Challenge; the retail Career row stays as it is.
// mcMenuModeSelect::PopulateMenu (0x18C770) adds Career (callback 0x18C560), Tokyo Challenge (0x18C580) and Arcade
// (0x18C530) through mcUiListMenu::AddEntry; AddEntry's item insert (0x348BE0) appends at the list's count, its first
// argument is only the id the item keeps, so a second AddEntry right after Tokyo Challenge's lands between it and
// Arcade. Its callback is the retail Career's own (which does not read this: screen 0x1C, 0x50FFC8 = 0) with a tagged
// this, and only that tag turns the MC2 career on (2.)
X86_FUNC(sub_00349A40) {
    const uint32_t list = ctx.ecx;
    uint32_t args[9];  // id, 0, 0, the callback {this, function, 0, 1}, 2, text
    for (int i = 0; i < 9; i++) args[i] = X86_MEM32(ctx.esp + 4 + 4 * uint32_t(i));
    if (args[4] == 0x18C560u && args[3] != kMc2MenuSelfTag && g_mc2_career_mode) {
        // the main menu is built again: whichever career was on has been left
        g_mc2_career_mode = false;
        mc2::SetCareerModeActive(false);
    }
    if (args[4] == 0x18C560u && args[3] != kMc2MenuSelfTag) RestoreRetailCareer(ctx, base);
    __imp__sub_00349A40(ctx, base);
    if (g_enabled && args[4] == 0x18C580u) {
        static uint32_t text_va = 0;
        if (!text_va) {
            static const char16_t kText[] = u"MC2 CAREER";
            text_va = mem::AllocSystem(sizeof(kText), 16);
            std::memcpy(base + text_va, kText, sizeof(kText));
        }
        GuestCall(ctx, base, __imp__sub_00349A40, list,
                  {0u, args[1], args[2], kMc2MenuSelfTag, 0x18C560u, args[5], args[6], args[7], text_va});
        LOGE("MC2 Career: injected 'MC2 CAREER' below 'TOKYO CHALLENGE' in Main Menu");
    }
}

// 2. The retail Career row and the MC2 CAREER row share the callback; the MC2 row's tagged this tells them apart.
// Tokyo Challenge (0x18C580) and Arcade (0x18C530) turn the MC2 career off as well
X86_FUNC(sub_0018C560) {
    g_mc2_career_mode = g_enabled && ctx.ecx == kMc2MenuSelfTag;
    mc2::SetCareerModeActive(g_mc2_career_mode);
    if (!g_mc2_career_mode) RestoreRetailCareer(ctx, base);
    LOGE("MC2 Career: OnSelectCareer (mc2_career_mode=%d)", int(g_mc2_career_mode));
    __imp__sub_0018C560(ctx, base);
}

X86_FUNC(sub_0018C580) {
    g_mc2_career_mode = false;
    mc2::SetCareerModeActive(false);
    RestoreRetailCareer(ctx, base);
    X86_MEM8(0x50FFC8) = 1;
    __imp__sub_0018C580(ctx, base);
}

X86_FUNC(sub_0018C530) {
    g_mc2_career_mode = false;
    mc2::SetCareerModeActive(false);
    RestoreRetailCareer(ctx, base);
    __imp__sub_0018C530(ctx, base);
}

// 3. CareerStartScreen::OnEnter (0x184510): pre-select active MC2 city and ensure owned vehicle
X86_FUNC(sub_00184510) {
    if (g_enabled && g_mc2_career_mode) {
        SaveRetailCareer(base);
        X86_MEM8(0x50FFC8) = 0;
        uint32_t rec = kMc2Cities[g_mc2_career_city].record;
        uint32_t career = X86_MEM32(0x557F0C);
        if (career && X86_MEM32(career + 0x0C)) {
            uint32_t cdata = X86_MEM32(career + 0x0C);
            X86_MEM32(cdata + 0x50) = rec;
            X86_MEM32(cdata + 0x54) = rec;
        }
        // Ensure profile has at least 1 owned vehicle so Row 0 generates START CAREER instead of PURCHASE A VEHICLE
        if (X86_MEM8(0x53F80C) == 0) {
            X86_MEM8(0x53F80C) = 1;
            X86_MEM16(0x53F2FE) = 0;
            std::memcpy(base + 0x53F810, base + kMc3Cars[g_mc2_career_car].record_va, 0x108);
            LOGE("MC2 Career: initialized garage slot 0 with vehicle %d", g_mc2_career_car);
        }
    } else if (g_enabled) {
        HealRetailCareerCity(ctx, base);
    }
    __imp__sub_00184510(ctx, base);
    if (g_enabled && !g_mc2_career_mode) HealRetailCareerCity(ctx, base);  // the screen may load it from the profile
}

// 4. mcUiScreen::SetHeaderTitle (0x154E40): show "MC2 CAREER" header when in MC2 Career Mode
X86_FUNC(sub_00154E40) {
    if (g_enabled && g_mc2_career_mode && X86_MEM32(ctx.esp + 4) == 0x4612B0u) {
        static uint32_t key_va = 0;
        if (!key_va) {
            static const char kKey[] = "CM_MC2Career";
            key_va = mem::AllocSystem(sizeof(kKey), 16);
            std::memcpy(base + key_va, kKey, sizeof(kKey));
        }
        X86_MEM32(ctx.esp + 4) = key_va;
    }
    __imp__sub_00154E40(ctx, base);
}

// 5. mcPgUnlockingRulesEval::IsCityUnlockedForCareer (0x289CE0): unlock MC2 cities & city selector row
X86_FUNC(sub_00289CE0) {
    if (g_enabled && g_mc2_career_mode) {
        uint32_t city = X86_MEM32(ctx.esp + 4);
        bool unlocked = (city == 1 || IsMc2Record(city));
        ctx.eax = (ctx.eax & 0xFFFFFF00u) | (unlocked ? 1u : 0u);
        ctx.esp += 8;
        return;
    }
    __imp__sub_00289CE0(ctx, base);
}

// 6. CareerStartScreen::PopulateMenu (0x1848E0) & AddMenuCommandRow (0x1558A0): inject Stage & Car Selector rows
X86_FUNC(sub_001848E0) {
    g_career_screen_self = ctx.ecx;
    __imp__sub_001848E0(ctx, base);
}

X86_FUNC(sub_001558A0) {
    if (g_enabled && g_mc2_career_mode && X86_MEM32(ctx.esp + 0x10) == 0x461300u) {
        // Let City be added as Row 1
        __imp__sub_001558A0(ctx, base);

        // Append MC3 Car Selector as Row 2
        uint32_t mgr = X86_MEM32(0x50F148);
        uint32_t screen = mgr ? X86_MEM32(mgr + 0x0C) : 0;
        uint32_t list_menu = screen ? X86_MEM32(screen + 0xFC) : 0;
        if (list_menu) {
            GuestCall(ctx, base, __imp__sub_00349A40, list_menu,
                      {2u, 0u, 2u, kMc2CarSelfTag, 0x184230u, 1u, 2u, 2u, CurrentCarLabelVa(base)});
            ctx.ebx += 1;
        }
        return;
    }
    __imp__sub_001558A0(ctx, base);
}

// 7. CareerStartScreen::StepCity (0x184230): step MC2 City (Row 1) or MC3 Vehicle (Row 2)
X86_FUNC(sub_00184230) {
    if (g_enabled && g_mc2_career_mode) {
        SaveRetailCareer(base);
        int32_t delta = int32_t(X86_MEM32(ctx.esp + 4));
        if (delta == 0) delta = 1;
        uint32_t screen = ctx.ecx;
        bool selector_tag = (screen == kMc2CarSelfTag);
        uint32_t real_screen = selector_tag ? g_career_screen_self : screen;
        uint32_t list_menu = real_screen ? X86_MEM32(real_screen + 0xFC) : 0;
        int32_t cursor = list_menu ? int32_t(X86_MEM32(list_menu + 0x25C)) : 1;

        if (cursor == 2 || selector_tag) {
            int n = int(std::size(mc2::kMc3Cars));
            g_mc2_career_car = (g_mc2_career_car + delta % n + n) % n;
            std::memcpy(base + 0x53F810, base + mc2::kMc3Cars[g_mc2_career_car].record_va, 0x108);
            mc2::GetCareerState().SetSelectedCar(g_mc2_career_car);
            LOGE("MC2 Career: stepped vehicle to %s (index %d)", mc2::kMc3Cars[g_mc2_career_car].name, g_mc2_career_car);
            if (real_screen) GuestCall(ctx, base, sub_001848E0, real_screen, {});
        } else {
            g_mc2_career_city = (g_mc2_career_city + delta % 3 + 3) % 3;
            mc2::GetCareerState().SetCurrentCity(static_cast<mc2::CityId>(g_mc2_career_city));
            uint32_t rec = kMc2Cities[g_mc2_career_city].record;
            X86_MEM32(X86_MEM32(0x53F2E4)) = rec;
            uint32_t career = X86_MEM32(0x557F0C);
            if (career && X86_MEM32(career + 0x0C)) {
                uint32_t cdata = X86_MEM32(career + 0x0C);
                GuestCall(ctx, base, sub_0027DAD0, cdata, {rec});
                X86_MEM32(cdata + 0x50) = rec;
                X86_MEM32(cdata + 0x54) = rec;
            }
            LOGE("MC2 Career: stepped city to %s (record %u)", kMc2Cities[g_mc2_career_city].name, rec);
            if (real_screen) GuestCall(ctx, base, sub_001848E0, real_screen, {});
        }
        ctx.esp += 8;  // ret 4
        return;
    }
    __imp__sub_00184230(ctx, base);
}

// 8. CareerStartScreen::OnSelectStartNewOrContinue (0x184770) & SetupNextCareerEvent (0x278270)
X86_FUNC(sub_00184770) {
    if (g_enabled && g_mc2_career_mode) {
        SaveRetailCareer(base);
        mc2::SetCareerModeActive(true);
        uint32_t rec = kMc2Cities[g_mc2_career_city].record;
        uint32_t career = X86_MEM32(0x557F0C);
        if (career && X86_MEM32(career + 0x0C)) {
            uint32_t cdata = X86_MEM32(career + 0x0C);
            X86_MEM32(cdata + 0x50) = rec;
            X86_MEM32(cdata + 0x54) = rec;
        }
        X86_MEM8(0x50FFC8) = 0;
        // Ensure owned vehicle count >= 1 and garage slot 0 is populated with selected MC3 vehicle
        X86_MEM8(0x53F80C) = 1;
        X86_MEM16(0x53F2FE) = 0;
        std::memcpy(base + 0x53F810, base + kMc3Cars[g_mc2_career_car].record_va, 0x108);
        LOGE("MC2 Career: OnSelectStartNewOrContinue: launching city=%s (record %u), car=%s",
             kMc2Cities[g_mc2_career_city].name, rec, kMc3Cars[g_mc2_career_car].name);
    }
    __imp__sub_00184770(ctx, base);
}

bool Mc2CityFirstDrawReady() {
    return g_mc2_city_first_draw_ready.load(std::memory_order_acquire);
}

// mcGameState::EnterStateMC3Frontend (0x21410, state 7 of EnterState 0x21D30): the frontend keeps the city of the
// next race config as its background, and reloads only when current and next differ in city, time or weather. A
// city record with a content mask at +0x4C (the downloadable dlc1..dlc6) is no frontend city: the frontend sets the
// next config to "sd" itself (0x215FB, mcRaceConfig::SetCity 0x26B050). An MC2 city is no frontend city either (the
// menu's camera looks at San Diego's tower, and MC3's road helpers would run on the MC2 roads), so the same is done
// for it here; its mask stays 0, which keeps it in every city list. The MC2 city goes off once San Diego's own city
// files open (Mc2LaPaths), after the Los Angeles layer is unloaded with the MC2 guards still on
X86_FUNC(sub_00021410) {
    const uint32_t next = X86_MEM32(0x53F2E4);
    if (g_enabled && next && g_mc2_active_city.load(std::memory_order_acquire) >= 0) {
        const uint32_t cities = X86_MEM32(kCities);
        const uint32_t idx = X86_MEM32(next);
        if (cities && IsMc2Record(idx)) {
            GuestCall(ctx, base, sub_0026B050, next, {0x448EC0u});  // "sd"
            g_mc2_leaving.store(true, std::memory_order_release);
            LOGE("MC2 city: frontend background %s -> sd", reinterpret_cast<const char*>(base + X86_MEM32(cities + idx * kRecordSize)));
        }
    }
    Retail(ctx, base, true, [&] { __imp__sub_00021410(ctx, base); });
}

X86_FUNC(sub_00278270) {
    if (g_enabled && g_mc2_career_mode) {
        SaveRetailCareer(base);
        int career_city = mc2::GetCurrentCareerCity();
        g_mc2_career_city = career_city;
        g_mc2_leaving.store(false, std::memory_order_release);
        const int previous_city = g_mc2_active_city.exchange(career_city, std::memory_order_acq_rel);
        if (previous_city != career_city) {
            LOGE("MC2 city: active = %s (%d)", kMc2Cities[career_city].name, career_city);
        }

        if (mc2::HandleSetupNextCareerEvent(ctx, base)) {
            ctx.esp += 4;
            return;
        }

        uint32_t career_esi = ctx.ecx;
        uint32_t rec = kMc2Cities[g_mc2_career_city].record;
        int stage = g_mc2_career_stage[g_mc2_career_city];

        uint32_t rc = X86_MEM32(0x53F2E4);
        if (rc) {
            if (X86_MEM32(rc + 0x120)) {
                GuestCall(ctx, base, sub_00272010, X86_MEM32(rc + 0x120), {kMc3Cars[g_mc2_career_car].record_va});
                X86_MEM8(0x53F2FC) = 1;
                X86_MEM8(0x50F131) = 0;
            }
            uint32_t race_arg = (stage <= 0) ? 0xFFFFFFFFu : uint32_t(stage);
            GuestCall(ctx, base, sub_0026B8E0, rc, {rec, race_arg});
            X86_MEM32(rc + 0x0C) = 4;
            X86_MEM32(rc + 0x10) = 4;
            X86_MEM32(rc + 0x3D4) = 0xFFFFFFFFu;
            X86_MEM32(rc + 0x40) = 8;
            X86_MEM8(rc + 0x10F) = 1; // ensure city resource loading is enabled
            const char* rname = reinterpret_cast<const char*>(base + rc + 0x68);
            LOGE("MC2 Career: launching city=%s (record %u), stage=%d, vehicle=%s, race='%s'",
                 kMc2Cities[g_mc2_career_city].name, rec, stage, kMc3Cars[g_mc2_career_car].name, rname);
        }
        X86_MEM32(career_esi + 0x2C) = (stage <= 0) ? 3u : 4u;
        X86_MEM32(career_esi + 0x30) = rec;

        // NOTE: Premature progression advancement removed as required by MC3-0007!
        ctx.esp += 4;
        return;
    }
    __imp__sub_00278270(ctx, base);
}

X86_FUNC(sub_00277EE0) {
    if (g_enabled && g_mc2_career_mode) {
        mc2::HandleReportRaceResult(ctx, base);
        Retail(ctx, base, false, [&] { __imp__sub_00277EE0(ctx, base); });  // the retail report saves the profile
        return;
    }
    __imp__sub_00277EE0(ctx, base);
}
