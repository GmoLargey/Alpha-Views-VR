// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 GmoLargey - https://github.com/GmoLargey/Alpha-Views-VR
// Alpha Views VR is free software under the GNU General Public License
// version 3. It comes with ABSOLUTELY NO WARRANTY.
//
// Alpha Views VR - the SteamVR (OpenVR) companion of Alpha Views XR. A SteamVR overlay app that draws
// "windows" over any OpenVR game: solid key colour (green / blue / pink) for streamers that colour-key
// it out to passthrough (e.g. Virtual Desktop), or the headset's own camera (Index, Reverb G2 with the
// Oasis driver, ...). It lives in the SteamVR dashboard's bottom bar, and starts with SteamVR.
//
// Windows are the same as Alpha Views XR's: rounded rectangles, circles or free shapes, up to 32 per
// game, saved per game, with three presets, and boxes (the window extruded: the green handle). No keep
// boxes or hands behind objects: OpenVR doesn't give overlays the game's depth.
//
// Edit mode (the Alpha Views entry in the SteamVR dashboard, or Ctrl+Alt+E):
// SteamVR's laser pointer takes over the controllers (the game gets none of their input). Trigger or
// grip on a window or handle grabs it. Handles as in Alpha Views XR: move (blue), corner (orange,
// resize; shrink until tiny to delete), round (magenta), and above the window lock, type (rectangle /
// free shape), circle or draw, snap. Free shapes: orange points (drag; tap removes), "+" (add a point),
// draw (arm, then trigger anywhere on its plane and trace), box (green: tap for a box, drag for depth).
// The thumbstick while holding a window: up / down pushes / pulls it. The guide panel has the rest: new / delete window, colour,
// move all windows, presets (tap loads, hold saves) and Done.
//
// Hotkeys (Ctrl+Alt + key): E edit mode, P windows on / off, N new window, X delete all windows,
// C colour, G guide panel on / off, 1 2 3 load preset (+Shift: save).
//
// Files: %LOCALAPPDATA%\AlphaViewsVR\ (log.txt, settings.cfg, windows_<game>.cfg and presets).

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <openvr.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;
using vr::HmdMatrix34_t;

#ifndef AVVR_VERSION
#define AVVR_VERSION "dev"
#endif
static const char* kAppKey = "gmo.alphaviewsvr";

// ================================================================ log, files, time
static std::string g_dir;
static FILE* g_log = nullptr;
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fflush(g_log);
}
static void InitDirAndLog() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        char buf[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, p, -1, buf, sizeof buf, nullptr, nullptr);
        CoTaskMemFree(p);
        g_dir = std::string(buf) + "\\AlphaViewsVR";
        CreateDirectoryA(g_dir.c_str(), nullptr);
        g_log = fopen((g_dir + "\\log.txt").c_str(), "w");
    }
}
static double Now() {
    static auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list a; va_start(a, f); _vsnwprintf_s(buf, _TRUNCATE, f, a); va_end(a);
    return buf;
}

// ================================================================ maths
static XMVECTOR V(const XMFLOAT3& v) { return XMLoadFloat3(&v); }
static float Dist(XMVECTOR a, XMVECTOR b) { return XMVectorGetX(XMVector3Length(a - b)); }
static float Dot3(XMVECTOR a, XMVECTOR b) { return XMVectorGetX(XMVector3Dot(a, b)); }
// OpenVR 3x4 (column vectors) <-> DirectXMath (row vectors).
static HmdMatrix34_t ToHmd(FXMMATRIX m) {
    XMFLOAT4X4 f; XMStoreFloat4x4(&f, m);
    HmdMatrix34_t h;
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) h.m[i][j] = f.m[j][i]; h.m[i][3] = f.m[3][i]; }
    return h;
}
static XMMATRIX FromHmd(const HmdMatrix34_t& h) {
    XMFLOAT4X4 f{};
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) f.m[j][i] = h.m[i][j]; f.m[3][i] = h.m[i][3]; }
    f.m[3][3] = 1;
    return XMLoadFloat4x4(&f);
}
// A rotation + translation with the given axes (x, y, z = right, up, out of the front).
static XMMATRIX Frame(XMVECTOR x, XMVECTOR y, XMVECTOR z, XMVECTOR p) {
    return XMMATRIX(XMVectorSetW(x, 0), XMVectorSetW(y, 0), XMVectorSetW(z, 0), XMVectorSetW(p, 1));
}

// ================================================================ windows (as in Alpha Views XR)
// Corner order (local x, y signs): 0 (-,-) bottom-left, 1 (+,-), 2 (+,+), 3 (-,+).
static const float kCS[4][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, 1} };
static const float kRadiusHandleInset = 0.045f;
static const float kCornerHandleR = 0.016f, kRadiusHandleR = 0.012f, kCenterHandleR = 0.016f;
static const float kPointR = 0.011f, kMidR = 0.0065f, kButtonR = 0.013f;
static const float kArcK = 1.0f - 0.70710678f;
static const int kMaxWindows = 32, kMaxShapePts = 32;

struct Window {
    XMFLOAT3 pos{ 0, 1.0f, -0.6f };
    XMFLOAT4 rot{ 0, 0, 0, 1 };   // local +Z faces you
    float hw = 0.2f, hh = 0.15f;  // half width / height, metres
    float radius[4] = {};
    float hd = 0;                 // (Alpha Views XR box depth: kept in the file, unused here)
    std::vector<XMFLOAT2> pts;    // free shape outline (local, counter-clockwise); empty = rectangle
    bool keep = false;            // (Alpha Views XR keep box: kept in the file, unused here)
    bool ellipse = false;
    bool lock = false;
    int boxMode = 0;              // which faces of a box show: 0 all, 1 top + far wall, 2 far wall + floor, 3 all but front and floor
};
static bool IsShape(const Window& w) { return w.pts.size() >= 3; }
static XMMATRIX WinMatrix(const Window& w) {
    return XMMatrixRotationQuaternion(XMLoadFloat4(&w.rot)) * XMMatrixTranslation(w.pos.x, w.pos.y, w.pos.z);
}
static XMVECTOR WinToWorld(const Window& w, float x, float y, float z = 0) {
    return XMVector3TransformCoord(XMVectorSet(x, y, z, 1), WinMatrix(w));
}
static XMFLOAT3 WorldToWin(const Window& w, XMVECTOR p) {
    XMFLOAT3 r; XMStoreFloat3(&r, XMVector3TransformCoord(p, XMMatrixInverse(nullptr, WinMatrix(w))));
    return r;
}
static void ClampRadii(Window& w) {
    float m = std::min(w.hw, w.hh);
    for (float& r : w.radius) r = std::clamp(r, 0.0f, m);
}
static XMFLOAT2 RadiusHandleLocal(const Window& w, int k) {
    float d = w.radius[k] * kArcK + kRadiusHandleInset;
    return { kCS[k][0] * (w.hw - d), kCS[k][1] * (w.hh - d) };
}
static void Perimeter(const Window& w, std::vector<XMFLOAT2>& out) {
    out.clear();
    if (IsShape(w)) { out = w.pts; return; }
    if (w.ellipse) {
        for (int i = 0; i < 64; ++i) { float a = XM_2PI * i / 64; out.push_back({ cosf(a) * w.hw, sinf(a) * w.hh }); }
        return;
    }
    const float startDeg[4] = { 180, 270, 0, 90 };
    for (int k = 0; k < 4; ++k) {
        float r = w.radius[k];
        float cx = kCS[k][0] * (w.hw - r), cy = kCS[k][1] * (w.hh - r);
        int segs = r > 0.001f ? 12 : 0;
        for (int i = 0; i <= segs; ++i) {
            float a = XMConvertToRadians(startDeg[k] + 90.0f * (segs ? float(i) / segs : 0.5f));
            out.push_back({ cx + cosf(a) * r, cy + sinf(a) * r });
            if (!segs) break;
        }
    }
}
static float SignedArea(const std::vector<XMFLOAT2>& p) {
    float a = 0;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += p[j].x * p[i].y - p[i].x * p[j].y;
    return a * 0.5f;
}
static void UpdateShapeBounds(Window& w) {
    float mx = 0.005f, my = 0.005f;
    for (auto& p : w.pts) { mx = std::max(mx, fabsf(p.x)); my = std::max(my, fabsf(p.y)); }
    w.hw = mx; w.hh = my;
}
static void RecentreShape(Window& w) {
    if (!IsShape(w)) return;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (auto& p : w.pts) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    XMStoreFloat3(&w.pos, WinToWorld(w, cx, cy));
    for (auto& p : w.pts) { p.x -= cx; p.y -= cy; }
    if (SignedArea(w.pts) < 0) std::reverse(w.pts.begin(), w.pts.end());
    UpdateShapeBounds(w);
}
static float SegDist(XMFLOAT2 p, XMFLOAT2 a, XMFLOAT2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 1e-12f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0;
    float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return sqrtf(ex * ex + ey * ey);
}
static std::vector<XMFLOAT2> SimplifyClosed(const std::vector<XMFLOAT2>& in, size_t maxN) {
    if (in.size() <= 3) return in;
    size_t farIdx = 0; float best = -1;
    for (size_t i = 1; i < in.size(); ++i) {
        float dx = in[i].x - in[0].x, dy = in[i].y - in[0].y, d = dx * dx + dy * dy;
        if (d > best) { best = d; farIdx = i; }
    }
    for (float tol = 0.002f; ; tol *= 1.4f) {
        std::vector<bool> keep(in.size(), false);
        keep[0] = keep[farIdx] = true;
        auto run = [&](size_t a, size_t b) {
            std::vector<std::pair<size_t, size_t>> st{ { a, b } };
            while (!st.empty()) {
                auto [s, e] = st.back(); st.pop_back();
                size_t n = (e + in.size() - s) % in.size();
                if (n < 2) continue;
                float md = -1; size_t mi = s;
                for (size_t k = 1; k < n; ++k) {
                    size_t i = (s + k) % in.size();
                    float d = SegDist(in[i], in[s], in[e]);
                    if (d > md) { md = d; mi = i; }
                }
                if (md > tol) { keep[mi] = true; st.push_back({ s, mi }); st.push_back({ mi, e }); }
            }
        };
        run(0, farIdx); run(farIdx, 0);
        std::vector<XMFLOAT2> out;
        for (size_t i = 0; i < in.size(); ++i) if (keep[i]) out.push_back(in[i]);
        if (out.size() <= maxN || tol > 1.0f) return out;
    }
}
static bool SegHit(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT2 c, XMFLOAT2 d, XMFLOAT2& at) {
    float rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-12f) return false;
    float t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den, u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
    if (t < 0 || t > 1 || u < 0 || u > 1) return false;
    at = { a.x + rx * t, a.y + ry * t };
    return true;
}
static const float kTraceClose = 0.03f, kTraceAway = 0.06f;
static bool TraceCanClose(const std::vector<XMFLOAT2>& path) {
    if (path.size() < 8) return false;
    float far2 = 0;
    for (auto& q : path) far2 = std::max(far2, (q.x - path[0].x) * (q.x - path[0].x) + (q.y - path[0].y) * (q.y - path[0].y));
    return far2 > kTraceAway * kTraceAway;
}
static bool TraceAtStart(const std::vector<XMFLOAT2>& path) {
    if (!TraceCanClose(path)) return false;
    float dx = path.back().x - path[0].x, dy = path.back().y - path[0].y;
    return dx * dx + dy * dy < kTraceClose * kTraceClose;
}
static std::vector<XMFLOAT2> TraceToShape(const std::vector<XMFLOAT2>& path) {
    std::vector<XMFLOAT2> loop = path;
    if (!TraceAtStart(path)) {
        std::vector<XMFLOAT2> best;
        float bestA = 0;
        for (size_t i = 2; i < path.size(); ++i)
            for (size_t j = 0; j + 2 < i; ++j) {
                XMFLOAT2 x;
                if (!SegHit(path[j], path[j + 1], path[i - 1], path[i], x)) continue;
                std::vector<XMFLOAT2> l(path.begin() + j + 1, path.begin() + i);
                l.insert(l.begin(), x);
                float a = fabsf(SignedArea(l));
                if (l.size() >= 3 && a > bestA) { bestA = a; best = std::move(l); }
            }
        if (!best.empty() && bestA >= 0.5f * fabsf(SignedArea(path))) loop = std::move(best);
    }
    if (loop.size() < 3) return {};
    std::vector<XMFLOAT2> s = SimplifyClosed(loop, kMaxShapePts);
    if (s.size() < 3 || fabsf(SignedArea(s)) < 0.0004f) return {};
    return s;
}
static void Triangulate(const std::vector<XMFLOAT2>& p, std::vector<int>& tris) {
    tris.clear();
    std::vector<int> idx(p.size());
    for (size_t i = 0; i < p.size(); ++i) idx[i] = (int)i;
    if (SignedArea(p) < 0) std::reverse(idx.begin(), idx.end());
    auto cross = [&](int a, int b, int c) {
        return (p[b].x - p[a].x) * (p[c].y - p[a].y) - (p[b].y - p[a].y) * (p[c].x - p[a].x);
    };
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < 4 * p.size()) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            int a = idx[(i + idx.size() - 1) % idx.size()], b = idx[i], c = idx[(i + 1) % idx.size()];
            if (cross(a, b, c) <= 1e-9f) continue;
            bool empty = true;
            for (int q : idx) {
                if (q == a || q == b || q == c) continue;
                if (cross(a, b, q) >= 0 && cross(b, c, q) >= 0 && cross(c, a, q) >= 0) { empty = false; break; }
            }
            if (!empty) continue;
            tris.insert(tris.end(), { a, b, c });
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) { tris.insert(tris.end(), { idx[idx.size() - 1], idx[0], idx[1] }); idx.erase(idx.begin()); }
    }
    if (idx.size() == 3) tris.insert(tris.end(), { idx[0], idx[1], idx[2] });
}
static void MakeShape(Window& w) {
    std::vector<XMFLOAT2> per;
    Perimeter(w, per);
    w.pts = SimplifyClosed(per, kMaxShapePts);
    if (w.pts.size() < 3) w.pts = { { -w.hw, -w.hh }, { w.hw, -w.hh }, { w.hw, w.hh }, { -w.hw, w.hh } };
    w.ellipse = false;
    RecentreShape(w);
}
static void MakeRect(Window& w) {
    RecentreShape(w);
    w.pts.clear();
    w.ellipse = false;
    for (float& r : w.radius) r = 0;
}

// ================================================================ state
enum GrabMode { GRAB_NONE, GRAB_MOVE, GRAB_CORNER, GRAB_RADIUS, GRAB_POINT, GRAB_ADDPT, GRAB_TYPE, GRAB_DRAWBTN,
                GRAB_TRACE, GRAB_CIRCLEBTN, GRAB_LOCKBTN, GRAB_SNAPBTN, GRAB_GUIDE, GRAB_ALL, GRAB_PRESET, GRAB_BUTTON, GRAB_DEPTH, GRAB_BOXMODE };
enum IconId { ICON_DOT, ICON_MOVE, ICON_RESIZE, ICON_ROUND, ICON_DEPTH, ICON_KEEP, ICON_TO_SHAPE, ICON_TO_RECT,
              ICON_DRAW, ICON_POINT, ICON_ADD, ICON_TO_CIRCLE, ICON_LOCK, ICON_UNLOCK, ICON_SNAP };

static std::vector<Window> g_windows;
static int g_selected = -1, g_drawArmed = -1;
static bool g_edit = false, g_windowsOn = true;
static int g_keyMode = 0;              // 0 green, 1 blue, 2 pink, 3 headset camera
static bool g_guideOn = true;
static XMVECTOR g_head = XMVectorSet(0, 1.6f, 0, 0);
static XMMATRIX g_headM = XMMatrixIdentity();
static bool g_headOk = false;
static std::string g_gameKey = "none", g_gameName = "No game";
static bool g_presetHas[3] = {};
static double g_presetFlashUntil = 0; static int g_presetFlash = -1; static bool g_presetFlashSaved = false;
static std::vector<XMFLOAT2> g_trace;
static Window g_tracePlane;
static XMFLOAT2 g_tracePrev{}; static bool g_tracePrevOk = false;
static double g_windowsFullUntil = 0;
static bool g_hasCamera = false;
static std::string g_cameraStatus = "checking";

static const char* kKeyName[4] = { "green", "blue", "pink", "camera" };
static const float kKeyCol[3][3] = { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 } };

