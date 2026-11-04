// Silvaron PiP, native side. Loaded by Warhammer3.exe as dinput8.dll (the game imports it, and Windows looks in the
// game folder first); DirectInput8Create is passed on to the system's dinput8.dll.
// The pack's battle script (silvaron_pip.lua) films the old close-up action cams (close charge, brace, hard hits)
// and writes its camera to silvaron_pip_cam.txt every 100 ms. This DLL hooks IDXGISwapChain::Present and the game's frame call (exe 0x19bc046): every
// second frame, just after the game draws its own (hk_frame), it has the game render the scene from that camera
// into the top-left of the back buffer (the game's own screenshot path, exe 0x1a804e8, without its tiles: see
// ROUTE_B.md), copies that part into a texture, and draws the texture in the box on the frames presented. F10 turns it off/on.
// On/off: the DLL does nothing unless the pack silvaron_pip.pack is ticked in the launcher (used_mods.txt in the
// game folder, written by the launcher at each launch, is read once at load); in game the pack's MCT checkbox
// writes silvaron_pip_settings.txt ("enabled 0|1"), read every 2 s.
// Also Silvaron UI Scale: with silvaron_ui_scale.pack ticked, the game's limit on UI scales above 1.0 below
// 2560x1440 is removed (ui_scale_patch).
// Also the controller's base layer (silvaron_overlord.pack ticked; see PAD.md): an XInput pad turned into mouse and
// keys on every screen (a DualSense/DS4 without XInput read over HID), and its state written to silvaron_pad.txt for the battle script (pad_start, independent of the PiP).
// Log: <game folder>/silvaron_pip_log.txt
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <share.h>
#include <dinput.h>
#include <xinput.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <intrin.h>
#include <math.h>

static HMODULE g_real;          // system dinput8.dll
static FILE *g_log;
static uintptr_t g_exe;          // Warhammer3.exe base
static bool g_box = true;        // F10
static bool g_enabled = true;    // the MCT checkbox (silvaron_pip_settings.txt)
static char g_dir[MAX_PATH];     // the game folder, with a trailing backslash

static void pad_wrap_di(REFIID r, LPVOID *o);   // the controller: wraps the game's DirectInput (below)

typedef HRESULT(WINAPI *DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u)
{
	if (!g_real) {
		char p[MAX_PATH];
		GetSystemDirectoryA(p, MAX_PATH);
		strcat_s(p, "\\dinput8.dll");
		g_real = LoadLibraryA(p);
	}
	DI8Create f = g_real ? (DI8Create)GetProcAddress(g_real, "DirectInput8Create") : nullptr;
	HRESULT hr = f ? f(h, v, r, o, u) : E_FAIL;
	if (SUCCEEDED(hr) && o && *o) pad_wrap_di(r, o);
	return hr;
}

static void logf(const char *fmt, ...)
{
	if (!g_log) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list a;
	va_start(a, fmt);
	vfprintf(g_log, fmt, a);
	va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

static uintptr_t rva(uintptr_t p) { return p >= g_exe && p < g_exe + 0x8000000 ? p - g_exe : 0; }

static double now_s()
{
	static LARGE_INTEGER f;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)f.QuadPart;
}

// ---- camera channel ------------------------------------------------------------------------------------------
// silvaron_pip_cam.txt: "seq cut active ex ey ez tx ty tz fovdeg" then "end", rewritten every 100 ms by the battle
// script (script x/y/z are the camera's own: camera:position() reads +8/+0xc/+0x10 as they are, exe 0x8da510).
// A reader thread polls it every 10 ms and stamps each new seq with the time it arrived; the render thread plays
// the samples back DELAY s late, interpolating between the two round that time (a cut sample isn't blended into).
struct Sample { double t; int cut, active; float e[3], tg[3], fov; };
static const int RING = 16;
static const double DELAY = 0.14, STALE = 1.0;
static CRITICAL_SECTION g_cs;
static Sample g_ring[RING];
static int g_ns;                  // samples in the ring (the newest last)

static bool read_cam_file(Sample &s, long &seq)
{
	char p[MAX_PATH];
	strcpy_s(p, g_dir);
	strcat_s(p, "silvaron_pip_cam.txt");
	FILE *f = _fsopen(p, "r", _SH_DENYNO);
	if (!f) return false;
	char buf[512];
	size_t k = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[k] = 0;
	if (!strstr(buf, "end")) return false;          // caught half written
	int n = sscanf_s(buf, "%ld %d %d %f %f %f %f %f %f %f", &seq, &s.cut, &s.active, &s.e[0], &s.e[1], &s.e[2],
	                 &s.tg[0], &s.tg[1], &s.tg[2], &s.fov);
	return n == 10;
}

static DWORD WINAPI cam_reader(LPVOID)
{
	long last = -1;
	bool first = true;
	int logged = 0;
	for (;;) {
		Sleep(10);
		Sample s;
		long seq;
		if (!read_cam_file(s, seq) || seq == last) continue;
		last = seq;
		if (first) { first = false; continue; }      // a file left from an earlier battle
		s.t = now_s();
		EnterCriticalSection(&g_cs);
		if (g_ns == RING) memmove(g_ring, g_ring + 1, sizeof(Sample) * (RING - 1)), g_ns--;
		g_ring[g_ns++] = s;
		LeaveCriticalSection(&g_cs);
		if (logged < 20 && (s.cut || (s.active && logged == 0))) {
			logged++;
			logf("camera %ld%s: eye %.1f %.1f %.1f target %.1f %.1f %.1f fov %.0f", seq, s.cut ? " (cut)" : "",
			     s.e[0], s.e[1], s.e[2], s.tg[0], s.tg[1], s.tg[2], s.fov);
		}
	}
}

static float lerp(float a, float b, float f) { return a + (b - a) * f; }

// the camera to show now; false = no shot on
static bool current_cam(Sample &out)
{
	double now = now_s(), d = now - DELAY;
	bool ok = false;
	EnterCriticalSection(&g_cs);
	if (g_ns > 0 && now - g_ring[g_ns - 1].t < STALE) {
		int i = g_ns - 1;
		while (i > 0 && g_ring[i].t > d) i--;
		const Sample &a = g_ring[i];
		if (a.t > d || i == g_ns - 1) out = a;           // before the first or after the last: hold it
		else {
			const Sample &b = g_ring[i + 1];
			out = a;
			if (!b.cut && a.active && b.active) {
				float f = (float)((d - a.t) / (b.t - a.t));
				for (int j = 0; j < 3; j++) out.e[j] = lerp(a.e[j], b.e[j], f), out.tg[j] = lerp(a.tg[j], b.tg[j], f);
				out.fov = lerp(a.fov, b.fov, f);
			}
		}
		ok = out.active != 0;
	}
	LeaveCriticalSection(&g_cs);
	return ok;
}

// ---- where the box goes ---------------------------------------------------------------------------------------
// From the pack's MCT page (silvaron_pip_settings.txt): a corner, the size as a share of the screen (the PiP keeps
// the screen's shape) and margins from the screen's edges in % of its width/height. Alt + drag the box in battle
// moves it anywhere: the spot is kept in silvaron_pip_pos.txt (top-left corner as shares of the screen) and wins
// over the corner and margins until the MCT settings are changed again.
static char g_corner[4] = "br";  // br bl tr tl
static float g_size = 0.25f, g_mx = 0.01f, g_my = 0.21f;
static volatile float g_px = -1, g_py = -1; // the dragged spot; -1 none

static void pos_path(char *p) { strcpy_s(p, MAX_PATH, g_dir); strcat_s(p, MAX_PATH, "silvaron_pip_pos.txt"); }

static void load_pos()
{
	char p[MAX_PATH];
	pos_path(p);
	FILE *f = _fsopen(p, "r", _SH_DENYNO);
	float x = -1, y = -1;
	if (f) {
		if (fscanf_s(f, "%f %f", &x, &y) != 2) x = y = -1;
		fclose(f);
	}
	g_px = x, g_py = y;
	if (x >= 0) logf("the box goes where it was dragged: %.3f, %.3f of the screen", x, y);
}

static void save_pos()
{
	char p[MAX_PATH];
	pos_path(p);
	FILE *f = _fsopen(p, "w", _SH_DENYWR);
	if (f) { fprintf(f, "%.4f %.4f\n", g_px, g_py); fclose(f); }
	logf("box dragged to %.3f, %.3f of the screen", g_px, g_py);
}

// the box in back buffer pixels (W x H): top-left x, y and its size w, h
static void layout(LONG W, LONG H, LONG &x, LONG &y, LONG &w, LONG &h)
{
	w = (LONG)(W * g_size) & ~1, h = (LONG)(H * g_size) & ~1;
	if (g_px >= 0 && g_py >= 0) x = (LONG)(g_px * W), y = (LONG)(g_py * H);
	else {
		LONG mx = (LONG)(g_mx * W), my = (LONG)(g_my * H);
		x = g_corner[1] == 'l' ? mx : W - w - mx;
		y = g_corner[0] == 't' ? my : H - h - my;
	}
	// the frame (6 px, draw_box) stays on the screen
	if (x > W - w - 6) x = W - w - 6;
	if (y > H - h - 6) y = H - h - 6;
	if (x < 6) x = 6;
	if (y < 6) y = 6;
}

// the battle script writes the camera file every 100 ms all battle, shot or not
static bool in_battle()
{
	EnterCriticalSection(&g_cs);
	bool b = g_ns > 0 && now_s() - g_ring[g_ns - 1].t < STALE;
	LeaveCriticalSection(&g_cs);
	return b;
}

// where a return address lies: module name + offset
static void where(void *a, char *out, size_t n)
{
	HMODULE m = nullptr;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
		GetModuleFileNameA(m, name, MAX_PATH);
		char *s = strrchr(name, '\\');
		if (s) memmove(name, s + 1, strlen(s));
	}
	sprintf_s(out, n, "%s+%llx", name, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
}

// ---- the second render (Route B) -------------------------------------------------------------------------------
// view = *(exe+0x3df3480) (vtable 0x34c5e48), R = *(view+0x1d0) (vtable 0x3a196c8). As the screenshot path does:
// copy the frame camera (R vf 0x48) twice with the camera's copy constructor 0x1803658 (plain data), set the PiP
// camera's eye/target/fov, rebuild its basis (0x180d864), set the render mode (R vf 0x88), the camera (R vf 0x50 ->
// 0x1a517a8 builds the matrices) and the rect (R vf 0x100), render a frame (view vf 0x7d8, 0x19acbf0, view vf 0x7e0)
// and put back the rect, camera and mode. Every address is checked against this exe before the first call.
struct Want { uintptr_t at; const char *bytes; };
static const Want WANT_FN[] = {
	{ 0x8dcc90, "488b81d0010000c3" },               // view vf 0x230: R = *(view+0x1d0)
	{ 0x1989a34, "c681d181160001" },                // view vf 0x7d8: begin frame
	{ 0x19a1fcc, "c681d181160000" },                // view vf 0x7e0: end frame
	{ 0x19acbf0, "4c8bdc49895b0856" },              // render the frame
	{ 0x1803658, "48895c240848896c2410" },          // camera copy constructor
	{ 0x180d864, "488bc455488bec4883ec50f3" },      // camera basis
	{ 0x8dd6a0, "488b8150600000480510010000c3" },   // R vf 0x48: the frame camera
	{ 0x1a51758, "48895c2408488974241057" },        // R vf 0x50: set the camera
	{ 0x8ddd80, "8991d83a0000448881704800" },       // R vf 0x88: render mode
	{ 0x1a512e8, "4883ec388b02894424208b42" },      // R vf 0x100: set the rect
	{ 0x1a54a08, "40534883ec40488bd94885d2" },      // R vf 0x108: set the rect (or none)
	{ 0x1a30988, "48895c2408574883ec408a99" },      // R vf 0x110: the rect now
	{ 0x19ac6d8, "48895c2408488974241048897c2418" },// the game's frame: begin, render, end (called from 0x19bc046)
	{ 0x1818bc4, "48895c2408574883ec30488b0248" },   // camera assign (the setter's first step)
	{ 0x1958380, "483bca74224c8bc2488bc14c2bc1" },   // copy of the camera's derived block into frame +0x900
	{ 0x8dad10, "40534883ec20f30f104124488bd9" },    // camera vf 0xa0: aspect
	{ 0x1813fcc, "488bc44889580848896810488970" },   // camera vf 0x178: derived block
	{ 0x18264d0, "488bc44889580848897010574881" },   // camera vf 0x188: view-projection matrix
	{ 0x1a517a8, "488bc4488958084889681048" },       // the camera setter (its 8 callers go through hk_setter)
	{ 0x1a56214, "48895c2410574883ec60488b" },       // R vf 0x150: swap the drawn and prepared frames (hk_swap)
	{ 0x1916458, "4883ec2885d2740583fa0175" },       // a culling system's vf 0x88: start its culling job
	{ 0x18e4cf0, "40534883ec20488bd9488b89" },       // wait for a culling system's job
	{ 0x18da3fc, "40534883ec204883b9381500" },       // start a culling system's job (waits the one before)
	{ 0x1a20d34, "48895c241048896c241848897424205748" }, // the scene update (draw_update_nodes): what each system draws
	{ 0x19b5ac2, "89d0010000e868b20600" },           // the game's per-frame scene job calling it (call at 0x19b5ac7)
	{ 0x19a18a1, "4883c4205fe989f40700" },           // the scene object's vf (slot 0x3a0ed98, 0x19a1810) jumping to it (0x19a18a6)
	{ 0x1a20988, "48895c2408" },                     // system 0's vf 0x38: starts its job groups (flag +0xcc0 + 0x28 k)
	{ 0x1a0ab74, "48895c2408574883" },               // system 0's vf 0x70: waits for its groups, clears their flags and lists
	{ 0x1a0aab4, "4883ec284c8b91b0" },               // system 1's vf 0x70: clears its flag (+0x3b0)+0x18c and lists
	{ 0x1a55634, "40534883ec20488b81e002000048" },   // the shadow pass's getter: *(R+0x2e0), 0x61e0 bytes
	{ 0x1ae5514, "48895c240848896c241048897424" },   // shadow pass vf 0x80: swaps its two shadow buffers +0x170/+0x178
	{ 0x1b02774, "488b81586100004c8d8180010000" },   // shadow pass vf 0x70: swaps its two cascade blocks +0x6150/+0x6158
	{ 0x1afd958, "488bc44889581048894808555657" },   // its camera notice (pass vf 0x28 at +0x18): the cascades
	{ 0x1a4f700, "48895c2408574883ec404c8bd1" },     // the main view's frustum test (slot 0x3a1d0d8; hk_frustum)
	{ 0x1a4f604, "48895c24084889742410574883ec20498bf0" },   // its box test (slot 0x3a1d0e0; hk_fbox)
	{ 0x1a4f694, "48895c24084889742410574883ec20498bf0" },   // its sphere + box test (slot 0x3a1d0d0; hk_fsb)
	{ 0x18239fc, "488bc448895808574883ec500f29" }, { 0x1823d0c, "488bc4534883ec50f30f105a0445" },  // their plane tests (planes, shape): 0 inside, 1 outside, 2 crossing
	{ 0x1a28bd4, "488bc44889581848895010555657" },   // a model's LOD pick by projected size (hk_lod)
	{ 0x1a2e028, "885c2420e8a3abffff" },             // its one call (0x1a2e02c, from 0x1a2df4c)
	{ 0x1a09318, "4c8bdc49895b0849896b104989731857" }, { 0x1a094c8, "4c8bdc49895b0849896b104989731857" },
	{ 0x1a09168, "4c8bdc49895b0849896b104989731857" },   // R vf 0x128/0x130/0x138: the visibility entries (hook_vis)
};
struct Slot { uintptr_t vt, off, fn; };
static const Slot WANT_SLOT[] = {
	{ 0x34c5e48, 0x230, 0x8dcc90 }, { 0x34c5e48, 0x7d8, 0x1989a34 }, { 0x34c5e48, 0x7e0, 0x19a1fcc },
	{ 0x3a196c8, 0x48, 0x8dd6a0 }, { 0x3a196c8, 0x50, 0x1a51758 }, { 0x3a196c8, 0x88, 0x8ddd80 },
	{ 0x3a196c8, 0x128, 0x1a09318 }, { 0x3a196c8, 0x138, 0x1a09168 },
	{ 0x3a196c8, 0x100, 0x1a512e8 }, { 0x3a196c8, 0x108, 0x1a54a08 }, { 0x3a196c8, 0x110, 0x1a30988 },
	{ 0x37af670, 0xa0, 0x8dad10 }, { 0x37af670, 0x178, 0x1813fcc }, { 0x37af670, 0x188, 0x18264d0 },
	{ 0x3a196c8, 0x150, 0x1a56214 },
	{ 0x37bd830, 0x70, 0x1b02774 }, { 0x37bd830, 0x80, 0x1ae5514 }, { 0x37bd710, 0x28, 0x1afd958 },
	{ 0x3a1e908, 0x38, 0x1a20988 }, { 0x3a1e908, 0x70, 0x1a0ab74 }, { 0x3a1b698, 0x70, 0x1a0aab4 },
	{ 0x3a1d0c8, 0x10, 0x1a4f700 }, { 0x3a1d0c8, 0x8, 0x1a4f694 }, { 0x3a1d0c8, 0x18, 0x1a4f604 },
};

static int g_checked;   // 0 not yet, 1 this exe, -1 another build: no rendering

static bool check_exe()
{
	if (g_checked) return g_checked > 0;
	g_checked = -1;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
	if (nt->OptionalHeader.SizeOfImage < 0x4480da8) { logf("not the Warhammer3.exe this was made for (image too small) - no PiP"); return false; }
	for (auto &w : WANT_FN) {
		unsigned char *p = (unsigned char *)(g_exe + w.at);
		size_t n = strlen(w.bytes) / 2;
		for (size_t i = 0; i < n; i++) {
			unsigned v;
			sscanf_s(w.bytes + 2 * i, "%2x", &v);
			if (p[i] != v) { logf("exe check: the code at %llx isn't what was studied (byte %zu) - another game version? no PiP", (unsigned long long)w.at, i); return false; }
		}
	}
	for (auto &s : WANT_SLOT) {
		uintptr_t f = *(uintptr_t *)(g_exe + s.vt + s.off);
		if (f != g_exe + s.fn) { logf("exe check: vtable %llx slot %llx points to %llx, not %llx - no PiP", (unsigned long long)s.vt,
		                              (unsigned long long)s.off, (unsigned long long)rva(f), (unsigned long long)s.fn); return false; }
	}
	g_checked = 1;
	logf("exe check: every function the PiP calls is as studied");
	return true;
}

typedef uintptr_t(*GetCamFn)(uintptr_t R);
typedef void(*SetCamFn)(uintptr_t R, void *cam, char);
typedef void(*ModeFn)(uintptr_t R, int mode, char flag, void *dims);
typedef void(*RectFn)(uintptr_t R, int *rect);
typedef char(*GetRectFn)(uintptr_t R, int *rect);
typedef char(*ViewFn)(uintptr_t view);
typedef uintptr_t(*RenderFn)(uintptr_t view);
typedef void *(*CamCopyFn)(void *dst, uintptr_t src);
typedef void(*BasisFn)(void *cam);
typedef void(*AssignFn)(uintptr_t dst, void *src);
typedef void(*AspectFn)(uintptr_t cam, float aspect);
typedef void *(*DerivedFn)(uintptr_t cam, void *out);
typedef void *(*BlockCopyFn)(uintptr_t dst, void *src);
typedef void *(*MatrixFn)(uintptr_t cam, void *out);

static __declspec(align(16)) unsigned char g_pcam[0x500];
static __declspec(align(16)) unsigned char g_saved[0x500];
static int g_mode = 1;          // the render mode for the PiP: 1 as the screenshot path; 0 if 1 leaves the back buffer as it was
static bool g_dead;             // a fault: no more PiP this run
static volatile bool g_rendering;
static volatile int g_where;      // how far the PiP render got (stages of do_render, 41-44 inside the frame), 0 outside it

static uintptr_t vf(uintptr_t obj, uintptr_t off) { return *(uintptr_t *)(*(uintptr_t *)obj + off); }

// The render lock (2026-10-30, the setter build hung after 80 s: the frame thread spun for ever in a culling system's
// lock +0x390, 0x18da3fc from the setter's system notice (R+0x638 vf 0x88 = 0x1916458), called by the PiP). In
// two-frame mode the frame is drawn on its own thread (0x19a27a4, inside the view's lock +0x58) while the game's other
// threads go on; the setter and the frame swap (R vf 0x150) are called from them too, and a culling system's job start
// (0x18da3fc: wait for the job before, xchg +0x390, a new job at +0x388) and wait (0x18e4cf0: clears +0x388, then +0x390)
// are not safe against each other from two threads, nor are the PiP's swaps of R+0x6048/0x6050. So every call to the
// setter (its 8 call sites) and the swap (its vtable slot) takes this lock, and the PiP holds it for its whole render;
// the frame thread only tries it (skipping that PiP frame), so it never waits on another thread through it.
static CRITICAL_SECTION g_rcs;
static volatile long g_busy;    // PiP frames skipped: the lock was held

// Units (2026-10-30, found statically). Each object is culled as a sphere against the six planes of the main view's
// frustum, *(V+0xa48) with V = *(R+0x330) (V+8 vf 0x10 = 0x1a4f700, from R vf 0x130 = 0x1a094c8 -> 0x19c6ae4), plus
// the cvar "cull battle unit when projected aabb is smaller than N pixels" (copied to 0x3d3a470 by 0x1a57db8). The
// setter's notice to V's pass (V+0x18 vf 0x28 = 0x1a514dc) rebuilds that frustum from the prepared camera, so it did
// follow the PiP's camera; but the objects are tested and put in the systems' draw lists by the scene update
// draw_update_nodes (0x1a20d34: each system's vf 0x38(ctx) and 0x58), which the game runs once a frame from its scene
// job (0x19b5a38, the call at 0x19b5ac7) for the main camera, before the PiP's render. The PiP only drew those lists.
// The game's own off-view render (the cubemap, 0x1a25334) does what the PiP needs: set the camera, run the scene
// update (ctx = *(exe+0x3df2fe0) vf 0x178, flag 0), render. So the PiP does the same while the frames are swapped,
// and runs it again once the main camera is back, so the lists of the frame just drawn are the main camera's again.
// The game's scene job goes through g_scs (hk_scene) so it never runs alongside the PiP's; the PiP only tries it.
static CRITICAL_SECTION g_scs;
static volatile long g_scene_busy;          // PiP frames skipped: the game's scene update was running
static double g_scene_ms, g_scene_max;       // the PiP's two scene updates, summed for the 10 s line
static int g_scene_n;
typedef void(*SceneFn)(uintptr_t R, uintptr_t ctx, int flag);
static bool scene_update(uintptr_t R)
{
	uintptr_t g = *(uintptr_t *)(g_exe + 0x3df2fe0);
	if (!g) return false;
	uintptr_t ctx = ((uintptr_t(*)(uintptr_t))vf(g, 0x178))(g);
	if (!ctx) return false;
	LARGE_INTEGER a, b, f;
	QueryPerformanceCounter(&a);
	((SceneFn)(g_exe + 0x1a20d34))(R, ctx, 0);
	QueryPerformanceCounter(&b), QueryPerformanceFrequency(&f);
	double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
	g_scene_ms += ms, g_scene_n++;
	if (ms > g_scene_max) g_scene_max = ms;
	return true;
}

// The groups' flags (2026-10-30, ninth try): the PiP called system 0's and 1's vf 0x70 (0x1a0ab74 / 0x1a0aab4) before
// its scene update, so their job groups would start again. Removed in the tenth try: in game every unit vanished from
// the PiP, seen by the main camera or not. vf 0x70 clears each group's list (0x1a09d74: vf 0x238 on every item, counts
// and per-pass flags zeroed), and the units' draw items are put in those lists by the game's own side of the frame,
// not by draw_update_nodes, so the PiP's render had none left. Culling against the render's camera happens as the
// items are drawn (0x19c45d4 -> each item's vf 0x250), so the lists as the game made them serve the PiP too.

