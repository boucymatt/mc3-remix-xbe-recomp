// headless host: <exe> <game folder> [--instance N], run from the folder that gets logs, hdd and cap
// the game folder may come from GAME_DIR instead
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#if XBE_LINUX_WINDOW
#include "platform.h"
#endif

#if XBE_HEADLESS || XBE_LINUX_WINDOW
extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

static std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}
#endif

static void Usage(const char* executable, FILE* out) {
    std::fprintf(out,
                 "usage: %s <game folder> [--instance N] [--mc3-option[=value] ...] [--mc2-test-state]\n"
                 "       %s --help\n"
                 "The game folder must contain " IMAGE_XBE " and its extracted game data.\n"
                 "GAME_DIR can supply the game folder. Linux SDL/OpenGL or headless runtime, according to the build preset.\n"
                 "--mc3-guest-memory=64|128 selects Xbox memory size (default 64 MB).\n",
                 executable, executable);
}

int main(int argc, char** argv) {
    std::string game;
    uint32_t guest_memory_mb = 64;
    bool positional = false;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (!positional && (!std::strcmp(arg, "--help") || !std::strcmp(arg, "-h"))) {
            Usage(argv[0], stdout);
            return 0;
        }
        if (!positional && !std::strcmp(arg, "--")) {
            positional = true;
            continue;
        }
        if (!positional && !std::strcmp(arg, "--instance")) {
            if (++i == argc) {
                std::fprintf(stderr, "--instance requires a nonnegative integer\n");
                return 2;
            }
            int instance;
            const char* end = argv[i] + std::strlen(argv[i]);
            auto parsed = std::from_chars(argv[i], end, instance);
            if (parsed.ec != std::errc{} || parsed.ptr != end || instance < 0) {
                std::fprintf(stderr, "--instance requires a nonnegative integer\n");
                return 2;
            }
            setenv("INSTANCE", argv[i], 1);
        } else if (!positional && !std::strncmp(arg, "--mc3-guest-memory=", 19)) {
            const char* value = arg + 19;
            if (!std::strcmp(value, "64")) guest_memory_mb = 64;
            else if (!std::strcmp(value, "128")) guest_memory_mb = 128;
            else {
                std::fprintf(stderr, "--mc3-guest-memory must be 64 or 128\n");
                return 2;
            }
        } else if (!positional && (!std::strncmp(arg, "--mc3-", 6) || !std::strncmp(arg, "--mc2-", 6))) {
            // Game diagnostics and renderer options read the original command line themselves.
            continue;
        } else if (!positional && arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg);
            Usage(argv[0], stderr);
            return 2;
        } else if (game.empty()) {
            game = arg;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", arg);
            Usage(argv[0], stderr);
            return 2;
        }
    }
    // a relaunch into another image comes back without arguments
    if (game.empty() && std::getenv("GAME_DIR")) game = std::getenv("GAME_DIR");
    if (game.empty()) {
        Usage(argv[0], stderr);
        return 2;
    }
    const auto image = std::filesystem::path(game) / IMAGE_XBE;
    std::ifstream f(image, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot read game image: %s\n", image.string().c_str());
        return 1;
    }
    std::vector<uint8_t> xbe((std::istreambuf_iterator<char>(f)), {});
    if (f.bad() || xbe.empty()) {
        std::fprintf(stderr, "game image is empty or unreadable: %s\n", image.string().c_str());
        return 1;
    }
#if XBE_HEADLESS || XBE_LINUX_WINDOW
    const auto sha = Sha1(xbe);
    if (sha != kImageSha1) {
        std::fprintf(stderr, "game image does not match this build: %s\nexpected SHA1: %s\nactual SHA1:   %s\n",
                     image.string().c_str(), kImageSha1, sha.c_str());
        return 1;
    }
#endif
    setenv("GAME_DIR", game.c_str(), 1);
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    mem::SetPhysicalSize(guest_memory_mb << 20);
    if (app::kGame.init) app::kGame.init();
#if XBE_LINUX_WINDOW
    if (!linuxapp::Init()) return 1;
#endif
    if (app::kGame.window) app::kGame.window(app::Window());
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game, (app::ExeDir() / hdd).string())) return 1;
#if XBE_LINUX_WINDOW
    linuxapp::Run();
#endif
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        logx::Flush();
    }
}