static float FarScale(XMVECTOR p) { return std::clamp(Dist(p, g_head) / 0.8f, 1.0f, 10.0f); }
static float SpreadTop(const Window& w) { return FarScale(WinToWorld(w, 0, w.hh, 0)); }
static float SpreadSide(const Window& w) { return FarScale(WinToWorld(w, w.hw, 0, 0)); }
// Box depth handle: outside the right edge, on the centre line (as in Alpha Views XR).
static const float kDepthOut = 0.075f, kDepthHandleR = 0.014f, kMaxHalfDepth = 50.0f;   // 100 m deep at most, as Alpha Views XR
static XMVECTOR DepthHandle(const Window& w) { return WinToWorld(w, w.hw + kDepthOut * SpreadSide(w), 0, 0); }
// The box mode button: just past the depth handle (where Alpha Views XR has its keep-box button).
static XMVECTOR BoxModeButton(const Window& w) { return WinToWorld(w, w.hw + (kDepthOut + 0.042f) * SpreadSide(w), 0, 0); }
static const char* kBoxModeName[4] = { "whole box", "top and far wall", "far wall and floor", "all but front and floor" };
static XMFLOAT2 TopButton(const Window& w, float x) { float s = SpreadTop(w); return { x * s, w.hh + 0.045f * s }; }
static XMFLOAT2 MidLocal(const Window& w, int k) {
    const XMFLOAT2 &a = w.pts[k], &b = w.pts[(k + 1) % w.pts.size()];
    return { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
}
static XMVECTOR TargetPoint(const Window& w, GrabMode mode, int k) {
    switch (mode) {
    case GRAB_POINT: if (k >= 0 && k < (int)w.pts.size()) return WinToWorld(w, w.pts[k].x, w.pts[k].y); break;
    case GRAB_ADDPT: if (k >= 0 && k < (int)w.pts.size()) { XMFLOAT2 m = MidLocal(w, k); return WinToWorld(w, m.x, m.y); } break;
    case GRAB_CORNER: return WinToWorld(w, kCS[k][0] * w.hw, kCS[k][1] * w.hh);
    case GRAB_RADIUS: { XMFLOAT2 r = RadiusHandleLocal(w, k); return WinToWorld(w, r.x, r.y); }
    case GRAB_LOCKBTN: { XMFLOAT2 b = TopButton(w, -0.09f); return WinToWorld(w, b.x, b.y); }
    case GRAB_TYPE: { XMFLOAT2 b = TopButton(w, -0.03f); return WinToWorld(w, b.x, b.y); }
    case GRAB_DRAWBTN: case GRAB_CIRCLEBTN: { XMFLOAT2 b = TopButton(w, 0.03f); return WinToWorld(w, b.x, b.y); }
    case GRAB_SNAPBTN: { XMFLOAT2 b = TopButton(w, 0.09f); return WinToWorld(w, b.x, b.y); }
    case GRAB_DEPTH: return DepthHandle(w);
    case GRAB_BOXMODE: return BoxModeButton(w);
    default: break;
    }
    return WinToWorld(w, 0, 0);
}

// ================================================================ settings and window files
static std::string SettingsPath() { return g_dir + "\\settings.cfg"; }
static void LoadSettings() {
    if (FILE* f = fopen(SettingsPath().c_str(), "r")) {
        int k = 0, g = 1;
        if (fscanf(f, "%d %d", &k, &g) >= 1) { g_keyMode = std::clamp(k, 0, 3); g_guideOn = g != 0; }
        fclose(f);
    }
}
static void SaveSettings() {
    if (FILE* f = fopen(SettingsPath().c_str(), "w")) { fprintf(f, "%d %d\n", g_keyMode, g_guideOn ? 1 : 0); fclose(f); }
}
static std::string ConfigPath() {
    std::string k;
    for (char c : g_gameKey) k += (isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    return g_dir + "\\windows_" + k + ".cfg";
}
static std::string PresetPath(int i) { std::string p = ConfigPath(); return p.substr(0, p.size() - 4) + "_preset" + std::to_string(i + 1) + ".cfg"; }

// Same format as Alpha Views XR (so the files read the same way).
static bool ReadWindows(const std::string& path, bool* enabled) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    std::vector<Window> ws;
    int count = 0, en = 1;
    static char line[4096];
    if (fgets(line, sizeof line, f) && sscanf(line, "%d %d", &count, &en) == 2) {
        if (enabled) *enabled = en != 0;
        for (int i = 0; i < count && i < kMaxWindows && fgets(line, sizeof line, f); ++i) {
            Window w; int s[4], used = 0;
            int n = sscanf(line, "%f %f %f %f %f %f %f %f %f %f %f %f %f %d %d %d %d %f%n",
                    &w.pos.x, &w.pos.y, &w.pos.z, &w.rot.x, &w.rot.y, &w.rot.z, &w.rot.w, &w.hw, &w.hh,
                    &w.radius[0], &w.radius[1], &w.radius[2], &w.radius[3], &s[0], &s[1], &s[2], &s[3], &w.hd, &used);
            if (n < 18) break;
            const char* c = line + used;
            int np = 0, adv = 0, keep = 0, ell = 0, lock = 0;
            bool ok = sscanf(c, "%d%n", &np, &adv) == 1;
            if (ok) c += adv;
            if (ok && np >= 3 && np <= kMaxShapePts) {
                for (int k = 0; k < np; ++k) {
                    XMFLOAT2 p;
                    if (sscanf(c, "%f %f%n", &p.x, &p.y, &adv) != 2) { ok = false; break; }
                    c += adv;
                    w.pts.push_back(p);
                }
                if (w.pts.size() < 3) w.pts.clear(); else UpdateShapeBounds(w);
            }
            if (ok && sscanf(c, "%d%n", &keep, &adv) == 1) {
                w.keep = keep != 0; c += adv;
                if (sscanf(c, "%d%n", &ell, &adv) == 1) { w.ellipse = ell != 0 && !IsShape(w); c += adv; if (sscanf(c, "%d%n", &lock, &adv) == 1) { w.lock = lock != 0; c += adv; int bm = 0; if (sscanf(c, "%d", &bm) == 1) w.boxMode = std::clamp(bm, 0, 3); } }
            }
            ws.push_back(w);
        }
    }
    fclose(f);
    g_windows = std::move(ws);
    return true;
}
static bool WriteWindows(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "%d %d\n", (int)g_windows.size(), g_windowsOn ? 1 : 0);
    for (auto& w : g_windows) {
        fprintf(f, "%f %f %f %f %f %f %f %f %f %f %f %f %f %d %d %d %d %f %d", w.pos.x, w.pos.y, w.pos.z,
            w.rot.x, w.rot.y, w.rot.z, w.rot.w, w.hw, w.hh, w.radius[0], w.radius[1], w.radius[2], w.radius[3],
            0, 0, 0, 0, w.hd, IsShape(w) ? (int)w.pts.size() : 0);
        if (IsShape(w)) for (auto& p : w.pts) fprintf(f, " %f %f", p.x, p.y);
        fprintf(f, " %d %d %d %d\n", w.keep ? 1 : 0, w.ellipse ? 1 : 0, w.lock ? 1 : 0, w.boxMode);
    }
    fclose(f);
    return true;
}
static void CheckPresets() { for (int i = 0; i < 3; ++i) g_presetHas[i] = GetFileAttributesA(PresetPath(i).c_str()) != INVALID_FILE_ATTRIBUTES; }
static void SaveWindows() { if (WriteWindows(ConfigPath())) Log("Saved %d window(s) for %s\n", (int)g_windows.size(), g_gameKey.c_str()); }
static void LoadWindows() {
    g_windows.clear(); g_selected = -1; g_drawArmed = -1; g_windowsOn = true;
    CheckPresets();
    if (ReadWindows(ConfigPath(), &g_windowsOn)) Log("Loaded %d window(s) for %s\n", (int)g_windows.size(), g_gameKey.c_str());
    else Log("No saved windows for %s\n", g_gameKey.c_str());
}
static void SavePreset(int i) {
    if (!WriteWindows(PresetPath(i))) { Log("Preset %d: couldn't save\n", i + 1); return; }
    g_presetHas[i] = true; g_presetFlash = i; g_presetFlashSaved = true; g_presetFlashUntil = Now() + 2;
    Log("Preset %d saved: %d window(s)\n", i + 1, (int)g_windows.size());
}
static void LoadPreset(int i) {
    if (!g_presetHas[i] || !ReadWindows(PresetPath(i), nullptr)) { Log("Preset %d is empty: hold its button to save one\n", i + 1); return; }
    g_selected = -1; g_drawArmed = -1;
    g_presetFlash = i; g_presetFlashSaved = false; g_presetFlashUntil = Now() + 2;
    Log("Preset %d loaded: %d window(s)\n", i + 1, (int)g_windows.size());
    SaveWindows();
}

// ================================================================ window actions
static XMMATRIX FacingHead(XMVECTOR p) {
    XMVECTOR z = g_head - p;
    if (XMVectorGetX(XMVector3Length(z)) < 0.05f) z = XMVectorSet(0, 0, 1, 0);
    z = XMVector3Normalize(z);
    if (fabsf(XMVectorGetY(z)) > 0.95f) z = XMVector3Normalize(XMVectorSetY(z, 0) + XMVectorSet(0, 0, 0.001f, 0));
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), z));
    XMVECTOR up = XMVector3Cross(z, right);
    return Frame(right, up, z, p);
}
static void SetWinFrame(Window& w, FXMMATRIX m) {
    XMVECTOR s, r, t;
    if (!XMMatrixDecompose(&s, &r, &t, m)) return;
    XMStoreFloat3(&w.pos, t); XMStoreFloat4(&w.rot, XMQuaternionNormalize(r));
}
static XMVECTOR HeadForward() {
    XMVECTOR f = XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), g_headM);
    f = XMVectorSetY(f, 0);
    return XMVectorGetX(XMVector3Length(f)) < 0.1f ? XMVectorSet(0, 0, -1, 0) : XMVector3Normalize(f);
}
static XMVECTOR Horizontal(XMVECTOR v, XMVECTOR fallback) {
    v = XMVectorSetY(v, 0);
    if (XMVectorGetX(XMVector3Length(v)) < 0.05f) v = XMVectorSetY(fallback, 0);
    if (XMVectorGetX(XMVector3Length(v)) < 1e-3f) v = XMVectorSet(0, 0, 1, 0);
    return XMVector3Normalize(v);
}
static void NewWindow() {
    if ((int)g_windows.size() >= kMaxWindows) { g_windowsFullUntil = Now() + 4; Log("%d windows is the most\n", kMaxWindows); return; }
    Window w;
    XMVECTOR p = g_head + HeadForward() * 0.7f + XMVectorSet(0, -0.25f, 0, 0);
    {   // upright, facing you across the room (not tilted up at your head)
        XMVECTOR z = Horizontal(g_head - p, HeadForward() * -1.0f);
        XMVECTOR up = XMVectorSet(0, 1, 0, 0), r = XMVector3Normalize(XMVector3Cross(up, z));
        SetWinFrame(w, Frame(r, up, z, p));
    }
    g_windows.push_back(w);
    g_selected = (int)g_windows.size() - 1;
    Log("New window %d\n", g_selected);
    SaveWindows();
}
static void DeleteWindow(int wi) {
    if (wi < 0 || wi >= (int)g_windows.size()) return;
    g_windows.erase(g_windows.begin() + wi);
    if (g_selected == wi) g_selected = g_windows.empty() ? -1 : (int)g_windows.size() - 1;
    else if (g_selected > wi) --g_selected;
    if (g_drawArmed == wi) g_drawArmed = -1; else if (g_drawArmed > wi) --g_drawArmed;
    Log("Window %d deleted\n", wi);
    SaveWindows();
}
static void ToggleShape(int wi) {
    Window& w = g_windows[wi];
    if (IsShape(w)) { MakeRect(w); if (g_drawArmed == wi) g_drawArmed = -1; } else MakeShape(w);
    Log("Window %d is now a %s\n", wi, IsShape(w) ? "free shape" : "rectangle");
    SaveWindows();
}
static void ToggleCircle(int wi) { Window& w = g_windows[wi]; w.ellipse = !w.ellipse; Log("Window %d is now a %s\n", wi, w.ellipse ? "circle" : "rectangle"); SaveWindows(); }
static void ToggleLock(int wi) { Window& w = g_windows[wi]; w.lock = !w.lock; Log("Window %d angle %s\n", wi, w.lock ? "locked" : "unlocked"); SaveWindows(); }
// Snap keeps the way the window faces round you and only straightens it: upright (standing straight up,
// facing the same compass direction it does now) or level (lying flat, its top edge pointing the same
// compass direction). k: 0 upright, 1 level.
static XMVECTOR SnapRot(const Window& w, int k) {
    XMVECTOR q = XMLoadFloat4(&w.rot);
    XMVECTOR n = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q), u = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q);
    XMVECTOR toHead = g_head - V(w.pos);
    XMVECTOR nt, up;
    if (k == 0) {   // upright: facing its own compass direction (lying flat: towards you)
        nt = Horizontal(n, -u);
        if (XMVectorGetX(XMVector3Length(XMVectorSetY(n, 0))) < 0.05f) nt = Horizontal(toHead, -u);
        up = XMVectorSet(0, 1, 0, 0);
    } else {        // level: face up or down (whichever side you're on), top edge keeps its compass direction
        float s = fabsf(XMVectorGetY(toHead)) > 1e-3f ? XMVectorGetY(toHead) : XMVectorGetY(n);
        nt = XMVectorSet(0, s < 0 ? -1.0f : 1.0f, 0, 0);
        up = Horizontal(u, -n);
    }
    XMVECTOR r = XMVector3Normalize(XMVector3Cross(up, nt));
    up = XMVector3Cross(nt, r);
    XMMATRIX m(r, up, nt, XMVectorSet(0, 0, 0, 1));
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(m));
}
static void SnapWindow(int wi) {
    Window& w = g_windows[wi];
    static const char* kName[2] = { "upright", "level" };
    XMVECTOR q = XMLoadFloat4(&w.rot);
    XMVECTOR n = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
    int k = fabsf(XMVectorGetY(n)) < 0.70710678f ? 0 : 1;
    XMVECTOR t = SnapRot(w, k);
    if (fabsf(XMVectorGetX(XMVector4Dot(q, t))) > 0.99996f) { k = 1 - k; t = SnapRot(w, k); }   // already straight: the other one
    XMStoreFloat4(&w.rot, t);
    Log("Window %d snapped %s\n", wi, kName[k]);
    SaveWindows();
}
static void CycleKeyMode() {
    do g_keyMode = (g_keyMode + 1) % 4; while (g_keyMode == 3 && !g_hasCamera);
    Log("Window colour: %s\n", kKeyName[g_keyMode]);
    SaveSettings();
}

// ================================================================ icons (Glyph() as in Alpha Views XR)
namespace glyph {
struct P { float x, y; };
static P operator-(P a, P b) { return { a.x - b.x, a.y - b.y }; }
static P operator+(P a, P b) { return { a.x + b.x, a.y + b.y }; }
static P operator*(P a, float s) { return { a.x * s, a.y * s }; }
static float Dot(P a, P b) { return a.x * b.x + a.y * b.y; }
static float Len(P a) { return sqrtf(Dot(a, a)); }
static P Norm(P a) { float l = Len(a); return { a.x / l, a.y / l }; }
static float Seg(P p, P a, P b) { P pa = p - a, ba = b - a; return Len(pa - ba * std::clamp(Dot(pa, ba) / Dot(ba, ba), 0.0f, 1.0f)); }
static float Chevron(P p, P tip, P dir, float h) { P n{ -dir.y, dir.x }; return std::min(Seg(p, tip, tip - (dir - n) * h), Seg(p, tip, tip - (dir + n) * h)); }
static float Arrow2(P p, P a, P b, float h) { P d = Norm(b - a); return std::min(Seg(p, a, b), std::min(Chevron(p, b, d, h), Chevron(p, a, d * -1, h))); }
static float Box(P p, P c, P h) {
    float dx = fabsf(p.x - c.x) - h.x, dy = fabsf(p.y - c.y) - h.y;
    return Len({ std::max(dx, 0.0f), std::max(dy, 0.0f) }) + std::min(std::max(dx, dy), 0.0f);
}
static float Side(P p, P a, P b) { P e = Norm(b - a), v = p - a; return v.x * e.y - v.y * e.x; }
static float Glyph(int id, P p) {
    using std::min; using std::max;
    if (id == ICON_MOVE) return min(Arrow2(p, { -0.6f, 0 }, { 0.6f, 0 }, 0.2f), Arrow2(p, { 0, -0.6f }, { 0, 0.6f }, 0.2f)) - 0.075f;
    if (id == ICON_RESIZE) return Arrow2(p, { -0.55f, 0 }, { 0.55f, 0 }, 0.24f) - 0.085f;
    if (id == ICON_ROUND) {
        P q = p - P{ -0.04f, -0.04f };
        float arc = (q.x > 0 && q.y > 0) ? fabsf(Len(q) - 0.44f) : 1e3f;
        return min(arc, min(Seg(p, { 0.4f, -0.5f }, { 0.4f, -0.04f }), Seg(p, { -0.04f, 0.4f }, { -0.5f, 0.4f }))) - 0.085f;
    }
    if (id == ICON_TO_SHAPE) {
        P Q[5] = { { -0.52f, -0.28f }, { 0.12f, -0.52f }, { 0.52f, 0.02f }, { 0.08f, 0.5f }, { -0.46f, 0.3f } };
        float d = 1e3f, dots = 1e3f;
        for (int i = 0; i < 5; ++i) { d = min(d, Seg(p, Q[i], Q[(i + 1) % 5])); dots = min(dots, Len(p - Q[i])); }
        return min(d - 0.055f, dots - 0.13f);
    }
    if (id == ICON_TO_RECT) return fabsf(Box(p, { 0, 0 }, { 0.36f, 0.2f }) - 0.16f) - 0.07f;
    if (id == ICON_DRAW) {
        P r{ (p.x + p.y) * 0.70710678f, (p.y - p.x) * 0.70710678f };
        float body = Box(r, { 0.2f, 0 }, { 0.36f, 0.15f });
        P A{ -0.58f, 0 }, B{ -0.16f, -0.15f }, C{ -0.16f, 0.15f };
        float tip = max(max(Side(r, A, B), Side(r, B, C)), Side(r, C, A));
        float gaps = min(fabsf(r.x + 0.12f), fabsf(r.x - 0.36f)) - 0.03f;
        return max(min(body, tip), -gaps);
    }
    if (id == ICON_POINT) return Len(p) - 0.32f;
    if (id == ICON_ADD) return min(Seg(p, { -0.5f, 0 }, { 0.5f, 0 }), Seg(p, { 0, -0.5f }, { 0, 0.5f })) - 0.12f;
    if (id == ICON_TO_CIRCLE) return fabsf(Len(p) - 0.48f) - 0.08f;
    if (id == ICON_LOCK || id == ICON_UNLOCK) {
        float body = max(Box(p, { 0, -0.2f }, { 0.36f, 0.27f }), -(Len(p - P{ 0, -0.17f }) - 0.08f));
        P c{ 0, id == ICON_LOCK ? 0.07f : 0.2f };
        P q = p - c;
        float arc = q.y >= 0 ? fabsf(Len(q) - 0.22f) : 1e3f;
        float legs = Seg(p, { -0.22f, c.y }, { -0.22f, 0.07f });
        if (id == ICON_LOCK) legs = min(legs, Seg(p, { 0.22f, c.y }, { 0.22f, 0.07f }));
        return min(body, min(arc, legs) - 0.07f);
    }
    if (id == ICON_KEEP) {   // a box
        P A{ -0.42f, -0.42f }, B{ 0.18f, -0.42f }, C{ 0.42f, -0.18f }, D{ 0.42f, 0.42f }, E{ -0.18f, 0.42f }, F{ -0.42f, 0.18f };
        float hex = max(max(max(Side(p, A, B), Side(p, B, C)), max(Side(p, C, D), Side(p, D, E))), max(Side(p, E, F), Side(p, F, A)));
        P K{ 0.18f, 0.18f };
        float lines = min(min(Seg(p, F, K), Seg(p, K, B)), Seg(p, K, D)) - 0.045f;
        return max(hex - 0.03f, -lines);
    }
    if (id == ICON_DEPTH) {
        float d = fabsf(Box(p, { -0.12f, -0.12f }, { 0.3f, 0.3f }));
        d = min(d, Seg(p, { -0.18f, 0.42f }, { 0.42f, 0.42f }));
        d = min(d, Seg(p, { 0.42f, -0.18f }, { 0.42f, 0.42f }));
        d = min(d, Seg(p, { -0.42f, 0.18f }, { -0.18f, 0.42f }));
        d = min(d, Seg(p, { 0.18f, 0.18f }, { 0.42f, 0.42f }));
        d = min(d, Seg(p, { 0.18f, -0.42f }, { 0.42f, -0.18f }));
        return d - 0.065f;
    }
    if (id == ICON_SNAP) {
        float d = min(Seg(p, { -0.5f, -0.4f }, { 0.5f, -0.4f }), Seg(p, { 0, -0.4f }, { 0, 0.52f }));
        d = min(d, min(Seg(p, { 0.2f, -0.4f }, { 0.2f, -0.2f }), Seg(p, { 0.2f, -0.2f }, { 0, -0.2f })));
        return d - 0.07f;
    }
    return 1e3f;
}
}  // namespace glyph