// Culling (2026-10-30, in game: units only the PiP sees weren't drawn). A culling system (vf 0x88 = 0x1916458) culls
// in a job started by the setter (0x18da3fc -> job 0x18e6dc8 -> 0x18d190c), which reads the camera (R vf 0x48, the
// prepared frame's) when it runs, and writes its lists into the buffer at +0x1530; the frame swap waits for it
// (0x191d170) and makes that buffer the one drawn (+0x1528). The PiP's job ran after its swaps had been undone, so
// it culled for the main camera, and the PiP drew from the main frame's lists anyway. Now the PiP waits for its own
// job while the frames are still swapped, draws from it (+0x1528/+0x1530 exchanged), puts them back, and starts the
// job again once the main camera is back in place (the swap waits for that one).
// Undone the same day: in game the units only the PiP sees were still missing, and the trees' shadows in the main view
// pointed away from the trees, moving with the camera. The system (the only one with this vf 0x88) culls a box round
// the camera's eye (0x18d190c: eye +- a radius), i.e. terrain and trees, not units, and drawing the main frame from
// buffers the PiP's render had used broke its shadows. Units are culled elsewhere: `render_ids` logs every render
// system and pass once, to find it.
static int cull_systems(uintptr_t R, uintptr_t *out, int max)
{
	int n = *(int *)(R + 0x634), k = 0;
	uintptr_t *a = *(uintptr_t **)(R + 0x638);
	for (int i = 0; a && i < n && i < 64 && k < max; i++)
		if (a[i] && vf(a[i], 0x88) == g_exe + 0x1916458) out[k++] = a[i];
	return k;
}
typedef void(*SysFn)(uintptr_t s);

// every render system (R+0x638, count +0x634) and pass (R+0x658, count +0x654) the camera setter tells of a new camera:
// its vtable and the functions it is told through (systems vf 0x88, passes vf 0x10 and 0x28), to find where units are
// culled
static void render_ids(uintptr_t R)
{
	char a[300], b[300], c[300];
	__try {
		int n = *(int *)(R + 0x634);
		uintptr_t *s = *(uintptr_t **)(R + 0x638);
		logf("render systems: %d", n);
		for (int i = 0; s && i < n && i < 64; i++) {
			if (!s[i]) continue;
			where(*(void **)s[i], a, sizeof a), where((void *)vf(s[i], 0x88), b, sizeof b);
			logf("  system %d: vtable %s, vf 0x88 %s", i, a, b);
		}
		n = *(int *)(R + 0x654);
		uintptr_t *p = *(uintptr_t **)(R + 0x658);
		logf("render passes: %d", n);
		for (int i = 0; p && i < n && i < 64; i++) {
			if (!p[i]) continue;
			where(*(void **)p[i], a, sizeof a), where((void *)vf(p[i], 0x10), b, sizeof b), where((void *)vf(p[i], 0x28), c, sizeof c);
			logf("  pass %d: vtable %s, vf 0x10 %s, vf 0x28 %s", i, a, b, c);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		logf("render systems/passes: fault reading them");
	}
}

// what a fault was and where it came from, logged from the __except filter while the throwing frames are still on the
// stack: the C++ type (and what() for std exceptions) from the MSVC ThrowInfo, and the return addresses
static int fault_info(EXCEPTION_POINTERS *ep, int stage)
{
	EXCEPTION_RECORD *er = ep->ExceptionRecord;
	logf("fault in the PiP render (stage %d, code %08lx)", stage, er->ExceptionCode);
	__try {
		if (er->ExceptionCode == 0xe06d7363 && er->NumberParameters >= 4) {
			uintptr_t obj = er->ExceptionInformation[1], ti = er->ExceptionInformation[2], base = er->ExceptionInformation[3];
			int cta = *(int *)(ti + 12);
			int n = *(int *)(base + cta);
			bool std_ex = false;
			for (int i = 0; i < n && i < 8; i++) {
				int ct = *(int *)(base + cta + 4 + 4 * i);
				const char *name = (const char *)(base + *(int *)(base + ct + 4) + 16);
				logf("  C++ type %d: %s", i, name);
				if (strstr(name, "exception@std")) std_ex = true;
			}
			if (std_ex && obj) {
				typedef const char *(*WhatFn)(uintptr_t);
				const char *w = ((WhatFn)(*(uintptr_t **)obj)[1])(obj);
				logf("  what(): %.300s", w ? w : "(null)");
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		logf("  (couldn't read the exception's type)");
	}
	void *fr[40];
	USHORT k = RtlCaptureStackBackTrace(0, 40, fr, nullptr);
	for (USHORT i = 0; i < k; i++) {
		char at[300];
		where(fr[i], at, sizeof at);
		logf("  stack %2d: %s", i, at);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

// The main view's shadows (2026-10-30, found statically after in game the terrain's and trees' shadows slid about with
// the main camera). The cascaded shadow pass O = *(R+0x2e0) (vtables 0x37bd830 at +0, 0x37bd710 at +0x18 = pass 26;
// 0x61e0 bytes, made by 0x1a55634) keeps its state from one render to the next: two shadow buffers +0x170/+0x178,
// swapped every render by its vf 0x80 (0x1ae5514: it draws into one and reads the other, the last render's, through
// 0x1ae9bfc, the "static shadows copy previous frame data" pass), two cascade blocks +0x180/+0x3168 (0x2fe8 each:
// +0x6150 drawn, +0x6158 prepared, swapped by vf 0x70, 0x1b02774), written by its camera notice (0x1afd958, from the
// setter), and its state words +0x6150..+0x61e0. The PiP's render swapped the buffers, so the main view's next frame
// took the PiP's shadows as its last frame's; and the PiP's two setter calls rewrote the prepared cascades. So the
// whole of it from +0x150 is kept before the PiP's first setter call and put back after its last, byte for byte (a
// changed pointer to memory outside O and the exe is left as it is and logged: none is expected).
static __declspec(align(16)) unsigned char g_shadow[0x61e0];
static uintptr_t shadow_pass(uintptr_t R)
{
	uintptr_t O = *(uintptr_t *)(R + 0x2e0);
	if (!O || *(uintptr_t *)O != g_exe + 0x37bd830 || *(uintptr_t *)(O + 0x18) != g_exe + 0x37bd710 || *(uintptr_t *)(O + 0x128) != R) return 0;
	uintptr_t a = *(uintptr_t *)(O + 0x6150), b = *(uintptr_t *)(O + 0x6158);
	if (!((a == O + 0x180 && b == O + 0x3168) || (a == O + 0x3168 && b == O + 0x180))) return 0;
	return O;
}
static void shadow_restore(uintptr_t O)
{
	int kept = 0;
	for (size_t i = 0x150; i + 8 <= 0x61e0; i += 8) {
		uintptr_t was = *(uintptr_t *)(g_shadow + i), now = *(uintptr_t *)(O + i);
		if (was == now) continue;
		auto heap = [&](uintptr_t p) {
			return p > 0x100000000ull && p < 0x7fffffffffffull && !(p & 7) && !(p >= O && p < O + 0x61e0) && !rva(p);
		};
		if (i != 0x170 && i != 0x178 && heap(was) && heap(now)) { kept++; continue; }
		*(uintptr_t *)(O + i) = was;
	}
	static int told;
	if (kept && told < 5) told++, logf("shadow pass: %d changed pointers left as they were", kept);
}

// The GPU's side of the main view (2026-10-30, tenth try, static; in game the ninth try still made the near terrain's
// lighting flicker at the PiP's rate). The renderer keeps GPU resources from one frame to the next besides the shadow
// pass's: the exe names a light accumulation copy (0x1a37fec), an ambient light probes accumulation copy (0x1a411d0),
// auto exposure with an adaptation speed (AUTO_EXPOSURE_cs, 0x6a0b1c), the static shadow cache, TAA history buffers;
// whatever the PiP's render writes into any of them, the main view's next frame reads as its own last frame. Rather
// than find each, everything the PiP's render writes is put back: while it renders (gpu_guard_begin .. gpu_guard_end)
// the immediate context's methods that write a resource are hooked (its vtable slots in d3d11.dll), and each resource
// is copied out (CopyResource into a copy of our own, kept for the next renders) before the PiP first writes it:
// targets and UAVs bound when a draw or dispatch runs, copy/update/resolve destinations, cleared views. One whose first
// use is a clear of the whole of it (or a whole CopyResource) is taken as made afresh each frame and isn't kept. After
// the render every kept one is copied back. The back buffer is kept by render_pip as before. Staging, dynamic and
// shared resources are left out. Command lists executed and other contexts used during the render are counted in the
// 10 s line (their writes aren't seen).
enum { V_DRAWIDX = 12, V_DRAW = 13, V_DRAWIDXI = 20, V_DRAWI = 21, V_OMSET = 33, V_OMSETUAV = 34, V_DRAWAUTO = 38,
       V_DRAWIDXIND = 39, V_DRAWIND = 40, V_DISPATCH = 41, V_DISPATCHIND = 42, V_COPYREG = 46, V_COPY = 47, V_UPDATE = 48,
       V_CLEARRTV = 50, V_CLEARUAVU = 51, V_CLEARUAVF = 52, V_CLEARDSV = 53, V_GENMIPS = 54, V_RESOLVE = 57,
       V_EXECLIST = 58, V_CSUAV = 68, V_NSLOT = 69 };
static void *g_dc_orig[V_NSLOT];
static void **g_dc_vt;                       // the immediate context's vtable, patched
static ID3D11DeviceContext *g_dc_imm;        // not AddRef'd: set for each render
static ID3D11Device *g_gdev;                 // the same
static ID3D11Resource *g_gskip;              // the back buffer
static volatile bool g_guard;
static CRITICAL_SECTION g_gcs;
struct GKept { ID3D11Resource *res, *copy; D3D11_RESOURCE_DIMENSION dim; unsigned char desc[64]; unsigned seen; size_t bytes; };
static GKept g_gk[768];
static int g_ngk;
static unsigned g_gren;                      // renders so far: an entry's seen == g_gren = touched in this render
static int g_gtouch[768], g_ntouch;          // entries copied out in this render
static ID3D11Resource *g_gpend[64];          // bound as outputs, not yet written
static int g_npend;
static long g_gfresh, g_gkept_n, g_gfail, g_gexec, g_gother;   // for the 10 s line
static size_t g_gbytes;

static int gk_find(ID3D11Resource *r)
{
	D3D11_RESOURCE_DIMENSION dim;
	r->GetType(&dim);
	unsigned char desc[64] = {};
	size_t bytes = 0;
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D *)r)->GetDesc(&d); memcpy(desc, &d, sizeof d);
		if (d.Usage != D3D11_USAGE_DEFAULT || (d.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE))) return -1;
		bytes = (size_t)d.Width * d.Height * d.ArraySize * d.SampleDesc.Count * 4;
	} else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
		D3D11_BUFFER_DESC d; ((ID3D11Buffer *)r)->GetDesc(&d); memcpy(desc, &d, sizeof d);
		if (d.Usage != D3D11_USAGE_DEFAULT || (d.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) return -1;
		bytes = d.ByteWidth;
	} else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
		D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D *)r)->GetDesc(&d); memcpy(desc, &d, sizeof d);
		if (d.Usage != D3D11_USAGE_DEFAULT || (d.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) return -1;
		bytes = (size_t)d.Width * d.Height * d.Depth * 4;
	} else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
		D3D11_TEXTURE1D_DESC d; ((ID3D11Texture1D *)r)->GetDesc(&d); memcpy(desc, &d, sizeof d);
		if (d.Usage != D3D11_USAGE_DEFAULT || (d.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) return -1;
		bytes = (size_t)d.Width * d.ArraySize * 4;
	} else return -1;
	for (int i = 0; i < g_ngk; i++) {
		if (g_gk[i].res != r) continue;
		if (g_gk[i].dim == dim && !memcmp(g_gk[i].desc, desc, sizeof desc)) return i;
		// the address was freed and taken by another resource: a new copy
		if (g_gk[i].copy) g_gbytes -= g_gk[i].bytes, g_gk[i].copy->Release();
		g_gk[i].copy = nullptr, g_gk[i].dim = dim, memcpy(g_gk[i].desc, desc, sizeof desc), g_gk[i].bytes = bytes, g_gk[i].seen = 0;
		return i;
	}
	if (g_ngk >= 768) return -1;
	GKept &k = g_gk[g_ngk];
	k.res = r, k.copy = nullptr, k.dim = dim, memcpy(k.desc, desc, sizeof desc), k.seen = 0, k.bytes = bytes;
	return g_ngk++;
}

typedef void(STDMETHODCALLTYPE *CopyResFn)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);

// the first write of r in this render: copy it out (fresh: all of it is overwritten, nothing to keep)
static void g_touch(ID3D11DeviceContext *self, ID3D11Resource *r, bool fresh)
{
	if (!r || r == g_gskip) return;
	if (self != g_dc_imm) { g_gother++; return; }
	int i = gk_find(r);
	if (i < 0 || g_gk[i].seen == g_gren) return;
	g_gk[i].seen = g_gren;
	if (fresh) { g_gfresh++; return; }
	GKept &k = g_gk[i];
	if (!k.copy) {
		HRESULT hr = E_FAIL;
		if (k.dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
			D3D11_TEXTURE2D_DESC d; memcpy(&d, k.desc, sizeof d);
			D3D11_TEXTURE2D_DESC p = d;
			p.BindFlags = 0, p.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
			hr = g_gdev->CreateTexture2D(&p, nullptr, (ID3D11Texture2D **)&k.copy);
			if (FAILED(hr)) hr = g_gdev->CreateTexture2D(&d, nullptr, (ID3D11Texture2D **)&k.copy);
		} else if (k.dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
			D3D11_BUFFER_DESC d; memcpy(&d, k.desc, sizeof d);
			hr = g_gdev->CreateBuffer(&d, nullptr, (ID3D11Buffer **)&k.copy);
		} else if (k.dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
			D3D11_TEXTURE3D_DESC d; memcpy(&d, k.desc, sizeof d);
			hr = g_gdev->CreateTexture3D(&d, nullptr, (ID3D11Texture3D **)&k.copy);
		} else if (k.dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
			D3D11_TEXTURE1D_DESC d; memcpy(&d, k.desc, sizeof d);
			hr = g_gdev->CreateTexture1D(&d, nullptr, (ID3D11Texture1D **)&k.copy);
		}
		if (FAILED(hr)) { k.copy = nullptr; g_gfail++; return; }
		g_gbytes += k.bytes;
	}
	((CopyResFn)g_dc_orig[V_COPY])(self, k.copy, r);
	g_gtouch[g_ntouch++] = i, g_gkept_n++;
}
static void g_pend(ID3D11View *v)
{
	if (!v || g_npend >= 64) return;
	ID3D11Resource *r = nullptr;
	v->GetResource(&r);
	if (!r) return;
	r->Release();                               // the view holds it
	for (int i = 0; i < g_npend; i++) if (g_gpend[i] == r) return;
	g_gpend[g_npend++] = r;
}
static void g_flush(ID3D11DeviceContext *self)
{
	for (int i = 0; i < g_npend; i++) g_touch(self, g_gpend[i], false);
	g_npend = 0;
}
// a clear of the whole resource through this view?
static bool g_whole(ID3D11View *v, ID3D11Resource *r, bool dsv, UINT flags)
{
	D3D11_RESOURCE_DIMENSION dim;
	r->GetType(&dim);
	if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
	D3D11_TEXTURE2D_DESC t; ((ID3D11Texture2D *)r)->GetDesc(&t);
	if (t.MipLevels != 1) return false;
	if (dsv) {
		DXGI_FORMAT f = t.Format;
		bool st = f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS;
		if (!(flags & D3D11_CLEAR_DEPTH) || (st && !(flags & D3D11_CLEAR_STENCIL))) return false;
		D3D11_DEPTH_STENCIL_VIEW_DESC d; ((ID3D11DepthStencilView *)v)->GetDesc(&d);
		if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D || d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMS) return t.ArraySize == 1;
		if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) return d.Texture2DArray.FirstArraySlice == 0 && d.Texture2DArray.ArraySize >= t.ArraySize;
		return false;
	}
	D3D11_RENDER_TARGET_VIEW_DESC d; ((ID3D11RenderTargetView *)v)->GetDesc(&d);
	if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D || d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS) return t.ArraySize == 1;
	if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) return d.Texture2DArray.FirstArraySlice == 0 && d.Texture2DArray.ArraySize >= t.ArraySize;
	return false;
}
static void g_clear(ID3D11DeviceContext *self, ID3D11View *v, bool dsv, UINT flags)
{
	if (!v) return;
	ID3D11Resource *r = nullptr;
	v->GetResource(&r);
	if (!r) return;
	g_touch(self, r, g_whole(v, r, dsv, flags));
	r->Release();
}

#define GLOCK EnterCriticalSection(&g_gcs)
#define GUNLOCK LeaveCriticalSection(&g_gcs)
#define ORIG(T, i) ((T)g_dc_orig[i])
static volatile long g_gall, g_gon;          // hook calls, and those during a PiP render (10 s line)
#define GON(s) (g_gall++, g_guard && ((s) == g_dc_imm || (g_gother++, false)) && (g_gon++, true))   // another context's writes: counted, not seen
#define FLUSH(s) if (GON(s)) { GLOCK; g_flush(s); GUNLOCK; }

