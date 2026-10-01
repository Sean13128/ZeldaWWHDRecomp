// Frame interpolation (60 fps output, game logic unchanged at 30 steps per second).
//
// With interpolation on, the main loop body runs every vsync (swap interval halved) but the game
// logic only on every other pass:
//   logic pass: logic advances N -> N+1; everything is drawn with the camera halfway (N, N+1)
//   hold pass:  no logic (no execute/create/delete, scene management, counters, audio);
//               everything is drawn again with the camera at N+1
// The painter at the start of each pass renders the previous pass's draw lists, so the screen shows
// halfway(N,N+1), N+1, halfway(N+1,N+2), N+2, ...
//
// Main loop functions in WWHD: see tools/recomp/hooks.txt and docs/decomp-notes.md.
// Camera layout (camera_draw, 024FFC40): near +0xCC, far +0xD0, fovy +0xD4, aspect +0xD8,
// eye +0xDC, center +0xE8, up +0xF4, bank (s16) +0x100.
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "runtime.h"

extern "C" {
void f_025F172C_orig(Cpu* c);  // main loop body
void f_025D42EC(Cpu* c);       // fapGm_Execute
void f_025DE788_orig(Cpu* c);  // fpcEx_Handler
void f_025DE024_orig(Cpu* c);  // fpcDt_Handler
void f_025E0EE4_orig(Cpu* c);  // fpcPi_Handler
void f_025DDCEC_orig(Cpu* c);  // fpcCt_Handler
void f_025D42C4_orig(Cpu* c);  // fapGm_After
void f_0200E6EC_orig(Cpu* c);  // cCt_Counter
void f_024FFC40_orig(Cpu* c);  // camera_draw
}

namespace interp {

static std::atomic<bool> g_on{[] { const char* e = getenv("WWHD_INTERP"); return e && atoi(e) != 0; }()};
bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool v) { g_on = v; LOG("[interp] frame interpolation %s", v ? "on (60 fps)" : "off"); }

// GX2SetSwapInterval: two paints per logic step need half the interval
uint32_t effective_swap_interval(uint32_t game) { return enabled() ? std::max<uint32_t>(1, game / 2) : game; }

namespace {
constexpr uint32_t kEye = 0xDC, kCenter = 0xE8, kUp = 0xF4, kFovy = 0xD4, kBank = 0x100;

struct CamState {
    float eye[3], center[3], up[3], fovy;
    int16_t bank;
};

CamState read_cam(uint32_t cam) {
    CamState s;
    for (int i = 0; i < 3; i++) {
        s.eye[i] = (float)ldf32(cam + kEye + 4 * i);
        s.center[i] = (float)ldf32(cam + kCenter + 4 * i);
        s.up[i] = (float)ldf32(cam + kUp + 4 * i);
    }
    s.fovy = (float)ldf32(cam + kFovy);
    s.bank = (int16_t)ld16(cam + kBank);
    return s;
}

void write_cam(uint32_t cam, const CamState& s) {
    for (int i = 0; i < 3; i++) {
        stf32(cam + kEye + 4 * i, s.eye[i]);
        stf32(cam + kCenter + 4 * i, s.center[i]);
        stf32(cam + kUp + 4 * i, s.up[i]);
    }
    stf32(cam + kFovy, s.fovy);
    st16(cam + kBank, (uint16_t)s.bank);
}

float dist(const float* a, const float* b) {
    float d = 0;
    for (int i = 0; i < 3; i++) d += (a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(d);
}

// halfway state; a cut (large jump) is not blended
CamState blend(const CamState& a, const CamState& b) {
    static const float kCut = getenv("WWHD_INTERP_CUT") ? (float)atof(getenv("WWHD_INTERP_CUT")) : 800.0f;
    if (dist(a.eye, b.eye) > kCut || dist(a.center, b.center) > kCut || std::fabs(a.fovy - b.fovy) > 20.0f) return b;
    CamState m;
    for (int i = 0; i < 3; i++) {
        m.eye[i] = 0.5f * (a.eye[i] + b.eye[i]);
        m.center[i] = 0.5f * (a.center[i] + b.center[i]);
        m.up[i] = 0.5f * (a.up[i] + b.up[i]);
    }
    m.fovy = 0.5f * (a.fovy + b.fovy);
    m.bank = (int16_t)(a.bank + (int16_t)(b.bank - a.bank) / 2);  // shortest way round
    return m;
}

bool g_hold = false;        // hold pass: draw only, no logic
bool g_logic_pass = false;  // logic pass with interpolation on: camera drawn halfway
bool g_hold_next = false;   // the next pass is a hold pass
// last camera state that was drawn normally, per camera process
struct Prev { uint32_t cam = 0; CamState s{}; bool valid = false; };
Prev g_prev[4];

Prev* prev_for(uint32_t cam) {
    for (auto& p : g_prev)
        if (p.cam == cam) return &p;
    for (auto& p : g_prev)
        if (!p.valid) { p.cam = cam; return &p; }
    g_prev[0] = Prev{cam};
    return &g_prev[0];
}
}  // namespace

}  // namespace interp