struct IconLook { IconId id; float c[3]; float g[3]; bool ownG; };
// Paint a handle disc (as psIcon in Alpha Views XR) into BGRA pixels. alpha: transparent round it.
static void PaintIcon(uint32_t* px, int W, int H, int cx, int cy, float rad, const IconLook& ic, bool alpha) {
    float aa = 1.0f / rad;
    float lum = 0.3f * ic.c[0] + 0.59f * ic.c[1] + 0.11f * ic.c[2];
    float gc[3] = { 1, 1, 1 };
    if (ic.ownG) { gc[0] = ic.g[0]; gc[1] = ic.g[1]; gc[2] = ic.g[2]; }
    else if (lum > 0.74f) { gc[0] = 0.08f; gc[1] = 0.09f; gc[2] = 0.11f; }
    int r = (int)ceilf(rad) + 1;
    for (int y = std::max(0, cy - r); y < std::min(H, cy + r); ++y)
        for (int x = std::max(0, cx - r); x < std::min(W, cx + r); ++x) {
            float px0 = (x + 0.5f - cx) / rad, py0 = -(y + 0.5f - cy) / rad, d = sqrtf(px0 * px0 + py0 * py0);
            float disc = std::clamp((1 - d) / aa + 0.5f, 0.0f, 1.0f);
            if (disc <= 0) continue;
            float rim = std::clamp((d - (1 - std::max(0.12f, aa))) / aa + 0.5f, 0.0f, 1.0f);
            float gcov = std::clamp(0.5f - glyph::Glyph(ic.id, { px0, py0 }) / aa, 0.0f, 1.0f);
            uint32_t& o = px[y * W + x];
            float dst[3] = { ((o >> 16) & 255) / 255.0f, ((o >> 8) & 255) / 255.0f, (o & 255) / 255.0f };
            uint32_t out = alpha ? (uint32_t(disc * 255 + 0.5f) << 24) : 0xFF000000u;
            for (int k = 0; k < 3; ++k) {
                float c = ic.c[k] * (1.05f - 0.15f * d * d);
                c += (ic.c[k] * 0.3f - c) * rim;
                c += (gc[k] - c) * gcov;
                if (!alpha) c = dst[k] + (c - dst[k]) * disc;
                out |= uint32_t(std::clamp(c, 0.0f, 1.0f) * 255 + 0.5f) << (16 - 8 * k);
            }
            o = out;
        }
}

// ================================================================ D3D11
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11InputLayout* g_il = nullptr;
static ID3D11Buffer* g_cb = nullptr;
static ID3D11Buffer* g_vb = nullptr;
static const UINT kVbFloats = 2 * 6 * 2048;
static ID3D11SamplerState* g_samp = nullptr;
static ID3D11RasterizerState* g_rs = nullptr;

static const char* kShader = R"(
cbuffer C : register(b0) {
    float4 col;              // colour (key colour, outline)
    float4 halfMode;         // window half width, half height, mode (0 colour, 1 camera), 0
    row_major float4x4 toCam; // window local -> camera clip
    float4 camRect;          // camera picture in the camera texture: u0, v0, du, dv
};
Texture2D camTex : register(t0);
SamplerState samp : register(s0);
struct VO { float4 pos : SV_Position; float2 loc : LOC; };
VO vs(float2 p : POS) { VO o; o.pos = float4(p, 0, 1); o.loc = p * halfMode.xy; return o; }
float4 ps(VO i) : SV_Target {
    if (halfMode.z < 0.5) return col;
    float4 c = mul(float4(i.loc, 0, 1), toCam);
    if (c.w <= 1e-4) return float4(0, 0, 0, 1);
    float2 ndc = c.xy / c.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
    return float4(camTex.SampleLevel(samp, camRect.xy + uv * camRect.zw, 0).rgb, 1);
}
)";
struct CB { XMFLOAT4 col, halfMode; XMFLOAT4X4 toCam; XMFLOAT4 camRect; };

static bool InitD3D() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                 D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) { Log("D3D11CreateDevice failed\n"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "avvr", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "avvr", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        Log("Shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    D3D11_INPUT_ELEMENT_DESC ie{ "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    g_dev->CreateInputLayout(&ie, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd{ sizeof(CB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_BUFFER_DESC vd{ kVbFloats * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
    g_dev->CreateBuffer(&vd, nullptr, &g_vb);
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_dev->CreateSamplerState(&sd, &g_samp);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.MultisampleEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rs);
    return g_vs && g_ps && g_il && g_cb && g_vb && g_samp && g_rs;
}
static ID3D11Texture2D* MakeTexture(int w, int h, DXGI_FORMAT f, const void* px, bool rt, int samples = 1) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
    td.SampleDesc.Count = samples; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    D3D11_SUBRESOURCE_DATA sd{ px, (UINT)w * 4, 0 };
    ID3D11Texture2D* t = nullptr;
    g_dev->CreateTexture2D(&td, px ? &sd : nullptr, &t);
    return t;
}

// A render target (4x MSAA, resolved into tex) for a window picture or the traced outline.
struct Canvas {
    int size = 0;
    ID3D11Texture2D *tex = nullptr, *msaa = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    bool Init(int n) {
        size = n;
        tex = MakeTexture(n, n, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, false);
        msaa = MakeTexture(n, n, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, true, 4);
        if (msaa) g_dev->CreateRenderTargetView(msaa, nullptr, &rtv);
        return tex && msaa && rtv;
    }
    void Begin(const float clear[4]) {
        g_ctx->ClearRenderTargetView(rtv, clear);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)size, (float)size, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rs);
        g_ctx->IASetInputLayout(g_il);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = 8, off = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps, nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->PSSetSamplers(0, 1, &g_samp);
    }
    void End() {
        ID3D11RenderTargetView* none = nullptr;
        g_ctx->OMSetRenderTargets(1, &none, nullptr);
        g_ctx->ResolveSubresource(tex, 0, msaa, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
};
static void DrawTris(const std::vector<XMFLOAT2>& v, const CB& cb) {
    if (v.empty()) return;
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    for (size_t first = 0; first < v.size(); first += kVbFloats / 2 / 3 * 3) {
        size_t n = std::min(v.size() - first, (size_t)(kVbFloats / 2 / 3 * 3));
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        memcpy(m.pData, v.data() + first, n * sizeof(XMFLOAT2));
        g_ctx->Unmap(g_vb, 0);
        g_ctx->Draw((UINT)n, 0);
    }
}
// A band along a closed (or open) outline, `wid` wide on its inside (points in metres, scaled by sx, sy
// to clip space).
static void Ribbon(const std::vector<XMFLOAT2>& p, bool closed, float wid, float sx, float sy, std::vector<XMFLOAT2>& out) {
    size_t n = p.size();
    if (n < 2) return;
    float sgn = closed && SignedArea(p) < 0 ? -1.0f : 1.0f;
    for (size_t i = 0; i + (closed ? 0 : 1) < n; ++i) {
        XMFLOAT2 a = p[i], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-6f) continue;
        float nx = -dy / l * wid * sgn, ny = dx / l * wid * sgn;   // inward for counter-clockwise outlines
        if (!closed) { nx *= 0.5f; ny *= 0.5f; a.x -= nx; a.y -= ny; b.x -= nx; b.y -= ny; nx *= 2; ny *= 2; }
        XMFLOAT2 q[4] = { a, b, { b.x + nx, b.y + ny }, { a.x + nx, a.y + ny } };
        for (int k : { 0, 1, 2, 0, 2, 3 }) out.push_back({ q[k].x * sx, q[k].y * sy });
        // round the joints a little: a small square at each point
        float h = wid * 0.5f;
        XMFLOAT2 c{ a.x + nx * 0.5f, a.y + ny * 0.5f };
        XMFLOAT2 s[4] = { { c.x - h, c.y - h }, { c.x + h, c.y - h }, { c.x + h, c.y + h }, { c.x - h, c.y + h } };
        for (int k : { 0, 1, 2, 0, 2, 3 }) out.push_back({ s[k].x * sx, s[k].y * sy });
    }
}

// ================================================================ headset camera
static vr::TrackedCameraHandle_t g_cam = INVALID_TRACKED_CAMERA_HANDLE;
static ID3D11ShaderResourceView* g_camSrv = nullptr;
static XMFLOAT4X4 g_camWorldToClip;   // world -> camera clip (row vectors), for the latest frame
static XMFLOAT4 g_camRect{ 0, 0, 1, 1 };
static uint32_t g_camSeq = 0;
static bool g_camFrameOk = false;

static void CheckCamera() {
    bool has = false;
    vr::VRTrackedCamera()->HasCamera(vr::k_unTrackedDeviceIndex_Hmd, &has);
    g_hasCamera = has;
    g_cameraStatus = has ? "available" : "none (headset or driver has no camera for SteamVR)";
    Log("Headset camera: %s\n", g_cameraStatus.c_str());
}
static void StartCamera() {
    if (g_cam != INVALID_TRACKED_CAMERA_HANDLE || !g_hasCamera) return;
    auto* tc = vr::VRTrackedCamera();
    tc->SetCameraTrackingSpace(vr::TrackingUniverseStanding);
    vr::EVRTrackedCameraError e = tc->AcquireVideoStreamingService(vr::k_unTrackedDeviceIndex_Hmd, &g_cam);
    if (e != vr::VRTrackedCameraError_None) { g_cameraStatus = std::string("couldn't start: ") + tc->GetCameraErrorNameFromEnum(e); Log("Camera: %s\n", g_cameraStatus.c_str()); g_cam = INVALID_TRACKED_CAMERA_HANDLE; return; }
    g_cameraStatus = "starting";
    int32_t layout = vr::VRSystem()->GetInt32TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraFrameLayout_Int32);
    Log("Camera streaming started (frame layout 0x%x)\n", layout);
}
static void StopCamera() {
    if (g_cam == INVALID_TRACKED_CAMERA_HANDLE) return;
    vr::VRTrackedCamera()->ReleaseVideoStreamingService(g_cam);
    g_cam = INVALID_TRACKED_CAMERA_HANDLE;
    g_camFrameOk = false;
    if (g_camSrv) { g_camSrv->Release(); g_camSrv = nullptr; }
    Log("Camera streaming stopped\n");
}
// Latest frame: its texture, where camera 0's picture is in it, and world -> camera clip.
static bool UpdateCamera() {
    if (g_cam == INVALID_TRACKED_CAMERA_HANDLE) return false;
    auto* tc = vr::VRTrackedCamera();
    vr::CameraVideoStreamFrameHeader_t hdr{};
    ID3D11ShaderResourceView* srv = nullptr;
    vr::EVRTrackedCameraError e = tc->GetVideoStreamTextureD3D11(g_cam, vr::VRTrackedCameraFrameType_MaximumUndistorted, g_dev, (void**)&srv, &hdr, sizeof hdr);
    if (e != vr::VRTrackedCameraError_None || !srv) {
        static int lastE = -1;
        if ((int)e != lastE) { Log("Camera frame: %s\n", tc->GetCameraErrorNameFromEnum(e)); lastE = (int)e; }
        g_cameraStatus = e == vr::VRTrackedCameraError_NoFrameAvailable ? "waiting for pictures" : tc->GetCameraErrorNameFromEnum(e);
        return g_camFrameOk;
    }
    if (g_camSrv && g_camSrv != srv) g_camSrv->Release();
    g_camSrv = srv;
    if (hdr.nFrameSequence == g_camSeq && g_camFrameOk) return true;
    g_camSeq = hdr.nFrameSequence;
    // Camera 0's picture (the left one on stereo cameras) inside the shared texture.
    vr::VRTextureBounds_t b{ 0, 0, 1, 1 }; uint32_t tw = 0, th = 0;
    tc->GetVideoStreamTextureSize(vr::k_unTrackedDeviceIndex_Hmd, vr::VRTrackedCameraFrameType_Undistorted, &b, &tw, &th);
    int32_t layout = vr::VRSystem()->GetInt32TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraFrameLayout_Int32);
    float u0 = b.uMin, v0 = b.vMin, du = b.uMax - b.uMin, dv = b.vMax - b.vMin;
    if (layout & vr::EVRTrackedCameraFrameLayout_Stereo) {
        if (layout & vr::EVRTrackedCameraFrameLayout_VerticalLayout) dv *= 0.5f; else du *= 0.5f;
    }
    g_camRect = { u0, v0, du, dv };
    // Camera pose: the headset at capture, times camera 0's place on the headset.
    HmdMatrix34_t camToHead[4]{};
    vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
    uint32_t got = vr::VRSystem()->GetArrayTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraToHeadTransforms_Matrix34_Array,
                                                                 vr::k_unHmdMatrix34PropertyTag, camToHead, sizeof camToHead, &pe);
    XMMATRIX c2h = got >= sizeof(HmdMatrix34_t) ? FromHmd(camToHead[0])
                 : FromHmd(vr::VRSystem()->GetMatrix34TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraToHeadTransform_Matrix34));
    XMMATRIX head = hdr.trackedDevicePose.bPoseIsValid ? FromHmd(hdr.trackedDevicePose.mDeviceToAbsoluteTracking) : g_headM;
    vr::HmdMatrix44_t pr{};
    tc->GetCameraProjection(vr::k_unTrackedDeviceIndex_Hmd, 0, vr::VRTrackedCameraFrameType_Undistorted, 0.05f, 100.0f, &pr);
    XMFLOAT4X4 prt;   // OpenVR's column-vector projection -> row vectors
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) prt.m[j][i] = pr.m[i][j];
    XMMATRIX worldToCam = XMMatrixInverse(nullptr, c2h * head);
    XMStoreFloat4x4(&g_camWorldToClip, worldToCam * XMLoadFloat4x4(&prt));
    if (!g_camFrameOk) Log("Camera pictures arriving: %ux%u, camera 0 in u %.3f..%.3f v %.3f..%.3f (layout 0x%x)\n", tw, th, u0, u0 + du, v0, v0 + dv, layout);
    g_camFrameOk = true;
    g_cameraStatus = "showing";
    return true;
}

// ================================================================ overlays
static vr::IVROverlay* O() { return vr::VROverlay(); }
static void SetTex(vr::VROverlayHandle_t ov, ID3D11Texture2D* t) {
    vr::Texture_t tex{ (void*)t, vr::TextureType_DirectX, vr::ColorSpace_Auto };
    vr::EVROverlayError e = O()->SetOverlayTexture(ov, &tex);
    if (e != vr::VROverlayError_None) { static int n = 0; if (n++ < 10) Log("SetOverlayTexture: %s\n", O()->GetOverlayErrorNameFromEnum(e)); }
}
// Laser mouse (edit mode only): SteamVR's laser pointer, which keeps the controllers from the game.
static void SetInteractive(vr::VROverlayHandle_t ov, bool on) {
    O()->SetOverlayInputMethod(ov, on ? vr::VROverlayInputMethod_Mouse : vr::VROverlayInputMethod_None);
    O()->SetOverlayFlag(ov, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, on);
    O()->SetOverlayFlag(ov, vr::VROverlayFlags_SendVRSmoothScrollEvents, on);
    vr::HmdVector2_t unit{ 1, 1 };
    O()->SetOverlayMouseScale(ov, &unit);
}
static vr::VROverlayHandle_t MakeOverlay(const std::string& key, const std::string& name) {
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError e = O()->CreateOverlay(key.c_str(), name.c_str(), &h);
    if (e != vr::VROverlayError_None) { Log("CreateOverlay %s: %s\n", key.c_str(), O()->GetOverlayErrorNameFromEnum(e)); return vr::k_ulOverlayHandleInvalid; }
    return h;
}

// Window overlays: one each, with its own picture.
struct WinOv { vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid; Canvas cv; std::string look; bool shown = false, inter = false;
              XMFLOAT4X4 frame{}; float halfW = 0.1f, halfH = 0.1f; };   // where the picture is (a box: the sheet facing you)
static std::vector<WinOv> g_winOv;
// Handle overlays: a pool, given out each frame to the selected window's handles.
struct HandleOv { vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid; ID3D11Texture2D* tex = nullptr; bool shown = false, inter = false, flip = false;
                  GrabMode mode = GRAB_NONE; int win = -1, k = -1; };
static std::vector<HandleOv> g_handleOv;
static std::map<std::string, ID3D11Texture2D*> g_iconTex;
static vr::VROverlayHandle_t g_guideOv = vr::k_ulOverlayHandleInvalid, g_traceOv = vr::k_ulOverlayHandleInvalid;
static vr::VROverlayHandle_t g_dashOv = vr::k_ulOverlayHandleInvalid, g_dashThumb = vr::k_ulOverlayHandleInvalid;
static Canvas g_traceCv;
static const float kTraceHalf = 1.25f;  // the drawing sheet: 2.5 m square on the window's plane