typedef void(STDMETHODCALLTYPE *DrawIdxFn)(ID3D11DeviceContext *, UINT, UINT, INT);
static void STDMETHODCALLTYPE h_drawidx(ID3D11DeviceContext *s, UINT a, UINT b, INT c) { FLUSH(s); ORIG(DrawIdxFn, V_DRAWIDX)(s, a, b, c); }
typedef void(STDMETHODCALLTYPE *DrawFn)(ID3D11DeviceContext *, UINT, UINT);
static void STDMETHODCALLTYPE h_draw(ID3D11DeviceContext *s, UINT a, UINT b) { FLUSH(s); ORIG(DrawFn, V_DRAW)(s, a, b); }
typedef void(STDMETHODCALLTYPE *DrawIdxIFn)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static void STDMETHODCALLTYPE h_drawidxi(ID3D11DeviceContext *s, UINT a, UINT b, UINT c, INT d, UINT e) { FLUSH(s); ORIG(DrawIdxIFn, V_DRAWIDXI)(s, a, b, c, d, e); }
typedef void(STDMETHODCALLTYPE *DrawIFn)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
static void STDMETHODCALLTYPE h_drawi(ID3D11DeviceContext *s, UINT a, UINT b, UINT c, UINT d) { FLUSH(s); ORIG(DrawIFn, V_DRAWI)(s, a, b, c, d); }
typedef void(STDMETHODCALLTYPE *DrawAutoFn)(ID3D11DeviceContext *);
static void STDMETHODCALLTYPE h_drawauto(ID3D11DeviceContext *s) { FLUSH(s); ORIG(DrawAutoFn, V_DRAWAUTO)(s); }
typedef void(STDMETHODCALLTYPE *DrawIndFn)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
static void STDMETHODCALLTYPE h_drawidxind(ID3D11DeviceContext *s, ID3D11Buffer *b, UINT o) { FLUSH(s); ORIG(DrawIndFn, V_DRAWIDXIND)(s, b, o); }
static void STDMETHODCALLTYPE h_drawind(ID3D11DeviceContext *s, ID3D11Buffer *b, UINT o) { FLUSH(s); ORIG(DrawIndFn, V_DRAWIND)(s, b, o); }
typedef void(STDMETHODCALLTYPE *DispatchFn)(ID3D11DeviceContext *, UINT, UINT, UINT);
static void STDMETHODCALLTYPE h_dispatch(ID3D11DeviceContext *s, UINT x, UINT y, UINT z) { FLUSH(s); ORIG(DispatchFn, V_DISPATCH)(s, x, y, z); }
static void STDMETHODCALLTYPE h_dispatchind(ID3D11DeviceContext *s, ID3D11Buffer *b, UINT o) { FLUSH(s); ORIG(DrawIndFn, V_DISPATCHIND)(s, b, o); }
typedef void(STDMETHODCALLTYPE *OMSetFn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static void STDMETHODCALLTYPE h_omset(ID3D11DeviceContext *s, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds)
{
	if (GON(s)) {
		GLOCK;
		for (UINT i = 0; rt && i < n && i < 8; i++) g_pend(rt[i]);
		g_pend(ds);
		GUNLOCK;
	}
	ORIG(OMSetFn, V_OMSET)(s, n, rt, ds);
}
typedef void(STDMETHODCALLTYPE *OMSetUavFn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
static void STDMETHODCALLTYPE h_omsetuav(ID3D11DeviceContext *s, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds, UINT us, UINT un, ID3D11UnorderedAccessView *const *uav, const UINT *c)
{
	if (GON(s)) {
		GLOCK;
		if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
			for (UINT i = 0; rt && i < n && i < 8; i++) g_pend(rt[i]);
			g_pend(ds);
		}
		if (un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) for (UINT i = 0; uav && i < un && i < 8; i++) g_pend(uav[i]);
		GUNLOCK;
	}
	ORIG(OMSetUavFn, V_OMSETUAV)(s, n, rt, ds, us, un, uav, c);
}
typedef void(STDMETHODCALLTYPE *CSUavFn)(ID3D11DeviceContext *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
static void STDMETHODCALLTYPE h_csuav(ID3D11DeviceContext *s, UINT st, UINT n, ID3D11UnorderedAccessView *const *uav, const UINT *c)
{
	if (GON(s)) { GLOCK; for (UINT i = 0; uav && i < n && i < 8; i++) g_pend(uav[i]); GUNLOCK; }
	ORIG(CSUavFn, V_CSUAV)(s, st, n, uav, c);
}
typedef void(STDMETHODCALLTYPE *CopyRegFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
static void STDMETHODCALLTYPE h_copyreg(ID3D11DeviceContext *s, ID3D11Resource *d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource *r, UINT rs, const D3D11_BOX *b)
{
	if (GON(s)) { GLOCK; g_touch(s, d, false); GUNLOCK; }
	ORIG(CopyRegFn, V_COPYREG)(s, d, ds, x, y, z, r, rs, b);
}
static void STDMETHODCALLTYPE h_copy(ID3D11DeviceContext *s, ID3D11Resource *d, ID3D11Resource *r)
{
	if (GON(s)) { GLOCK; g_touch(s, d, true); GUNLOCK; }   // all of it overwritten
	ORIG(CopyResFn, V_COPY)(s, d, r);
}
typedef void(STDMETHODCALLTYPE *UpdateFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
static void STDMETHODCALLTYPE h_update(ID3D11DeviceContext *s, ID3D11Resource *d, UINT ds, const D3D11_BOX *b, const void *p, UINT rp, UINT dp)
{
	if (GON(s)) { GLOCK; g_touch(s, d, false); GUNLOCK; }
	ORIG(UpdateFn, V_UPDATE)(s, d, ds, b, p, rp, dp);
}
typedef void(STDMETHODCALLTYPE *ClearRtvFn)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
static void STDMETHODCALLTYPE h_clearrtv(ID3D11DeviceContext *s, ID3D11RenderTargetView *v, const FLOAT c[4])
{
	if (GON(s)) { GLOCK; g_clear(s, v, false, 0); GUNLOCK; }
	ORIG(ClearRtvFn, V_CLEARRTV)(s, v, c);
}
typedef void(STDMETHODCALLTYPE *ClearUavUFn)(ID3D11DeviceContext *, ID3D11UnorderedAccessView *, const UINT[4]);
static void STDMETHODCALLTYPE h_clearuavu(ID3D11DeviceContext *s, ID3D11UnorderedAccessView *v, const UINT c[4])
{
	if (GON(s)) { GLOCK; g_pend(v); g_flush(s); GUNLOCK; }
	ORIG(ClearUavUFn, V_CLEARUAVU)(s, v, c);
}
typedef void(STDMETHODCALLTYPE *ClearUavFFn)(ID3D11DeviceContext *, ID3D11UnorderedAccessView *, const FLOAT[4]);
static void STDMETHODCALLTYPE h_clearuavf(ID3D11DeviceContext *s, ID3D11UnorderedAccessView *v, const FLOAT c[4])
{
	if (GON(s)) { GLOCK; g_pend(v); g_flush(s); GUNLOCK; }
	ORIG(ClearUavFFn, V_CLEARUAVF)(s, v, c);
}
typedef void(STDMETHODCALLTYPE *ClearDsvFn)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
static void STDMETHODCALLTYPE h_cleardsv(ID3D11DeviceContext *s, ID3D11DepthStencilView *v, UINT f, FLOAT d, UINT8 st)
{
	if (GON(s)) { GLOCK; g_clear(s, v, true, f); GUNLOCK; }
	ORIG(ClearDsvFn, V_CLEARDSV)(s, v, f, d, st);
}
typedef void(STDMETHODCALLTYPE *GenMipsFn)(ID3D11DeviceContext *, ID3D11ShaderResourceView *);
static void STDMETHODCALLTYPE h_genmips(ID3D11DeviceContext *s, ID3D11ShaderResourceView *v)
{
	if (GON(s)) { GLOCK; g_pend(v); g_flush(s); GUNLOCK; }
	ORIG(GenMipsFn, V_GENMIPS)(s, v);
}
typedef void(STDMETHODCALLTYPE *ResolveFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *, UINT, DXGI_FORMAT);
static void STDMETHODCALLTYPE h_resolve(ID3D11DeviceContext *s, ID3D11Resource *d, UINT ds, ID3D11Resource *r, UINT rs, DXGI_FORMAT f)
{
	if (GON(s)) { GLOCK; g_touch(s, d, false); GUNLOCK; }
	ORIG(ResolveFn, V_RESOLVE)(s, d, ds, r, rs, f);
}
typedef void(STDMETHODCALLTYPE *ExecFn)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
static void STDMETHODCALLTYPE h_exec(ID3D11DeviceContext *s, ID3D11CommandList *l, BOOL k)
{
	if (GON(s)) g_gexec++;
	ORIG(ExecFn, V_EXECLIST)(s, l, k);
}

static bool gpu_hook(ID3D11DeviceContext *ctx)
{
	void **vt = *(void ***)ctx;
	if (g_dc_vt) return g_dc_vt == vt;
	static bool failed;
	if (failed) return false;
	InitializeCriticalSection(&g_gcs);
	struct { int i; void *f; } h[] = {
		{ V_DRAWIDX, (void *)h_drawidx }, { V_DRAW, (void *)h_draw }, { V_DRAWIDXI, (void *)h_drawidxi }, { V_DRAWI, (void *)h_drawi },
		{ V_OMSET, (void *)h_omset }, { V_OMSETUAV, (void *)h_omsetuav }, { V_DRAWAUTO, (void *)h_drawauto },
		{ V_DRAWIDXIND, (void *)h_drawidxind }, { V_DRAWIND, (void *)h_drawind }, { V_DISPATCH, (void *)h_dispatch },
		{ V_DISPATCHIND, (void *)h_dispatchind }, { V_COPYREG, (void *)h_copyreg }, { V_COPY, (void *)h_copy },
		{ V_UPDATE, (void *)h_update }, { V_CLEARRTV, (void *)h_clearrtv }, { V_CLEARUAVU, (void *)h_clearuavu },
		{ V_CLEARUAVF, (void *)h_clearuavf }, { V_CLEARDSV, (void *)h_cleardsv }, { V_GENMIPS, (void *)h_genmips },
		{ V_RESOLVE, (void *)h_resolve }, { V_EXECLIST, (void *)h_exec }, { V_CSUAV, (void *)h_csuav },
	};
	DWORD old;
	if (!VirtualProtect(vt, V_NSLOT * sizeof(void *), PAGE_READWRITE, &old)) {
		failed = true;
		logf("GPU guard: the context's vtable can't be patched - the GPU's state isn't kept round the PiP");
		return false;
	}
	for (auto &e : h) g_dc_orig[e.i] = vt[e.i];
	for (auto &e : h) InterlockedExchangePointer(&vt[e.i], e.f);
	VirtualProtect(vt, V_NSLOT * sizeof(void *), old, &old);
	g_dc_vt = vt;
	logf("GPU guard: %d methods of the immediate context hooked (vtable %p)", (int)(sizeof h / sizeof h[0]), (void *)vt);
	return true;
}

static void gpu_guard_begin()
{
	if (!g_dc_vt || !g_dc_imm) return;
	EnterCriticalSection(&g_gcs);
	g_gren++, g_ntouch = 0, g_npend = 0;
	g_guard = true;
	LeaveCriticalSection(&g_gcs);
}
static void gpu_guard_end()
{
	if (!g_guard) return;
	EnterCriticalSection(&g_gcs);
	g_guard = false;
	for (int j = 0; j < g_ntouch; j++) {
		GKept &k = g_gk[g_gtouch[j]];
		if (k.copy) ((CopyResFn)g_dc_orig[V_COPY])(g_dc_imm, k.res, k.copy);
	}
	g_ntouch = 0, g_npend = 0;
	LeaveCriticalSection(&g_gcs);
}

// Joint culling and LOD (2026-10-30, eleventh try, static; asked for: "the decisions about culling an object would be
// made jointly by both cameras, as would the LOD of those objects"). The PiP's own scene update can't make draw items
// (system 0's job groups are still flagged from the game's frame), so the game's own scene update decides for both:
// - Culling: the main view's frustum test (0x1a4f700, vtable 0x3a1d0c8 slot 0x10, self = V+8, V = *(R+0x330)) tests a
//   sphere {x, y, z, r} against 6 planes at *(self+0xa40) (*(self+0xa38) for its alt flag; plane k = floats 4k..4k+3,
//   a.x + b.y + c.z + d > 0 inside), and on "inside" or "crossing" sets the view's bit in the object's mask. hk_frustum
//   calls it, and when it says outside tests the sphere (radius + 3 m: the PiP's planes are a frame or two old) against
//   the other camera's planes too, setting the same bit. The other camera is the PiP's (copied from V right after the
//   PiP's setter call) outside the PiP's render, and the main camera's (copied just before it) inside it.
// - LOD and the pixel-size cull: 0x1a28bd4(model, screen size, view-projection, world, ...) projects the model's box
//   with the frame camera's matrix (*(frame+0xab8)), returns 0 under the cull size (model+0x88) or the first LOD whose
//   switch size the projected area passes. hk_lod (its one call, 0x1a2e02c) asks for both cameras' matrices and keeps
//   the finer LOD (lower index of model+0x30), drawn if either camera would draw it.
// Both only while the PiP is on (planes and matrix younger than 1 s). Costs: units only the PiP sees are drawn in the
// main view too (off screen), and finer LODs.
struct Planes { float p[2][24]; float m[16]; bool has[2], hm; };
static Planes g_jp[2][2];                 // [0] main camera, [1] PiP; each double buffered
static volatile LONG g_jcur[2] = { -1, -1 };
static volatile ULONGLONG g_jt;           // GetTickCount64 of the last PiP capture
static volatile bool g_in_pip;            // between the PiP's setter call and the main camera's
static volatile uintptr_t g_V8;           // the main view's frustum object (V+8)
static volatile long g_jf_calls, g_jf_out, g_jf_kept, g_jl_calls, g_jl_finer, g_jl_saved;
// twelfth try: totals for the report thread (never reset), whatever the PiP does
static volatile long g_jt_all, g_jt_self, g_jt_out, g_jt_pip, g_jt_kept, g_jt_inpip, g_jl_all, g_jl_kept, g_renders;
static volatile bool g_drawall;           // F11: every sphere the main view culls kept, and a model with no LOD for the
                                          // camera drawn at its finest, while the PiP is on (a test: are units culled here?)
static const char *volatile g_jcap = "never called";
typedef char(*FrustFn)(uintptr_t self, const float *s, uint64_t *mask, char alt);
static FrustFn g_frust_fn;
typedef uintptr_t(*LodFn)(uintptr_t model, uint64_t wh, const float *mat, const float *world, char f5, char f6);

static void joint_capture(uintptr_t R, int which)
{
	uintptr_t V = *(uintptr_t *)(R + 0x330);
	if (!V) { g_jcap = "no main view at R+0x330"; return; }
	if (*(uintptr_t *)(V + 8) != g_exe + 0x3a1d0c8) {
		static bool told;
		if (!told) told = true, logf("joint culling: the main view's frustum object has vtable %llx, not 3a1d0c8 - not captured",
		                             (unsigned long long)(*(uintptr_t *)(V + 8) - g_exe));
		g_jcap = "the main view's frustum vtable differs";
		return;
	}
	g_jcap = "ok";
	g_V8 = V + 8;
	LONG c = g_jcur[which];
	Planes &P = g_jp[which][c == 0 ? 1 : 0];
	for (int k = 0; k < 2; k++) {
		const float *s = *(const float **)(V + (k ? 0xa40 : 0xa48));
		P.has[k] = s != nullptr;
		if (s) memcpy(P.p[k], s, sizeof P.p[k]);
	}
	uintptr_t fr = *(uintptr_t *)(R + 0x6050);
	const float *m = fr ? *(const float **)(fr + 0xab8) : nullptr;
	P.hm = m != nullptr;
	if (m) memcpy(P.m, m, sizeof P.m);
	InterlockedExchange(&g_jcur[which], c == 0 ? 1 : 0);
	if (which) g_jt = GetTickCount64();
}
static const Planes *joint_other()
{
	if (GetTickCount64() - g_jt > 1000) return nullptr;
	int w = g_in_pip ? 0 : 1;
	LONG c = g_jcur[w];
	return c < 0 ? nullptr : &g_jp[w][c];
}
// the game's test on other planes: 1 outside, 2 inside, 3 crossing (0x18e9a1c's mapping)
static char plane_test(const float *p, const float *s, float pad)
{
	float r = s[3] + pad;
	static const int order[6] = { 8, 16, 12, 20, 0, 4 };
	bool cross = false;
	for (int k : order) {
		float d = s[0] * p[k] + s[1] * p[k + 1] + s[2] * p[k + 2] + p[k + 3];
		if (d <= -r) return 1;
		if (d < r) cross = true;
	}
	return cross ? 3 : 2;
}
static char hk_frustum(uintptr_t self, const float *s, uint64_t *mask, char alt)
{
	char r = g_frust_fn(self, s, mask, alt);
	g_jt_all++;
	if (self != g_V8) return r;
	g_jt_self++;
	if (g_in_pip) g_jt_inpip++;
	if (r != 1) return r;
	g_jf_out++, g_jt_out++;
	const Planes *P = joint_other();
	if (g_drawall && GetTickCount64() - g_jt <= 1000) {
		*mask |= 1ull << (((uintptr_t(*)(uintptr_t))vf(self, 0x20))(self) & 63);
		g_jf_kept++, g_jt_kept++;
		return 3;
	}
	if (!P || !P->has[alt ? 1 : 0]) return r;
	g_jf_calls++, g_jt_pip++;
	char r2 = plane_test(P->p[alt ? 1 : 0], s, 3.0f);
	if (r2 == 1) return r;
	*mask |= 1ull << (((uintptr_t(*)(uintptr_t))vf(self, 0x20))(self) & 63);
	g_jf_kept++, g_jt_kept++;
	return r2;
}
// The view's two other tests (twelfth run: the sphere test alone kept thousands for the PiP and drew no unit the main camera
// missed, F11 too): slot 0x18 (0x1a4f604) a box {min xyz, max xyz} (an empty box is outside), slot 0x8 (0x1a4f694) a
// sphere with its box {centre, radius, min, max}. Both take the planes *(self+0xa40) (alt +0xa38) to a pure plane test
// (0x18239fc / 0x1823d0c: 0 inside, 1 outside, 2 crossing; 0x18e9a1c maps them to levels 2, 1, 3) and set the view's
// mask bit on levels 2 and 3. Joined as the sphere test: outside the main camera, the same test on the other camera's
// 24 floats (joint_capture copies them from the same pointers).
typedef char(*ShapeFn)(uintptr_t self, const float *s, uint64_t *mask, char alt);
typedef unsigned char(*PlaneTestFn)(const float *planes, const float *shape);
static ShapeFn g_fbox_fn, g_fsb_fn;
static volatile long g_jb_all, g_jb_self, g_jb_out, g_jb_kept, g_jsb_all, g_jsb_self, g_jsb_out, g_jsb_kept;
static char joint_shape(ShapeFn orig, uintptr_t ptest, uintptr_t self, const float *s, uint64_t *mask, char alt,
                        volatile long &all, volatile long &mine, volatile long &out, volatile long &kept)
{
	char r = orig(self, s, mask, alt);
	all++;
	if (self != g_V8) return r;
	mine++;
	if (r != 1) return r;
	out++;
	char lvl;
	if (g_drawall && GetTickCount64() - g_jt <= 1000) lvl = 3;
	else {
		const Planes *P = joint_other();
		if (!P || !P->has[alt ? 1 : 0]) return r;
		unsigned char t = ((PlaneTestFn)(g_exe + ptest))(P->p[alt ? 1 : 0], s);
		lvl = t == 0 ? 2 : t == 2 ? 3 : 1;
		if (lvl == 1) return r;
	}
	*mask |= 1ull << (((uintptr_t(*)(uintptr_t))vf(self, 0x20))(self) & 63);
	kept++;
	return lvl;
}
static char hk_fbox(uintptr_t self, const float *s, uint64_t *mask, char alt)
{
	return joint_shape(g_fbox_fn, 0x18239fc, self, s, mask, alt, g_jb_all, g_jb_self, g_jb_out, g_jb_kept);
}
static char hk_fsb(uintptr_t self, const float *s, uint64_t *mask, char alt)
{
	return joint_shape(g_fsb_fn, 0x1823d0c, self, s, mask, alt, g_jsb_all, g_jsb_self, g_jsb_out, g_jsb_kept);
}
// Fourteenth try: the renderer's own visibility entries, R vf 0x128 (0x1a09318 -> 0x19c6894, sphere + box), vf 0x130
// (0x1a094c8 -> 0x19c6ae4, sphere; also every batched test, vf 0x140) and vf 0x138 (0x1a09168 -> 0x19c6644, box). They
// fill a 0x18-byte result: +0 level (1 culled: the model submit 0x19feee0 returns at once), +8 the view and pass mask
// bits (ANDed with each pass's mask at submit), +0x10 the flags. The per-object cull tests the main view (19c6ec4, the
// frustum slots joined above) only without flag 0x10, and copies a parent's result untested; the rest (shadow pass,
// the two cascades R+0x398 +0x11b0/+0x1ac0, views 5 and R+0x558) follow the main camera, so a unit flagged 0x10 never
// met the joined frustum tests. Here the result itself is joined: when the main view's bit is missing and the other
// camera's planes keep the shape, the main bit is set, the level raised from 1 to 3, and the bits an object with the
// same flags got when the main camera saw it (`g_vis_mask`, learnt from passing results) ORed in. F11: every shape.
typedef unsigned char *(*VisFn)(uintptr_t R, unsigned char *out, const float *s, uint64_t flags, uintptr_t parent, float size, char f7);
static VisFn g_vis_fn[3];                 // 0x128 sphere + box, 0x130 sphere, 0x138 box (whatever the slot held: the counting stub)
static uint64_t g_vis_mask[0x10000];      // by flags & 0xffff: the mask of the last result with the main view's bit
static volatile long g_jv_all[3], g_jv_missing[3], g_jv_kept[3], g_jv_f10[3];
static unsigned char *joint_vis(int k, uintptr_t R, unsigned char *out, const float *s, uint64_t flags, uintptr_t parent, float size, char f7)
{
	unsigned char *r = g_vis_fn[k](R, out, s, flags, parent, size, f7);
	g_jv_all[k]++;
	uintptr_t v8 = g_V8;
	if (!r || !v8 || GetTickCount64() - g_jt > 1000) return r;
	uint64_t bit = 1ull << (((uintptr_t(*)(uintptr_t))vf(v8, 0x20))(v8) & 63);
	uint64_t *mask = (uint64_t *)(r + 8);
	unsigned f = (unsigned)(flags & 0xffff);
	if (r[0] != 1 && (*mask & bit)) { g_vis_mask[f] = *mask; return r; }
	g_jv_missing[k]++;
	if (flags & 0x10) g_jv_f10[k]++;
	if (!g_drawall) {
		const Planes *P = joint_other();
		if (!P || !P->has[0]) return r;
		bool in;
		if (k == 1) in = plane_test(P->p[0], s, 3.0f) != 1;
		else in = ((PlaneTestFn)(g_exe + (k == 0 ? 0x1823d0c : 0x18239fc)))(P->p[0], s) != 1;
		if (!in) return r;
	}
	*mask |= bit | g_vis_mask[f];
	if (r[0] == 1) r[0] = 3;
	g_jv_kept[k]++;
	return r;
}
static unsigned char *hk_vis_sb(uintptr_t R, unsigned char *o, const float *s, uint64_t f, uintptr_t p, float z, char c) { return joint_vis(0, R, o, s, f, p, z, c); }
static unsigned char *hk_vis_s(uintptr_t R, unsigned char *o, const float *s, uint64_t f, uintptr_t p, float z, char c) { return joint_vis(1, R, o, s, f, p, z, c); }
static unsigned char *hk_vis_b(uintptr_t R, unsigned char *o, const float *s, uint64_t f, uintptr_t p, float z, char c) { return joint_vis(2, R, o, s, f, p, z, c); }
static void hook_vis()
{
	static const uintptr_t fn[3] = { 0x1a09318, 0x1a094c8, 0x1a09168 };
	void *hk[3] = { (void *)hk_vis_sb, (void *)hk_vis_s, (void *)hk_vis_b };
	void **slot = (void **)(g_exe + 0x3a196c8 + 0x128);
	DWORD old;
	if (!VirtualProtect(slot, 24, PAGE_READWRITE, &old)) { logf("joint visibility: R's vtable slots can't be patched"); return; }
	int n = 0;
	for (int k = 0; k < 3; k++) {
		g_vis_fn[k] = (VisFn)slot[k];
		if ((uintptr_t)slot[k] != g_exe + fn[k] && k != 1) { logf("joint visibility: slot %x holds %llx, not as studied", 0x128 + 8 * k, (unsigned long long)((uintptr_t)slot[k] - g_exe)); continue; }
		InterlockedExchangePointer(slot + k, hk[k]);
		n++;
	}
	VirtualProtect(slot, 24, old, &old);
	logf("joint visibility: %d of 3 of the renderer's visibility entries (R vf 0x128/0x130/0x138) also keep what the PiP's camera sees", n);
}
static int lod_index(uintptr_t model, uintptr_t lod)
{
	unsigned n = *(unsigned *)(model + 0x2c);
	uintptr_t *a = *(uintptr_t **)(model + 0x30);
	for (unsigned i = 0; a && i < n; i++) if (a[i] == lod) return (int)i;
	return 1 << 20;
}
static uintptr_t hk_lod(uintptr_t model, uint64_t wh, const float *mat, const float *world, char f5, char f6)
{
	LodFn f = (LodFn)(g_exe + 0x1a28bd4);
	uintptr_t r = f(model, wh, mat, world, f5, f6);
	g_jl_all++;
	if (!r && g_drawall && GetTickCount64() - g_jt <= 1000 && *(unsigned *)(model + 0x2c) && *(uintptr_t **)(model + 0x30)) {
		g_jl_kept++;
		return (*(uintptr_t **)(model + 0x30))[0];
	}
	const Planes *P = joint_other();
	if (!P || !P->hm || f5) return r;   // f5: the first LOD whatever the size
	g_jl_calls++;
	uintptr_t r2 = f(model, wh, P->m, world, f5, f6);
	if (r2 == r || !r2) return r;
	if (!r) { g_jl_saved++, g_jl_kept++; return r2; }
	if (lod_index(model, r2) < lod_index(model, r)) { g_jl_finer++; return r2; }
	return r;
}

// 0 rendered, 1 no view/renderer (not in a battle view), 2 the frame couldn't begin, -1 a fault (restored if it could be)
// 2026-10-30, first try: the PiP went round the setter (camera written straight into the drawn frame, put back byte
// for byte) to keep LoD on the main camera; the scenery flicker was the back buffer (see render_pip), not LoD.
// 2026-10-30: the camera setter is back. Without it (the PiP camera written straight into the drawn frame, the frame
// block put back byte for byte) the game crashed in its own next frame at once, both tries (exe 0x1b1aee1 reading -1:
// a draw item of a pass's list in reused memory). The setter tells every render pass and system the camera changed
// (R+0x658 vf 0x28, R+0x638 vf 0x88), and that is when they build their lists for the next render; the PiP's render used
// up the lists made for the main camera, and only the setter call putting the main camera back makes them again.
static int do_render(const Sample &c, int w, int h)
{
	uintptr_t view = 0, R = 0;
	int stage = 0, rect[4] = { 0, 0, 0, 0 };
	char valid = 0;
	int mode0 = 0;
	char flag0 = 0;
	unsigned char dims0[12];
	// The renderer keeps two frames' data (R+0x6048 the one drawn, R+0x6050 the one being prepared; R vf 0x150,
	// 0x1a56214, swaps them once a frame when the view's +0xb8 is set), and the camera setter (vf 0x50, 0x1a517a8)
	// and getter (vf 0x48) work on the prepared one: set as the screenshot path does, the PiP drew the frame's own
	// camera (2026-10-29, in game). So the two are swapped round the get and the set: our camera goes into the
	// frame drawn, and the main camera is put back there after.
	bool swapped = false;
	auto swp = [&]() {
		uintptr_t t = *(uintptr_t *)(R + 0x6048);
		*(uintptr_t *)(R + 0x6048) = *(uintptr_t *)(R + 0x6050);
		*(uintptr_t *)(R + 0x6050) = t;
		swapped = !swapped;
	};
	unsigned char cut0 = 0;
	uintptr_t O = 0;            // the shadow pass, kept and put back
	unsigned char clock0[12];   // R+0x200 the time of the last render, +0x208 the rate from it: the game's own kept
	__try {
		view = *(uintptr_t *)(g_exe + 0x3df3480);
		if (!view || *(uintptr_t *)view != g_exe + 0x34c5e48) return 1;
		R = *(uintptr_t *)(view + 0x1d0);
		if (!R || *(uintptr_t *)R != g_exe + 0x3a196c8) return 1;
		if (!*(uintptr_t *)(R + 0x6048) || !*(uintptr_t *)(R + 0x6050)) return 1;
		static bool told;
		if (!told) {
			told = true;
			logf("frame data: drawn %llx, prepared %llx (view +0xb8 %s)", (unsigned long long)(*(uintptr_t *)(R + 0x6048) - R),
			     (unsigned long long)(*(uintptr_t *)(R + 0x6050) - R), *(uintptr_t *)(view + 0xb8) ? "set: two frames" : "not set");
		}
		memcpy(clock0, (void *)(R + 0x200), 12);
		O = shadow_pass(R);
		static bool told_sh;
		if (!told_sh) told_sh = true, logf(O ? "shadow pass at R+0x2e0: kept and put back round each PiP render" : "shadow pass not found at R+0x2e0 - its state isn't kept");
		if (O) memcpy(g_shadow + 0x150, (void *)(O + 0x150), 0x61e0 - 0x150);
		// Static shadow caching off for the PiP's render (2026-10-30, ninth try, static; the eighth try's GPU copy found no
		// texture and the flicker came with each PiP render): +0x15c is the cache's switch. On, the pass's vf 0x80
		// (0x1ae5514) swaps and redraws the two cached buffers +0x170/+0x178, which the main view's next frame then read.
		// Off, every caster is drawn each render (0x1b021fc's mask) and the shader is told not to read the cache
		// (0x1afffc4 writes the switch to its constants), so the cached buffers aren't touched. shadow_restore puts it back.
		if (O) *(unsigned char *)(O + 0x15c) = 0;
		swp();
		cut0 = *(unsigned char *)(*(uintptr_t *)(R + 0x6050) + 0xaac);
		uintptr_t cam0 = ((GetCamFn)vf(R, 0x48))(R);
		if (!cam0) return 1;
		((CamCopyFn)(g_exe + 0x1803658))(g_saved, cam0);
		((CamCopyFn)(g_exe + 0x1803658))(g_pcam, cam0);
		float *f = (float *)g_pcam;
		f[2] = c.e[0], f[3] = c.e[1], f[4] = c.e[2];             // +8 eye
		f[5] = c.tg[0], f[6] = c.tg[1], f[7] = c.tg[2];          // +0x14 target
		float fov = c.fov * 0.01745329252f;
		if (fov < 0.1f) fov = 0.1f;
		if (fov > 2.0f) fov = 2.0f;
		f[9] = fov;                                               // +0x24
		*(float *)(g_pcam + 0x3c0) = 2 * tanf(fov / 2);
		((BasisFn)(g_exe + 0x180d864))(g_pcam);
		mode0 = *(int *)(R + 0x3ad8), flag0 = *(char *)(R + 0x4870);
		memcpy(dims0, (void *)(R + 0x4874), 12);
		stage = g_where = 1;
		unsigned char zero[12] = {};
		if (g_mode != mode0) ((ModeFn)vf(R, 0x88))(R, g_mode, 0, zero);
		memcpy(rect, (void *)(g_exe + 0x392de00), 16);           // the screenshot path's starting value
		valid = ((GetRectFn)vf(R, 0x110))(R, rect);
		stage = g_where = 2;
		joint_capture(R, 0);                                      // the main camera's planes and matrix
		gpu_guard_begin();
		((SetCamFn)vf(R, 0x50))(R, g_pcam, 1);
		joint_capture(R, 1);                                      // the PiP's, for the game's next scene update
		g_in_pip = true;
		static bool told_ids;
		if (!told_ids) told_ids = true, render_ids(R);
		stage = g_where = 21;
		scene_update(R);                                          // the draw lists for the PiP's camera
		stage = g_where = 2;
		swp();
		int r[4] = { 0, 0, w, h };
		((RectFn)vf(R, 0x100))(R, r);
		stage = g_where = 3;
		int rc = 2;
		g_where = 41;
		if (((ViewFn)vf(view, 0x7d8))(view)) {
			g_where = 42;
			((RenderFn)(g_exe + 0x19acbf0))(view);
			g_where = 43;
			((ViewFn)vf(view, 0x7e0))(view);
			rc = 0;
		}
		g_where = 44;
		((RectFn)vf(R, 0x108))(R, valid ? rect : nullptr);
		stage = g_where = 4;
		gpu_guard_end();
		swp();
		((SetCamFn)vf(R, 0x50))(R, g_saved, 1);
		g_in_pip = false;
		g_where = 45;
		scene_update(R);                                          // the lists for the main camera again
		g_where = 4;
		*(unsigned char *)(*(uintptr_t *)(R + 0x6050) + 0xaac) = cut0;
		swp();
		if (O) shadow_restore(O);
		memcpy((void *)(R + 0x200), clock0, 12);
		if (g_mode != mode0) ((ModeFn)vf(R, 0x88))(R, mode0, flag0, dims0);
		g_where = 0;
		return rc;
	} __except (fault_info(GetExceptionInformation(), stage)) {
		logf("putting the camera back and turning the PiP off");
	}
	__try {
		g_in_pip = false;
		if (stage == 3) ((RectFn)vf(R, 0x108))(R, valid ? rect : nullptr);
		if (stage >= 1 && !swapped) swp();
		if (stage >= 2) {
			((SetCamFn)vf(R, 0x50))(R, g_saved, 1);
			*(unsigned char *)(*(uintptr_t *)(R + 0x6050) + 0xaac) = cut0;
		}
		if (swapped) swp();
		gpu_guard_end();
		if (O && stage >= 1) shadow_restore(O);
		if (stage >= 1) {
			memcpy((void *)(R + 0x200), clock0, 12);
			((ModeFn)vf(R, 0x88))(R, mode0, flag0, dims0);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		logf("fault while putting it back");
	}
	g_where = 0;
	return -1;
}

static ID3D11Texture2D *g_pip_tex;
static bool g_pip_have;          // g_pip_tex holds a picture of the shot on now
static ID3D11Texture2D *g_stage;
static ID3D11Texture2D *g_keep;  // the game's whole back buffer while the PiP renders (2026-10-30, in game: keeping only the
                                 // PiP's part left the rest black on every PiP frame: the render clears the whole target)
static int g_verify = 3;         // renders left to check against the back buffer read back
static int g_mode_fails;

// 8x8 bytes per pixel from the middle of the PiP's quarter, read back (stalls: only the first few renders)
static bool sample_px(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *bb, const D3D11_TEXTURE2D_DESC &d,
                      UINT w, UINT h, unsigned char *out, int bpp)
{
	if (!g_stage) {
		D3D11_TEXTURE2D_DESC s = {};
		s.Width = 8, s.Height = 8, s.MipLevels = 1, s.ArraySize = 1, s.Format = d.Format, s.SampleDesc.Count = 1;
		s.Usage = D3D11_USAGE_STAGING, s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(dev->CreateTexture2D(&s, nullptr, &g_stage))) { g_stage = nullptr; return false; }
	}
	D3D11_BOX box = { w / 2 - 4, h / 2 - 4, 0, w / 2 + 4, h / 2 + 4, 1 };
	ctx->CopySubresourceRegion(g_stage, 0, 0, 0, 0, bb, 0, &box);
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(g_stage, 0, D3D11_MAP_READ, 0, &m))) return false;
	for (int y = 0; y < 8; y++) memcpy(out + y * 8 * bpp, (unsigned char *)m.pData + y * m.RowPitch, 8 * bpp);
	ctx->Unmap(g_stage, 0);
	return true;
}


static uintptr_t read_ptr(uintptr_t a)
{
	__try { return *(uintptr_t *)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// after the game's frame (hk_frame): render the PiP camera into the top-left of the back buffer, keep that part, put the game's back
static void render_pip(IDXGISwapChain *sc, const Sample &c)
{
	ID3D11Device *dev = nullptr;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev))) return;
	ID3D11DeviceContext *ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	ID3D11Texture2D *bb = nullptr;
	if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) {
		D3D11_TEXTURE2D_DESC d;
		bb->GetDesc(&d);
		LONG bx, by, bw, bh;
		layout((LONG)d.Width, (LONG)d.Height, bx, by, bw, bh);
		UINT w = (UINT)bw, h = (UINT)bh;
		if (g_pip_tex) {
			D3D11_TEXTURE2D_DESC p;
			g_pip_tex->GetDesc(&p);
			if (p.Width != w || p.Height != h || p.Format != d.Format) g_pip_tex->Release(), g_pip_tex = nullptr, g_pip_have = false;
		}
		if (!g_pip_tex && d.SampleDesc.Count == 1) {
			D3D11_TEXTURE2D_DESC p = {};
			p.Width = w, p.Height = h, p.MipLevels = 1, p.ArraySize = 1, p.Format = d.Format, p.SampleDesc.Count = 1;
			p.Usage = D3D11_USAGE_DEFAULT;
			HRESULT hr = dev->CreateTexture2D(&p, nullptr, &g_pip_tex);
			logf("PiP texture %ux%u (back buffer %ux%u format %d): %08x", w, h, d.Width, d.Height, (int)d.Format, (unsigned)hr);
			if (FAILED(hr)) g_pip_tex = nullptr;
		}
		if (g_keep) {
			D3D11_TEXTURE2D_DESC k;
			g_keep->GetDesc(&k);
			if (k.Width != d.Width || k.Height != d.Height || k.Format != d.Format) g_keep->Release(), g_keep = nullptr;
		}
		if (!g_keep && g_pip_tex) {
			D3D11_TEXTURE2D_DESC k = {};
			k.Width = d.Width, k.Height = d.Height, k.MipLevels = 1, k.ArraySize = 1, k.Format = d.Format, k.SampleDesc.Count = 1;
			k.Usage = D3D11_USAGE_DEFAULT;
			if (FAILED(dev->CreateTexture2D(&k, nullptr, &g_keep))) g_keep = nullptr, logf("no texture for the back buffer's copy - no PiP");
		}
		if (g_pip_tex && g_keep) {
			// the game's frame is drawn already: keep all of it, and put it back after
			ctx->CopyResource(g_keep, bb);
			int bpp = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
			unsigned char before[8 * 8 * 8], after[8 * 8 * 8];
			bool check = g_verify > 0;
			ID3D11DeviceContext1 *ctx1 = nullptr;
			if (check) {
				// a colour no game frame has, so a render that leaves the quarter as it was shows up
				ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
				ID3D11RenderTargetView *rtv = nullptr;
				if (ctx1 && SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &rtv))) {
					const float mag[4] = { 1, 0, 1, 1 };
					D3D11_RECT q = { 0, 0, (LONG)w, (LONG)h };
					ctx1->ClearView(rtv, mag, &q, 1);
					rtv->Release();
				}
				check = sample_px(dev, ctx, bb, d, w, h, before, bpp);
			}
			double t0 = now_s();
			g_gdev = dev, g_dc_imm = ctx, g_gskip = bb;
			gpu_hook(ctx);
			g_rendering = true;
			int rc = do_render(c, (int)w, (int)h);
			g_renders++;
			g_rendering = false;
			double ms = (now_s() - t0) * 1000;
			{   // a line every 10 s while rendering: how many renders and their CPU time (a hang shows as the lines stopping)
				static double t_log, sum, mx;
				static int n;
				n++, sum += ms;
				if (ms > mx) mx = ms;
				if (t0 - t_log >= 10) {
					if (t_log > 0) logf("PiP: %d renders in the last %.0f s, %.1f ms mean, %.1f ms longest, %ld skipped (lock held), %ld (scene update running) so far; scene updates %.1f ms mean, %.1f ms longest",
					                    n, t0 - t_log, sum / n, mx, g_busy, g_scene_busy, g_scene_n ? g_scene_ms / g_scene_n : 0.0, g_scene_max);
					if (t_log > 0 && g_dc_vt) logf("GPU guard: %ld resources copied out and back, %ld made afresh (not kept), %ld copies failed, %ld command lists, %ld calls from other contexts so far; %d resources known, %.0f MB of copies",
					                    g_gkept_n, g_gfresh, g_gfail, g_gexec, g_gother, g_ngk, g_gbytes / 1048576.0);
					if (t_log > 0 && g_dc_vt) {
						// were the hooks called each render? (the tenth try's totals looked like one render's)
						int moved = 0;
						for (int i = 0; i < V_NSLOT; i++) if (g_dc_orig[i] && g_dc_vt[i] == g_dc_orig[i]) moved++;
						logf("GPU guard: %ld hook calls in the last 10 s, %ld during PiP renders (%.0f a render); the context's vtable %s, %d slots back to d3d11's own",
						     g_gall, g_gon, n ? (double)g_gon / n : 0.0, *(void ***)ctx == g_dc_vt ? "is the one hooked" : "has changed", moved);
						g_gall = g_gon = 0;
					}
					if (t_log > 0) {
						logf("joint culling: %ld tests outside the main camera, %ld against the PiP's, %ld kept for it; joint LOD: %ld picks, %ld finer from the other camera, %ld drawn only for it",
						     g_jf_out, g_jf_calls, g_jf_kept, g_jl_calls, g_jl_finer, g_jl_saved);
						g_jf_out = g_jf_calls = g_jf_kept = g_jl_calls = g_jl_finer = g_jl_saved = 0;
					}
					g_scene_ms = g_scene_max = 0, g_scene_n = 0;
					t_log = t0, n = 0, sum = 0, mx = 0;
				}
			}
			if (rc < 0) g_dead = true;
			else if (rc == 0) {
				bool drawn = true;
				if (check && sample_px(dev, ctx, bb, d, w, h, after, bpp)) {
					drawn = memcmp(before, after, 64 * bpp) != 0;
					g_verify--;
					logf("PiP render (mode %d, %.1f ms on the CPU): the quarter %s", g_mode, ms,
					     drawn ? "changed - the scene was drawn into the back buffer" : "is as it was - nothing drawn there");
					if (!drawn) {
						if (++g_mode_fails >= 2) {
							if (g_mode == 1) { g_mode = 0; g_mode_fails = 0; g_verify = 3; logf("trying the normal render mode (0) instead"); }
							else { g_dead = true; logf("neither render mode draws into the back buffer - PiP off"); }
						}
					}
				}
				if (drawn) {
					D3D11_BOX box = { 0, 0, 0, w, h, 1 };
					ctx->CopySubresourceRegion(g_pip_tex, 0, 0, 0, 0, bb, 0, &box);
					g_pip_have = true;
				}
			} else {
				static int logged;
				if (logged++ < 5) logf("PiP render skipped (%s)", rc == 1 ? "no battle view" : "the frame couldn't begin");
			}
			ctx->CopyResource(bb, g_keep);
			if (ctx1) ctx1->Release();
		}
		bb->Release();
	}
	ctx->Release();
	dev->Release();
}