// camera_draw(camera_process_class*)
extern "C" void hook_024FFC40(Cpu* c) {
    using namespace interp;
    uint32_t cam = c->r[3];
    Prev* p = prev_for(cam);
    if (g_logic_pass) {  // halfway between the step drawn last (N) and the new one (N+1)
        CamState cur = read_cam(cam);
        if (p->valid) write_cam(cam, blend(p->s, cur));
        f_024FFC40_orig(c);
        write_cam(cam, cur);
        return;
    }
    p->s = read_cam(cam);  // exact step: remember it for the next halfway frame
    p->valid = true;
    f_024FFC40_orig(c);
}

// main loop body: frame counter, audio, fapGm_Execute
extern "C" void hook_025F172C(Cpu* c) {
    using namespace interp;
    // test aid: WWHD_INTERP_AT_STEP=n switches interpolation on after n loop passes
    static uint64_t passes = 0;
    static const uint64_t at = getenv("WWHD_INTERP_AT_STEP") ? strtoull(getenv("WWHD_INTERP_AT_STEP"), nullptr, 10) : 0;
    if (at && ++passes == at) set_enabled(true);
    bool& hold_next = g_hold_next;
    if (!enabled()) {
        hold_next = false;
        g_logic_pass = false;
        f_025F172C_orig(c);
        return;
    }
    if (hold_next) {
        g_hold = true;
        f_025D42EC(c);  // draw (and paint the previous pass) without logic
        g_hold = false;
    } else {
        g_logic_pass = true;
        f_025F172C_orig(c);
        g_logic_pass = false;
    }
    hold_next = !hold_next;
    // diagnostics: logic steps per second
    static uint64_t n = 0, t0 = timebase::now();
    if (!hold_next && ++n % 300 == 0) {
        uint64_t t = timebase::now();
        LOG("[interp] %.1f logic steps/s", 300.0 * timebase::kTicksPerSec / (double)(t - t0));
        t0 = t;
    }
}

// logic parts of fpcM_Management / fapGm_Execute, skipped on hold passes
extern "C" void hook_025DE788(Cpu* c) { if (!interp::g_hold) f_025DE788_orig(c); }
extern "C" void hook_025DE024(Cpu* c) { if (!interp::g_hold) f_025DE024_orig(c); }
extern "C" void hook_025E0EE4(Cpu* c) { if (interp::g_hold) c->r[3] = 1; else f_025E0EE4_orig(c); }
extern "C" void hook_025DDCEC(Cpu* c) { if (interp::g_hold) c->r[3] = 1; else f_025DDCEC_orig(c); }
extern "C" void hook_025D42C4(Cpu* c) { if (!interp::g_hold) f_025D42C4_orig(c); }
extern "C" void hook_0200E6EC(Cpu* c) { if (!interp::g_hold) f_0200E6EC_orig(c); }

namespace interp {
// The pads are read at the end of every frame. JUTGamePad derives "pressed this frame" from
// consecutive reads, so a change first seen on the read after a logic pass would be spent on a pass
// without logic. While interpolating, that read repeats the previous sample (see VPADRead /
// KPADReadEx), so every change reaches the logic exactly once.
bool repeat_input() { return enabled() && g_hold_next; }
}  // namespace interp