static ID3D11Texture2D* IconTexture(const IconLook& ic) {
    char key[160];
    snprintf(key, sizeof key, "%d %.3f %.3f %.3f %.3f %.3f %.3f %d", ic.id, ic.c[0], ic.c[1], ic.c[2], ic.g[0], ic.g[1], ic.g[2], ic.ownG);
    auto it = g_iconTex.find(key);
    if (it != g_iconTex.end()) return it->second;
    const int n = 128;
    std::vector<uint32_t> px((size_t)n * n, 0);
    PaintIcon(px.data(), n, n, n / 2, n / 2, n * 0.45f, ic, true);
    ID3D11Texture2D* t = MakeTexture(n, n, DXGI_FORMAT_B8G8R8A8_UNORM, px.data(), false);
    g_iconTex[key] = t;
    return t;
}

// ================================================================ guide panel and dashboard (GDI pictures)
struct Gdi {
    int W = 0, H = 0; HDC dc = nullptr; void* bits = nullptr;
    HFONT fTitle, fHead, fBtn, fText, fSmall;
    bool Init(int w, int h) {
        W = w; H = h;
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return false;
        SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        auto font = [](int px, int weight) { return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                                                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI"); };
        fTitle = font(44, FW_BOLD); fHead = font(32, FW_BOLD); fBtn = font(30, FW_SEMIBOLD); fText = font(27, FW_NORMAL); fSmall = font(23, FW_NORMAL);
        return true;
    }
    void Fill(RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
    void Round(RECT r, int rad, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
    }
    void Text(const std::wstring& s, HFONT f, COLORREF c, RECT r, UINT flags) { SelectObject(dc, f); SetTextColor(dc, c); DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX); }
    // A button: lit = hovered, on = held or active.
    void Button(RECT b, const std::wstring& label, const std::wstring& sub, bool lit, bool on, COLORREF base = RGB(48, 62, 84)) {
        COLORREF bg = on ? RGB(40, 150, 255) : lit ? RGB(64, 92, 130) : base;
        Round(b, 30, lit || on ? RGB(170, 215, 255) : RGB(80, 100, 130));
        Round({ b.left + 3, b.top + 3, b.right - 3, b.bottom - 3 }, 26, bg);
        if (sub.empty()) Text(label, fBtn, RGB(245, 250, 255), b, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        else {
            Text(label, fBtn, RGB(245, 250, 255), { b.left + 6, b.top + 8, b.right - 6, (b.top + b.bottom) / 2 + 6 }, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            Text(sub, fSmall, RGB(200, 215, 230), { b.left + 6, (b.top + b.bottom) / 2 + 2, b.right - 6, b.bottom - 6 }, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
    std::vector<uint32_t> Pixels() {
        GdiFlush();
        std::vector<uint32_t> px((const uint32_t*)bits, (const uint32_t*)bits + W * H);
        for (auto& p : px) p |= 0xFF000000u;
        return px;
    }
};

// Guide panel (edit mode): buttons along the top, then the controls, handle legend and status.
static const int kGuideW = 1900, kGuideH = 1080;
static const float kGuideWidthM = 0.95f;
enum GuideBtn { GB_NEW, GB_DELETE, GB_COLOUR, GB_ALL, GB_P1, GB_P2, GB_P3, GB_DONE, GB_COUNT };
static RECT GuideBtnRect(int i) {
    const int x0 = 30, gap = 14, w = (kGuideW - 2 * x0 - gap * (GB_COUNT - 1)) / GB_COUNT;
    return { x0 + i * (w + gap), 104, x0 + i * (w + gap) + w, 214 };
}
static int GuideBtnAt(int x, int y) {
    for (int i = 0; i < GB_COUNT; ++i) { RECT r = GuideBtnRect(i); if (x >= r.left - 6 && x < r.right + 6 && y >= r.top - 6 && y < r.bottom + 6) return i; }
    return -1;
}
static float g_guideMoveU = -1, g_guideMoveV = -1, g_dashMoveU = -1, g_dashMoveV = -1;   // last laser point (mouse) on each panel
// --export-pictures: the guide and dashboard panels drawn into these pixels instead of SteamVR.
static bool g_exporting = false;
static std::vector<uint32_t> g_exportPx;
static Gdi g_guideGdi;
static ID3D11Texture2D* g_guideTex = nullptr;
static std::wstring g_guideKey;
static int g_guideHover = -1, g_guideHeld = -1;
static double g_guideHeldSince = 0;
static bool g_guidePlaced = false;
static XMFLOAT4X4 g_guideM;   // panel frame (world)

static XMVECTOR GuideNormal() { return XMVector3Normalize(XMLoadFloat4x4(&g_guideM).r[2]); }

static void RenderGuide() {
    // What it shows; redrawn only when that changes.
    std::wstring sel;
    if (g_selected >= 0 && g_selected < (int)g_windows.size()) {
        const Window& w = g_windows[g_selected];
        sel = Fmt(L"Window %d of %d  ·  %.0f x %.0f cm  ·  %s%s  ·  %.1f m away", g_selected + 1, (int)g_windows.size(), w.hw * 200, w.hh * 200,
                  IsShape(w) ? L"free shape" : w.ellipse ? L"circle" : L"rectangle", ((w.lock ? std::wstring(L", angle locked") : std::wstring()) + (w.hd > 0 ? Fmt(L", box %.0f cm deep (%s)", w.hd * 200, Widen(kBoxModeName[w.boxMode]).c_str()) : std::wstring())).c_str(), Dist(g_head, V(w.pos)));
    } else sel = g_windows.empty() ? L"No windows yet: New window (above) makes one" : Fmt(L"None selected (%d windows): point at one and pull the trigger", (int)g_windows.size());
    if (g_drawArmed >= 0) sel += L"\nDrawing: hold the trigger on the window and trace the outline, let go to finish";
    if (Now() < g_windowsFullUntil) sel += Fmt(L"\n%d windows is the most: delete one to make another", kMaxWindows);
    double held = g_guideHeld >= 0 ? Now() - g_guideHeldSince : 0;
    int presetStep = (g_guideHeld >= GB_P1 && g_guideHeld <= GB_P3) ? std::min(10, (int)(held / 1.5 * 10)) : 0;
    bool flash = g_presetFlash >= 0 && Now() < g_presetFlashUntil;
    std::wstring key = sel + Fmt(L"|%d %d", (int)(g_guideMoveU * 400), (int)(g_guideMoveV * 400)) + Fmt(L"|%d %d %d %d %d %d %d%d%d %d %d", g_guideHover, g_guideHeld, presetStep, flash ? g_presetFlash : -1, g_presetFlashSaved,
                                 g_keyMode, g_presetHas[0], g_presetHas[1], g_presetHas[2], (int)g_windows.size(), g_windowsOn)
                     + Widen(g_gameName + g_cameraStatus);
    if (key == g_guideKey && g_guideTex) return;
    g_guideKey = key;
    Gdi& g = g_guideGdi;
    const COLORREF cHead = RGB(110, 230, 170), cText = RGB(232, 236, 242), cDim = RGB(150, 158, 170), cLine = RGB(52, 58, 68);
    g.Fill({ 0, 0, kGuideW, kGuideH }, RGB(22, 25, 31));
    g.Fill({ 0, 0, kGuideW, 86 }, RGB(30, 70, 60));
    g.Text(L"ALPHA VIEWS VR  ·  EDIT MODE", g.fTitle, RGB(235, 255, 245), { 36, 0, kGuideW - 36, 86 }, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    g.Text(L"Point the laser, pull the trigger (or grip)  ·  grab this panel to move it", g.fSmall, RGB(170, 210, 195), { 36, 0, kGuideW - 36, 86 }, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    static const wchar_t* kLabel[GB_COUNT] = { L"New window", L"Delete", L"Colour", L"Move all", L"Preset 1", L"Preset 2", L"Preset 3", L"Done" };
    for (int i = 0; i < GB_COUNT; ++i) {
        std::wstring sub;
        bool on = g_guideHeld == i;
        COLORREF base = RGB(48, 62, 84);
        if (i == GB_COLOUR) sub = Widen(kKeyName[g_keyMode]);
        if (i == GB_ALL) sub = on ? L"moving: let go to place" : L"hold and move";
        if (i == GB_DELETE) sub = g_selected >= 0 ? Fmt(L"window %d", g_selected + 1) : L"none selected";
        if (i == GB_DONE) { sub = L"save and finish"; base = RGB(30, 110, 70); }
        if (i >= GB_P1 && i <= GB_P3) {
            int p = i - GB_P1;
            bool fl = flash && g_presetFlash == p;
            sub = fl ? (g_presetFlashSaved ? L"saved" : L"loaded") : on ? (presetStep >= 10 ? L"saved" : L"saving...")
                : g_guideHover == i ? L"hold = save" : g_presetHas[p] ? L"tap = load" : L"empty";
            if (!g_presetHas[p]) base = RGB(34, 40, 50);
            if (fl) base = g_presetFlashSaved ? RGB(30, 130, 80) : RGB(40, 120, 200);
            on = on && presetStep < 10;
        }
        g.Button(GuideBtnRect(i), kLabel[i], sub, g_guideHover == i, on, base);
    }
    // Columns: laser controls, keyboard, status.
    const int top = 250, x1 = 36, x2 = 760, x3 = 1330, xEnd = kGuideW - 36;
    g.Text(L"LASER (edit mode)", g.fHead, cHead, { x1, top, x2, top + 44 }, DT_LEFT | DT_SINGLELINE);
    static const wchar_t* kLaser[] = {
        L"Trigger or grip on a window: select it and move it",
        L"While holding it, thumbstick up / down: push / pull",
        L"Handles on the selected window: grab and drag",
        L"Box handle (green): tap = box, hold + thumbstick = depth",
    };
    int y = top + 54;
    for (auto* s : kLaser) { g.Text(s, g.fText, cText, { x1, y, x2 - 30, y + 40 }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS); y += 44; }
    g.Text(L"KEYBOARD  (Ctrl+Alt + key)", g.fHead, cHead, { x2, top, x3, top + 44 }, DT_LEFT | DT_SINGLELINE);
    static const wchar_t* kKeys[][2] = { { L"E", L"Edit mode on / off" }, { L"N", L"New window" }, { L"X", L"Delete all windows" },
        { L"P", L"Windows on / off" }, { L"C", L"Colour" }, { L"G", L"Hide this panel" }, { L"1 2 3", L"Load preset (+Shift: save)" } };
    y = top + 54;
    for (auto& k : kKeys) {
        g.Round({ x2, y + 2, x2 + 76, y + 38 }, 12, RGB(70, 78, 92));
        g.Text(k[0], g.fSmall, RGB(245, 248, 252), { x2, y + 2, x2 + 76, y + 38 }, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        g.Text(k[1], g.fText, cText, { x2 + 92, y, x3 - 30, y + 40 }, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        y += 44;
    }
    g.Text(L"STATUS", g.fHead, cHead, { x3, top, xEnd, top + 44 }, DT_LEFT | DT_SINGLELINE);
    std::wstring st = Widen("Game: " + g_gameName) + Fmt(L"\nWindows: %d%s", (int)g_windows.size(), g_windowsOn ? L"" : L" (hidden: Ctrl+Alt+P)")
                    + L"\nColour: " + Widen(kKeyName[g_keyMode]) + L"\nCamera: " + Widen(g_cameraStatus);
    RECT sr{ x3, top + 54, xEnd, top + 54 + 300 };
    g.Text(st, g.fText, cText, sr, DT_LEFT | DT_WORDBREAK);
    for (int x : { x2 - 22, x3 - 22 }) g.Fill({ x, top, x + 3, top + 360 }, cLine);
    // Handle legend.
    y = top + 380;
    g.Fill({ x1, y, xEnd, y + 2 }, cLine);
    y += 16;
    static const IconLook legend[] = {
        { ICON_MOVE, { 0.15f, 0.45f, 1 } }, { ICON_RESIZE, { 1, 0.45f, 0 } }, { ICON_ROUND, { 1, 0.2f, 1 } },
        { ICON_TO_SHAPE, { 0.4f, 0.2f, 0.8f } }, { ICON_TO_CIRCLE, { 0.05f, 0.62f, 0.7f } }, { ICON_DRAW, { 0.92f, 0.92f, 0.92f } },
        { ICON_POINT, { 1, 0.45f, 0 } }, { ICON_ADD, { 1, 0.78f, 0.55f } }, { ICON_LOCK, { 0.98f, 0.76f, 0.12f } },
        { ICON_SNAP, { 0.55f, 0.8f, 1 }, { 0.08f, 0.09f, 0.11f }, true },
        { ICON_DEPTH, { 0.1f, 0.68f, 0.22f } },
        { ICON_KEEP, { 0.95f, 0.55f, 0.1f } },
    };
    static const wchar_t* words[] = { L"move", L"resize (tiny = delete)", L"round corner", L"free shape", L"circle", L"draw outline",
                                      L"point (tap = remove)", L"add point", L"lock angle", L"snap upright / level", L"box depth (tap / thumbstick)", L"box faces (cycle)" };
    g.Text(L"HANDLES", g.fHead, cHead, { x1, y, xEnd, y + 44 }, DT_LEFT | DT_SINGLELINE);
    y += 54;
    std::vector<std::pair<int, int>> spots;
    int x = x1;
    for (size_t i = 0; i < _countof(legend); ++i) {
        SelectObject(g.dc, g.fText);
        RECT need{ 0, 0, 600, 40 }; DrawTextW(g.dc, words[i], -1, &need, DT_CALCRECT | DT_SINGLELINE);
        if (x + 56 + need.right > xEnd) { x = x1; y += 52; }
        spots.push_back({ x + 22, y + 21 });
        g.Text(words[i], g.fText, cText, { x + 54, y, x + 54 + need.right + 4, y + 42 }, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 54 + need.right + 46;
    }
    y += 70;
    g.Fill({ x1, y, xEnd, y + 2 }, cLine);
    y += 16;
    g.Text(L"SELECTED WINDOW", g.fHead, cHead, { x1, y, xEnd, y + 44 }, DT_LEFT | DT_SINGLELINE);
    g.Text(sel, g.fText, cText, { x1, y + 52, xEnd, kGuideH - 10 }, DT_LEFT | DT_WORDBREAK);
    std::vector<uint32_t> px = g.Pixels();
    for (size_t i = 0; i < spots.size(); ++i) PaintIcon(px.data(), kGuideW, kGuideH, spots[i].first, spots[i].second, 20.0f, legend[i], false);
    if (g_exporting) { g_exportPx = std::move(px); return; }
    if (!g_guideTex) g_guideTex = MakeTexture(kGuideW, kGuideH, DXGI_FORMAT_B8G8R8A8_UNORM, px.data(), false);
    else g_ctx->UpdateSubresource(g_guideTex, 0, nullptr, px.data(), kGuideW * 4, 0);
    SetTex(g_guideOv, g_guideTex);
}

// Dashboard panel (the bottom-bar entry in the SteamVR dashboard).
static const int kDashW = 1600, kDashH = 900;
enum DashBtn { DB_EDIT, DB_WINDOWS, DB_COLOUR, DB_GUIDE, DB_AUTOSTART, DB_DELALL, DB_COUNT };
static RECT DashBtnRect(int i) {
    int col = i % 3, row = i / 3, w = 480, h = 150;
    return { 40 + col * (w + 20), 150 + row * (h + 24), 40 + col * (w + 20) + w, 150 + row * (h + 24) + h };
}
static Gdi g_dashGdi;
static ID3D11Texture2D* g_dashTex = nullptr;
static std::wstring g_dashKey;
static int g_dashHover = -1, g_dashHeld = -1;
static double g_dashHeldSince = 0;
static bool g_autoStart = false;

static void RenderDash() {
    double held = g_dashHeld == DB_DELALL ? Now() - g_dashHeldSince : 0;
    std::wstring key = Fmt(L"%d %d ", (int)(g_dashMoveU * 400), (int)(g_dashMoveV * 400)) + Fmt(L"%d %d %d %d %d %d %d %d %d", g_dashHover, g_dashHeld, (int)(held * 4), g_edit, g_windowsOn, g_keyMode, g_guideOn, g_autoStart, (int)g_windows.size())
                     + Widen(g_gameName + g_cameraStatus);
    if (key == g_dashKey && g_dashTex) return;
    g_dashKey = key;
    Gdi& g = g_dashGdi;
    const COLORREF cHead = RGB(110, 230, 170), cText = RGB(232, 236, 242), cDim = RGB(150, 158, 170);
    g.Fill({ 0, 0, kDashW, kDashH }, RGB(22, 25, 31));
    g.Fill({ 0, 0, kDashW, 110 }, RGB(30, 70, 60));
    g.Text(L"Alpha Views VR", g.fTitle, RGB(235, 255, 245), { 40, 0, kDashW - 40, 110 }, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    g.Text(Widen(std::string("version ") + AVVR_VERSION + "  ·  by GmoLargey"), g.fSmall, RGB(170, 210, 195), { 40, 0, kDashW - 40, 110 }, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    struct B { const wchar_t* label; std::wstring sub; bool on; COLORREF base; };
    B b[DB_COUNT] = {
        { g_edit ? L"Finish editing" : L"Edit windows", g_edit ? L"on: close the dashboard to edit" : L"move, resize, add windows", g_edit, RGB(30, 110, 70) },
        { g_windowsOn ? L"Windows: on" : L"Windows: off", g_windowsOn ? L"click to hide them" : L"click to show them", false, RGB(48, 62, 84) },
        { L"Colour", Widen(kKeyName[g_keyMode]) + (g_hasCamera ? L"  (green / blue / pink / camera)" : L"  (green / blue / pink)"), false, RGB(48, 62, 84) },
        { g_guideOn ? L"Guide panel: on" : L"Guide panel: off", L"shown in edit mode", false, RGB(48, 62, 84) },
        { g_autoStart ? L"Start with SteamVR: on" : L"Start with SteamVR: off", L"runs whenever SteamVR starts", false, RGB(48, 62, 84) },
        { L"Delete all windows", held > 0 ? Fmt(L"keep holding... %.0f", std::max(0.0, 2.0 - held) + 0.5) : std::wstring(L"hold for 2 seconds"), held > 0, RGB(110, 40, 40) },
    };
    for (int i = 0; i < DB_COUNT; ++i) g.Button(DashBtnRect(i), b[i].label, b[i].sub, g_dashHover == i, b[i].on, b[i].base);
    int y = 150 + 2 * 174 + 20;
    g.Text(L"STATUS", g.fHead, cHead, { 40, y, kDashW - 40, y + 44 }, DT_LEFT | DT_SINGLELINE);
    std::wstring st = Widen("Game: " + g_gameName) + Fmt(L"   ·   Windows for this game: %d", (int)g_windows.size())
                    + L"\nHeadset camera: " + Widen(g_cameraStatus)
                    + L"\nColour-key streamers (e.g. Virtual Desktop): set the same colour there as here.";
    g.Text(st, g.fText, cText, { 40, y + 52, kDashW - 40, y + 200 }, DT_LEFT | DT_WORDBREAK);
    g.Text(L"Edit mode: Edit windows (above) or Ctrl+Alt+E; finish with Done on the guide panel or here.  In edit mode SteamVR's laser pointer\n"
           L"works the windows and the game gets no controller input until you finish.",
           g.fSmall, cDim, { 40, kDashH - 110, kDashW - 40, kDashH - 20 }, DT_LEFT | DT_WORDBREAK);
    std::vector<uint32_t> px = g.Pixels();
    if (g_exporting) { g_exportPx = std::move(px); return; }
    if (!g_dashTex) g_dashTex = MakeTexture(kDashW, kDashH, DXGI_FORMAT_B8G8R8A8_UNORM, px.data(), false);
    else g_ctx->UpdateSubresource(g_dashTex, 0, nullptr, px.data(), kDashW * 4, 0);
    SetTex(g_dashOv, g_dashTex);
}
static void MakeDashThumb() {
    const int n = 256;
    std::vector<uint32_t> px((size_t)n * n, 0);
    // A rounded window in the key colour with a move handle: the app's mark.
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float dx = std::max(fabsf(x + 0.5f - n / 2.0f) - 70, 0.0f), dy = std::max(fabsf(y + 0.5f - n / 2.0f) - 46, 0.0f);
            float d = sqrtf(dx * dx + dy * dy) - 40;
            float a = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (a > 0) px[y * n + x] = (uint32_t(a * 255) << 24) | 0x0020D060;
        }
    IconLook mv{ ICON_MOVE, { 0.15f, 0.45f, 1 } };
    PaintIcon(px.data(), n, n, n / 2, n / 2, 40, mv, false);
    ID3D11Texture2D* t = MakeTexture(n, n, DXGI_FORMAT_B8G8R8A8_UNORM, px.data(), false);
    SetTex(g_dashThumb, t);
}

// ================================================================ edit mode and grabbing
struct Grab {
    GrabMode mode = GRAB_NONE; int win = -1, k = -1;
    bool dragging = false, newPoint = false;
    double t0 = 0;
    XMFLOAT4X4 c0{}, rel{};
    Window start;                 // the window when the drag began
    XMFLOAT3 hit0{};              // laser point (world) when the drag began
    XMFLOAT3 offset{};            // handle minus laser point on the plane
    float push = 0;
    XMFLOAT3 pushDir{};           // push / pull direction in the controller's axes (the laser at the grab)
    float dist0 = 1;              // laser length to the grab point when the drag began
    std::vector<XMFLOAT4X4> allRel;
};
static Grab g_grab;
static int g_mouseDev = -1;         // controller holding SteamVR's laser
static uint32_t g_mouseBtn = 0;     // mouse buttons down (1 trigger, 4 grip)
static XMVECTOR g_laserLocal = XMVectorSet(0, 0, -1, 0);   // laser direction in the controller's axes (measured)
static XMMATRIX g_ctl = XMMatrixIdentity();
static bool g_ctlOk = false;
static float g_scrollX = 0, g_scrollY = 0;
// What the laser is on (for highlights).
struct Hover { int kind = 0; int idx = -1; };   // kind 0 none, 1 window, 2 handle slot, 3 guide
static Hover g_hover;

static XMVECTOR LaserDir() { return XMVector3Normalize(XMVector3TransformNormal(g_laserLocal, g_ctl)); }
static void Calibrate(XMVECTOR p) {
    // Not while something is held: up close, small errors in the hit point would tilt the laser (and so
    // the push direction) as the window moves. Not from very near points either.
    if (!g_ctlOk || g_grab.mode != GRAB_NONE) return;
    if (Dist(p, g_ctl.r[3]) < 0.3f) return;
    XMVECTOR d = p - g_ctl.r[3];
    if (XMVectorGetX(XMVector3Length(d)) < 0.05f) return;
    d = XMVector3Normalize(d);
    XMMATRIX inv = XMMatrixTranspose(XMMATRIX(g_ctl.r[0], g_ctl.r[1], g_ctl.r[2], XMVectorSet(0, 0, 0, 1)));
    XMVECTOR l = XMVector3Normalize(XMVector3TransformNormal(d, inv));
    static bool first = true;
    g_laserLocal = first ? l : XMVector3Normalize(g_laserLocal * 0.7f + l * 0.3f);
    if (first) { first = false; Log("Laser measured: %.0f degrees from the controller's raw forward\n", acosf(std::clamp(-XMVectorGetZ(l), -1.0f, 1.0f)) * 57.3f); }
}
// Where the laser meets a plane (point p, normal n). False if it points away or along it.
static bool LaserPlane(XMVECTOR p, XMVECTOR n, XMVECTOR& hit) {
    if (!g_ctlOk) return false;
    XMVECTOR o = g_ctl.r[3], d = LaserDir();
    float den = Dot3(d, n);
    if (fabsf(den) < 1e-3f) return false;
    float t = Dot3(p - o, n) / den;
    if (t <= 0 || t > 60) return false;
    hit = o + d * t;
    return true;
}
static bool LaserOnWindow(const Window& w, XMVECTOR& hit) {
    return LaserPlane(V(w.pos), XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&w.rot)), hit);
}

static void PlaceGuide() {
    XMVECTOR p = g_head + HeadForward() * 0.95f + XMVectorSet(0, 0.12f, 0, 0);
    XMStoreFloat4x4(&g_guideM, FacingHead(p));
    g_guidePlaced = true;
}
static void SetEdit(bool on, const char* why) {
    if (g_edit == on) return;
    g_edit = on;
    if (!on) {
        if (g_grab.mode != GRAB_NONE && g_grab.win >= 0 && g_grab.win < (int)g_windows.size() && g_grab.dragging) g_windows[g_grab.win] = g_grab.start;
        g_grab = Grab{};
        g_drawArmed = -1; g_trace.clear();
        SaveWindows();
    } else {
        PlaceGuide();
        if (g_selected < 0 && !g_windows.empty()) g_selected = 0;
    }
    g_mouseBtn = 0; g_hover = {};
    Log("Edit mode %s (%s)\n", on ? "on" : "off: saved", why);
}

static void PressGuide(int px, int py) {
    int b = GuideBtnAt(px, py);
    g_guideHeld = b; g_guideHeldSince = Now();
    switch (b) {
    case GB_NEW: NewWindow(); break;
    case GB_DELETE: if (g_selected >= 0) DeleteWindow(g_selected); break;
    case GB_COLOUR: CycleKeyMode(); break;
    case GB_DONE: SetEdit(false, "Done button"); g_guideHeld = -1; return;
    case GB_ALL:
        g_grab = Grab{}; g_grab.mode = GRAB_ALL; g_grab.t0 = Now();
        for (auto& w : g_windows) { XMFLOAT4X4 r; XMStoreFloat4x4(&r, WinMatrix(w) * XMMatrixInverse(nullptr, g_ctl)); g_grab.allRel.push_back(r); }
        break;
    case GB_P1: case GB_P2: case GB_P3: g_grab = Grab{}; g_grab.mode = GRAB_PRESET; g_grab.k = b - GB_P1; g_grab.t0 = Now(); break;
    default:   // the panel itself: move it
        g_grab = Grab{}; g_grab.mode = GRAB_GUIDE;
        XMStoreFloat4x4(&g_grab.rel, XMLoadFloat4x4(&g_guideM) * XMMatrixInverse(nullptr, g_ctl));
        break;
    }
}

static void BeginWindowGrab(GrabMode mode, int wi, int k) {
    if (wi < 0 || wi >= (int)g_windows.size()) return;
    Window& w = g_windows[wi];
    // Buttons act straight away.
    switch (mode) {
    case GRAB_TYPE: ToggleShape(wi); g_grab = Grab{}; g_grab.mode = GRAB_BUTTON; return;
    case GRAB_CIRCLEBTN: ToggleCircle(wi); g_grab = Grab{}; g_grab.mode = GRAB_BUTTON; return;
    case GRAB_LOCKBTN: ToggleLock(wi); g_grab = Grab{}; g_grab.mode = GRAB_BUTTON; return;
    case GRAB_SNAPBTN: SnapWindow(wi); g_grab = Grab{}; g_grab.mode = GRAB_BUTTON; return;
    case GRAB_BOXMODE: w.boxMode = (w.boxMode + 1) % 4; Log("Window %d box: %s\n", wi, kBoxModeName[w.boxMode]); SaveWindows();
                       g_grab = Grab{}; g_grab.mode = GRAB_BUTTON; return;
    case GRAB_DRAWBTN: g_drawArmed = g_drawArmed == wi ? -1 : wi; Log("Drawing %s for window %d\n", g_drawArmed == wi ? "armed" : "off", wi);
                       g_grab = Grab{}; g_grab.mode = GRAB_BUTTON; return;
    default: break;
    }
    g_grab = Grab{};
    g_grab.mode = mode; g_grab.win = wi; g_grab.k = k; g_grab.t0 = Now();
    XMStoreFloat4x4(&g_grab.c0, g_ctl);
    g_grab.start = w;
    XMVECTOR hit = V(w.pos);
    LaserOnWindow(w, hit);
    XMStoreFloat3(&g_grab.hit0, hit);
    if (mode == GRAB_MOVE) { g_selected = wi; return; }
    if (mode == GRAB_TRACE) {
        g_tracePlane = w;
        if (g_ctlOk) {
            // As in Alpha Views XR: wherever you start, draw on a fresh plane facing you, at least 50 cm out
            // along the laser (as far as the window, if that's further). The window moves there when done.
            float d = std::max(0.5f, Dist(g_ctl.r[3], V(w.pos)));
            hit = g_ctl.r[3] + LaserDir() * d;
            SetWinFrame(g_tracePlane, FacingHead(hit));
        }
        XMFLOAT3 l = WorldToWin(g_tracePlane, hit);
        g_trace.assign(1, { l.x, l.y }); g_tracePrev = { l.x, l.y }; g_tracePrevOk = true;
        Log("Tracing a new outline for window %d\n", wi);
        return;
    }
    if (mode == GRAB_ADDPT && IsShape(w) && (int)w.pts.size() < kMaxShapePts) {
        XMFLOAT2 m = MidLocal(w, k);
        w.pts.insert(w.pts.begin() + k + 1, m);
        g_grab.mode = GRAB_POINT; g_grab.k = k + 1; g_grab.newPoint = true;
        Log("Shape point added (%d points)\n", (int)w.pts.size());
    } else if (mode == GRAB_ADDPT) { g_grab.mode = GRAB_BUTTON; return; }
    XMStoreFloat3(&g_grab.offset, TargetPoint(w, g_grab.mode, g_grab.k) - hit);
}

static void UpdateGrab(double now) {
    Grab& G = g_grab;
    if (!g_ctlOk) return;
    if (G.mode == GRAB_GUIDE) { XMStoreFloat4x4(&g_guideM, XMLoadFloat4x4(&G.rel) * g_ctl); return; }
    if (G.mode == GRAB_ALL) {
        for (size_t i = 0; i < g_windows.size() && i < G.allRel.size(); ++i) SetWinFrame(g_windows[i], XMLoadFloat4x4(&G.allRel[i]) * g_ctl);
        return;
    }
    if (G.mode == GRAB_PRESET) {
        if (now - G.t0 >= 1.5 && G.win != 1) { SavePreset(G.k); G.win = 1; }   // win = 1: saved during this hold
        return;
    }
    if (G.win < 0 || G.win >= (int)g_windows.size()) { if (G.mode != GRAB_BUTTON) G.mode = GRAB_NONE; return; }
    Window& w = g_windows[G.win];
    XMMATRIX c0 = XMLoadFloat4x4(&G.c0);
    if (!G.dragging) {
        // Nothing moves until the controller clearly has (pulling the trigger nudges it), then it starts
        // from there without a jump.
        bool go = Dist(g_ctl.r[3], c0.r[3]) > 0.01f || g_scrollX != 0 || g_scrollY != 0 || G.mode == GRAB_TRACE;
        XMVECTOR q0 = XMQuaternionRotationMatrix(c0), q1 = XMQuaternionRotationMatrix(g_ctl);
        go = go || fabsf(XMVectorGetX(XMQuaternionDot(q0, q1))) < cosf(XMConvertToRadians(1.5f));
        XMVECTOR hit;
        if (!go && G.mode != GRAB_MOVE && LaserOnWindow(w, hit)) go = Dist(hit, V(G.hit0)) > 0.01f * FarScale(V(w.pos));
        if (G.mode == GRAB_DEPTH) go = g_scrollX != 0 || g_scrollY != 0;   // the box: only the thumbstick
        if (!go) return;
        G.dragging = true;
        G.start = w;
        if (LaserOnWindow(w, hit)) XMStoreFloat3(&G.hit0, hit);
        XMStoreFloat4x4(&G.rel, WinMatrix(w) * XMMatrixInverse(nullptr, g_ctl));
        G.push = 0;
        XMStoreFloat3(&G.pushDir, g_laserLocal);
        G.dist0 = std::max(0.15f, Dist(g_ctl.r[3], V(G.hit0)));
        if (G.mode != GRAB_MOVE && G.mode != GRAB_TRACE) XMStoreFloat3(&G.offset, TargetPoint(w, G.mode, G.k) - V(G.hit0));
    }
    if (G.mode == GRAB_MOVE) {
        // Push / pull: along the laser, so the window rides on it like on a stick. How far out it is comes
        // from the grab (not measured again each frame), so pulling it close stops smoothly with the
        // grabbed point 30 cm out instead of bouncing.
        XMMATRIX base = XMLoadFloat4x4(&G.rel) * g_ctl;
        G.push = std::clamp(G.push + g_scrollY * 0.2f * std::max(0.3f, G.dist0 + G.push), 0.3f - G.dist0, 40.0f);
        XMMATRIX m = base * XMMatrixTranslationFromVector(XMVector3Normalize(XMVector3TransformNormal(V(G.pushDir), g_ctl)) * G.push);
        if (!w.lock) SetWinFrame(w, m);
        else {   // locked: keep the angle, follow the point the laser took hold of
            XMVECTOR local = XMVector3TransformCoord(V(G.hit0), XMMatrixInverse(nullptr, WinMatrix(G.start)));
            XMVECTOR nowP = XMVector3TransformCoord(local, m);
            XMStoreFloat3(&w.pos, V(G.start.pos) + (nowP - V(G.hit0)));
            w.rot = G.start.rot;
        }
        return;
    }
    if (G.mode == GRAB_DEPTH) {
        // Thumbstick only (moving the controller does nothing): up deeper, down shallower, starting at once
        // and faster as it gets deeper. The box grows evenly in front of and behind the window. Back to
        // ~1 cm: flat.
        G.push += g_scrollY * 0.15f * std::max(0.1f, w.hd);
        float hd = G.start.hd + G.push;
        if (hd < 0) G.push = -G.start.hd;   // no winding up below flat
        w.hd = hd < 0.005f ? 0 : std::min(hd, kMaxHalfDepth);
        return;
    }
    XMVECTOR hit;
    if (!LaserOnWindow(G.mode == GRAB_TRACE ? g_tracePlane : w, hit)) return;
    XMVECTOR p = hit + V(G.offset);
    if (G.mode == GRAB_CORNER) {
        int k = G.k;
        XMFLOAT3 l = WorldToWin(w, p);
        float ox = -kCS[k][0] * w.hw, oy = -kCS[k][1] * w.hh;
        XMVECTOR c = WinToWorld(w, (l.x + ox) * 0.5f, (l.y + oy) * 0.5f);
        w.hw = std::max(0.005f, fabsf(l.x - ox) * 0.5f);
        w.hh = std::max(0.005f, fabsf(l.y - oy) * 0.5f);
        XMStoreFloat3(&w.pos, c);
        ClampRadii(w);
    } else if (G.mode == GRAB_RADIUS) {
        int k = G.k;
        XMFLOAT3 l = WorldToWin(w, p);
        float u = ((w.hw - kCS[k][0] * l.x) + (w.hh - kCS[k][1] * l.y)) * 0.5f;
        w.radius[k] = std::clamp((u - kRadiusHandleInset) / kArcK, 0.0f, std::min(w.hw, w.hh));
    } else if (G.mode == GRAB_POINT && G.k >= 0 && G.k < (int)w.pts.size()) {
        XMFLOAT3 l = WorldToWin(w, p);
        w.pts[G.k] = { l.x, l.y };
        UpdateShapeBounds(w);
    } else if (G.mode == GRAB_TRACE) {
        XMFLOAT3 l = WorldToWin(g_tracePlane, hit);
        float jx = l.x - g_tracePrev.x, jy = l.y - g_tracePrev.y;
        bool good = g_tracePrevOk && jx * jx + jy * jy <= 0.15f * 0.15f;
        g_tracePrev = { l.x, l.y }; g_tracePrevOk = true;
        const XMFLOAT2& last = g_trace.back();
        float dx = l.x - last.x, dy = l.y - last.y;
        if (good && dx * dx + dy * dy > 0.008f * 0.008f && g_trace.size() < 600) g_trace.push_back({ l.x, l.y });
        if (TraceAtStart(g_trace)) G.k = -2;   // closed on the start dot: finish on release
    }
}
static void FinishTrace(int wi) {
    Window& w = g_windows[wi];
    std::vector<XMFLOAT2> s = TraceToShape(g_trace);
    if (s.empty()) Log("Traced outline too small or too short (%d samples): shape unchanged, still armed\n", (int)g_trace.size());
    else {
        w.pos = g_tracePlane.pos; w.rot = g_tracePlane.rot;
        w.pts = s; w.ellipse = false;
        RecentreShape(w);
        g_drawArmed = -1;
        Log("Window %d outline drawn: %d points\n", wi, (int)w.pts.size());
    }
    g_trace.clear();
}
static void EndGrab(double now) {
    Grab& G = g_grab;
    if (G.mode == GRAB_PRESET) { if (G.win != 1) LoadPreset(G.k); }
    else if (G.mode == GRAB_GUIDE || G.mode == GRAB_ALL) { if (G.mode == GRAB_ALL) SaveWindows(); }
    else if (G.win >= 0 && G.win < (int)g_windows.size()) {
        Window& w = g_windows[G.win];
        bool tap = now - G.t0 < 0.35 && !G.dragging;
        bool shrunk = G.dragging && (w.hw < G.start.hw - 1e-4f || w.hh < G.start.hh - 1e-4f);
        if (G.mode == GRAB_CORNER && shrunk && (w.hw < 0.01f || w.hh < 0.01f)) {
            Log("Window %d shrunk to %.1f x %.1f cm: deleting it\n", G.win, w.hw * 200, w.hh * 200);
            int wi = G.win; g_grab = Grab{}; DeleteWindow(wi); return;
        }
        if (G.mode == GRAB_POINT && IsShape(w)) {
            if (tap && !G.newPoint && w.pts.size() > 3 && G.k >= 0 && G.k < (int)w.pts.size()) {
                w.pts.erase(w.pts.begin() + G.k);
                Log("Shape point removed (%d left)\n", (int)w.pts.size());
            }
            RecentreShape(w);
        }
        if (G.mode == GRAB_TRACE) FinishTrace(G.win);
        if (G.mode == GRAB_DEPTH) {
            if (tap) w.hd = w.hd > 0 ? 0 : 0.1f;   // tap: a 20 cm box, or flat again
            Log("Window %d depth %.0f cm%s\n", G.win, w.hd * 200, w.hd > 0 ? "" : " (flat)");
            SaveWindows();
        }
        if (G.dragging || G.mode == GRAB_POINT || G.mode == GRAB_TRACE) SaveWindows();
    }
    g_grab = Grab{};
    g_guideHeld = -1;
}

// ================================================================ handles for the selected window
struct UiIcon { XMVECTOR at; float r; IconLook look; XMVECTOR ax, ay; GrabMode mode; int k; };
static void BuildHandles(std::vector<UiIcon>& out) {
    out.clear();
    if (!g_edit || g_selected < 0 || g_selected >= (int)g_windows.size() || !g_windowsOn) return;
    const int wi = g_selected;
    const Window& w = g_windows[wi];
    XMVECTOR wq = XMLoadFloat4(&w.rot);
    XMVECTOR wr = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), wq), wu = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), wq);
    const XMVECTOR face = XMVectorZero();
    auto hovered = [&](GrabMode m, int k) {
        if (g_hover.kind != 2 || g_hover.idx < 0 || g_hover.idx >= (int)g_handleOv.size()) return false;
        const HandleOv& h = g_handleOv[g_hover.idx];
        return h.mode == m && h.k == k && h.win == wi;
    };
    auto active = [&](GrabMode m, int k) { return g_grab.win == wi && g_grab.mode == m && g_grab.k == k; };
    auto add = [&](GrabMode m, int k, float r, IconLook look, XMVECTOR ax, XMVECTOR ay) {
        out.push_back({ TargetPoint(w, m, k), r, look, ax, ay, m, k });
    };
    { bool h = hovered(GRAB_MOVE, -1), a = active(GRAB_MOVE, -1);
      add(GRAB_MOVE, -1, kCenterHandleR * (h || a ? 1.25f : 1), a ? IconLook{ ICON_MOVE, { 1, 0.9f, 0.2f } } : IconLook{ ICON_MOVE, { h ? 0.35f : 0.15f, h ? 0.65f : 0.45f, 1 } }, wr, wu); }
    { bool h = hovered(GRAB_TYPE, -1); float s = h ? 1.0f : 0.8f;
      add(GRAB_TYPE, -1, kButtonR * (h ? 1.25f : 1), { IsShape(w) ? ICON_TO_RECT : ICON_TO_SHAPE, { 0.5f * s, 0.25f * s, 1.0f * s } }, face, face); }
    if (!IsShape(w)) { bool h = hovered(GRAB_CIRCLEBTN, -1); float s = h ? 1.0f : 0.82f;
      add(GRAB_CIRCLEBTN, -1, kButtonR * (h ? 1.25f : 1), { w.ellipse ? ICON_TO_RECT : ICON_TO_CIRCLE, { 0.05f * s, 0.62f * s, 0.7f * s } }, face, face); }
    { bool h = hovered(GRAB_LOCKBTN, -1);
      if (w.lock) add(GRAB_LOCKBTN, -1, kButtonR * 1.25f, { ICON_LOCK, { 0.98f, 0.76f, 0.12f } }, face, face);
      else { float b = h ? 0.62f : 0.42f; add(GRAB_LOCKBTN, -1, kButtonR * (h ? 1.25f : 1), { ICON_UNLOCK, { b, b, b } }, face, face); } }
    { bool h = hovered(GRAB_SNAPBTN, -1); float s = h ? 1.0f : 0.85f;
      add(GRAB_SNAPBTN, -1, kButtonR * (h ? 1.25f : 1), { ICON_SNAP, { 0.55f * s, 0.8f * s, 1.0f * s }, { 0.08f, 0.09f, 0.11f }, true }, face, face); }
    { bool h = hovered(GRAB_DEPTH, -1), a = active(GRAB_DEPTH, -1);
      IconLook look = a ? IconLook{ ICON_DEPTH, { 1, 0.9f, 0.2f } } : IconLook{ ICON_DEPTH, { h ? 0.3f : 0.1f, h ? 0.9f : 0.68f, h ? 0.4f : 0.22f } };
      add(GRAB_DEPTH, -1, kDepthHandleR * (h || a ? 1.25f : 1), look, face, face); }
    if (w.hd > 0) {   // box mode: which faces show (each press: the next)
        static const float kCol[4][3] = { { 0.1f, 0.55f, 0.25f }, { 0.95f, 0.55f, 0.1f }, { 0.1f, 0.6f, 0.9f }, { 0.6f, 0.3f, 0.95f } };
        bool h = hovered(GRAB_BOXMODE, -1); float s = h ? 1.15f : 0.9f;
        const float* c = kCol[w.boxMode];
        add(GRAB_BOXMODE, -1, kButtonR * (h ? 1.25f : 1), { ICON_KEEP, { std::min(1.0f, c[0] * s), std::min(1.0f, c[1] * s), std::min(1.0f, c[2] * s) } }, face, face);
    }
    if (IsShape(w)) {
        bool h = hovered(GRAB_DRAWBTN, -1), armed = g_drawArmed == wi;
        add(GRAB_DRAWBTN, -1, kButtonR * (h || armed ? 1.25f : 1), { ICON_DRAW, { armed ? 0.95f : 0.92f, armed ? 0.2f : 0.92f, armed ? 0.2f : 0.92f } }, face, face);
        for (int k = 0; k < (int)w.pts.size(); ++k) {
            bool ph = hovered(GRAB_POINT, k), pa = active(GRAB_POINT, k);
            add(GRAB_POINT, k, kPointR * (ph || pa ? 1.25f : 1), { ICON_POINT, { 1, pa ? 1.0f : (ph ? 0.7f : 0.45f), pa ? 0.5f : 0 } }, wr, wu);
            bool mh = hovered(GRAB_ADDPT, k);
            if ((int)w.pts.size() < kMaxShapePts) add(GRAB_ADDPT, k, kMidR * (mh ? 1.4f : 1), { ICON_ADD, { 1, 0.78f, 0.55f } }, wr, wu);
        }
        return;
    }
    for (int k = 0; k < 4; ++k) {
        float sx = kCS[k][0], sy = kCS[k][1];
        bool h = hovered(GRAB_CORNER, k), a = active(GRAB_CORNER, k);
        XMVECTOR diag = XMVector3Normalize(wr * sx + wu * sy), perp = XMVector3Normalize(wu * sx - wr * sy);
        add(GRAB_CORNER, k, kCornerHandleR * (h || a ? 1.25f : 1), { ICON_RESIZE, { 1, a ? 1.0f : (h ? 0.7f : 0.45f), a ? 0.5f : 0 } }, diag, perp);
        if (w.ellipse) continue;
        bool rh = hovered(GRAB_RADIUS, k), ra = active(GRAB_RADIUS, k);
        add(GRAB_RADIUS, k, kRadiusHandleR * (rh || ra ? 1.25f : 1), { ICON_ROUND, { 1, 0.2f, 1 } }, wr * sx, wu * sy);
    }
}
static void ShowHandles(const std::vector<UiIcon>& icons) {
    while (g_handleOv.size() < icons.size()) {
        HandleOv h;
        h.ov = MakeOverlay("gmo.avvr.handle" + std::to_string(g_handleOv.size()), "Alpha Views VR handle");
        if (h.ov == vr::k_ulOverlayHandleInvalid) break;
        O()->SetOverlaySortOrder(h.ov, 20);
        g_handleOv.push_back(h);
    }
    for (size_t i = 0; i < g_handleOv.size(); ++i) {
        HandleOv& h = g_handleOv[i];
        if (i >= icons.size()) {
            if (h.shown) { O()->HideOverlay(h.ov); h.shown = false; }
            if (h.inter) { SetInteractive(h.ov, false); h.inter = false; }
            h.mode = GRAB_NONE; h.win = -1;
            continue;
        }
        const UiIcon& u = icons[i];
        ID3D11Texture2D* t = IconTexture(u.look);
        if (t != h.tex) { SetTex(h.ov, t); h.tex = t; }
        // Facing: buttons face your head; move, corner and rounding handles lie in the window's plane.
        XMVECTOR x, y, z;
        if (XMVectorGetX(XMVector3LengthSq(u.ax)) < 1e-6f) {
            XMMATRIX f = FacingHead(u.at); x = f.r[0]; y = f.r[1]; z = f.r[2];
        } else { x = XMVector3Normalize(u.ax); y = XMVector3Normalize(u.ay); z = XMVector3Cross(x, y); }
        // An overlay shows its front only: if the front faces away, turn it round and mirror the picture.
        bool flip = Dot3(z, g_head - u.at) < 0;
        if (flip) { x = -x; z = -z; }
        if (flip != h.flip) { vr::VRTextureBounds_t b = flip ? vr::VRTextureBounds_t{ 1, 0, 0, 1 } : vr::VRTextureBounds_t{ 0, 0, 1, 1 }; O()->SetOverlayTextureBounds(h.ov, &b); h.flip = flip; }
        XMVECTOR at = u.at + z * 0.002f;
        HmdMatrix34_t m = ToHmd(Frame(x, y, z, at));
        O()->SetOverlayTransformAbsolute(h.ov, vr::TrackingUniverseStanding, &m);
        O()->SetOverlayWidthInMeters(h.ov, 2 * u.r * FarScale(u.at) / 0.9f);
        h.mode = u.mode; h.win = g_selected; h.k = u.k;
        if (!h.inter) { SetInteractive(h.ov, true); h.inter = true; }
        if (!h.shown) { O()->ShowOverlay(h.ov); h.shown = true; }
    }
}

// ================================================================ window pictures
static std::string WindowLook(const Window& w, bool selected, bool hover) {
    char b[256];
    snprintf(b, sizeof b, "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %d %d %d %d %d %zu", w.hw, w.hh, w.hd, w.radius[0], w.radius[1], w.radius[2], w.radius[3],
             w.ellipse, g_keyMode, selected, hover, g_edit, w.pts.size());
    std::string s = b;
    for (auto& p : w.pts) { snprintf(b, sizeof b, " %.4f %.4f", p.x, p.y); s += b; }
    return s;
}
// A box: an overlay is flat, so it shows a picture of the whole box as seen from your head, on a sheet
// facing you just behind the window's centre plane (redrawn as you move). From your head it looks the
// same as the box itself, and the handles (on the centre plane) stay in front of it. Long boxes that
// reach past you are cut off just in front of your head, and the sheet stops at a wide view angle (the
// edges of the picture are then simply off to the sides). Gives the sheet's frame and half size, the
// solid triangles and the outlines (centre line, front, back; sheet metres from its centre), and how
// much bigger things look on the sheet than at the window. False when you're inside the box.
struct WinView { XMFLOAT4X4 frame; float halfW, halfH, scale; std::vector<XMFLOAT2> tris; std::vector<XMFLOAT2> outline[3]; bool box; };
static bool BoxView(const Window& w, const std::vector<XMFLOAT2>& per, const std::vector<int>& idx, WinView& v) {
    // The sheet faces you from the box's middle; if you're inside the box (or its middle is beside or
    // behind you), it faces you straight ahead instead, and shows the walls in front of you.
    XMVECTOR c = V(w.pos);
    XMFLOAT3 hl = WorldToWin(w, g_head);
    bool inside = fabsf(hl.z) < w.hd + 0.05f && fabsf(hl.x) < w.hw + 0.05f && fabsf(hl.y) < w.hh + 0.05f;
    XMMATRIX f = FacingHead(c);
    XMVECTOR x = f.r[0], y = f.r[1], z = f.r[2];
    float cd = Dot3(c - g_head, z);
    if (inside || cd > -0.1f) {
        z = -XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), g_headM));
        XMVECTOR up = fabsf(XMVectorGetY(z)) > 0.95f ? XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), g_headM) : XMVectorSet(0, 1, 0, 0);
        x = XMVector3Normalize(XMVector3Cross(up, z));
        y = XMVector3Cross(z, x);
        cd = Dot3(c - g_head, z);
    }
    size_t n = per.size();
    static const float kZ[3] = { 0, 1, -1 };   // centre line, front (+hd), back
    std::vector<XMVECTOR> P(3 * n);
    float planeD = -0.3f;
    for (int r = 0; r < 3; ++r)
        for (size_t i = 0; i < n; ++i) {
            P[r * n + i] = WinToWorld(w, per[i].x, per[i].y, kZ[r] * w.hd);
            if (r == 0) planeD = std::min(planeD, Dot3(P[i] - g_head, z));
        }
    planeD = std::max(planeD - 0.03f, -20.0f);   // just behind the handles
    v.scale = cd < -0.1f ? planeD / cd : 1.0f;
    XMVECTOR c0 = g_head + z * planeD;
    c0 = c0 + x * Dot3(c - g_head, x) * (cd < -0.1f ? v.scale : 0.0f) + y * Dot3(c - g_head, y) * (cd < -0.1f ? v.scale : 0.0f);
    auto proj = [&](XMVECTOR p) {
        XMVECTOR q = g_head + (p - g_head) * (planeD / Dot3(p - g_head, z));
        return XMFLOAT2{ Dot3(q - c0, x), Dot3(q - c0, y) };
    };
    const float kNear = 0.08f;
    std::vector<XMFLOAT2> tri;   // sheet metres
    auto addTri = [&](XMVECTOR a, XMVECTOR b, XMVECTOR e) {
        XMVECTOR in[3] = { a, b, e };
        float d[3];
        for (int i = 0; i < 3; ++i) d[i] = -Dot3(in[i] - g_head, z) - kNear;   // >= 0: far enough in front
        XMVECTOR poly[4]; int m = 0;
        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3;
            if (d[i] >= 0) poly[m++] = in[i];
            if ((d[i] >= 0) != (d[j] >= 0)) poly[m++] = in[i] + (in[j] - in[i]) * (d[i] / (d[i] - d[j]));
        }
        for (int k = 1; k + 1 < m; ++k) { tri.push_back(proj(poly[0])); tri.push_back(proj(poly[k])); tri.push_back(proj(poly[k + 1])); }
    };
    // Box mode: which faces show. Fixed to the box itself (they never swap as you look round or turn it):
    // the window's front is the near wall and its back the far wall; the face along the window's top edge
    // is the top, along its bottom edge the floor, and the rest are the sides.
    enum { kFaceTop, kFaceFloor, kFaceFar, kFaceNear, kFaceSide };
    auto kindOf = [&](XMVECTOR localN, bool cap) {
        if (cap) return XMVectorGetZ(localN) > 0 ? (int)kFaceNear : (int)kFaceFar;
        float ny = XMVectorGetY(localN);
        return ny > 0.7f ? (int)kFaceTop : ny < -0.7f ? (int)kFaceFloor : (int)kFaceSide;
    };
    auto shown = [&](int k) {
        switch (w.boxMode) {
        case 1: return k == kFaceTop || k == kFaceFar;                    // top and far wall
        case 2: return k == kFaceFar || k == kFaceFloor;              // far wall and floor
        case 3: return k != kFaceNear && k != kFaceFloor;              // all but the front and floor
        default: return true;                                      // the whole box
        }
    };
    const size_t F = n, B = 2 * n;
    for (int s = 0; s < 2; ++s) {   // the front (+z) and back faces
        if (!shown(kindOf(XMVectorSet(0, 0, s ? -1.0f : 1.0f, 0), true))) continue;
        size_t base = s ? B : F;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) addTri(P[base + idx[t]], P[base + idx[t + 1]], P[base + idx[t + 2]]);
    }
    for (size_t i = 0; i < n; ++i) {   // the sides, one per edge of the outline
        size_t j = (i + 1) % n;
        float dx = per[j].x - per[i].x, dy = per[j].y - per[i].y;
        if (dx * dx + dy * dy < 1e-10f) continue;
        XMVECTOR nrm = XMVector3Normalize(XMVectorSet(dy, -dx, 0, 0));   // outward, in the box's axes (outline is anticlockwise)
        if (!shown(kindOf(nrm, false))) continue;
        addTri(P[F + i], P[F + j], P[B + j]);
        addTri(P[F + i], P[B + j], P[B + i]);
    }
    if (tri.empty()) {
        if (w.boxMode == 0) return false;
        // None of the chosen faces is in view from here: show nothing (a tiny empty sheet).
        v.halfW = v.halfH = 0.01f;
        XMStoreFloat4x4(&v.frame, Frame(x, y, z, c0));
        v.box = true;
        return true;
    }
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (auto& s : tri) { x0 = std::min(x0, s.x); x1 = std::max(x1, s.x); y0 = std::min(y0, s.y); y1 = std::max(y1, s.y); }
    // No wider than ~68 degrees either side of the box's middle (past that it's out to the side anyway).
    float lim = 2.5f * -planeD;
    x0 = std::max(x0, -lim); x1 = std::min(x1, lim); y0 = std::max(y0, -lim); y1 = std::min(y1, lim);
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, pad = 0.01f * v.scale;
    v.halfW = (x1 - x0) * 0.5f + pad; v.halfH = (y1 - y0) * 0.5f + pad;
    XMStoreFloat4x4(&v.frame, Frame(x, y, z, c0 + x * cx + y * cy));
    for (auto& s : tri) v.tris.push_back({ (s.x - cx) / v.halfW, (s.y - cy) / v.halfH });
    for (int r = 0; r < 3; ++r) {
        bool ok = true;
        for (size_t i = 0; i < n && ok; ++i) ok = -Dot3(P[r * n + i] - g_head, z) > kNear;
        if (!ok) continue;   // that ring reaches past you: no outline for it
        for (size_t i = 0; i < n; ++i) { XMFLOAT2 s = proj(P[r * n + i]); v.outline[r].push_back({ s.x - cx, s.y - cy }); }
    }
    v.box = true;
    return true;
}
// Box faces as real overlays: each face of a box in its true place, so both eyes see real depth (one
// picture for the whole box looks only as deep as the picture). Used for every box except the one
// selected in edit mode (its handles must stay in front) and in camera mode (one picture per box).
// The front face is the window's own overlay (moved out to the front); the back and the sides come from
// this pool, shared by all boxes.
struct FaceOv { vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid; bool shown = false, inter = false; int mirror = -1; ID3D11Texture2D* tex = nullptr; int win = -1; };
static std::vector<FaceOv> g_faceOv;
static bool g_facePoolFull = false;
static ID3D11Texture2D* g_solidTex[3] = {};
static ID3D11Texture2D* SolidTexture(int key) {
    if (!g_solidTex[key]) {
        const float* c = kKeyCol[key];
        uint32_t px[16];
        for (auto& p : px) p = 0xFF000000u | (uint32_t(c[0] * 255) << 16) | (uint32_t(c[1] * 255) << 8) | uint32_t(c[2] * 255);
        g_solidTex[key] = MakeTexture(4, 4, DXGI_FORMAT_B8G8R8A8_UNORM, px, false);
    }
    return g_solidTex[key];
}
static int FaceKindOf(XMVECTOR localN, bool cap) {   // as in BoxView: 0 top, 1 floor, 2 far, 3 near, 4 side
    if (cap) return XMVectorGetZ(localN) > 0 ? 3 : 2;
    float ny = XMVectorGetY(localN);
    return ny > 0.7f ? 0 : ny < -0.7f ? 1 : 4;
}
static bool FaceShown(int mode, int k) {
    switch (mode) {
    case 1: return k == 0 || k == 2;
    case 2: return k == 2 || k == 1;
    case 3: return k != 3 && k != 1;
    default: return true;
    }
}
struct Face { XMVECTOR c, x, y, z; float w, h; ID3D11Texture2D* tex; bool mirror; int win; };
static bool UseRealFaces(const Window& w, int wi) {
    bool camera = g_keyMode == 3 && g_camFrameOk && g_camSrv;
    return w.hd > 0 && !camera && !(g_edit && g_selected == wi);
}
// The back face and the sides of a box (the front face is the window's overlay).
static void BoxFaces(const Window& w, int wi, ID3D11Texture2D* capTex, std::vector<Face>& out) {
    XMVECTOR q = XMLoadFloat4(&w.rot);
    XMVECTOR wx = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q), wy = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), q), wz = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
    XMVECTOR c = V(w.pos);
    if (FaceShown(w.boxMode, 2) && capTex)   // back: the window's picture seen from behind
        out.push_back({ c - wz * w.hd, -wx, wy, -wz, 2 * w.hw, 2 * w.hh, capTex, true, wi });
    std::vector<XMFLOAT2> per;
    Perimeter(w, per);
    if (per.size() > 24) per = SimplifyClosed(per, 24);   // round boxes: at most 24 sides (SteamVR allows 128 overlays in all)
    size_t n = per.size();
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        float dx = per[j].x - per[i].x, dy = per[j].y - per[i].y, len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-4f) continue;
        if (!FaceShown(w.boxMode, FaceKindOf(XMVectorSet(dy / len, -dx / len, 0, 0), false))) continue;
        XMVECTOR out3 = XMVector3Normalize(wx * (dy / len) + wy * (-dx / len));
        XMVECTOR mid = c + wx * ((per[i].x + per[j].x) * 0.5f) + wy * ((per[i].y + per[j].y) * 0.5f);
        XMVECTOR ax = XMVector3Cross(wz, out3);   // along the edge, so that (ax, wz, out) is right-handed
        out.push_back({ mid, ax, wz, out3, len + 0.003f, 2 * w.hd, SolidTexture(std::min(g_keyMode, 2)), false, wi });
    }
}
static void ShowFaces(const std::vector<Face>& faces) {
    while (g_faceOv.size() < faces.size() && !g_facePoolFull) {
        FaceOv f;
        f.ov = MakeOverlay("gmo.avvr.face" + std::to_string(g_faceOv.size()), "Alpha Views VR box face");
        if (f.ov == vr::k_ulOverlayHandleInvalid) { g_facePoolFull = true; Log("Box faces: no more overlays (%d in use)\n", (int)g_faceOv.size()); break; }
        O()->SetOverlaySortOrder(f.ov, 1);
        g_faceOv.push_back(f);
    }
    for (size_t i = 0; i < g_faceOv.size(); ++i) {
        FaceOv& f = g_faceOv[i];
        if (i >= faces.size()) {
            if (f.shown) { O()->HideOverlay(f.ov); f.shown = false; }
            if (f.inter) { SetInteractive(f.ov, false); f.inter = false; }
            f.win = -1;
            continue;
        }
        const Face& fc = faces[i];
        if (fc.tex != f.tex) { SetTex(f.ov, fc.tex); f.tex = fc.tex; }
        // An overlay is drawn from its front: if you're behind this face, turn it round and mirror.
        XMVECTOR x = fc.x, z = fc.z;
        bool mirror = fc.mirror;
        if (Dot3(z, g_head - fc.c) < 0) { x = -x; z = -z; mirror = !mirror; }
        if ((int)mirror != f.mirror) {
            vr::VRTextureBounds_t b = mirror ? vr::VRTextureBounds_t{ 1, 0, 0, 1 } : vr::VRTextureBounds_t{ 0, 0, 1, 1 };
            O()->SetOverlayTextureBounds(f.ov, &b); f.mirror = mirror;
        }
        HmdMatrix34_t m = ToHmd(Frame(x, fc.y, z, fc.c));
        O()->SetOverlayTransformAbsolute(f.ov, vr::TrackingUniverseStanding, &m);
        O()->SetOverlayWidthInMeters(f.ov, fc.w);
        O()->SetOverlayTexelAspect(f.ov, fc.w / std::max(1e-4f, fc.h));
        f.win = fc.win;
        // Edit mode: any face selects its box (the front face may be hidden by the box mode).
        const Window& fw = g_windows[fc.win];
        XMFLOAT3 hl = WorldToWin(fw, g_head);
        bool inter = g_edit && !(fabsf(hl.z) < fw.hd + 0.05f && fabsf(hl.x) < fw.hw + 0.05f && fabsf(hl.y) < fw.hh + 0.05f);   // not from inside it
        if (g_grab.mode != GRAB_NONE) inter = f.inter;
        if (inter != f.inter) { SetInteractive(f.ov, inter); f.inter = inter; }
        if (!f.shown) { O()->ShowOverlay(f.ov); f.shown = true; }
    }
}
static void ViewOf(const Window& w, WinView& v, bool flat = false) {
    std::vector<XMFLOAT2> per;
    Perimeter(w, per);
    std::vector<int> idx;
    Triangulate(per, idx);
    v.tris.clear(); for (auto& o : v.outline) o.clear(); v.box = false; v.scale = 1;
    if (w.hd > 0 && !flat && BoxView(w, per, idx, v)) return;
    v.tris.clear(); for (auto& o : v.outline) o.clear(); v.scale = 1;
    XMStoreFloat4x4(&v.frame, WinMatrix(w));
    v.halfW = w.hw; v.halfH = w.hh;
    for (int i : idx) v.tris.push_back({ per[i].x / w.hw, per[i].y / w.hh });
    v.outline[0] = per;
}
static void RenderWindow(int wi, WinOv& o, bool selected, bool hover, bool camNew) {
    const Window& w = g_windows[wi];
    const bool real = UseRealFaces(w, wi);   // a box drawn face by face: this is its (flat) front face
    std::string look = WindowLook(w, selected, hover) + (real ? " real" : "");
    bool camera = g_keyMode == 3 && g_camFrameOk && g_camSrv;
    // Redrawn when it changes; boxes and the camera every frame (they change as you move).
    bool every = camera || (w.hd > 0 && !real);
    WinView v;
    ViewOf(w, v, real);
    if (real) XMStoreFloat4x4(&v.frame, XMMatrixTranslation(0, 0, w.hd) * WinMatrix(w));
    o.frame = v.frame; o.halfW = v.halfW; o.halfH = v.halfH;
    if (look == o.look && !every) return;
    o.look = look;
    if (!o.cv.tex && !o.cv.Init(512)) { Log("Window picture: couldn't make a render target\n"); return; }
    const float* kc = kKeyCol[std::min(g_keyMode, 2)];
    float clear[4] = { camera ? 0.0f : kc[0], camera ? 0.0f : kc[1], camera ? 0.0f : kc[2], 0 };   // edge pixels keep the colour
    o.cv.Begin(clear);
    CB cb{};
    cb.col = { kc[0], kc[1], kc[2], 1 };
    cb.halfMode = { v.halfW, v.halfH, camera ? 1.0f : 0.0f, 0 };
    if (camera) {
        XMStoreFloat4x4(&cb.toCam, XMLoadFloat4x4(&v.frame) * XMLoadFloat4x4(&g_camWorldToClip));
        cb.camRect = g_camRect;
        g_ctx->PSSetShaderResources(0, 1, &g_camSrv);
    }
    DrawTris(v.tris, cb);
    // Edit mode: the selected window's outline (white), or the one the laser is on (grey). A box: its
    // centre line (where the handles are) like that, its front and back edges thinner and dimmer.
    if (g_edit && (selected || hover)) {
        float wid = 0.006f * FarScale(V(w.pos)) * v.scale;
        for (int r : { 1, 2, 0 }) {
            if (v.outline[r].size() < 2) continue;
            std::vector<XMFLOAT2> rib;
            Ribbon(v.outline[r], true, r ? wid * 0.6f : wid, 1 / v.halfW, 1 / v.halfH, rib);
            CB oc{};
            float b = (selected ? 1.0f : 0.6f) * (r ? 0.45f : 1.0f);
            oc.col = { b, b, b, 1 };
            oc.halfMode = { v.halfW, v.halfH, 0, 0 };
            DrawTris(rib, oc);
        }
    }
    ID3D11ShaderResourceView* none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);
    o.cv.End();
    SetTex(o.ov, o.cv.tex);
}
static void ShowWindows(bool camNew) {
    std::vector<Face> faces;
    while (g_winOv.size() < g_windows.size()) {
        WinOv o;
        o.ov = MakeOverlay("gmo.avvr.window" + std::to_string(g_winOv.size()), "Alpha Views VR window");
        if (o.ov == vr::k_ulOverlayHandleInvalid) break;
        O()->SetOverlaySortOrder(o.ov, 1);
        g_winOv.push_back(o);
    }
    for (size_t i = 0; i < g_winOv.size(); ++i) {
        WinOv& o = g_winOv[i];
        bool want = i < g_windows.size() && g_windowsOn;
        bool real = want && UseRealFaces(g_windows[i], (int)i);
        if (real) BoxFaces(g_windows[i], (int)i, o.cv.tex, faces);
        if (real && !FaceShown(g_windows[i].boxMode, 3)) {   // front face not shown in this box mode
            if (o.shown) { O()->HideOverlay(o.ov); o.shown = false; }
            if (o.inter) { SetInteractive(o.ov, false); o.inter = false; }
            RenderWindow((int)i, o, false, false, camNew);   // keeps its picture current (the back face uses it)
            continue;
        }
        if (!want) {
            if (o.shown) { O()->HideOverlay(o.ov); o.shown = false; }
            if (o.inter) { SetInteractive(o.ov, false); o.inter = false; }
            continue;
        }
        bool hover = g_hover.kind == 1 && g_hover.idx == (int)i;
        RenderWindow((int)i, o, g_edit && g_selected == (int)i, g_edit && hover, camNew);
        HmdMatrix34_t m = ToHmd(XMLoadFloat4x4(&o.frame));
        O()->SetOverlayTransformAbsolute(o.ov, vr::TrackingUniverseStanding, &m);
        O()->SetOverlayWidthInMeters(o.ov, 2 * o.halfW);
        O()->SetOverlayTexelAspect(o.ov, o.halfW / o.halfH);
        // Inside a box its picture fills your view: it doesn't take the laser then (the guide panel and the
        // handles stay reachable).
        XMFLOAT3 hl = WorldToWin(g_windows[i], g_head);
        const Window& wi = g_windows[i];
        bool inside = wi.hd > 0 && fabsf(hl.z) < wi.hd + 0.05f && fabsf(hl.x) < wi.hw + 0.05f && fabsf(hl.y) < wi.hh + 0.05f;
        bool inter = g_edit && !inside;
        if (g_grab.mode != GRAB_NONE && g_grab.win == (int)i) inter = o.inter;   // held: unchanged, or its let-go would be lost
        if (o.inter != inter) { SetInteractive(o.ov, inter); o.inter = inter; }
        if (!o.shown) { O()->ShowOverlay(o.ov); o.shown = true; }
    }
    ShowFaces(faces);
}
// Drawing: a big see-through sheet (armed: following the laser; tracing: on the drawing plane), so the trace can start and run
// anywhere, not only on the window. It shows the outline so far (white) and the start dot (yellow).
static XMFLOAT4X4 g_traceSheetM;
static void ShowTrace() {
    static bool inter = false;
    bool tracing = g_edit && g_grab.mode == GRAB_TRACE && !g_trace.empty();
    bool armed = g_edit && g_drawArmed >= 0 && g_drawArmed < (int)g_windows.size();
    if (!tracing && !armed) {
        O()->HideOverlay(g_traceOv);
        if (inter) { SetInteractive(g_traceOv, false); inter = false; }
        return;
    }
    if (!g_traceCv.tex && !g_traceCv.Init(1024)) return;
    const Window& plane = tracing ? g_tracePlane : g_windows[g_drawArmed];
    float clear[4] = { 1, 1, 1, 0 };
    g_traceCv.Begin(clear);
    if (tracing) {
        std::vector<XMFLOAT2> rib;
        Ribbon(g_trace, false, 0.006f, 1 / kTraceHalf, 1 / kTraceHalf, rib);
        const XMFLOAT2 s = g_trace[0];
        std::vector<XMFLOAT2> dot;
        for (int i = 0; i < 24; ++i) {
            float a0 = XM_2PI * i / 24, a1 = XM_2PI * (i + 1) / 24;
            dot.push_back({ s.x / kTraceHalf, s.y / kTraceHalf });
            dot.push_back({ (s.x + cosf(a0) * kTraceClose) / kTraceHalf, (s.y + sinf(a0) * kTraceClose) / kTraceHalf });
            dot.push_back({ (s.x + cosf(a1) * kTraceClose) / kTraceHalf, (s.y + sinf(a1) * kTraceClose) / kTraceHalf });
        }
        CB cb{}; cb.halfMode = { kTraceHalf, kTraceHalf, 0, 0 };
        cb.col = { 1, 0.85f, 0.1f, 1 }; DrawTris(dot, cb);
        cb.col = { 1, 1, 1, 1 }; DrawTris(rib, cb);
    }
    g_traceCv.End();
    SetTex(g_traceOv, g_traceCv.tex);
    XMMATRIX sm = XMMatrixTranslation(0, 0, -0.003f) * WinMatrix(plane);
    if (!tracing && g_ctlOk) {
        // Armed: the sheet follows the laser, square on to it, just past the window (or 50 cm out), so a
        // press anywhere you point lands on it; the window and its handles stay in front of it.
        XMVECTOR d = LaserDir(), z = -d;
        float dist = std::max(0.5f, Dist(g_ctl.r[3], V(plane.pos))) + 0.05f;
        XMVECTOR up = fabsf(XMVectorGetY(z)) > 0.95f ? XMVectorSet(0, 0, -1, 0) : XMVectorSet(0, 1, 0, 0);
        XMVECTOR r = XMVector3Normalize(XMVector3Cross(up, z));
        sm = Frame(r, XMVector3Cross(z, r), z, g_ctl.r[3] + d * dist);
    }
    XMStoreFloat4x4(&g_traceSheetM, sm);
    HmdMatrix34_t m = ToHmd(sm);
    O()->SetOverlayTransformAbsolute(g_traceOv, vr::TrackingUniverseStanding, &m);
    O()->SetOverlayWidthInMeters(g_traceOv, 2 * kTraceHalf);
    if (!inter) { SetInteractive(g_traceOv, true); inter = true; }
    O()->ShowOverlay(g_traceOv);
}
// Where the laser meets the guide panel, in its pixels (for when the drawing sheet is in front of it).
static bool LaserOnGuide(int& px, int& py) {
    if (!g_guidePlaced || !g_guideOn) return false;
    XMMATRIX gm = XMLoadFloat4x4(&g_guideM);
    XMVECTOR hit;
    if (!LaserPlane(gm.r[3], XMVector3Normalize(gm.r[2]), hit)) return false;
    XMVECTOR l = XMVector3TransformCoord(hit, XMMatrixInverse(nullptr, gm));
    float hh = kGuideWidthM * kGuideH / kGuideW;
    float u = XMVectorGetX(l) / kGuideWidthM + 0.5f, v = XMVectorGetY(l) / hh + 0.5f;
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    px = (int)(u * kGuideW); py = (int)((1 - v) * kGuideH);
    return true;
}
static void ShowGuide() {
    bool on = g_edit && g_guideOn && g_guidePlaced;
    static bool inter = false;
    if (!on) { O()->HideOverlay(g_guideOv); if (inter) { SetInteractive(g_guideOv, false); inter = false; } return; }
    RenderGuide();
    HmdMatrix34_t m = ToHmd(XMLoadFloat4x4(&g_guideM));
    O()->SetOverlayTransformAbsolute(g_guideOv, vr::TrackingUniverseStanding, &m);
    O()->SetOverlayWidthInMeters(g_guideOv, kGuideWidthM);
    if (!inter) { SetInteractive(g_guideOv, true); inter = true; }
    O()->ShowOverlay(g_guideOv);
}