// the box: a frame in the game's colours round the last PiP picture (after the game's panel_back_border: 1 px black,
// 3 px bronze #68452A, 1 px highlight #A07A4A, 1 px black inside, 6x6 corner studs #8A6A42; ClearView rects only, no
// textures); picture false = an empty box to drag (Alt held, no shot on): the same frame at half strength over #0B0B0B
static void box_rgb(float out[4], unsigned rgb, bool half)
{
	const float bg[3] = { 0x0b / 255.0f, 0x0b / 255.0f, 0x0b / 255.0f };
	float c[3] = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f };
	for (int i = 0; i < 3; i++) out[i] = half ? (c[i] + bg[i]) * 0.5f : c[i];
	out[3] = 1;
}

static void box_ring(ID3D11DeviceContext1 *ctx1, ID3D11RenderTargetView *rtv, LONG x0, LONG y0, LONG x1, LONG y1, LONG t, const float col[4])
{
	// a ring t px wide just inside the rect x0,y0 - x1,y1
	D3D11_RECT r[4] = {
		{ x0, y0, x1, y0 + t }, { x0, y1 - t, x1, y1 },
		{ x0, y0 + t, x0 + t, y1 - t }, { x1 - t, y0 + t, x1, y1 - t } };
	ctx1->ClearView(rtv, col, r, 4);
}

static void draw_box(IDXGISwapChain *sc, bool picture)
{
	ID3D11Device *dev = nullptr;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev))) return;
	ID3D11DeviceContext *ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	ID3D11DeviceContext1 *ctx1 = nullptr;
	ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
	ID3D11Texture2D *bb = nullptr;
	if (ctx1 && SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) {
		D3D11_TEXTURE2D_DESC d, p = {};
		bb->GetDesc(&d);
		if (picture) g_pip_tex->GetDesc(&p);
		LONG x, y, w, h;
		layout((LONG)d.Width, (LONG)d.Height, x, y, w, h);
		bool fits = !picture || (p.Format == d.Format && (LONG)p.Width == w && (LONG)p.Height == h);
		ID3D11RenderTargetView *rtv = nullptr;
		if (fits && SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &rtv))) {
			bool half = !picture;
			float black[4], bronze[4], hi[4], stud[4];
			box_rgb(black, 0x000000, half);
			box_rgb(bronze, 0x68452A, half);
			box_rgb(hi, 0xA07A4A, half);
			box_rgb(stud, 0x8A6A42, half);
			const LONG F = 6;                                                   // the frame's width outside the picture
			LONG x0 = x - F, y0 = y - F, x1 = x + w + F, y1 = y + h + F;
			box_ring(ctx1, rtv, x0, y0, x1, y1, 1, black);                      // outer black
			box_ring(ctx1, rtv, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 3, bronze);     // bronze
			box_ring(ctx1, rtv, x0 + 4, y0 + 4, x1 - 4, y1 - 4, 1, hi);         // highlight
			box_ring(ctx1, rtv, x0 + 5, y0 + 5, x1 - 5, y1 - 5, 1, black);      // inner black
			D3D11_RECT studs[4] = {
				{ x0, y0, x0 + F, y0 + F }, { x1 - F, y0, x1, y0 + F },
				{ x0, y1 - F, x0 + F, y1 }, { x1 - F, y1 - F, x1, y1 } };
			ctx1->ClearView(rtv, stud, studs, 4);
			if (picture) ctx->CopySubresourceRegion(bb, 0, x, y, 0, g_pip_tex, 0, nullptr);
			else {
				D3D11_RECT in = { x, y, x + w, y + h };
				float dark[4];
				box_rgb(dark, 0x0B0B0B, false);
				ctx1->ClearView(rtv, dark, &in, 1);
			}
			rtv->Release();
		}
		bb->Release();
	}
	if (ctx1) ctx1->Release();
	ctx->Release();
	dev->Release();
}

// ---- the PiP render, before the game's own frame ------------------------------------------------------------
// 2026-10-29, in game: rendered from Present (after the real one), the camera setter (R vf 0x50) threw a C++
// exception (stage 2), and threw again putting the saved camera back: Present runs inside the game's frame (the
// overlay's and the game's locks held), where the screenshot path never runs. The game draws a frame in 0x19ac6d8
// (view vf 0x7d8 begin, 0x19acbf0 render, vf 0x7e0 end), called from the view's update at 0x19bc046; that call is
// pointed at hk_frame, which calls the game's frame and then renders the PiP (its own begin/render/end, as the
// screenshot path) when Present asked for one; the part of the back buffer it draws over is put back after. Rendered
// before the game's frame, the UI flickered every other frame (2026-10-29, in game: the PiP took that frame's UI?)

typedef char(*FrameFn)(uintptr_t view);
static FrameFn g_frame_fn;          // 0x19ac6d8
static bool g_frame_hooked;
static IDXGISwapChain *g_sc;        // the swap chain Present was last called on (not AddRef'd)
static volatile bool g_want;        // Present asks for a PiP render before the next game frame
static Sample g_want_cam;
static DWORD g_present_tid;
static volatile long g_frame_calls;

static char hk_frame(uintptr_t view)
{
	g_frame_calls++;
	char rc = g_frame_fn(view);
	if (rc && g_want) {
		IDXGISwapChain *sc = g_sc;
		Sample c = g_want_cam;
		g_want = false;
		static bool told;
		if (!told) {
			told = true;
			logf("the PiP renders after the game's frame, on thread %lu (Present on thread %lu)", GetCurrentThreadId(), g_present_tid);
		}
		if (sc && !g_dead && check_exe()) {
			if (TryEnterCriticalSection(&g_rcs)) {
				if (TryEnterCriticalSection(&g_scs)) {
					render_pip(sc, c);
					LeaveCriticalSection(&g_scs);
				} else g_scene_busy++;
				LeaveCriticalSection(&g_rcs);
			} else g_busy++;
		}
	}
	return rc;
}

