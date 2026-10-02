// Ecliptic entry point: loads a Tapwave Native Application and runs it.
#include "palmos.h"
#include "host.h"
#include "gfx.h"
#include "audio.h"
#include <chrono>
#include <thread>
#include <cstdlib>
#include <zlib.h>

Emu g;
void glue_report();
bool rom_load(const std::string& path);
void midi_shutdown();

static auto t0 = std::chrono::steady_clock::now();
uint32_t now_ms() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static std::vector<uint8_t> gunzip(const uint8_t* p, uint32_t n) {
    std::vector<uint8_t> out;
    z_stream z{};
    inflateInit2(&z, 16 + MAX_WBITS);
    z.next_in = const_cast<uint8_t*>(p);
    z.avail_in = n;
    uint8_t buf[65536];
    int r;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (r == Z_OK);
    inflateEnd(&z);
    if (r != Z_STREAM_END) LOG("!! gunzip failed (%d)", r);
    return out;
}

static void usage() {
    fprintf(stderr,
            "usage: ecliptic GAME.prc [options]\n"
            "  --card DIR          host directory mounted as the SD card (default: card)\n"
            "  --rom FILE          Zodiac ROM image (default: ref/roms/zodiac_1.1.rom)\n"
            "  --ram DIR           where databases created by the game are saved (default: ram)\n"
            "  --install FILE      extra PRC/PDB to load into RAM (repeatable)\n"
            "  --trace             log every TwGlue call\n"
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

    // Locate the native code: ARMC #1 or gzipped ARMZ #1.
    std::vector<uint8_t> code;
    for (auto& r : app->recs) {
        if (r.id != 1) continue;
        uint32_t p = heap::deref(r.h), n = heap::size(p);
        if (r.type == fourcc("ARMC")) code.assign(host_ptr(p, n), host_ptr(p, n) + n);
        if (r.type == fourcc("ARMZ")) code = gunzip(host_ptr(p, n), n);
    }
    if (code.size() < 72 || memcmp(&code[28], "ANWT", 4) != 0) {
        LOG("not a Tapwave Native Application (no ARMC/ARMZ #1 with TWNA header)");
        return 1;
    }
    g.code_size = (uint32_t)code.size();
    g.code_base = heap::alloc(g.code_size);
    mem_write(g.code_base, code.data(), g.code_size);
    LOG("TNA code: %u bytes at %08x", g.code_size, g.code_base);

    glue_init();
    cpu_fix_bx_pc_veneers(g.code_base, g.code_size);
    if (!g.opt.blocks_file.empty()) cpu_trace_blocks(g.code_base, g.code_size);
    host_init();
    gfx::init();
    audio::init();
    dm::open(app, 1);   // the running application's database is open (read-only)

    uint32_t call68k = glue_extra_trap("x_Call68KFunc", [](Args& a) -> uint32_t {
        LOG("!! Call68KFunc(emulState=%x, trap/addr=%x, args=%x, size=%x) not supported", a[0], a[1], a[2], a[3]);
        return 0;
    });
    uint32_t emul_state = heap::alloc(64, true);

    // Watchdog: stops the CPU even if the game spins without calling the OS.
    if (g.opt.exit_after_ms >= 0) {
        std::thread([] {
            while (now_ms() < (uint32_t)g.opt.exit_after_ms + 1500) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!g.done) {
                LOG("watchdog: stopping CPU (pc=code+%x)", reg(15) - g.code_base);
                g.quit = true;
                g.watchdog_fired = true;
                uc_emu_stop(g.uc);
            }
        }).detach();
    }

    // 1st call: emulStateP == NULL, userData = TwGlue table (relocation + runtime init)
    uint32_t r = cpu_call(g.code_base, {0, g.glue_table, call68k});
    LOG("TNA init returned %u", r);
    // 2nd call: normal launch -> PilotMain(sysAppLaunchCmdNormalLaunch, NULL, 0)
    if (!g.quit) {
        r = cpu_call(g.code_base, {emul_state, 0, call68k});
        LOG("PilotMain returned %u", r);
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