// ================================================================ laser mouse events
static void HandleOverlayEvents(double now) {
    vr::VREvent_t ev;
    g_scrollX = g_scrollY = 0;
    bool released = false;
    auto onEvent = [&](int kind, int idx, const vr::VREvent_t& e) {
        switch (e.eventType) {
        case vr::VREvent_MouseMove: {
            g_hover = { kind == 5 ? 1 : kind, idx };   // 5: a box face, as its window
            g_mouseDev = (int)e.trackedDeviceIndex;
            // SteamVR mouse (scale 1x1): x 0..1 across; y measured in widths from the centre, up (so a wide
            // panel only spans 0.5 -+ height/width/2). Turn y into 0..1 of the height.
            float u = e.data.mouse.x, v = e.data.mouse.y;
            if (kind == 3) { v = 0.5f + (v - 0.5f) * kGuideW / kGuideH; g_guideMoveU = u; g_guideMoveV = v; }
            if (kind == 1 && idx < (int)g_winOv.size()) { const WinOv& o = g_winOv[idx]; Calibrate(XMVector3TransformCoord(XMVectorSet((u - 0.5f) * 2 * o.halfW, (v - 0.5f) * 2 * o.halfW, 0, 1), XMLoadFloat4x4(&o.frame))); }
            if (kind == 3) {
                XMMATRIX gm = XMLoadFloat4x4(&g_guideM);
                float hh = kGuideWidthM * kGuideH / kGuideW;
                Calibrate(XMVector3TransformCoord(XMVectorSet((u - 0.5f) * kGuideWidthM, (v - 0.5f) * hh, 0, 1), gm));
                g_guideHover = GuideBtnAt((int)(u * kGuideW), (int)((1 - v) * kGuideH));
            }
            if (kind == 4) {   // the drawing sheet: square, so y needs no change
                Calibrate(XMVector3TransformCoord(XMVectorSet((u - 0.5f) * 2 * kTraceHalf, (v - 0.5f) * 2 * kTraceHalf, 0, 1), XMLoadFloat4x4(&g_traceSheetM)));
                int gx, gy;
                g_guideHover = LaserOnGuide(gx, gy) ? GuideBtnAt(gx, gy) : -1;
            }
            break;
        }
        case vr::VREvent_FocusLeave:
            if (g_hover.kind == (kind == 5 ? 1 : kind) && g_hover.idx == idx) g_hover = {};
            if (kind == 3) g_guideHover = -1;
            break;
        case vr::VREvent_MouseButtonDown: {
            bool wasDown = g_mouseBtn != 0;
            g_mouseBtn |= e.data.mouse.button;
            g_mouseDev = (int)e.trackedDeviceIndex;
            if (wasDown || g_grab.mode != GRAB_NONE) break;
            if ((kind == 1 || kind == 5) && idx >= 0 && idx < (int)g_windows.size()) {
                if (g_drawArmed == idx) BeginWindowGrab(GRAB_TRACE, idx, -1);
                else { g_selected = idx; BeginWindowGrab(GRAB_MOVE, idx, -1); }
            } else if (kind == 2 && idx < (int)g_handleOv.size()) {
                const HandleOv& h = g_handleOv[idx];
                if (h.win >= 0) BeginWindowGrab(h.mode, h.win, h.k);
            } else if (kind == 3) {
                PressGuide((int)(g_guideMoveU * kGuideW), (int)((1 - g_guideMoveV) * kGuideH));
            } else if (kind == 4) {
                // The drawing sheet: if the laser is really on the guide panel behind it, that; else start tracing.
                int gx, gy;
                if (LaserOnGuide(gx, gy)) PressGuide(gx, gy);
                else if (g_drawArmed >= 0 && g_drawArmed < (int)g_windows.size()) BeginWindowGrab(GRAB_TRACE, g_drawArmed, -1);
            }
            break;
        }
        case vr::VREvent_MouseButtonUp:
            g_mouseBtn &= ~e.data.mouse.button;
            if (!g_mouseBtn) released = true;
            break;
        case vr::VREvent_ScrollSmooth: case vr::VREvent_ScrollDiscrete:
            g_scrollX += e.data.scroll.xdelta; g_scrollY += e.data.scroll.ydelta;
            break;
        default: break;
        }
    };
    for (size_t i = 0; i < g_winOv.size(); ++i) while (O()->PollNextOverlayEvent(g_winOv[i].ov, &ev, sizeof ev)) onEvent(1, (int)i, ev);
    for (size_t i = 0; i < g_handleOv.size(); ++i) while (O()->PollNextOverlayEvent(g_handleOv[i].ov, &ev, sizeof ev)) onEvent(2, (int)i, ev);
    while (O()->PollNextOverlayEvent(g_guideOv, &ev, sizeof ev)) onEvent(3, 0, ev);
    while (O()->PollNextOverlayEvent(g_traceOv, &ev, sizeof ev)) onEvent(4, 0, ev);
    for (size_t i = 0; i < g_faceOv.size(); ++i) while (O()->PollNextOverlayEvent(g_faceOv[i].ov, &ev, sizeof ev)) if (g_faceOv[i].win >= 0) onEvent(5, g_faceOv[i].win, ev);

    // The controller holding the laser.
    vr::TrackedDevicePose_t pose;
    g_ctlOk = false;
    if (g_mouseDev >= 0 && g_mouseDev < (int)vr::k_unMaxTrackedDeviceCount) {
        vr::TrackedDevicePose_t all[vr::k_unMaxTrackedDeviceCount];
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, all, vr::k_unMaxTrackedDeviceCount);
        pose = all[g_mouseDev];
        if (pose.bPoseIsValid) { g_ctl = FromHmd(pose.mDeviceToAbsoluteTracking); g_ctlOk = true; }
    }
    if (!g_edit) return;
    // Backup for letting go: SteamVR only sends the button-up to an overlay still under the laser. If the
    // controller's own state has shown the button held during this grab, its release counts too.
    if (g_grab.mode != GRAB_NONE && g_mouseDev >= 0 && !released) {
        static bool seenDown = false; static double grabT = -1;
        if (grabT != g_grab.t0) { grabT = g_grab.t0; seenDown = false; }
        vr::VRControllerState_t cs{};
        if (vr::VRSystem()->GetControllerState((vr::TrackedDeviceIndex_t)g_mouseDev, &cs, sizeof cs)) {
            const uint64_t mask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger) | vr::ButtonMaskFromId(vr::k_EButton_Grip);
            bool down = (cs.ulButtonPressed & mask) != 0 || cs.rAxis[1].x > 0.3f;
            if (down) seenDown = true;
            else if (seenDown) { released = true; g_mouseBtn = 0; Log("Let go (from the controller: the laser had left what it held)\n"); }
        }
    }
    if (g_grab.mode != GRAB_NONE) UpdateGrab(now);
    if (released && g_grab.mode != GRAB_NONE) EndGrab(now);
    if (released) g_guideHeld = -1;
}