// the game's calls to the camera setter and the frame swap, under the render lock; the first call from each thread and
// place is logged (which threads they come from was never seen)
static void note_caller(const char *what, void *ret)
{
	static struct { DWORD tid; void *at; } seen[16];
	static int n;
	DWORD tid = GetCurrentThreadId();
	for (int i = 0; i < n; i++) if (seen[i].tid == tid && seen[i].at == ret) return;
	if (n >= 16) return;
	seen[n].tid = tid, seen[n].at = ret, n++;
	char at[300];
	where(ret, at, sizeof at);
	logf("%s called from %s on thread %lu (the frame thread is %lu)", what, at, tid, g_present_tid);
}
typedef void(*SetterFn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static void hk_setter(uintptr_t R, uintptr_t cam, uintptr_t a, uintptr_t b, uintptr_t c)
{
	EnterCriticalSection(&g_rcs);
	note_caller("camera setter", _ReturnAddress());
	((SetterFn)(g_exe + 0x1a517a8))(R, cam, a, b, c);
	LeaveCriticalSection(&g_rcs);
}
static void hk_scene(uintptr_t R, uintptr_t ctx, int flag)
{
	EnterCriticalSection(&g_scs);
	note_caller("scene update", _ReturnAddress());
	((SceneFn)(g_exe + 0x1a20d34))(R, ctx, flag);
	LeaveCriticalSection(&g_scs);
}
static void hk_swap(uintptr_t R)
{
	EnterCriticalSection(&g_rcs);
	note_caller("frame swap", _ReturnAddress());
	((void (*)(uintptr_t))(g_exe + 0x1a56214))(R);
	LeaveCriticalSection(&g_rcs);
}

// a jump within 2 GB of the exe (a call's rel32 can't reach the DLL): jmp [rip+0]; dq to
static void *near_stub(void *to)
{
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
	uintptr_t a = (g_exe + nt->OptionalHeader.SizeOfImage + 0xffff) & ~(uintptr_t)0xffff;
	for (int i = 0; i < 16384; i++, a += 0x10000) {
		unsigned char *t = (unsigned char *)VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (!t) continue;
		if ((uintptr_t)t - g_exe > 0x70000000) { VirtualFree(t, 0, MEM_RELEASE); break; }
		t[0] = 0xff; t[1] = 0x25; *(uint32_t *)(t + 2) = 0;
		*(uint64_t *)(t + 6) = (uint64_t)to;
		FlushInstructionCache(GetCurrentProcess(), t, 16);
		return t;
	}
	return nullptr;
}

// points the call at exe+at (to exe+want) at the stub; the 4 bytes sit in one aligned qword when they can: swapped whole,
// so a game thread never sees half a call
static bool patch_call(uintptr_t at, uintptr_t want, void *stub, unsigned char op = 0xe8)
{
	unsigned char *call = (unsigned char *)(g_exe + at);
	if (call[0] != op || (uintptr_t)(call + 5 + *(int32_t *)(call + 1)) != g_exe + want) return false;
	int32_t rel = (int32_t)((intptr_t)stub - (intptr_t)(call + 5));
	volatile LONG64 *q = (volatile LONG64 *)((uintptr_t)(call + 1) & ~(uintptr_t)7);
	size_t off = (uintptr_t)(call + 1) - (uintptr_t)q;
	DWORD old;
	if (off <= 4) {
		if (!VirtualProtect((void *)q, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
		LONG64 was = *q, now = was;
		memcpy((unsigned char *)&now + off, &rel, 4);
		_InterlockedCompareExchange64(q, now, was);
		VirtualProtect((void *)q, 8, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void *)q, 8);
		return true;
	}
	// the 4 bytes cross a qword (3 of the 8 setter calls): within one aligned 16 bytes, swapped whole with cmpxchg16b
	volatile LONG64 *o = (volatile LONG64 *)((uintptr_t)(call + 1) & ~(uintptr_t)15);
	off = (uintptr_t)(call + 1) - (uintptr_t)o;
	if (!VirtualProtect((void *)o, 32, PAGE_EXECUTE_READWRITE, &old)) return false;
	if (off <= 12) {
		__declspec(align(16)) LONG64 was[2] = { o[0], o[1] };
		LONG64 now[2] = { was[0], was[1] };
		memcpy((unsigned char *)now + off, &rel, 4);
		_InterlockedCompareExchange128(o, now[1], now[0], was);
	} else {
		// crosses 16 bytes too: written plainly (this runs at load, before the game draws)
		memcpy(call + 1, &rel, 4);
	}
	VirtualProtect((void *)o, 32, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void *)o, 32);
	return true;
}

static const uintptr_t SETTER_CALLS[] = { 0x19b2e86, 0x1a0523b, 0x1a24caf, 0x1a51792, 0x1a7d2ae, 0x1ab077c, 0x1ab0b97, 0x1ab1168 };

// Counting stubs (twelfth try): a vtable slot pointed at `lock inc qword [counter]; jmp [orig]`, which changes no register
// the call passes (only the flags), so the function's own arguments, floats included, reach it as they were.
struct Counted { const char *name; uintptr_t vt, off, fn; volatile LONG64 *n; };
static Counted g_counted[] = {
	{ "visibility tests (R vf 0x130)", 0x3a196c8, 0x130, 0x1a094c8, nullptr },
	{ "batched visibility tests (R vf 0x140)", 0x3a196c8, 0x140, 0x1a09070, nullptr },
	{ "model submits (system 0 vf 0x180)", 0x3a1e908, 0x180, 0x19feee0, nullptr },
	{ "distance submits (system 0 vf 0x190)", 0x3a1e908, 0x190, 0x1a009c8, nullptr },
};
static void count_slots()
{
	int ok = 0;
	for (Counted &c : g_counted) {
		void **slot = (void **)(g_exe + c.vt + c.off);
		if ((uintptr_t)*slot != g_exe + c.fn) { logf("count: %s - the slot holds %llx, not as studied", c.name, (unsigned long long)((uintptr_t)*slot - g_exe)); continue; }
		unsigned char *t = (unsigned char *)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (!t) continue;
		static const unsigned char code[] = { 0xf0, 0x48, 0xff, 0x05, 24, 0, 0, 0, 0xff, 0x25, 0, 0, 0, 0 };
		memcpy(t, code, sizeof code);
		*(uint64_t *)(t + 14) = (uint64_t)(g_exe + c.fn);
		*(uint64_t *)(t + 32) = 0;
		FlushInstructionCache(GetCurrentProcess(), t, 64);
		c.n = (volatile LONG64 *)(t + 32);
		DWORD old;
		if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { c.n = nullptr; continue; }
		InterlockedExchangePointer(slot, t);
		VirtualProtect(slot, 8, old, &old);
		ok++;
	}
	logf("count: %d of %d of the game's visibility and submit calls counted", ok, (int)(sizeof g_counted / sizeof *g_counted));
}
// A report every 3 s for the first 10, then every 10 s, from its own thread (the eleventh try's counters were only
// written from inside PiP renders, and none came): renders, the frustum hook's calls and what it kept, the LOD hook's,
// and the counted calls. Only once something moved.
static DWORD WINAPI reporter(LPVOID)
{
	long long last[8] = {};
	int k = 0;
	for (;;) {
		Sleep(k < 10 ? 3000 : 10000);
		long long now[8] = { g_frame_calls, g_renders, g_jt_all, 0, 0, 0, 0, 0 };
		for (int i = 0; i < 4; i++) now[4 + i] = g_counted[i].n ? *g_counted[i].n : -1;
		bool moved = false;
		for (int i = 0; i < 8; i++) moved |= now[i] != last[i];
		memcpy(last, now, sizeof last);
		if (!moved) continue;
		k++;
		ULONGLONG age = g_jt ? GetTickCount64() - g_jt : 0;
		logf("report: %lld frame calls, %ld PiP renders%s; frustum hook %ld calls, %ld on the main view (%ld inside PiP renders), %ld outside the main camera, %ld tested on the other camera, %ld kept; LOD hook %ld picks, %ld kept for the other camera; capture %s, PiP planes %s%llu ms old",
		     now[0], g_renders, g_drawall ? " (draw everything on)" : "", g_jt_all, g_jt_self, g_jt_inpip, g_jt_out, g_jt_pip, g_jt_kept,
		     g_jl_all, g_jl_kept, g_jcap, g_jt ? "" : "never taken, ", (unsigned long long)age);
		logf("report: box tests %ld, %ld on the main view, %ld outside it, %ld kept; sphere + box tests %ld, %ld on the main view, %ld outside it, %ld kept",
		     g_jb_all, g_jb_self, g_jb_out, g_jb_kept, g_jsb_all, g_jsb_self, g_jsb_out, g_jsb_kept);
		logf("report: visibility entries (sphere + box / sphere / box): %ld/%ld/%ld calls, %ld/%ld/%ld without the main view's bit (%ld/%ld/%ld flagged 0x10), %ld/%ld/%ld kept for the other camera",
		     g_jv_all[0], g_jv_all[1], g_jv_all[2], g_jv_missing[0], g_jv_missing[1], g_jv_missing[2],
		     g_jv_f10[0], g_jv_f10[1], g_jv_f10[2], g_jv_kept[0], g_jv_kept[1], g_jv_kept[2]);
		logf("report: %s %lld, %s %lld, %s %lld, %s %lld", g_counted[0].name, now[4], g_counted[1].name, now[5],
		     g_counted[2].name, now[6], g_counted[3].name, now[7]);
	}
}

static void hook_frame()
{
	if (!check_exe()) return;
	// the render lock first: without it the PiP's setter calls race the game's (see g_rcs), so no lock, no PiP
	for (uintptr_t a : SETTER_CALLS) {
		unsigned char *c = (unsigned char *)(g_exe + a);
		if (c[0] != 0xe8 || (uintptr_t)(c + 5 + *(int32_t *)(c + 1)) != g_exe + 0x1a517a8) {
			logf("the camera setter's call at %llx isn't as studied - no PiP", (unsigned long long)a);
			return;
		}
	}
	void *sstub = near_stub((void *)hk_setter), *fstub = near_stub((void *)hk_frame), *nstub = near_stub((void *)hk_scene);
	if (!sstub || !fstub || !nstub) { logf("no memory within 2 GB of the exe for the hooks - no PiP"); return; }
	void **slot = (void **)(g_exe + 0x3a196c8 + 0x150);
	DWORD old;
	if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { logf("the frame swap's vtable slot can't be patched - no PiP"); return; }
	InterlockedExchangePointer(slot, (void *)hk_swap);
	VirtualProtect(slot, 8, old, &old);
	int n = 0;
	for (uintptr_t a : SETTER_CALLS) n += patch_call(a, 0x1a517a8, sstub);
	logf("render lock: the frame swap and %d of %d camera setter calls go through it", n, (int)(sizeof SETTER_CALLS / sizeof *SETTER_CALLS));
	if (n != (int)(sizeof SETTER_CALLS / sizeof *SETTER_CALLS)) { logf("not every setter call is locked - no PiP"); return; }
	if (!patch_call(0x19b5ac7, 0x1a20d34, nstub)) { logf("the scene job's call at 19b5ac7 isn't as studied - no PiP"); return; }
	if (!patch_call(0x19a18a6, 0x1a20d34, nstub, 0xe9)) { logf("the scene object's jump at 19a18a6 isn't as studied - no PiP"); return; }
	logf("scene lock: the game's scene update (the call at 19b5ac7 and the jump at 19a18a6) goes through it");
	// joint culling and LOD (see hk_frustum): failing either leaves the PiP on with the main camera's culling
	void **fslot = (void **)(g_exe + 0x3a1d0c8 + 0x10);
	if (VirtualProtect(fslot - 1, 24, PAGE_READWRITE, &old)) {
		g_frust_fn = (FrustFn)(g_exe + 0x1a4f700);
		g_fbox_fn = (ShapeFn)(g_exe + 0x1a4f604);
		g_fsb_fn = (ShapeFn)(g_exe + 0x1a4f694);
		InterlockedExchangePointer(fslot, (void *)hk_frustum);
		InterlockedExchangePointer(fslot + 1, (void *)hk_fbox);
		InterlockedExchangePointer(fslot - 1, (void *)hk_fsb);
		VirtualProtect(fslot - 1, 24, old, &old);
		logf("joint culling: the main view's frustum tests (sphere slot 3a1d0d8, box 3a1d0e0, sphere + box 3a1d0d0) also pass what the PiP's camera sees");
	} else logf("joint culling: the frustum test's vtable slot can't be patched - culled by the main camera alone");
	void *lstub = near_stub((void *)hk_lod);
	if (lstub && patch_call(0x1a2e02c, 0x1a28bd4, lstub)) logf("joint LOD: the LOD pick (call at 1a2e02c) takes the finer of the two cameras'");
	else logf("joint LOD: the LOD pick's call at 1a2e02c isn't as studied - LOD by the main camera alone");
	count_slots();
	hook_vis();   // after count_slots: slot 0x130 calls through its counting stub
	CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr);
	g_frame_fn = (FrameFn)(g_exe + 0x19ac6d8);
	if (!patch_call(0x19bc046, 0x19ac6d8, fstub)) { logf("the call to the game's frame at 19bc046 isn't as studied - no PiP"); return; }
	g_frame_hooked = true;
	logf("the game's frame call (19bc046) now goes through the PiP");
}

// the mod list the game was started with: the launcher (twmm.exe: "{game_exe}" "{twmm_appdata}\games\wh3\used_mods.txt";)
// passes its path on the command line; else used_mods.txt in the game folder (the old launcher)
static void mod_list_path(char *p, size_t n)
{
	const char *cl = GetCommandLineA();
	const char *e = cl ? strstr(cl, ".txt") : nullptr;
	while (e) {
		const char *s = e;
		if (e[4] == '"') while (s > cl && s[-1] != '"') s--;   // a quoted path may hold spaces
		else while (s > cl && s[-1] != '"' && s[-1] != ' ') s--;
		size_t len = (size_t)(e + 4 - s);
		if (len < n) {
			memcpy(p, s, len); p[len] = 0;
			if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return;
		}
		e = strstr(e + 4, ".txt");
	}
	strcpy_s(p, n, g_dir);
	strcat_s(p, n, "used_mods.txt");
}

// is this pack ticked in the launcher? (lines like  mod "x.pack";  or  mod "V:\...\data\x.pack";)
static bool pack_ticked(const char *pack)
{
	static char p[MAX_PATH];
	if (!p[0]) { mod_list_path(p, sizeof p); logf("mod list: %s (command line: %s)", p, GetCommandLineA()); }
	FILE *f = _fsopen(p, "r", _SH_DENYNO);
	if (!f) { logf("no mod list at %s - %s taken as off", p, pack); return false; }
	char q[128], b[128];
	sprintf_s(q, "\"%s\"", pack);
	sprintf_s(b, "\\%s\"", pack);
	char line[1024];
	bool on = false;
	while (fgets(line, sizeof line, f))
		if (strstr(line, "mod ") == line && (strstr(line, q) || strstr(line, b))) on = true;
	fclose(f);
	return on;
}

// Silvaron UI Scale (silvaron_ui_scale.pack ticked): UI scales above 1.0 at any window size.
// The UI scale setter (vf 0x50 of the view at exe+0x3de7ed0; the only writer of the live scale) clamps to
// [vf 0x68, vf 0xb10]: max 2.0 and min 0.5 x 2 when vf 0x290 says the window may scale up, else 1.0 and 1.0. vf 0x290
// says yes only for a window at least 2560 wide (0xa00) and 1439 high (0x59f). Patched here, before the game's code
// runs (the exe imports dinput8.dll), to always say yes: mov al, 1; ret. Found by its bytes (the call's rel32 left
// out) in the first code section, one match only, and a vtable entry (a qword in a section that isn't code) must point
// to it: 2026-10-29 exe 0x8b9364 (view vtable 0x34bd208), 2026-11-03 patch 0x5a3af0 (view vtable 0x34c5e48).
static void ui_scale_patch()
{
	static const short want[] = { 0x48, 0x83, 0xec, 0x38, 0x48, 0x8d, 0x54, 0x24, 0x20, 0xe8, -1, -1, -1, -1,
	                              0x8b, 0x44, 0x24, 0x28, 0x2b, 0x44, 0x24, 0x20, 0x3d, 0x00, 0x0a, 0x00, 0x00, 0x0f, 0x83 };
	const size_t n = sizeof want / sizeof want[0];
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
	IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
	int nsec = nt->FileHeader.NumberOfSections;
	unsigned char *found = 0;
	int hits = 0;
	for (int i = 0; i < nsec && !found; i++) {
		if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
		unsigned char *b = (unsigned char *)(g_exe + sec[i].VirtualAddress);
		size_t len = sec[i].Misc.VirtualSize;
		for (size_t k = 0; k + n <= len; k++) {
			if (b[k] != 0x48 || b[k + 1] != 0x83 || b[k + 2] != 0xec || b[k + 3] != 0x38) continue;
			size_t j = 4;
			while (j < n && (want[j] < 0 || b[k + j] == (unsigned char)want[j])) j++;
			if (j == n) { if (!hits++) found = b + k; }
		}
		break;	// the first code section is the game's own code
	}
	if (hits != 1) { logf("ui scale: the window-size check found %d times (want 1) - another game version? not patched", hits); return; }
	uintptr_t at = (uintptr_t)found - g_exe, slot = 0;
	for (int i = 0; i < nsec && !slot; i++) {
		if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
		uintptr_t *q = (uintptr_t *)(g_exe + sec[i].VirtualAddress);
		size_t len = sec[i].Misc.VirtualSize / 8;
		for (size_t k = 0; k < len; k++) if (q[k] == (uintptr_t)found) { slot = (uintptr_t)&q[k] - g_exe; break; }
	}
	if (!slot) { logf("ui scale: no vtable points to the check at %llx - not patched", (unsigned long long)at); return; }
	DWORD old;
	if (!VirtualProtect(found, 3, PAGE_EXECUTE_READWRITE, &old)) { logf("ui scale: VirtualProtect failed (%lu) - not patched", GetLastError()); return; }
	found[0] = 0xb0; found[1] = 0x01; found[2] = 0xc3;
	VirtualProtect(found, 3, old, &old);
	FlushInstructionCache(GetCurrentProcess(), found, 3);
	logf("ui scale: the 2560x1440 limit removed (%llx, vtable slot %llx -> mov al, 1; ret): ui_scale 1.0-2.0 applies at any window size",
	     (unsigned long long)at, (unsigned long long)slot);
}

// the pack's MCT page, written by its scripts ("enabled 1", "corner br", "size 25", "margin_x 1", "margin_y 21");
// no file = on, bottom right. A change to the corner, size or margins drops the dragged spot.
static void read_settings()
{
	char p[MAX_PATH];
	strcpy_s(p, g_dir);
	strcat_s(p, "silvaron_pip_settings.txt");
	FILE *f = _fsopen(p, "r", _SH_DENYNO);
	bool on = true;
	char corner[4] = "br";
	float size = 25, mx = 1, my = 21;
	if (f) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			int v;
			float x;
			char c[8];
			if (sscanf_s(line, "enabled %d", &v) == 1) on = v != 0;
			else if (sscanf_s(line, "corner %7s", c, (unsigned)sizeof c) == 1 && strlen(c) == 2 && strchr("bt", c[0]) && strchr("lr", c[1])) strcpy_s(corner, c);
			else if (sscanf_s(line, "size %f", &x) == 1) size = x;
			else if (sscanf_s(line, "margin_x %f", &x) == 1) mx = x;
			else if (sscanf_s(line, "margin_y %f", &x) == 1) my = x;
		}
		fclose(f);
	}
	if (on != g_enabled) logf("PiP %s (mod settings)", on ? "on" : "off");
	g_enabled = on;
	size = size < 10 ? 10 : size > 50 ? 50 : size;
	mx = mx < 0 ? 0 : mx > 50 ? 50 : mx;
	my = my < 0 ? 0 : my > 80 ? 80 : my;
	static bool first = true;
	bool changed = strcmp(corner, g_corner) || size / 100 != g_size || mx / 100 != g_mx || my / 100 != g_my;
	if (changed) {
		strcpy_s(g_corner, corner);
		g_size = size / 100, g_mx = mx / 100, g_my = my / 100;
		logf("box: corner %s, %.0f%% of the screen, margins %.0f%% / %.0f%%", corner, size, mx, my);
		if (!first && g_px >= 0) {
			g_px = g_py = -1;
			char pp[MAX_PATH];
			pos_path(pp);
			DeleteFileA(pp);
			logf("the MCT settings changed: the dragged spot is dropped");
		}
	}
	first = false;
}

// the game builds startpos files with a user script and quits: no rendering to hook then
static bool building_startpos()
{
	char p[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("APPDATA", p, MAX_PATH);
	if (!n || n >= MAX_PATH) return false;
	strcat_s(p, "\\The Creative Assembly\\Warhammer3\\scripts\\user.script.txt");
	FILE *f = nullptr;
	if (fopen_s(&f, p, "r") || !f) return false;
	char buf[4096];
	size_t k = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[k] = 0;
	return strstr(buf, "process_campaign_startpos") != nullptr;
}

// ---- Present hook ------------------------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
static PresentFn g_present;

// 2026-10-29: the first frame drawn crashed the game in its stack probe (__chkstk, exe 0x1124e58: out of stack).
// The Steam overlay (gameoverlayrenderer64.dll) calls Present through the vtable slot we patch, and the Present we
// had saved led back into the overlay, so the calls went round until the stack ran out. The hook calls the saved
// Present only from the outermost call (so the overlay still draws); a nested call goes to dxgi's own Present, read
// from dxgi.dll's file. If that too comes back (depth 2), the hook takes itself out and drops that one frame.
// A Present made while the PiP frame renders (the game's end of frame may present) is not passed on: that frame
// is ours and isn't shown.
static const int DRAW_AFTER = 300, EVERY = 2;
static __declspec(thread) int t_depth;
static void **g_slot;            // the vtable slot we patched
static PresentFn g_genuine;      // dxgi's own Present, from the file
static bool g_off;
static int g_frames;

static void unhook(const char *why)
{
	if (g_off) return;
	g_off = true;
	DWORD old;
	VirtualProtect(g_slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
	*g_slot = (void *)g_present;
	VirtualProtect(g_slot, sizeof(void *), old, &old);
	logf("hook taken out: %s", why);
}

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
	if (g_rendering) {   // the game's end of frame may present: the PiP's frame isn't shown
		static int logged;
		if (logged++ < 3) logf("a Present during the PiP render - not shown");
		return S_OK;
	}
	if (t_depth > 0) {
		if (t_depth == 1 && g_genuine) {
			t_depth++;
			HRESULT hr = g_genuine(sc, sync, flags);
			t_depth--;
			return hr;
		}
		unhook("Present still comes back nested - this frame dropped, the game goes on without the PiP");
		return S_OK;
	}
	if (g_off) return g_present(sc, sync, flags);
	if (g_frames == 0) {
		char from[300], vt[300];
		where(_ReturnAddress(), from, sizeof from);
		where(*(void **)sc, vt, sizeof vt);
		logf("first Present: its vtable %s (%s the one patched), called from %s", vt, *(void ***)sc + 8 == g_slot ? "is" : "not", from);
	}
	t_depth++;
	static ULONGLONG next_read;
	ULONGLONG now = GetTickCount64();
	if (now >= next_read) { next_read = now + 2000; read_settings(); }
	static bool key_was;
	bool key = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
	if (key && !key_was) { g_box = !g_box; logf("PiP %s (F10)", g_box ? "on" : "off"); }
	key_was = key;
	static bool key11_was;
	bool key11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
	if (key11 && !key11_was) {
		g_drawall = !g_drawall;
		logf("draw everything %s (F11): %s", g_drawall ? "on" : "off", g_drawall ? "nothing the main view's frustum test culls is culled, models too small for the camera drawn at their finest LOD" : "joint culling as before");
	}
	key11_was = key11;
	Sample c;
	bool test = (flags & DXGI_PRESENT_TEST) != 0;
	bool on = g_enabled && g_box && !g_dead && !test && g_frames >= DRAW_AFTER && current_cam(c);
	static bool was_on;
	if (on != was_on) {
		was_on = on;
		static int logged;
		if (logged++ < 40) logf("shot %s", on ? "on - rendering the PiP" : "over");
		if (!on) g_pip_have = false;
	}
	// Alt + drag the box (in battle, whether a shot is on or not; an empty box shows while Alt is held)
	bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	bool show_empty = false;
	static bool dragging, lb_was;
	static float grab_x, grab_y;
	if (g_enabled && g_box && !test && g_frames >= DRAW_AFTER && (alt || dragging) && in_battle()) {
		DXGI_SWAP_CHAIN_DESC sd;
		POINT pt;
		RECT cr;
		bool lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		if (SUCCEEDED(sc->GetDesc(&sd)) && GetForegroundWindow() == sd.OutputWindow && GetCursorPos(&pt)
		    && ScreenToClient(sd.OutputWindow, &pt) && GetClientRect(sd.OutputWindow, &cr) && cr.right > 0 && cr.bottom > 0) {
			float fx = (float)pt.x / cr.right, fy = (float)pt.y / cr.bottom;
			LONG W = 10000, H = 10000, x, y, w, h;
			layout(W, H, x, y, w, h);
			float bx = x / (float)W, by = y / (float)H, bw = w / (float)W, bh = h / (float)H;
			if (lb && !lb_was && alt && fx >= bx && fx <= bx + bw && fy >= by && fy <= by + bh)
				dragging = true, grab_x = fx - bx, grab_y = fy - by;
			if (dragging) {
				float nx = fx - grab_x, ny = fy - grab_y;
				g_px = nx < 0 ? 0 : nx > 1 - bw ? 1 - bw : nx;
				g_py = ny < 0 ? 0 : ny > 1 - bh ? 1 - bh : ny;
				if (!lb) { dragging = false; save_pos(); }
			}
			show_empty = !(on && g_pip_have && g_pip_tex);
		}
		lb_was = lb;
	} else {
		if (dragging) { dragging = false; save_pos(); }
		lb_was = false;
	}
	if (on && g_pip_have && g_pip_tex) draw_box(sc, true);
	else if (show_empty) draw_box(sc, false);
	g_frames++;
	HRESULT hr = g_present(sc, sync, flags);
	g_present_tid = GetCurrentThreadId();
	g_sc = sc;
	if (on && SUCCEEDED(hr) && g_frames % EVERY == 0 && g_frame_hooked) {
		static int waited;
		if (!g_want) { g_want_cam = c; g_want = true; waited = 0; }
		else if (++waited == 120) logf("the game's frame hasn't been drawn in 120 Presents (%ld frame calls so far) - the PiP can't render", g_frame_calls);
	}
	if (!on) g_want = false;
	t_depth--;
	return hr;
}

// n bytes at p as they are in its module's file (before anything in the process patched them); ib = the file's
// ImageBase (the loaded header's has been rebased to where the module sits)
static bool file_bytes(void *p, void *out, size_t n, uint64_t *ib = nullptr)
{
	HMODULE m = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m)) return false;
	char path[MAX_PATH];
	GetModuleFileNameA(m, path, MAX_PATH);
	uintptr_t base = (uintptr_t)m, r = (uintptr_t)p - base;
	auto nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
	auto sec = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		if (r < sec->VirtualAddress || r + n > sec->VirtualAddress + sec->SizeOfRawData) continue;
		FILE *f = nullptr;
		if (fopen_s(&f, path, "rb") || !f) return false;
		size_t k = 1;
		if (ib) {
			_fseeki64(f, ((IMAGE_DOS_HEADER *)m)->e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader.ImageBase), SEEK_SET);
			k = fread(ib, 8, 1, f);
		}
		_fseeki64(f, r - sec->VirtualAddress + sec->PointerToRawData, SEEK_SET);
		k += fread(out, n, 1, f);
		fclose(f);
		return k == 2;
	}
	return false;
}

// the value the pointer at p had in its module's file, relocated to where the module is loaded
static void *from_file(void *p)
{
	HMODULE m = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m)) return nullptr;
	uint64_t v = 0, ib = 0;
	if (!file_bytes(p, &v, 8, &ib) || !v || v < ib) return nullptr;
	return (void *)(v - ib + (uintptr_t)m);
}

// dxgi's Present (Windows 11 26200) starts with 26 bytes that don't depend on where they sit; an overlay that patches
// its entry (a 5 or 14 byte jump) is skipped by running the file's copy of them and jumping on to Present + 26
static const unsigned char PROLOGUE[26] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56,
	0x48, 0x8d, 0x6c, 0x24, 0x90, 0x48, 0x81, 0xec, 0x70, 0x01, 0x00, 0x00 };

static PresentFn trampoline(void *fn)
{
	unsigned char file[40];
	if (!file_bytes(fn, file, sizeof file)) { logf("trampoline: can't read dxgi.dll's file"); return nullptr; }
	if (memcmp(file, PROLOGUE, sizeof PROLOGUE)) { logf("trampoline: dxgi's Present doesn't start as expected - none made"); return nullptr; }
	if (memcmp((unsigned char *)fn + 26, file + 26, sizeof file - 26)) { logf("trampoline: Present is patched past its first 26 bytes - none made"); return nullptr; }
	unsigned char *t = (unsigned char *)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t) return nullptr;
	memcpy(t, PROLOGUE, 26);
	t[26] = 0xff; t[27] = 0x25; *(uint32_t *)(t + 28) = 0;   // jmp qword ptr [rip+0]
	*(uint64_t *)(t + 32) = (uint64_t)fn + 26;
	FlushInstructionCache(GetCurrentProcess(), t, 64);
	return (PresentFn)t;
}

