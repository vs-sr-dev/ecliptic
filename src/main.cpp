// Ecliptic entry point: loads a Tapwave Native Application and runs it.
#include "palmos.h"
#include "host.h"
#include "gfx.h"
#include "audio.h"
#include <chrono>
#include <thread>
#include <cstdlib>
#include "m68k.h"
#include "tna.h"

Emu g;
void glue_report();
bool rom_load(const std::string& path);
void rom_hook_entries();
uint32_t dal_init();
void midi_shutdown();

static auto t0 = std::chrono::steady_clock::now();
void request_stop(const char* why) {
    if (g.stop_requested) return;
    g.stop_time = now_ms();
    g.stop_requested = true;
    LOG("%s: asking the application to quit", why);
}
uint32_t now_ms() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void usage() {
    fprintf(stderr,
            "usage: ecliptic GAME.prc [options]\n"
            "  --card DIR          host directory mounted as the SD card (default: card)\n"
            "  --rom FILE          Zodiac ROM image (default: ref/roms/zodiac_1.1.rom)\n"
            "  --ram DIR           where databases created by the game are saved (default: ram)\n"
            "  --install FILE      extra PRC/PDB to load into RAM (repeatable)\n"
            "  --trace             log every TwGlue call\n"
            "  --no-frame-cap      do not cap full-screen display updates at 60 per second\n"
            "  --strict            stop at the first unimplemented call\n"
            "  --headless          no window\n"
            "  --scale N           window scale (default 2)\n"
            "  --exit-after MS     quit after MS milliseconds\n"
            "  --screenshot FILE   save a BMP of the screen on exit\n"
            "  --keys LIST         scripted keys, e.g. \"R,R,L,A\"\n"
            "  --key-start MS      first scripted key time (default 1000)\n"
            "  --key-interval MS   time between scripted keys (default 500)\n");
    exit(1);
}

int main(int argc, char** argv) {
    std::vector<std::string> installs;
    std::string ram_dir = "ram";
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (s == "--card") g.opt.card_dir = next();
        else if (s == "--rom") g.opt.rom_path = next();
        else if (s == "--ram") ram_dir = next();
        else if (s == "--install") installs.push_back(next());
        else if (s == "--trace") g.opt.trace = true;
        else if (s == "--no-68k") g.opt.no68k = true;
        else if (s == "--no-frame-cap") g.opt.frame_cap = false;
        else if (s == "--strict") g.opt.strict = true;
        else if (s == "--headless") g.opt.headless = true;
        else if (s == "--scale") g.opt.scale = atoi(next().c_str());
        else if (s == "--exit-after") g.opt.exit_after_ms = atol(next().c_str());
        else if (s == "--screenshot") g.opt.screenshot = next();
        else if (s == "--keys") g.opt.keys = next();
        else if (s == "--blocks") g.opt.blocks_file = next();
        else if (s == "--key-start") g.opt.key_start = atoi(next().c_str());
        else if (s == "--key-interval") g.opt.key_interval = atoi(next().c_str());
        else if (s[0] == '-') usage();
        else g.opt.prc_path = s;
    }
    if (g.opt.prc_path.empty()) usage();

    cpu_init();
    heap::init();
    dm::set_ram_dir(ram_dir);

    // ROM databases first, so the application's database is searched before them.
    {
        std::vector<std::string> cands;
        if (!g.opt.rom_path.empty()) cands.push_back(g.opt.rom_path);
        cands.push_back("ref/roms/zodiac_1.1.rom");
        std::string exe = argv[0];
        size_t sl = exe.find_last_of("/\\");
        if (sl != std::string::npos) cands.push_back(exe.substr(0, sl) + "/../ref/roms/zodiac_1.1.rom");
        bool ok = false;
        for (auto& c : cands)
            if ((ok = rom_load(c))) break;
        if (!ok) LOG("warning: no Zodiac ROM found (use --rom); system resources and some DRM checks unavailable");
    }
    Db* app = dm::load_file(g.opt.prc_path, false);
    if (!app) { LOG("cannot load %s", g.opt.prc_path.c_str()); return 1; }
    for (auto& f : installs) if (!dm::load_file(f, false)) LOG("cannot load %s", f.c_str());
    host_load_ram_dir(ram_dir);
    dm::set_app_db(app);
    LOG("app '%s' type %s creator %s, %zu resources", app->name.c_str(), fourcc_str(app->type).c_str(),
        fourcc_str(app->creator).c_str(), app->recs.size());

    if (!tna::has_native_code(app, 1)) {
        LOG("not a Tapwave Native Application (no ARMC/ARMZ #1)");
        return 1;
    }

    glue_init();
    rom_hook_entries();
    set_reg(9, dal_init());   // Palm OS 5 system globals: module dispatch tables at [r9-4n]
    host_init();
    gfx::init();
    audio::init();
    dm::open(app, 1);   // the running application's database is open (read-only)
    m68k::init();
    tna::init(glue_extra_trap("x_Call68KFunc", [](Args& a) -> uint32_t { return m68k::call68k(a[1], a[2], a[3]); }));

    // Watchdog: --exit-after even if the game never calls the OS, and the hard stop when an
    // application has not quit 3 s after the stop request.
    std::thread([] {
        while (!g.done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (g.opt.exit_after_ms >= 0 && now_ms() >= (uint32_t)g.opt.exit_after_ms) request_stop("exit-after reached");
            if (g.stop_requested && !g.quit && !g.done && now_ms() - g.stop_time > 3000) {
                LOG("watchdog: the application did not quit, stopping the CPU (pc=code+%x)", reg(15) - g.code_base);
                g.quit = true;
                g.watchdog_fired = true;
                uc_emu_stop(g.uc);
            }
        }
    }).detach();

    // Normal launch: the 68k code #1 sets things up and starts the native code with
    // TwLoadModule. Fall back to starting the native code directly if it does not.
    if (!g.opt.no68k) m68k::run_app(app);
    if (m68k::faulted() && !tna::launched()) g.quit = false;   // fall back below
    if (!g.quit && !tna::launched()) {
        if (!g.opt.no68k) LOG("68k launch code did not start the native code; starting it directly");
        uint32_t entry = tna::load(app, 0, 1);
        if (entry) tna::run(entry, 0);
    }
    g.done = true;
    if (getenv("ECLIPTIC_DUMP_SURFACES")) gfx::debug_dump_surfaces();
    midi_shutdown();
    audio::shutdown();
    host_shutdown();
    dm::save_all();
    if (!g.opt.blocks_file.empty()) cpu_dump_blocks(g.opt.blocks_file);
    glue_report();
    return 0;
}