static void HandleDashEvents(double now) {
    vr::VREvent_t ev;
    while (O()->PollNextOverlayEvent(g_dashOv, &ev, sizeof ev)) {
        int b = -1;
        if (ev.eventType == vr::VREvent_MouseMove) { g_dashMoveU = ev.data.mouse.x; g_dashMoveV = 0.5f + (ev.data.mouse.y - 0.5f) * kDashW / kDashH; }
        if (ev.eventType == vr::VREvent_MouseMove || ev.eventType == vr::VREvent_MouseButtonDown) {
            int x = (int)(g_dashMoveU * kDashW), y = (int)((1 - g_dashMoveV) * kDashH);
            for (int i = 0; i < DB_COUNT; ++i) { RECT r = DashBtnRect(i); if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) b = i; }
        }
        if (ev.eventType == vr::VREvent_MouseMove) g_dashHover = b;
        else if (ev.eventType == vr::VREvent_FocusLeave) { g_dashHover = -1; g_dashHeld = -1; }
        else if (ev.eventType == vr::VREvent_MouseButtonDown && b >= 0) {
            g_dashHeld = b; g_dashHeldSince = now;
            switch (b) {
            case DB_EDIT: SetEdit(!g_edit, "dashboard"); break;
            case DB_WINDOWS: g_windowsOn = !g_windowsOn; Log("Windows %s\n", g_windowsOn ? "on" : "off"); SaveWindows(); break;
            case DB_COLOUR: CycleKeyMode(); break;
            case DB_GUIDE: g_guideOn = !g_guideOn; SaveSettings(); break;
            case DB_AUTOSTART:
                g_autoStart = !g_autoStart;
                vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, g_autoStart);
                Log("Start with SteamVR: %s\n", g_autoStart ? "on" : "off");
                break;
            default: break;
            }
        } else if (ev.eventType == vr::VREvent_MouseButtonUp) g_dashHeld = -1;
    }
    if (g_dashHeld == DB_DELALL && now - g_dashHeldSince > 2.0) {
        g_windows.clear(); g_selected = -1; g_drawArmed = -1;
        Log("All windows deleted (dashboard)\n");
        SaveWindows();
        g_dashHeld = -1;
    }
}