static LRESULT CALLBACK dummy_wnd(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static DWORD WINAPI setup(LPVOID)
{
	// the game loads d3d11 itself; wait for it so our dummy device uses the same dxgi.dll
	for (int i = 0; i < 600 && !GetModuleHandleA("d3d11.dll"); i++) Sleep(100);
	HMODULE d3d = LoadLibraryA("d3d11.dll");
	auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain");
	WNDCLASSA wc = {};
	wc.lpfnWndProc = dummy_wnd;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "silvaron_pip_dummy";
	RegisterClassA(&wc);
	HWND hw = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 1;
	sd.BufferDesc.Width = 64;
	sd.BufferDesc.Height = 64;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hw;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	IDXGISwapChain *sc = nullptr;
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx) : E_FAIL;
	if (FAILED(hr)) { logf("dummy swap chain failed: %08x", (unsigned)hr); return 0; }
	void **vt = *(void ***)sc;
	DWORD old;
	VirtualProtect(&vt[8], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
	g_slot = &vt[8];
	g_present = (PresentFn)vt[8];
	PresentFn own = (PresentFn)from_file(&vt[8]);
	if (own) {
		unsigned char now[26];
		memcpy(now, (void *)own, 26);
		bool patched = memcmp(now, PROLOGUE, 26) != 0;
		logf("dxgi's own Present entry: %02x %02x %02x %02x %02x %02x (%s)", now[0], now[1], now[2], now[3], now[4], now[5],
		     patched ? "patched by something" : "as in the file");
		// patched entry: the trampoline; else the function itself (when the slot was patched instead)
		g_genuine = patched ? trampoline((void *)own) : (own != g_present ? own : trampoline((void *)own));
	}
	vt[8] = (void *)hk_present;
	VirtualProtect(&vt[8], sizeof(void *), old, &old);
	char a[300], b[300], t[300];
	where((void *)g_present, a, sizeof a);
	where((void *)g_genuine, b, sizeof b);
	where((void *)vt, t, sizeof t);
	unsigned char *c = (unsigned char *)g_present;
	logf("Present hooked: vtable %s, slot held %s (bytes %02x %02x %02x %02x %02x %02x), dxgi's own %s", t, a, c[0], c[1], c[2], c[3], c[4], c[5], b);
	sc->Release();
	ctx->Release();
	dev->Release();
	DestroyWindow(hw);
	hook_frame();   // here, not in DllMain: the game's code is checked once d3d11 is up, as for the render
	return 0;
}

// ---- a stall: the game's frames stop (2026-10-30, in game the game hung twice with the PiP on, no dump) --------
// Every second: frame calls unchanged for 5 s after frames had been running -> the frame thread (Present's) is
// suspended just long enough to copy its registers and 16 KB of its stack, then the log gets where it is: rip and the
// return addresses on the stack (module+offset, after a call), and how far a PiP render had got (g_where).
static DWORD WINAPI watchdog(LPVOID)
{
	long last = -1;
	int still = 0, told = 0;
	static unsigned char stk[0x4000];
	for (;;) {
		Sleep(1000);
		long n = g_frame_calls;
		if (n != last) { last = n, still = 0; continue; }
		if (n == 0 || ++still != 5 || told >= 3) continue;
		told++;
		DWORD tid = g_present_tid;
		HANDLE th = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : nullptr;
		if (!th) { logf("stall: no frame for 5 s (%ld frame calls, PiP stage %d), the frame thread can't be opened", n, g_where); continue; }
		CONTEXT cx = {};
		cx.ContextFlags = CONTEXT_FULL;
		SIZE_T got = 0;
		bool ok = false;
		int where_now = g_where, rendering = g_rendering;
		if (SuspendThread(th) != (DWORD)-1) {
			ok = GetThreadContext(th, &cx) != 0;
			if (ok) ReadProcessMemory(GetCurrentProcess(), (void *)cx.Rsp, stk, sizeof stk, &got);
			ResumeThread(th);
		}
		CloseHandle(th);
		// logged once the thread runs again: it may hold the heap's or the loader's lock
		logf("stall: no frame for 5 s (%ld frame calls so far); frame thread %lu, PiP %s (stage %d)", n, tid,
		     rendering ? "rendering" : "not rendering", where_now);
		if (!ok) continue;
		char at[300];
		where((void *)cx.Rip, at, sizeof at);
		logf("  rip %s", at);
		int k = 0;
		for (SIZE_T i = 0; i + 8 <= got && k < 40; i += 8) {
			uintptr_t a = *(uintptr_t *)(stk + i);
			HMODULE m = nullptr;
			if (a < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) continue;
			unsigned char pre[6];
			SIZE_T r = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), (void *)(a - 6), pre, 6, &r) || r != 6) continue;
			if (pre[1] != 0xe8 && pre[0] != 0xff && pre[3] != 0xff && pre[4] != 0xff) continue;   // not after a call
			where((void *)a, at, sizeof at);
			logf("  stack +%04llx: %s", (unsigned long long)i, at);
			k++;
		}
	}
}

// ---- controller: the native base layer (2026-11-01, untested in game; see PAD.md) ----------------------------
// With silvaron_overlord.pack ticked (and not building a startpos), whether or not the PiP's pack is: a thread reads an
// XInput pad (xinput1_4.dll, else xinput9_1_0.dll, through GetProcAddress: no new link libraries) and
// (1) writes silvaron_pad.txt for the battle script (CD_PAD), atomically (tmp + MoveFileExA), on change and at least
//     every 250 ms:
//       seq conn btns lx ly rx ry lt rt ix iy t     (sticks -1..1 after a radial deadzone, y up; triggers 0..1;
//                                                    ix/iy the right stick integrated over real seconds; t ms since start)
//       p <16 press counts, XInput wButtons bit order: up down left right start back l3 r3 lb rb - - a b x y>
//       r <16 release counts>
//       end
// (2) turns the pad into mouse and keys on every screen (frontend, campaign, battle): left stick the cursor, A/B the
//     left/right mouse button (held: drag), right stick W/A/S/D, LB/RB Q/E, LT/RT the wheel, D-pad arrows, X Enter,
//     Y Space, Start Escape, Back Tab; bindings in silvaron_pad.cfg (written with these defaults when missing).
//     Only while a window of the game is in front. Two paths, `inject` in the cfg: SendInput (scancodes) and the
//     game's own DirectInput devices, wrapped here (DirectInput8Create -> IDirectInput8W::CreateDevice -> the system
//     keyboard and mouse: our keys, buttons, motion and wheel ORed into GetDeviceState / added to GetDeviceData).
//     Which of them the game reads is unknown: the log says which DI methods it calls, how often, and its raw input
//     registrations.
// The battle script takes inputs for itself through silvaron_pad_mode.txt ("mode <battle|order> <t>", "take <names>",
// "end"; names dpad a b x y lb rb lt rt ls rs start back l3 r3): those aren't turned into mouse/keys while the file's
// last write is under 1.5 s old.

static bool g_pad_on;                 // the controller layer runs this session (DllMain)

enum { IN_DUP, IN_DDOWN, IN_DLEFT, IN_DRIGHT, IN_START, IN_BACK, IN_L3, IN_R3, IN_LB, IN_RB, IN_A, IN_B, IN_X, IN_Y,
       IN_LT, IN_RT, IN_RSU, IN_RSD, IN_RSL, IN_RSR, IN_N };
static const char *const IN_NAME[IN_N] = { "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "l3", "r3",
	"lb", "rb", "a", "b", "x", "y", "lt", "rt", "rs_up", "rs_down", "rs_left", "rs_right" };
static const WORD IN_BIT[14] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000 };
// the mode file's "take" bits: 0-15 as wButtons, then these
enum { TAKE_LT = 1 << 16, TAKE_RT = 1 << 17, TAKE_LS = 1 << 18, TAKE_RS = 1 << 19 };
static unsigned in_take_bit(int i) { return i < 14 ? IN_BIT[i] : i == IN_LT ? TAKE_LT : i == IN_RT ? TAKE_RT : TAKE_RS; }

enum { PA_NONE, PA_KEY, PA_MOUSE, PA_WHEEL };
struct PadAct { unsigned char type, code; };   // key: DIK code (scancode, | 0x80 extended); mouse 0 left 1 right 2 middle; wheel 1 up 2 down
enum { INJ_DI = 1, INJ_SI = 2 };
struct PadCfg {
	bool enabled;
	int inject;            // INJ_DI | INJ_SI
	bool cursor_si;        // the cursor by SendInput relative moves (else SetCursorPos, clamped to the window)
	bool ls_cursor;        // the left stick moves the cursor
	float speed;           // cursor pixels a second at full tilt, at 1080 high (scaled by the window's height)
	float dz;              // radial deadzone of both sticks
	int glyphs;            // the pad's family for the scripts' button pictures: 0 auto, 1 xbox, 2 playstation
	PadAct act[IN_N];
};

struct KeyName { const char *name; WORD vk; bool ext; };
static const KeyName KEYS[] = {
	{ "enter", VK_RETURN, false }, { "space", VK_SPACE, false }, { "escape", VK_ESCAPE, false }, { "tab", VK_TAB, false },
	{ "backspace", VK_BACK, false }, { "up", VK_UP, true }, { "down", VK_DOWN, true }, { "left", VK_LEFT, true },
	{ "right", VK_RIGHT, true }, { "shift", VK_LSHIFT, false }, { "ctrl", VK_LCONTROL, false }, { "alt", VK_LMENU, false },
	{ "home", VK_HOME, true }, { "end", VK_END, true }, { "pageup", VK_PRIOR, true }, { "pagedown", VK_NEXT, true },
	{ "insert", VK_INSERT, true }, { "delete", VK_DELETE, true }, { "grave", VK_OEM_3, false }, { "minus", VK_OEM_MINUS, false },
	{ "equals", VK_OEM_PLUS, false }, { "comma", VK_OEM_COMMA, false }, { "period", VK_OEM_PERIOD, false },
	{ "f1", VK_F1, false }, { "f2", VK_F2, false }, { "f3", VK_F3, false }, { "f4", VK_F4, false }, { "f5", VK_F5, false },
	{ "f6", VK_F6, false }, { "f7", VK_F7, false }, { "f8", VK_F8, false }, { "f9", VK_F9, false }, { "f10", VK_F10, false },
	{ "f11", VK_F11, false }, { "f12", VK_F12, false },
};
static WORD g_dik_vk[256];            // DIK code -> the virtual key it was named by (SendInput's wVk)

// a key name -> its DIK code (the scancode of the current layout, | 0x80 extended), 0 unknown
static int key_code(const char *n)
{
	WORD vk = 0;
	bool ext = false;
	if (n[0] && !n[1] && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) vk = (WORD)(n[0] >= 'a' ? n[0] - 32 : n[0]);
	for (size_t i = 0; !vk && i < sizeof KEYS / sizeof KEYS[0]; i++)
		if (!_stricmp(n, KEYS[i].name)) vk = KEYS[i].vk, ext = KEYS[i].ext;
	if (!vk) return 0;
	UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
	if (!sc || sc > 0x7f) return 0;
	int dik = (int)sc | (ext ? 0x80 : 0);
	g_dik_vk[dik] = vk;
	return dik;
}

static bool parse_act(const char *v, PadAct &a)
{
	a.type = PA_NONE, a.code = 0;
	if (!_stricmp(v, "none")) return true;
	if (!_stricmp(v, "mouse_left")) { a.type = PA_MOUSE, a.code = 0; return true; }
	if (!_stricmp(v, "mouse_right")) { a.type = PA_MOUSE, a.code = 1; return true; }
	if (!_stricmp(v, "mouse_middle")) { a.type = PA_MOUSE, a.code = 2; return true; }
	if (!_stricmp(v, "wheel_up")) { a.type = PA_WHEEL, a.code = 1; return true; }
	if (!_stricmp(v, "wheel_down")) { a.type = PA_WHEEL, a.code = 2; return true; }
	int k = key_code(v);
	if (!k) return false;
	a.type = PA_KEY, a.code = (unsigned char)k;
	return true;
}

static const char PAD_CFG_DEFAULT[] =
	"# Silvaron Overlord controller, the native base layer (dinput8.dll). Lines: name = value; # starts a comment.\n"
	"# Delete this file to get the defaults back. Read at launch and again when it changes.\n"
	"enabled = 1            # 0: no controller at all (no mouse/keys, no silvaron_pad.txt)\n"
	"inject = both          # both | di (the game's DirectInput) | sendinput (Windows input) | none (silvaron_pad.txt only)\n"
	"cursor = setpos        # setpos (put the cursor, kept in the window) | sendinput (relative moves)\n"
	"cursor_speed = 900     # pixels a second with the stick fully over (at 1080 high), faster after 0.8 s held\n"
	"deadzone = 0.24        # 0-0.9, both sticks\n"
	"# the button pictures the mod's scripts show: auto (PlayStation for a Sony pad, even through Steam Input; else Xbox)\n"
	"glyphs = auto          # auto | xbox | playstation\n"
	"# what each input does: a key (a-z 0-9 f1-f12 enter space escape tab backspace up down left right shift ctrl alt\n"
	"# home end pageup pagedown insert delete grave minus equals comma period), mouse_left mouse_right mouse_middle,\n"
	"# wheel_up wheel_down (repeated while held) or none\n"
	"ls = cursor            # cursor | none\n"
	"a = mouse_left\n"
	"b = mouse_right\n"
	"x = enter\n"
	"y = space\n"
	"start = escape\n"
	"back = tab\n"
	"lb = q\n"
	"rb = e\n"
	"lt = wheel_down\n"
	"rt = wheel_up\n"
	"dpad_up = up\n"
	"dpad_down = down\n"
	"dpad_left = left\n"
	"dpad_right = right\n"
	"rs_up = w\n"
	"rs_down = s\n"
	"rs_left = a\n"
	"rs_right = d\n"
	"l3 = none\n"
	"r3 = none\n";

// the cfg's text -> c (starting from the defaults); returns the number of lines not understood (the first in err)
static int pad_parse_cfg(const char *text, PadCfg &c, char *err, size_t errn)
{
	c.enabled = true, c.inject = INJ_DI | INJ_SI, c.cursor_si = false, c.ls_cursor = true, c.speed = 900, c.dz = 0.24f, c.glyphs = 0;
	for (int i = 0; i < IN_N; i++) c.act[i].type = PA_NONE, c.act[i].code = 0;
	int bad = 0;
	if (err && errn) err[0] = 0;
	for (int pass = 0; pass < 2; pass++) {    // the defaults first, then the file's lines over them
		const char *s = pass ? text : PAD_CFG_DEFAULT;
		while (s && *s) {
			char line[256];
			size_t n = strcspn(s, "\r\n");
			size_t m = n < sizeof line - 1 ? n : sizeof line - 1;
			memcpy(line, s, m); line[m] = 0;
			s += n;
			while (*s == '\r' || *s == '\n') s++;
			char *h = strchr(line, '#');
			if (h) *h = 0;
			char *e = strchr(line, '=');
			if (e) *e = ' ';
			char k[64], v[64];
			int got = sscanf_s(line, "%63s %63s", k, (unsigned)sizeof k, v, (unsigned)sizeof v);
			if (got <= 0) continue;
			bool ok = got == 2;
			if (!ok) {}
			else if (!_stricmp(k, "enabled")) c.enabled = atoi(v) != 0;
			else if (!_stricmp(k, "inject")) {
				if (!_stricmp(v, "both")) c.inject = INJ_DI | INJ_SI;
				else if (!_stricmp(v, "di")) c.inject = INJ_DI;
				else if (!_stricmp(v, "sendinput")) c.inject = INJ_SI;
				else if (!_stricmp(v, "none")) c.inject = 0;
				else ok = false;
			}
			else if (!_stricmp(k, "cursor")) { if (!_stricmp(v, "setpos")) c.cursor_si = false; else if (!_stricmp(v, "sendinput")) c.cursor_si = true; else ok = false; }
			else if (!_stricmp(k, "cursor_speed")) { float f = (float)atof(v); if (f > 0 && f <= 10000) c.speed = f; else ok = false; }
			else if (!_stricmp(k, "deadzone")) { float f = (float)atof(v); if (f >= 0 && f <= 0.9f) c.dz = f; else ok = false; }
			else if (!_stricmp(k, "glyphs")) {
				if (!_stricmp(v, "auto")) c.glyphs = 0;
				else if (!_stricmp(v, "xbox")) c.glyphs = 1;
				else if (!_stricmp(v, "playstation")) c.glyphs = 2;
				else ok = false;
			}
			else if (!_stricmp(k, "ls")) { if (!_stricmp(v, "cursor")) c.ls_cursor = true; else if (!_stricmp(v, "none")) c.ls_cursor = false; else ok = false; }
			else {
				int i = 0;
				while (i < IN_N && _stricmp(k, IN_NAME[i])) i++;
				PadAct a;
				if (i < IN_N && parse_act(v, a)) c.act[i] = a;
				else ok = false;
			}
			if (!ok && pass) {
				if (!bad && err && errn) sprintf_s(err, errn, "%s", line);
				bad++;
			}
		}
	}
	return bad;
}

// silvaron_pad_mode.txt: "mode <name> <t>", "take <names>", "end" -> the name and the inputs taken; false if incomplete
// "remap ls_keys lbrb_wheel": while the battle script's order mode is on the game's tactical map, the left stick
// holds the right stick's keys (the map pans with them) instead of moving the cursor, and LB/RB turn the wheel
enum { REMAP_LS_KEYS = 1, REMAP_LBRB_WHEEL = 2 };
// "pulse <id> <wheel> <fwd_ms> <right_ms>": a one-off camera nudge on the game's tactical map (whose camera ignores
// script moves): <wheel> notches of the wheel (+ up), the right stick's forward/back keys held <fwd_ms> (- back),
// left/right <right_ms> (- left); each new id applied once, for the script's zoom-to-fit
struct PadMode { char name[32]; unsigned take, remap; unsigned pulse_id; int pulse_wheel, pulse_fwd, pulse_right; };
static bool pad_parse_mode(const char *text, PadMode &m)
{
	m.name[0] = 0, m.take = 0, m.remap = 0, m.pulse_id = 0, m.pulse_wheel = m.pulse_fwd = m.pulse_right = 0;
	bool end = false;
	const char *s = text;
	while (s && *s && !end) {
		char line[512];
		size_t n = strcspn(s, "\r\n");
		size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
		memcpy(line, s, k); line[k] = 0;
		s += n;
		while (*s == '\r' || *s == '\n') s++;
		char w[64];
		if (sscanf_s(line, "%63s", w, (unsigned)sizeof w) != 1) continue;
		if (!strcmp(w, "end")) end = true;
		else if (!strcmp(w, "mode")) sscanf_s(line, "mode %31s", m.name, (unsigned)sizeof m.name);
		else if (!strcmp(w, "pulse")) {
			unsigned id = 0; int wh = 0, fw = 0, rt = 0;
			if (sscanf_s(line, "pulse %u %d %d %d", &id, &wh, &fw, &rt) == 4) {
				m.pulse_id = id;
				m.pulse_wheel = wh < -20 ? -20 : wh > 20 ? 20 : wh;
				m.pulse_fwd = fw < -3000 ? -3000 : fw > 3000 ? 3000 : fw;
				m.pulse_right = rt < -3000 ? -3000 : rt > 3000 ? 3000 : rt;
			}
		}
		else if (!strcmp(w, "remap")) {
			char *ctx = nullptr;
			for (char *t = strtok_s(line + 5, " 	", &ctx); t; t = strtok_s(nullptr, " 	", &ctx)) {
				if (!strcmp(t, "ls_keys")) m.remap |= REMAP_LS_KEYS;
				else if (!strcmp(t, "lbrb_wheel")) m.remap |= REMAP_LBRB_WHEEL;
			}
		}
		else if (!strcmp(w, "take")) {
			char *ctx = nullptr;
			for (char *t = strtok_s(line + 4, " \t", &ctx); t; t = strtok_s(nullptr, " \t", &ctx)) {
				static const char *const NM[] = { "start", "back", "l3", "r3", "lb", "rb", "a", "b", "x", "y" };
				static const unsigned BT[] = { 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000 };
				if (!strcmp(t, "dpad")) m.take |= 0xf;
				else if (!strcmp(t, "lt")) m.take |= TAKE_LT;
				else if (!strcmp(t, "rt")) m.take |= TAKE_RT;
				else if (!strcmp(t, "ls")) m.take |= TAKE_LS;
				else if (!strcmp(t, "rs")) m.take |= TAKE_RS;
				else for (int i = 0; i < 10; i++) if (!strcmp(t, NM[i])) m.take |= BT[i];
			}
		}
	}
	return end;
}

// a stick through a radial deadzone: 0 inside it, then 0..1 over the rest, the direction kept
static void pad_deadzone(float x, float y, float dz, float &ox, float &oy)
{
	float m = sqrtf(x * x + y * y);
	if (m <= dz || m <= 0) { ox = oy = 0; return; }
	float k = (m - dz) / (1 - dz);
	if (k > 1) k = 1;
	ox = x / m * k, oy = y / m * k;
}

struct PadOut {
	long seq; int conn; unsigned btns;
	float lx, ly, rx, ry, lt, rt;
	double ix, iy;
	unsigned long long t;
	int fam, src;          // the pad's family (0 none, 1 xbox, 2 playstation), where it is read (0 none, 1 XInput, 2 HID)
	unsigned p[16], r[16];
};

// the family the scripts draw buttons for: none without a pad; the cfg's glyphs if set; PlayStation when the pad is
// read over HID, or when XInput is in use with a Sony pad present (Steam Input makes a DualSense an XInput pad)
static int pad_family(int src, bool sony, int glyphs)
{
	if (!src) return 0;
	if (glyphs) return glyphs;
	return src == 2 || sony ? 2 : 1;
}

// the pad file's text; returns its length
static int pad_format(char *buf, size_t n, const PadOut &o)
{
	int k = sprintf_s(buf, n, "%ld %d %u %.3f %.3f %.3f %.3f %.3f %.3f %.4f %.4f %llu %d %d\np", o.seq, o.conn, o.btns,
	                  o.lx, o.ly, o.rx, o.ry, o.lt, o.rt, o.ix, o.iy, o.t, o.fam, o.src);
	for (int i = 0; i < 16 && k > 0; i++) k += sprintf_s(buf + k, n - k, " %u", o.p[i]);
	if (k > 0) k += sprintf_s(buf + k, n - k, "\nr");
	for (int i = 0; i < 16 && k > 0; i++) k += sprintf_s(buf + k, n - k, " %u", o.r[i]);
	if (k > 0) k += sprintf_s(buf + k, n - k, "\nend\n");
	return k;
}

// path.tmp written whole, then moved over path (fails while a reader holds path open without delete sharing: retried)
static bool write_atomic(const char *path, const char *data, size_t n)
{
	char tmp[MAX_PATH];
	if (sprintf_s(tmp, "%s.tmp", path) < 0) return false;
	HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	BOOL ok = WriteFile(f, data, (DWORD)n, &w, nullptr) && w == n;
	CloseHandle(f);
	return ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

// ---- what the pad holds down, by path -------------------------------------------------------------------------
static CRITICAL_SECTION g_pcs;        // the DI side's state below (the pad thread writes, the game's input thread reads)
static PadCfg g_pcfg;
static unsigned char g_di_key[256];   // keys held for the DI path, ORed into the keyboard's GetDeviceState
static unsigned char g_di_btn;        // mouse buttons held for the DI path (bit 0 left, 1 right, 2 middle)
static unsigned char g_di_kq[256], g_di_bq;   // held keys/buttons whose down event was queued (their up is queued too)
static long g_di_dx, g_di_dy, g_di_dz;   // motion and wheel not yet read by the mouse's GetDeviceState
struct DiEv { DWORD ofs, data; };
static DiEv g_q[2][256];              // events for GetDeviceData, [0] keyboard [1] mouse
static int g_qh[2], g_qn[2];
static volatile long g_di_state_n[2], g_di_data_n[2];   // calls seen, by kind (0 keyboard, 1 mouse)
static unsigned char g_si_key[256], g_si_btn;           // held through SendInput
static long g_si_n, g_si_fail;        // SendInput events sent / refused

static void di_push(int kind, DWORD ofs, DWORD data)   // under g_pcs
{
	if (!g_di_data_n[kind]) return;   // the game doesn't read that device's buffer: nothing to queue
	if (g_qn[kind] == 256) g_qh[kind] = (g_qh[kind] + 1) & 255, g_qn[kind]--;
	g_q[kind][(g_qh[kind] + g_qn[kind]) & 255] = { ofs, data };
	g_qn[kind]++;
}

static bool si_send(INPUT *in)
{
	// no extra info: the game's window procedure drops mouse messages whose GetMessageExtraInfo() isn't 0 (exe
	// 0x1412772bd, test rax / jne past the handler; touch and pen input carry it), and a tag there lost every click
	if (in->type == INPUT_KEYBOARD) in->ki.dwExtraInfo = 0;
	else in->mi.dwExtraInfo = 0;
	if (SendInput(1, in, sizeof *in) == 1) { g_si_n++; return true; }
	if (g_si_fail++ < 5) logf("pad: SendInput refused (%lu)", GetLastError());
	return false;
}
static bool si_key(int dik, bool down)
{
	INPUT in = {};
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = g_dik_vk[dik];
	in.ki.wScan = (WORD)(dik & 0x7f);
	in.ki.dwFlags = KEYEVENTF_SCANCODE | (dik & 0x80 ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
	return si_send(&in);
}
static bool si_mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0)
{
	INPUT in = {};
	in.type = INPUT_MOUSE;
	in.mi.dwFlags = flags, in.mi.dx = dx, in.mi.dy = dy, in.mi.mouseData = data;
	return si_send(&in);
}
static const DWORD BTN_DOWN[3] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
static const DWORD BTN_UP[3] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };

// make what is held match want_key/want_btn on each path (paths not in `inject` hold nothing). With both paths the
// SendInput events reach the game's DI devices too (their buffers and deltas), so the DI side then only ORs what is
// held into GetDeviceState (idempotent) and queues no GetDeviceData events of its own: each press arrives once.
static void pad_apply(const unsigned char *want_key, unsigned char want_btn, int inject)
{
	bool si = (inject & INJ_SI) != 0, di = (inject & INJ_DI) != 0, di_ev = di && !si;
	for (int k = 1; k < 256; k++) {
		bool w = want_key[k] != 0;
		if ((g_si_key[k] != 0) != (w && si) && si_key(k, w && si)) g_si_key[k] = w && si;   // kept until Windows takes it
	}
	for (int b = 0; b < 3; b++) {
		bool w = (want_btn >> b) & 1;
		bool h = (g_si_btn >> b) & 1;
		if (h != (w && si) && si_mouse(w && si ? BTN_DOWN[b] : BTN_UP[b])) g_si_btn ^= (unsigned char)(1 << b);
	}
	EnterCriticalSection(&g_pcs);
	for (int k = 1; k < 256; k++) {
		unsigned char w = want_key[k] && di ? 0x80 : 0;
		if (g_di_key[k] != w) {
			g_di_key[k] = w;
			if (w ? di_ev : g_di_kq[k] != 0) di_push(0, (DWORD)k, w);
			g_di_kq[k] = w && di_ev;
		}
	}
	for (int b = 0; b < 3; b++) {
		bool w = ((want_btn >> b) & 1) && di;
		if ((((g_di_btn >> b) & 1) != 0) != w) {
			g_di_btn ^= (unsigned char)(1 << b);
			if (w ? di_ev : ((g_di_bq >> b) & 1) != 0) di_push(1, (DWORD)(DIMOFS_BUTTON0 + b), w ? 0x80 : 0);
			g_di_bq = (unsigned char)(w && di_ev ? g_di_bq | (1 << b) : g_di_bq & ~(1 << b));
		}
	}
	LeaveCriticalSection(&g_pcs);
}

static void pad_release_all()
{
	static const unsigned char none[256] = {};
	pad_apply(none, 0, 0);
}

// ---- the game's DirectInput, wrapped ---------------------------------------------------------------------------
// GUIDs written out (dxguid.lib not linked)
static const GUID PAD_IID_DI8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID PAD_IID_DI8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUID PAD_IID_DEV8W = { 0x54D41081, 0xDC15, 0x4833, { 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79 } };
static const GUID PAD_SYSMOUSE = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID PAD_SYSKEYBOARD = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

enum { DM_STATE, DM_DATA, DM_POLL, DM_ACQUIRE, DM_UNACQUIRE, DM_FORMAT, DM_COOP, DM_PROP, DM_EVENT, DM_OTHER, DM_N };
static const char *const DM_NAME[DM_N] = { "GetDeviceState", "GetDeviceData", "Poll", "Acquire", "Unacquire",
	"SetDataFormat", "SetCooperativeLevel", "SetProperty", "SetEventNotification", "other" };

struct PadDev : IDirectInputDevice8W {
	IDirectInputDevice8W *r;
	int kind;                         // 0 keyboard, 1 mouse
	volatile long n[DM_N];
	ULONGLONG t0;
	bool reported;
	PadDev(IDirectInputDevice8W *real, int k) : r(real), kind(k), t0(GetTickCount64()), reported(false) { memset((void *)n, 0, sizeof n); }
	const char *nm() const { return kind ? "mouse" : "keyboard"; }
	void count(int m)
	{
		if (InterlockedIncrement(&n[m]) == 1 && m != DM_OTHER) logf("pad: the game's DI %s: first %s call", nm(), DM_NAME[m]);
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, LPVOID *o) override
	{
		if (i == PAD_IID_DEV8W || i == __uuidof(IUnknown)) { *o = this; AddRef(); return S_OK; }
		return r->QueryInterface(i, o);
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return r->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG k = r->Release();
		if (!k) { logf("pad: the game's DI %s released", nm()); }   // not deleted: the pad thread's report may still read it
		return k;
	}
	HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS a) override { count(DM_OTHER); return r->GetCapabilities(a); }
	HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW a, LPVOID b, DWORD c) override { count(DM_OTHER); return r->EnumObjects(a, b, c); }
	HRESULT STDMETHODCALLTYPE GetProperty(REFGUID a, LPDIPROPHEADER b) override { count(DM_OTHER); return r->GetProperty(a, b); }
	HRESULT STDMETHODCALLTYPE SetProperty(REFGUID a, LPCDIPROPHEADER b) override
	{
		count(DM_PROP);
		static int logged;
		// DIPROP_* are MAKEDIPROP(n): the GUID's address is n (1 buffer size, 2 axis mode)
		if (logged++ < 12) logf("pad: the game's DI %s SetProperty %llu (value %lu)", nm(), (unsigned long long)(uintptr_t)&a,
		                        b && b->dwSize >= sizeof(DIPROPDWORD) ? ((const DIPROPDWORD *)b)->dwData : 0);
		return r->SetProperty(a, b);
	}
	HRESULT STDMETHODCALLTYPE Acquire() override { count(DM_ACQUIRE); return r->Acquire(); }
	HRESULT STDMETHODCALLTYPE Unacquire() override { count(DM_UNACQUIRE); return r->Unacquire(); }
	HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID d) override
	{
		count(DM_STATE);
		InterlockedIncrement(&g_di_state_n[kind]);
		HRESULT hr = r->GetDeviceState(cb, d);
		if (FAILED(hr) || !d) return hr;
		EnterCriticalSection(&g_pcs);
		if (kind == 0 && cb >= 256) {
			unsigned char *k = (unsigned char *)d;
			for (int i = 1; i < 256; i++) k[i] |= g_di_key[i];
		} else if (kind == 1 && cb >= sizeof(DIMOUSESTATE)) {
			DIMOUSESTATE *m = (DIMOUSESTATE *)d;
			m->lX += g_di_dx, m->lY += g_di_dy, m->lZ += g_di_dz;
			g_di_dx = g_di_dy = g_di_dz = 0;
			for (int b = 0; b < 3; b++) if ((g_di_btn >> b) & 1) m->rgbButtons[b] |= 0x80;
		}
		LeaveCriticalSection(&g_pcs);
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cb, LPDIDEVICEOBJECTDATA d, LPDWORD io, DWORD fl) override
	{
		count(DM_DATA);
		InterlockedIncrement(&g_di_data_n[kind]);
		DWORD cap = io ? *io : 0;
		HRESULT hr = r->GetDeviceData(cb, d, io, fl);
		if (FAILED(hr) || !io) return hr;
		EnterCriticalSection(&g_pcs);
		if (!d) {
			if (fl & DIGDD_PEEK) *io += (DWORD)g_qn[kind];   // "how many are buffered" counts ours too
			else if (cap == INFINITE) g_qh[kind] = g_qn[kind] = 0;   // a flush
		} else if (cb >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
			DWORD got = *io, add = 0;
			static DWORD seq = 0x40000000;
			while (got + add < cap && add < (DWORD)g_qn[kind]) {
				const DiEv &e = g_q[kind][(g_qh[kind] + add) & 255];
				DIDEVICEOBJECTDATA_DX3 *o = (DIDEVICEOBJECTDATA_DX3 *)((unsigned char *)d + (size_t)(got + add) * cb);
				memset(o, 0, cb);
				o->dwOfs = e.ofs, o->dwData = e.data, o->dwTimeStamp = GetTickCount(), o->dwSequence = seq++;
				add++;
			}
			if (!(fl & DIGDD_PEEK)) g_qh[kind] = (g_qh[kind] + (int)add) & 255, g_qn[kind] -= (int)add;
			*io = got + add;
		}
		LeaveCriticalSection(&g_pcs);
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT a) override
	{
		count(DM_FORMAT);
		logf("pad: the game's DI %s SetDataFormat (data size %lu, %lu objects)", nm(), a ? a->dwDataSize : 0, a ? a->dwNumObjs : 0);
		return r->SetDataFormat(a);
	}
	HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE a) override { count(DM_EVENT); return r->SetEventNotification(a); }
	HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND a, DWORD b) override
	{
		count(DM_COOP);
		logf("pad: the game's DI %s SetCooperativeLevel flags %lx (%s%s%s)", nm(), b, b & DISCL_EXCLUSIVE ? "exclusive" : "non-exclusive",
		     b & DISCL_FOREGROUND ? ", foreground" : ", background", b & DISCL_NOWINKEY ? ", no Windows key" : "");
		return r->SetCooperativeLevel(a, b);
	}
	HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW a, DWORD b, DWORD c) override { count(DM_OTHER); return r->GetObjectInfo(a, b, c); }
	HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW a) override { count(DM_OTHER); return r->GetDeviceInfo(a); }
	HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { count(DM_OTHER); return r->RunControlPanel(a, b); }
	HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b, REFGUID c) override { count(DM_OTHER); return r->Initialize(a, b, c); }
	HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID a, LPCDIEFFECT b, LPDIRECTINPUTEFFECT *c, LPUNKNOWN d) override { count(DM_OTHER); return r->CreateEffect(a, b, c, d); }
	HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW a, LPVOID b, DWORD c) override { count(DM_OTHER); return r->EnumEffects(a, b, c); }
	HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOW a, REFGUID b) override { count(DM_OTHER); return r->GetEffectInfo(a, b); }
	HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD a) override { count(DM_OTHER); return r->GetForceFeedbackState(a); }
	HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD a) override { count(DM_OTHER); return r->SendForceFeedbackCommand(a); }
	HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b, DWORD c) override { count(DM_OTHER); return r->EnumCreatedEffectObjects(a, b, c); }
	HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE a) override { count(DM_OTHER); return r->Escape(a); }
	HRESULT STDMETHODCALLTYPE Poll() override { count(DM_POLL); return r->Poll(); }
	HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override { count(DM_OTHER); return r->SendDeviceData(a, b, c, d); }
	HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCWSTR a, LPDIENUMEFFECTSINFILECALLBACK b, LPVOID c, DWORD d) override { count(DM_OTHER); return r->EnumEffectsInFile(a, b, c, d); }
	HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCWSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) override { count(DM_OTHER); return r->WriteEffectToFile(a, b, c, d); }
	HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATW a, LPCWSTR b, DWORD c) override { count(DM_OTHER); return r->BuildActionMap(a, b, c); }
	HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATW a, LPCWSTR b, DWORD c) override { count(DM_OTHER); return r->SetActionMap(a, b, c); }
	HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW a) override { count(DM_OTHER); return r->GetImageInfo(a); }
};
static PadDev *g_devs[32];
static volatile long g_ndevs;

struct PadDI : IDirectInput8W {
	IDirectInput8W *r;
	explicit PadDI(IDirectInput8W *real) : r(real) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, LPVOID *o) override
	{
		if (i == PAD_IID_DI8W || i == __uuidof(IUnknown)) { *o = this; AddRef(); return S_OK; }
		return r->QueryInterface(i, o);
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return r->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override { return r->Release(); }   // the wrapper is kept (a few bytes)
	HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, LPDIRECTINPUTDEVICE8W *d, LPUNKNOWN u) override
	{
		HRESULT hr = r->CreateDevice(g, d, u);
		int kind = g == PAD_SYSKEYBOARD ? 0 : g == PAD_SYSMOUSE ? 1 : -1;
		static int logged;
		if (logged++ < 12)
			logf("pad: the game creates a DI device %08lx-%04x (%s): %08lx%s", g.Data1, g.Data2, kind == 0 ? "the system keyboard" : kind == 1 ? "the system mouse" : "other",
			     (unsigned long)hr, SUCCEEDED(hr) && kind >= 0 ? " - wrapped" : "");
		if (SUCCEEDED(hr) && d && *d && kind >= 0) {   // always wrapped; only the first 32 are kept for the use report
			PadDev *w = new PadDev(*d, kind);
			if (g_ndevs < 32) g_devs[InterlockedIncrement(&g_ndevs) - 1] = w;
			*d = w;
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE EnumDevices(DWORD a, LPDIENUMDEVICESCALLBACKW b, LPVOID c, DWORD d) override { return r->EnumDevices(a, b, c, d); }
	HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID a) override { return r->GetDeviceStatus(a); }
	HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { return r->RunControlPanel(a, b); }
	HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b) override { return r->Initialize(a, b); }
	HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a, LPCWSTR b, LPGUID c) override { return r->FindDevice(a, b, c); }
	HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR a, LPDIACTIONFORMATW b, LPDIENUMDEVICESBYSEMANTICSCBW c, LPVOID d, DWORD e) override { return r->EnumDevicesBySemantics(a, b, c, d, e); }
	HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a, LPDICONFIGUREDEVICESPARAMSW b, DWORD c, LPVOID d) override { return r->ConfigureDevices(a, b, c, d); }
};

static void pad_wrap_di(REFIID r, LPVOID *o)
{
	static int logged;
	bool w = r == PAD_IID_DI8W;
	if (logged++ < 4)
		logf("pad: DirectInput8Create(%s)%s", w ? "IID_IDirectInput8W" : r == PAD_IID_DI8A ? "IID_IDirectInput8A" : "another interface",
		     !g_pad_on ? "" : w ? " - wrapped" : " - not wrapped (only the W interface is)");
	if (!g_pad_on || !w) return;
	*o = static_cast<IDirectInput8W *>(new PadDI((IDirectInput8W *)*o));
}

// ---- Sony pads over HID ----------------------------------------------------------------------------------------
// A DualSense or DualShock 4 has no XInput unless Steam Input or DS4Windows makes one up, so with Steam Input off (as
// PAD.md advises) it would be invisible: hid_thread reads its HID reports itself (USB and Bluetooth) into an
// XINPUT_GAMEPAD, which the pad thread uses whenever no XInput pad is connected. Layouts (byte offsets after the
// report id): DualSense full report (USB 0x01 of 64 bytes, Bluetooth 0x31 one byte further on): sticks 0-3, L2/R2 4-5,
// buttons 7-9; the DS4's (USB 0x01, Bluetooth 0x11 two bytes on) and a Bluetooth pad's short 0x01 report before its
// full reports are switched on (by reading feature report 0x05 / 0x02): sticks 0-3, buttons 4-6, L2/R2 7-8. Buttons:
// the low nibble of the first is the D-pad (0 up, clockwise, 8 none), then square 0x10, cross 0x20, circle 0x40,
// triangle 0x80; the second L1 1, R1 2, create/share 0x10, options 0x20, L3 0x40, R3 0x80; the third PS 1, touchpad 2.
// Mapped as Xbox: cross A, circle B, square X, triangle Y, options Start, create/share and the touchpad click Back.
struct SonyPad { USHORT pid; const char *name; bool ds; };
static const SonyPad SONY[] = {
	{ 0x0ce6, "DualSense", true }, { 0x0df2, "DualSense Edge", true },
	{ 0x05c4, "DualShock 4", false }, { 0x09cc, "DualShock 4", false }, { 0x0ba0, "DualShock 4 (wireless adapter)", false },
};

// one input report -> g; false if it isn't one we read. len = the device's input report length (the buffer's).
static bool sony_parse(const unsigned char *r, int len, bool ds, XINPUT_GAMEPAD &g)
{
	int b;           // where the sticks start
	bool full;       // DualSense full layout
	if (ds) {
		if (r[0] == 0x01 && len == 64) b = 1, full = true;            // USB (Bluetooth's reports are 78 bytes)
		else if (r[0] == 0x31 && len >= 12) b = 2, full = true;       // Bluetooth, full report
		else if (r[0] == 0x01 && len >= 10) b = 1, full = false;      // Bluetooth, short report
		else return false;
	} else {
		if (r[0] == 0x01 && len >= 10) b = 1;                         // USB, or Bluetooth's short report
		else if (r[0] == 0x11 && len >= 12) b = 3;                    // Bluetooth
		else return false;
		full = false;
	}
	const unsigned char *p = r + b;
	unsigned char b0, b1, b2, l2, r2;
	if (full) l2 = p[4], r2 = p[5], b0 = p[7], b1 = p[8], b2 = p[9];
	else b0 = p[4], b1 = p[5], b2 = p[6], l2 = p[7], r2 = p[8];
	auto axis = [](unsigned char v, bool flip) {
		int a = (int)v - 128;
		if (flip) a = -a - 1;          // y: 0 is up on the pad, up is + in XInput
		int s = a >= 0 ? a * 32767 / 127 : a * 32768 / 128;
		return (SHORT)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	};
	memset(&g, 0, sizeof g);
	g.sThumbLX = axis(p[0], false), g.sThumbLY = axis(p[1], true);
	g.sThumbRX = axis(p[2], false), g.sThumbRY = axis(p[3], true);
	g.bLeftTrigger = l2, g.bRightTrigger = r2;
	static const WORD HAT[8] = {
		XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
		XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
		XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT,
	};
	WORD w = (b0 & 15) < 8 ? HAT[b0 & 15] : 0;
	if (b0 & 0x10) w |= XINPUT_GAMEPAD_X;
	if (b0 & 0x20) w |= XINPUT_GAMEPAD_A;
	if (b0 & 0x40) w |= XINPUT_GAMEPAD_B;
	if (b0 & 0x80) w |= XINPUT_GAMEPAD_Y;
	if (b1 & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (b1 & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
	if (b1 & 0x10) w |= XINPUT_GAMEPAD_BACK;
	if (b1 & 0x20) w |= XINPUT_GAMEPAD_START;
	if (b1 & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (b1 & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
	if (b2 & 0x02) w |= XINPUT_GAMEPAD_BACK;
	g.wButtons = w;
	return true;
}

static CRITICAL_SECTION g_hcs;
static XINPUT_GAMEPAD g_hid;     // the Sony pad's last state
static bool g_hid_conn;
static char g_hid_name[96];
static volatile bool g_xi_conn;  // an XInput pad is in use (the pad thread): the Sony pad is then only looked for, not read
static volatile bool g_hid_seen; // a Sony pad is there (read, or found by the presence check)

static void hid_set(bool conn, const XINPUT_GAMEPAD *g)
{
	EnterCriticalSection(&g_hcs);
	g_hid_conn = conn;
	if (g) g_hid = *g; else memset(&g_hid, 0, sizeof g_hid);
	LeaveCriticalSection(&g_hcs);
}

// the first Sony pad in the HID list, opened for overlapped reads; INVALID_HANDLE_VALUE if none. read false: only
// whether one is there (pad set, nothing opened for reading, INVALID_HANDLE_VALUE returned)
static HANDLE hid_open(const SonyPad *&pad, USHORT &inlen, USHORT &featlen, bool &bt, bool read)
{
	GUID hg;
	HidD_GetHidGuid(&hg);
	HDEVINFO set = SetupDiGetClassDevsW(&hg, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
	HANDLE found = INVALID_HANDLE_VALUE;
	SP_DEVICE_INTERFACE_DATA id = { sizeof id };
	for (DWORD i = 0; found == INVALID_HANDLE_VALUE && !(!read && pad) && SetupDiEnumDeviceInterfaces(set, nullptr, &hg, i, &id); i++) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &id, nullptr, 0, &need, nullptr);
		if (!need || need > 4096) continue;
		static unsigned char buf[4096];
		auto *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
		det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		if (!SetupDiGetDeviceInterfaceDetailW(set, &id, det, need, nullptr, nullptr)) continue;
		// no access first: the attributes need none, and opening every HID device for reading is rude (and refused for keyboards)
		HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) continue;
		HIDD_ATTRIBUTES at = { sizeof at };
		const SonyPad *sp = nullptr;
		if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054c)
			for (const SonyPad &s : SONY) if (s.pid == at.ProductID) sp = &s;
		CloseHandle(h);
		if (!sp) continue;
		if (!read) { pad = sp; continue; }
		h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			static int n;
			if (n++ < 3) logf("pad: a %s is there but couldn't be opened (%lu) - another program may hold it alone (HidHide/DS4Windows?)", sp->name, GetLastError());
			continue;
		}
		PHIDP_PREPARSED_DATA pp = nullptr;
		HIDP_CAPS caps = {};
		if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
		if (caps.InputReportByteLength < 10) { CloseHandle(h); continue; }
		pad = sp, inlen = caps.InputReportByteLength, featlen = caps.FeatureReportByteLength;
		// a Bluetooth DualSense's input report is 78 bytes, USB 64; a DS4's 547 over Bluetooth, 64 on USB
		bt = sp->ds ? inlen != 64 : inlen > 64;
		found = h;
	}
	SetupDiDestroyDeviceInfoList(set);
	return found;
}