// ================================================================ keyboard and controllers (outside edit mode)
static bool KeyPressed(int vk) {
    static bool down[256] = {};
    bool now = (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    bool pressed = now && !down[vk];
    down[vk] = now;
    return pressed;
}
static void HandleKeys() {
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (KeyPressed('E')) SetEdit(!g_edit, "Ctrl+Alt+E");
    if (KeyPressed('P')) { g_windowsOn = !g_windowsOn; Log("Windows %s (Ctrl+Alt+P)\n", g_windowsOn ? "on" : "off"); SaveWindows(); }
    if (KeyPressed('N')) NewWindow();
    if (KeyPressed('X')) { g_windows.clear(); g_selected = -1; g_drawArmed = -1; Log("All windows deleted (Ctrl+Alt+X)\n"); SaveWindows(); }
    if (KeyPressed('C')) CycleKeyMode();
    if (KeyPressed('G')) { g_guideOn = !g_guideOn; SaveSettings(); Log("Guide panel %s\n", g_guideOn ? "on" : "off"); }
    for (int i = 0; i < 3; ++i) if (KeyPressed('1' + i)) { if (shift) SavePreset(i); else LoadPreset(i); }
}


// ================================================================ the game in the scene
static void CheckGame() {
    uint32_t pid = vr::VRApplications()->GetCurrentSceneProcessId();
    char key[vr::k_unMaxApplicationKeyLength] = "";
    if (pid) vr::VRApplications()->GetApplicationKeyByProcessId(pid, key, sizeof key);
    std::string k = key[0] ? key : "none";
    if (k == g_gameKey) return;
    if (g_edit) SetEdit(false, "game changed");
    else SaveWindows();
    g_gameKey = k;
    char name[256] = "";
    if (key[0]) vr::VRApplications()->GetApplicationPropertyString(key, vr::VRApplicationProperty_Name_String, name, sizeof name);
    g_gameName = name[0] ? name : key[0] ? key : "No game (SteamVR Home or none)";
    Log("Game: %s (%s)\n", g_gameName.c_str(), g_gameKey.c_str());
    LoadWindows();
}

// ================================================================ main
static void RegisterApp(const std::string& exeDir) {
    auto* apps = vr::VRApplications();
    std::string manifest = exeDir + "\\alphaviewsvr.vrmanifest";
    vr::EVRApplicationError e = apps->AddApplicationManifest(manifest.c_str(), false);
    if (e != vr::VRApplicationError_None) Log("AddApplicationManifest: %s\n", apps->GetApplicationsErrorNameFromEnum(e));
    apps->IdentifyApplication(GetCurrentProcessId(), kAppKey);
    // First run: start with SteamVR (the dashboard button turns it off).
    bool first = GetFileAttributesA((g_dir + "\\autostart_set.flag").c_str()) == INVALID_FILE_ATTRIBUTES;
    if (first) {
        apps->SetApplicationAutoLaunch(kAppKey, true);
        if (FILE* f = fopen((g_dir + "\\autostart_set.flag").c_str(), "w")) fclose(f);
    }
    g_autoStart = apps->GetApplicationAutoLaunch(kAppKey);
    Log("Registered with SteamVR (%s), start with SteamVR: %s\n", manifest.c_str(), g_autoStart ? "on" : "off");
}

// ================================================================ example pictures (--export-pictures <folder>)
static bool SavePng(const std::wstring& path, const std::vector<uint32_t>& px, int W, int H) {
    IWICImagingFactory* f = nullptr; IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))
           && SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
           && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) && SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache))
           && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(W, H));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(fr->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppBGRA
            && SUCCEEDED(fr->WritePixels(H, W * 4, W * H * 4, (BYTE*)px.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
    if (fr) fr->Release(); if (e) e->Release(); if (s) s->Release(); if (f) f->Release();
    return ok;
}
// The guide panel and the dashboard panel as they look in the headset, with an example window selected.
static int ExportPictures(const std::wstring& dir) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_exporting = true;
    if (!g_guideGdi.Init(kGuideW, kGuideH) || !g_dashGdi.Init(kDashW, kDashH)) return 1;
    g_gameName = "Example game";
    g_cameraStatus = "none (headset or driver has no camera for SteamVR)";
    g_head = XMVectorSet(0, 1.6f, 0, 0);
    Window w; w.pos = { 0, 1.2f, -0.8f }; w.hw = 0.25f; w.hh = 0.15f; w.hd = 0.2f; w.boxMode = 1;
    for (float& r : w.radius) r = 0.03f;
    g_windows = { w, Window{} };
    g_selected = 0; g_edit = true; g_autoStart = true;
    g_presetHas[0] = true;
    int bad = 0;
    RenderGuide();
    if (!SavePng(dir + L"/Guide.png", g_exportPx, kGuideW, kGuideH)) ++bad;
    RenderDash();
    if (!SavePng(dir + L"/Dashboard.png", g_exportPx, kDashW, kDashH)) ++bad;
    CoUninitialize();
    return bad;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    {   // --export-pictures <folder>: save the example pictures for the README, then quit
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 3 && wcscmp(argv[1], L"--export-pictures") == 0) { int r = ExportPictures(argv[2]); LocalFree(argv); return r; }
        if (argv) LocalFree(argv);
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\AlphaViewsVR");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // already running
    InitDirAndLog();
    Log("Alpha Views VR %s\n", AVVR_VERSION);
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string exeDir = exe; exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));

    vr::EVRInitError ie = vr::VRInitError_None;
    vr::VR_Init(&ie, vr::VRApplication_Overlay);
    if (ie != vr::VRInitError_None) {
        Log("Couldn't connect to SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(ie));
        MessageBoxW(nullptr, L"Alpha Views VR needs SteamVR. Start SteamVR first, then run it again.\n\n"
                             L"(Once it has run, it starts with SteamVR by itself and lives in the SteamVR dashboard.)",
                    L"Alpha Views VR", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    Log("Connected to SteamVR\n");
    if (!InitD3D()) { vr::VR_Shutdown(); return 1; }
    RegisterApp(exeDir);
    LoadSettings();
    CheckCamera();

    g_guideOv = MakeOverlay("gmo.avvr.guide", "Alpha Views VR guide");
    g_traceOv = MakeOverlay("gmo.avvr.trace", "Alpha Views VR outline");
    O()->SetOverlaySortOrder(g_guideOv, 10);
    O()->SetOverlaySortOrder(g_traceOv, 15);
    vr::EVROverlayError de = O()->CreateDashboardOverlay("gmo.avvr.dashboard", "Alpha Views", &g_dashOv, &g_dashThumb);
    if (de != vr::VROverlayError_None) Log("CreateDashboardOverlay: %s\n", O()->GetOverlayErrorNameFromEnum(de));
    else {
        O()->SetOverlayWidthInMeters(g_dashOv, 2.4f);
        O()->SetOverlayInputMethod(g_dashOv, vr::VROverlayInputMethod_Mouse);
        vr::HmdVector2_t unit{ 1, 1 };
        O()->SetOverlayMouseScale(g_dashOv, &unit);
        MakeDashThumb();
    }
    if (!g_guideGdi.Init(kGuideW, kGuideH) || !g_dashGdi.Init(kDashW, kDashH)) Log("GDI setup failed\n");
    CheckGame();

    bool running = true;
    double last = Now(), lastGameCheck = 0;
    std::vector<UiIcon> icons;
    while (running) {
        double now = Now();
        float dt = (float)std::min(0.1, now - last);
        last = now;
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
            if (ev.eventType == vr::VREvent_Quit) { Log("SteamVR is quitting\n"); vr::VRSystem()->AcknowledgeQuit_Exiting(); running = false; }
            if (ev.eventType == vr::VREvent_SceneApplicationChanged) lastGameCheck = 0;
        }
        if (!running) break;
        if (now - lastGameCheck > 1.0) { lastGameCheck = now; CheckGame(); }

        vr::TrackedDevicePose_t hmd[1];
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, hmd, 1);
        if (hmd[0].bPoseIsValid) { g_headM = FromHmd(hmd[0].mDeviceToAbsoluteTracking); g_head = g_headM.r[3]; g_headOk = true; }

        HandleKeys();
        HandleDashEvents(now);
        HandleOverlayEvents(now);

        // Camera: stream only while it's in use.
        bool wantCam = g_keyMode == 3 && g_windowsOn && !g_windows.empty();
        if (wantCam) StartCamera(); else if (g_cam != INVALID_TRACKED_CAMERA_HANDLE) StopCamera();
        static uint32_t seenSeq = 0;
        bool camNew = wantCam && UpdateCamera() && g_camSeq != seenSeq;
        if (camNew) seenSeq = g_camSeq;

        ShowWindows(camNew);
        BuildHandles(icons);
        ShowHandles(icons);
        ShowTrace();
        ShowGuide();
        if (O()->IsDashboardVisible()) RenderDash();

        std::this_thread::sleep_for(std::chrono::milliseconds(wantCam ? 8 : 11));
    }
    SaveWindows();
    StopCamera();
    vr::VR_Shutdown();
    Log("Closed\n");
    if (single) CloseHandle(single);
    return 0;
}