static DWORD WINAPI hid_thread(LPVOID)
{
	static unsigned char rep[1024];
	OVERLAPPED ov = {};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	bool probe_was = false;
	for (;;) {
		const SonyPad *pad = nullptr;
		USHORT inlen = 0, featlen = 0;
		bool bt = false;
		if (g_xi_conn) {
			// XInput in use (maybe this same DualSense through Steam Input): never opened for reading then, only looked
			// for every 2 s, so the scripts still show PlayStation buttons
			hid_open(pad, inlen, featlen, bt, false);
			if (!probe_was || (pad != nullptr) != g_hid_seen)
				logf("pad: XInput in use - %s", pad ? "a Sony pad is there too (PlayStation buttons)" : "no Sony pad over HID");
			g_hid_seen = pad != nullptr, probe_was = true;
			Sleep(2000);
			continue;
		}
		probe_was = false;
		HANDLE h = hid_open(pad, inlen, featlen, bt, true);
		if (h == INVALID_HANDLE_VALUE) { g_hid_seen = false; Sleep(2000); continue; }
		g_hid_seen = true;
		if (bt) {   // reading the calibration feature report switches a Bluetooth pad to its full reports
			unsigned char f[128] = {};
			f[0] = pad->ds ? 0x05 : 0x02;
			BOOL ok = HidD_GetFeature(h, f, featlen && featlen <= sizeof f ? featlen : (pad->ds ? 41 : 37));
			logf("pad: %s full reports %s", pad->name, ok ? "asked for" : "not asked for (the feature read failed) - its short reports are read");
		}
		sprintf_s(g_hid_name, "%s (%s)", pad->name, bt ? "Bluetooth" : "USB");
		logf("pad: %s found over HID, reports of %u bytes - used whenever no XInput pad is connected", g_hid_name, inlen);
		int bad = 0, n = 0;
		unsigned char last_id = 0;
		for (;;) {
			if (g_xi_conn) { logf("pad: an XInput pad is in use - the %s isn't read", g_hid_name); break; }
			DWORD got = 0;
			ResetEvent(ov.hEvent);
			if (!ReadFile(h, rep, inlen, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
			DWORD w = WaitForSingleObject(ov.hEvent, 2000);
			if (w != WAIT_OBJECT_0) { CancelIo(h); GetOverlappedResult(h, &ov, &got, TRUE); break; }   // a pad that stops reporting is gone
			if (!GetOverlappedResult(h, &ov, &got, FALSE) || !got) break;
			XINPUT_GAMEPAD g;
			if (sony_parse(rep, inlen, pad->ds, g)) {
				hid_set(true, &g);
				bad = 0;
				if (rep[0] != last_id) { logf("pad: %s report 0x%02x (%s)", g_hid_name, rep[0], n++ ? "switched" : "first"); last_id = rep[0]; }
			} else if (++bad == 200) logf("pad: %s sends reports we don't read (id 0x%02x)", g_hid_name, rep[0]);
		}
		CloseHandle(h);
		hid_set(false, nullptr);
		if (!g_xi_conn) logf("pad: %s gone (%lu)", g_hid_name, GetLastError());
		Sleep(1000);
	}
}

// ---- the pad thread --------------------------------------------------------------------------------------------
typedef DWORD(WINAPI *XGetState)(DWORD, XINPUT_STATE *);

static void pad_path(char *p, const char *name) { strcpy_s(p, MAX_PATH, g_dir); strcat_s(p, MAX_PATH, name); }

static WORD g_diag_prev;   // the buttons at the last poll, for the press log
static FILETIME g_cfg_time;
static void pad_load_cfg(bool first)
{
	char p[MAX_PATH];
	pad_path(p, "silvaron_pad.cfg");
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) {
		if (first) {
			bool ok = write_atomic(p, PAD_CFG_DEFAULT, sizeof PAD_CFG_DEFAULT - 1);
			logf("pad: no silvaron_pad.cfg - %s", ok ? "written with the defaults" : "couldn't write one: the defaults");
			GetFileAttributesExA(p, GetFileExInfoStandard, &a);
		} else return;
	} else if (!first && CompareFileTime(&a.ftLastWriteTime, &g_cfg_time) == 0) return;
	g_cfg_time = a.ftLastWriteTime;
	static char text[16384];
	text[0] = 0;
	FILE *f = _fsopen(p, "rb", _SH_DENYNO);
	if (f) { size_t k = fread(text, 1, sizeof text - 1, f); text[k] = 0; fclose(f); }
	PadCfg c;
	char err[256];
	int bad = pad_parse_cfg(text, c, err, sizeof err);
	EnterCriticalSection(&g_pcs);
	g_pcfg = c;
	LeaveCriticalSection(&g_pcs);
	logf("pad: settings %s: %s, inject %s, cursor %s, speed %.0f, deadzone %.2f%s%s", first ? "read" : "changed", c.enabled ? "on" : "off (enabled 0)",
	     c.inject == 3 ? "both" : c.inject == INJ_DI ? "di" : c.inject == INJ_SI ? "sendinput" : "none", c.cursor_si ? "sendinput" : "setpos",
	     c.speed, c.dz, bad ? " - lines not understood, first: " : "", bad ? err : "");
}

// the battle script's claim on inputs: the take bits, 0 when the file is missing or its last write is 1.5 s old
static PadMode g_pmode;
static unsigned g_remap;   // the remap bits of the fresh mode file, 0 when stale
static PadMode g_pulse;    // the fresh mode file's pulse (pulse_id 0 when stale or none)
static unsigned pad_taken()
{
	static FILETIME seen;
	static bool have, stale = true;
	char p[MAX_PATH];
	pad_path(p, "silvaron_pad_mode.txt");
	WIN32_FILE_ATTRIBUTE_DATA a;
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	bool ok = GetFileAttributesExA(p, GetFileExInfoStandard, &a) != 0;
	if (ok && CompareFileTime(&a.ftLastWriteTime, &seen) != 0) {
		char text[1024] = {};
		FILE *f = _fsopen(p, "rb", _SH_DENYNO);
		if (f) { size_t k = fread(text, 1, sizeof text - 1, f); text[k] = 0; fclose(f); }
		PadMode m;
		if (pad_parse_mode(text, m)) {   // incomplete (being written): tried again next time
			seen = a.ftLastWriteTime;
			if (!have || strcmp(m.name, g_pmode.name) || m.take != g_pmode.take || m.remap != g_pmode.remap)
				logf("pad: mode %s, the script takes %05x%s%s", m.name[0] ? m.name : "?", m.take,
				     m.remap & REMAP_LS_KEYS ? ", left stick = the right stick's keys" : "", m.remap & REMAP_LBRB_WHEEL ? ", LB/RB = the wheel" : "");
			g_pmode = m, have = true;
		}
	}
	ULONGLONG age = ok ? (((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime)) : ~0ull;
	bool st = !have || age > 15000000ull;   // 1.5 s in 100 ns
	if (st != stale) { logf(st ? "pad: the mode file is stale or gone - every input is the base layer's" : "pad: the mode file is fresh again (mode %s)", g_pmode.name); stale = st; }
	g_remap = st ? 0 : g_pmode.remap;
	g_pulse = g_pmode;
	if (st) g_pulse.pulse_id = 0;
	return st ? 0 : g_pmode.take;
}

// a window of ours in front: the game's (its client rect in screen coordinates)
static bool game_in_front(RECT &rc)
{
	HWND h = GetForegroundWindow();
	DWORD pid = 0;
	if (!h || !GetWindowThreadProcessId(h, &pid) || pid != GetCurrentProcessId()) return false;
	if (!GetClientRect(h, &rc)) return false;
	POINT a = { rc.left, rc.top }, b = { rc.right, rc.bottom };
	ClientToScreen(h, &a); ClientToScreen(h, &b);
	rc = { a.x, a.y, b.x, b.y };
	return rc.right > rc.left && rc.bottom > rc.top;
}

static void log_raw_input(const char *when)
{
	UINT n = 0;
	if (GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE)) == (UINT)-1 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;
	RAWINPUTDEVICE d[16];
	if (n > 16) n = 16;
	UINT got = n ? GetRegisteredRawInputDevices(d, &n, sizeof(RAWINPUTDEVICE)) : 0;
	if (got == (UINT)-1) got = 0;
	char s[512] = "";
	for (UINT i = 0; i < got; i++) {
		char b[64];
		sprintf_s(b, " page %x usage %x flags %lx%s", d[i].usUsagePage, d[i].usUsage, d[i].dwFlags, d[i].usUsage == 2 ? " (mouse)" : d[i].usUsage == 6 ? " (keyboard)" : "");
		strcat_s(s, b);
	}
	logf("pad: raw input registered by the process %s: %u%s", when, got, s);
}

static void log_di_use()
{
	for (long i = 0; i < g_ndevs; i++) {
		PadDev *d = g_devs[i];
		if (!d || d->reported || GetTickCount64() - d->t0 < 10000) continue;
		d->reported = true;
		char s[512] = "";
		for (int m = 0; m < DM_N; m++) if (d->n[m]) { char b[64]; sprintf_s(b, " %s %ld", DM_NAME[m], d->n[m]); strcat_s(s, b); }
		logf("pad: the game's DI %s, first 10 s:%s", d->nm(), s[0] ? s : " no calls");
	}
}

static DWORD WINAPI pad_thread(LPVOID)
{
	pad_load_cfg(true);
	const char *dll = "xinput1_4.dll";
	HMODULE x = LoadLibraryA(dll);
	if (!x) x = LoadLibraryA(dll = "xinput9_1_0.dll");
	XGetState get = x ? (XGetState)GetProcAddress(x, "XInputGetState") : nullptr;
	logf(get ? "pad: XInput from %s" : "pad: XInput not found (%s) - no controller; the pad file says not connected", get ? dll : "neither xinput1_4.dll nor xinput9_1_0.dll");
	char sv[512];
	if (GetEnvironmentVariableA("SteamVirtualGamepadInfo", sv, sizeof sv) || GetEnvironmentVariableA("EnableConfiguratorSupport", sv, sizeof sv))
		logf("pad: Steam Input looks on for the game (Steam's environment variables are set): it may turn the pad into its own mouse/keys or a virtual pad - turn Steam Input off for TWW3");
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
	char path[MAX_PATH];
	pad_path(path, "silvaron_pad.txt");

	PadOut o = {};
	int slot = -1;
	WORD prev = 0;
	double t0 = now_s(), last = t0, next_probe = 0, next_mode = 0, next_cfg = 0, last_write = -1, raw_at[2] = { t0 + 10, t0 + 60 };
	char body[1024], last_body[1024] = "", text[1024];
	bool was_on = true, front_was = false;
	long wfail = 0;
	unsigned take = 0;
	double fx = 0, fy = 0, full_t = 0;           // the cursor's fractions of a pixel, how long the stick has been full over
	double wheel_next[IN_N] = {};
	bool held[IN_N] = {};
	bool kept[IN_N] = {};   // taken by the script while held: stays out of the base layer until released
	for (;;) {
		double t = now_s(), dt = t - last;
		last = t;
		if (dt > 0.1) dt = 0.1;
		if (t >= next_cfg) { next_cfg = t + 2; pad_load_cfg(false); }
		if (t >= next_mode) { next_mode = t + 0.02; take = pad_taken(); }   // a mode change seen within a frame or two
		for (int i = 0; i < 2; i++) if (raw_at[i] && t >= raw_at[i]) { log_raw_input(i ? "at 60 s" : "at 10 s"); raw_at[i] = 0; }
		log_di_use();
		PadCfg c;
		EnterCriticalSection(&g_pcs);
		c = g_pcfg;
		LeaveCriticalSection(&g_pcs);
		if (!c.enabled) {
			if (was_on) { pad_release_all(); logf("pad: off (enabled 0 in silvaron_pad.cfg)"); was_on = false; slot = -1; }
			Sleep(200);
			continue;
		}
		if (!was_on) { logf("pad: on again"); was_on = true; }

		// the pad
		XINPUT_STATE xs = {};
		bool conn = false;
		if (get && slot >= 0) {
			conn = get((DWORD)slot, &xs) == ERROR_SUCCESS;
			if (!conn) { logf("pad: slot %d disconnected", slot); slot = -1; next_probe = t + 2; }
		} else if (get && t >= next_probe) {
			next_probe = t + 2;   // a disconnected slot can stall the call: probed every 2 s only
			for (int i = 0; i < 4 && slot < 0; i++)
				if (get((DWORD)i, &xs) == ERROR_SUCCESS) { slot = i, conn = true; logf("pad: connected, slot %d (%s)", i, dll); }
		}
		if (!conn) memset(&xs, 0, sizeof xs);
		// no XInput pad: a Sony pad read over HID
		static int src_was = -1;
		if (!conn) {
			EnterCriticalSection(&g_hcs);
			if (g_hid_conn) xs.Gamepad = g_hid, conn = true;
			LeaveCriticalSection(&g_hcs);
		}
		g_xi_conn = slot >= 0;
		int src = !conn ? 0 : slot >= 0 ? 1 : 2;
		o.src = src, o.fam = pad_family(src, g_hid_seen, c.glyphs);
		static int fam_was = -1;
		if (o.fam != fam_was) {
			logf("pad: buttons shown as %s%s", o.fam == 2 ? "PlayStation" : o.fam == 1 ? "Xbox" : "none (no pad)", c.glyphs && src ? " (glyphs in silvaron_pad.cfg)" : "");
			fam_was = o.fam;
		}
		if (src != src_was) {
			if (src == 2) logf("pad: in use: the %s read over HID", g_hid_name);
			else if (src == 1 && src_was == 2) logf("pad: in use: XInput slot %d (it comes first)", slot);
			src_was = src;
		}
		const XINPUT_GAMEPAD &g = xs.Gamepad;
		o.conn = conn;
		o.btns = g.wButtons;
		pad_deadzone(g.sThumbLX / 32767.f, g.sThumbLY / 32767.f, c.dz, o.lx, o.ly);
		pad_deadzone(g.sThumbRX / 32767.f, g.sThumbRY / 32767.f, c.dz, o.rx, o.ry);
		o.lt = g.bLeftTrigger / 255.f, o.rt = g.bRightTrigger / 255.f;
		o.ix += o.rx * dt, o.iy += o.ry * dt;
		for (int b = 0; b < 16; b++) {
			bool now = (g.wButtons >> b) & 1, was = (prev >> b) & 1;
			if (now && !was) o.p[b]++;
			if (!now && was) o.r[b]++;
		}
		prev = g.wButtons;

		// the pad file: on change (at most every 16 ms) and every 250 ms
		o.seq = 0, o.t = 0;
		pad_format(body, sizeof body, o);
		bool changed = strcmp(body, last_body) != 0;
		if ((changed && t - last_write >= 0.016) || t - last_write >= 0.25) {
			static long seq;
			o.seq = ++seq, o.t = (unsigned long long)((t - t0) * 1000);
			int n = pad_format(text, sizeof text, o);
			if (n > 0 && write_atomic(path, text, (size_t)n)) { last_write = t; strcpy_s(last_body, body); }
			else if (wfail++ < 5) logf("pad: silvaron_pad.txt not written (%lu) - tried again", GetLastError());
		}

		// the base layer
		RECT rc;
		bool front = game_in_front(rc);
		if (front != front_was) {
			static int logged;
			if (logged++ < 20) logf(front ? "pad: the game is in front - the pad drives mouse and keys" : "pad: the game isn't in front - nothing sent");
			front_was = front;
		}
		int inject = front ? c.inject : 0;
		unsigned char want_key[256] = {};
		unsigned char want_btn = 0;
		int wheel = 0;
		const unsigned remap = g_remap;
		PadAct act[IN_N];
		for (int i = 0; i < IN_N; i++) act[i] = c.act[i];
		if (remap & REMAP_LBRB_WHEEL) act[IN_LB].type = act[IN_RB].type = PA_WHEEL, act[IN_LB].code = 2, act[IN_RB].code = 1;
		for (int i = 0; i < IN_N; i++) {
			bool on;
			if (i < 14) on = (g.wButtons & IN_BIT[i]) != 0;
			else if (i == IN_LT || i == IN_RT) { float v = i == IN_LT ? o.lt : o.rt; on = v > (held[i] ? 0.2f : 0.35f); }
			else { float v = i == IN_RSU ? o.ry : i == IN_RSD ? -o.ry : i == IN_RSL ? -o.rx : o.rx; on = v > (held[i] ? 0.35f : 0.5f); }
			held[i] = on;
			// an input the script took stays the script's until it is let go: a press that closes order mode (B, A on
			// Go) is still down when the script gives the input back, and would otherwise become a click too
			kept[i] = on && (kept[i] || (take & in_take_bit(i)) != 0);
			if (!on || kept[i] || !inject) { wheel_next[i] = 0; continue; }
			const PadAct &a = act[i];
			if (a.type == PA_KEY) want_key[a.code] = 1;
			else if (a.type == PA_MOUSE) want_btn |= (unsigned char)(1 << a.code);
			else if (a.type == PA_WHEEL && t >= wheel_next[i]) {
				// a tick at once, then repeated: every 250 ms lightly pressed, every 60 ms at full (triggers), 120 ms for buttons
				float pull = i == IN_LT ? o.lt : i == IN_RT ? o.rt : 0.5f;
				wheel += a.code == 1 ? 1 : -1;
				wheel_next[i] = t + (i == IN_LT || i == IN_RT ? 0.25 - 0.19 * pull : 0.12);
			}
		}
		// the left stick as the right stick's keys (remap ls_keys): past half tilt, let go below a third
		{
			static bool lh[4];
			float v4[4] = { o.ly, -o.ly, -o.lx, o.lx };
			for (int k = 0; k < 4; k++) {
				lh[k] = (remap & REMAP_LS_KEYS) && !(take & TAKE_LS) && v4[k] > (lh[k] ? 0.35f : 0.5f);
				const PadAct &a = act[IN_RSU + k];
				if (lh[k] && inject && a.type == PA_KEY) want_key[a.code] = 1;
			}
		}
		// the script's pulse (zoom-to-fit on the tactical map): each new id once; wheel notches 40 ms apart, keys held
		{
			static unsigned pulse_seen;
			static int pw;
			static double pw_next, fw_until, rt_until;
			static int fw_dir, rt_dir;
			if (!g_pmode.pulse_id) pulse_seen = 0;   // (a mode file without a pulse: the next id 1 is new again)
			if (g_pulse.pulse_id && g_pulse.pulse_id != pulse_seen) {
				pulse_seen = g_pulse.pulse_id;
				pw = g_pulse.pulse_wheel, pw_next = t;
				fw_dir = g_pulse.pulse_fwd > 0 ? 1 : g_pulse.pulse_fwd < 0 ? -1 : 0, fw_until = t + abs(g_pulse.pulse_fwd) / 1000.0;
				rt_dir = g_pulse.pulse_right > 0 ? 1 : g_pulse.pulse_right < 0 ? -1 : 0, rt_until = t + abs(g_pulse.pulse_right) / 1000.0;
				static int nlog;
				if (nlog++ < 40) logf("pad: pulse %u: wheel %d, forward %d ms, right %d ms%s", pulse_seen, g_pulse.pulse_wheel, g_pulse.pulse_fwd, g_pulse.pulse_right, inject ? "" : " (not sent: the game isn't in front)");
			}
			if (inject) {
				if (pw && t >= pw_next) { wheel += pw > 0 ? 1 : -1; pw += pw > 0 ? -1 : 1; pw_next = t + 0.04; }
				const PadAct &af = act[fw_dir > 0 ? IN_RSU : IN_RSD], &ar = act[rt_dir > 0 ? IN_RSR : IN_RSL];
				if (fw_dir && t < fw_until && af.type == PA_KEY) want_key[af.code] = 1;
				if (rt_dir && t < rt_until && ar.type == PA_KEY) want_key[ar.code] = 1;
			} else pw = 0, fw_dir = rt_dir = 0;
		}
		pad_apply(want_key, want_btn, inject);
		// what the pad did, for an in-game test where "nothing happened" (2026-11-02): each new press for the first
		// 40 (where it came from, whether the game was in front, what it became, the cursor and the window under it),
		// and every 10 s while connected the SendInput events and cursor moves sent
		{
			static const char *BN[16] = { "up", "down", "left", "right", "start", "back", "l3", "r3", "lb", "rb", "?", "?", "a", "b", "x", "y" };
			static int nd;
			static long si_was, moves;
			static double next_sum;
			static POINT cur_was;
			POINT cp = {};
			GetCursorPos(&cp);
			if (cp.x != cur_was.x || cp.y != cur_was.y) moves++, cur_was = cp;
			for (int b = 0; b < 16 && nd < 40; b++) {
				if (!((g.wButtons >> b) & 1) || ((g_diag_prev >> b) & 1)) continue;
				nd++;
				HWND hw = WindowFromPoint(cp);
				DWORD pid = 0;
				char cls[64] = "";
				if (hw) { GetWindowThreadProcessId(hw, &pid); GetClassNameA(hw, cls, sizeof cls); }
				int ii = -1;
				for (int i = 0; i < 14; i++) if (IN_BIT[i] == (WORD)(1 << b)) ii = i;
				const PadAct *a = ii >= 0 ? &act[ii] : nullptr;
				logf("pad: press %s (%s): %s, inject %d, %s, cursor %ld,%ld over '%s' (%s)", BN[b], slot >= 0 ? "XInput" : "HID",
				     front ? "game in front" : "game NOT in front", inject,
				     ii >= 0 && kept[ii] ? "taken by the script" : !a ? "no binding" : a->type == PA_KEY ? "a key" : a->type == PA_MOUSE ? "a mouse button" : a->type == PA_WHEEL ? "the wheel" : "nothing",
				     cp.x, cp.y, cls, !hw ? "no window" : pid == GetCurrentProcessId() ? "the game's" : "ANOTHER process's");
			}
			g_diag_prev = g.wButtons;
			if (conn && t >= next_sum) {
				if (next_sum) logf("pad: last 10 s: %ld SendInput events, the cursor moved on %ld polls, now at %ld,%ld; buttons %04x, left stick %.2f,%.2f; game %s",
				                   g_si_n - si_was, moves, cp.x, cp.y, g.wButtons, o.lx, o.ly, front ? "in front" : "not in front");
				next_sum = t + 10, si_was = g_si_n, moves = 0;
			}
		}
		if (wheel && inject) {
			if (inject & INJ_SI) si_mouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(wheel * WHEEL_DELTA));
			if ((inject & INJ_DI) && !(inject & INJ_SI)) {   // with both, the SendInput tick already reaches the DI mouse
				EnterCriticalSection(&g_pcs);
				if (g_di_state_n[1]) g_di_dz += wheel * WHEEL_DELTA;
				di_push(1, DIMOFS_Z, (DWORD)(wheel * WHEEL_DELTA));
				LeaveCriticalSection(&g_pcs);
			}
		}
		// the cursor: speed by tilt squared (fine at small tilts), up to 2.5x after 0.8 s fully over
		float m = sqrtf(o.lx * o.lx + o.ly * o.ly);
		full_t = m > 0.9f ? full_t + dt : 0;
		if (inject && c.ls_cursor && !(take & TAKE_LS) && !(remap & REMAP_LS_KEYS) && m > 0) {
			double acc = 1 + 1.5 * (full_t > 0.8 ? 1 : full_t / 0.8);
			double v = c.speed * (rc.bottom - rc.top) / 1080.0 * m * acc * dt;   // x the stick (length m): pixels by m squared
			fx += o.lx * v, fy -= o.ly * v;
			LONG dx = (LONG)fx, dy = (LONG)fy;
			fx -= dx, fy -= dy;
			if (dx || dy) {
				if (c.cursor_si && (inject & INJ_SI)) si_mouse(MOUSEEVENTF_MOVE, dx, dy);
				else {
					POINT pt;
					if (GetCursorPos(&pt)) {
						LONG nx = pt.x + dx, ny = pt.y + dy;
						nx = nx < rc.left ? rc.left : nx >= rc.right ? rc.right - 1 : nx;
						ny = ny < rc.top ? rc.top : ny >= rc.bottom ? rc.bottom - 1 : ny;
						SetCursorPos(nx, ny);
					}
				}
				if ((inject & INJ_DI) && !(c.cursor_si && (inject & INJ_SI))) {   // SetCursorPos makes no DI motion; SendInput moves do
					EnterCriticalSection(&g_pcs);
					if (g_di_state_n[1]) {
						g_di_dx += dx, g_di_dy += dy;
						g_di_dx = g_di_dx > 10000 ? 10000 : g_di_dx < -10000 ? -10000 : g_di_dx;
						g_di_dy = g_di_dy > 10000 ? 10000 : g_di_dy < -10000 ? -10000 : g_di_dy;
					}
					if (dx) di_push(1, DIMOFS_X, (DWORD)dx);
					if (dy) di_push(1, DIMOFS_Y, (DWORD)dy);
					LeaveCriticalSection(&g_pcs);
				}
			}
		} else fx = fy = 0;

		// every 8 ms with a pad, 50 ms without
		LONGLONG due = -(conn ? 80000LL : 500000LL);   // 100 ns units, relative
		if (timer && SetWaitableTimer(timer, (LARGE_INTEGER *)&due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
		else Sleep(conn ? 8 : 50);
	}
}

static void pad_start()
{
	InitializeCriticalSection(&g_pcs);
	PadCfg c;
	pad_parse_cfg("", c, nullptr, 0);   // the defaults until the thread reads the file
	g_pcfg = c;
	g_pad_on = true;
	InitializeCriticalSection(&g_hcs);
	CreateThread(nullptr, 0, hid_thread, nullptr, 0, nullptr);
	CreateThread(nullptr, 0, pad_thread, nullptr, 0, nullptr);
	logf("pad: the controller layer starts (silvaron_overlord.pack ticked)");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID)
{
	if (why == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(h);
		g_exe = (uintptr_t)GetModuleHandleA(nullptr);
		char p[MAX_PATH];
		GetModuleFileNameA(nullptr, p, MAX_PATH);
		char *s = strrchr(p, '\\');
		if (s) s[1] = 0;
		strcpy_s(g_dir, p);
		strcat_s(p, "silvaron_pip_log.txt");
		g_log = _fsopen(p, "w", _SH_DENYWR);
		logf("silvaron pip loaded (Route B: the game renders the PiP camera), exe base %llx", (unsigned long long)g_exe);
		if (pack_ticked("silvaron_ui_scale.pack")) ui_scale_patch();
		// the controller: its own thread and the DirectInput wrapper only, none of the PiP's hooks
		if (pack_ticked("silvaron_overlord.pack")) {
			if (building_startpos()) logf("pad: the game is building a startpos (user.script.txt): no controller");
			else pad_start();
		}
		if (!pack_ticked("silvaron_pip.pack")) {
			logf("silvaron_pip.pack is not ticked in the launcher: staying off (no hooks)");
			return TRUE;
		}
		read_settings();
		load_pos();
		if (building_startpos()) {
			logf("the game is building a startpos (user.script.txt): staying off");
			return TRUE;
		}
		InitializeCriticalSection(&g_cs);
		InitializeCriticalSection(&g_rcs);
		InitializeCriticalSection(&g_scs);
		CreateThread(nullptr, 0, setup, nullptr, 0, nullptr);
		CreateThread(nullptr, 0, cam_reader, nullptr, 0, nullptr);
		CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
	} else if (why == DLL_PROCESS_DETACH && g_pad_on) {
		pad_release_all();   // nothing left held down in Windows once the game is gone
	}
	return TRUE;
}
