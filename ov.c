/* ov.c — Win+Tab style overview for xfwm4 on X11 (xfwm4 compositor must be enabled)
 * build:  gcc -O2 -o ov.bin ov.c $(pkg-config --cflags --libs x11 xcomposite xrender xft fontconfig xinerama xdamage)
 * usage:  ov.bin          toggle the overview (talks to the daemon if one is running, otherwise runs one-shot)
 *         ov.bin -t       alt-tab switcher: bind to Alt+Tab (unbind xfwm4's "Cycle windows"); hold ALTMOD, Tab/Shift+Tab cycle, release ALTMOD to activate;
 *                         a quick tap (ALTMOD already up when it appears) activates the second window immediately
 *         ov.bin -s       sticky switcher: bind to e.g. KP_5; Tab/KP_5 advance, Shift+Tab/Shift+KP_5 go back, Enter/click activate, Esc cancel
 *                         (both switcher modes grab only the keys they use, so media keys and other xfce shortcuts keep working; the overview grabs nothing)
 *         ov.bin -d       daemon: keeps named window pixmaps cached, so minimised windows and windows on
 *                         other desktops still have thumbnails and opening the overview is instant.
 *                         (X only lets you name the pixmap of a *viewable* window, so one-shot mode
 *                         shows minimised / other-desktop windows as icon+title only.)
 * keys:   Esc close · ←/→/Tab/↑/↓ select · Enter/Space activate · Ctrl+W close selected window · 1-9 switch desktop · wheel scroll
 *         (the overlay only takes focus, never grabs the keyboard, so xfce/xfwm4 shortcuts keep working: e.g. Win+Tab re-runs ov.bin which closes it, Ctrl+Super+←/→ still switches desktop)
 * mouse:  click window = activate · click its × = close it · hover desktop = switch (stay open) · click desktop = switch and close · drag window onto desktop = move it · click elsewhere = close */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xdamage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>
#include <X11/XF86keysym.h>

#define MAXW 256   /* max windows / cached pixmaps */
#define MAXD 32    /* max desktops */
#define GAP 16     /* spacing */
#define DH 180     /* desktop thumbnail height (strip height = DH + font height + 3*GAP) */
#define DESKLEFT 1 /* 1 = left-align the desktop strip, 0 = centre it */
#define SELW 1     /* thickness of the selected-window outline */
#define HOVA 0.35f /* opacity of the weaker outline on the window under the pointer (a click commits it; Enter/Alt-release use the active selection) */
#define DESKW 1    /* thickness of the current-desktop outline */
#define ALTMOD Mod1Mask /* modifier whose release activates the window in alt-tab (-t) mode */
#define FPS 60     /* max redraw rate for live thumbnail updates and fades */
#define FADEMS 150 /* outline fade in/out time */
#define BGA 0xd800 /* black tint over the wallpaper in the window area (0 = plain wallpaper, 0xffff = opaque black) */
#define TOPA 0xf400 /* black tint over the wallpaper in the desktop strip */
#define SWA 0x8000 /* switcher modes: black tint over the live screen (they are translucent, no wallpaper backdrop) */
#define TH 220     /* window thumbnail height (constant; windows are never upscaled past 1:1) */
#define HOVERMS 0   /* hover time over a desktop before switching to it */
#define STRIP 26   /* title strip height */
#define ICON 18    /* icon size */
#define FONT "Fixed-10" /* a fontconfig pattern; the whole fontconfig-sorted candidate list (so your fonts.conf fallbacks, e.g. Dotum12) is used for per-glyph fallback */
#define MAXF 8    /* how many fonts from that list to open */
/* media keys while the alt-tab switcher (-t) is open: ALTMOD is held, so they arrive as e.g. Alt+XF86AudioRaiseVolume and xfce's
   plain bindings don't match; the switcher grabs only the ALTMOD combinations and runs these instead (plain media keys still go to xfce) */
struct { KeySym k; char *cmd; } media[] = {
	{ XF86XK_AudioRaiseVolume, "pactl set-sink-volume @DEFAULT_SINK@ +5%" },
	{ XF86XK_AudioLowerVolume, "pactl set-sink-volume @DEFAULT_SINK@ -5%" },
	{ XF86XK_AudioMute,        "pactl set-sink-mute @DEFAULT_SINK@ toggle" },
	{ XF86XK_AudioMicMute,     "pactl set-source-mute @DEFAULT_SOURCE@ toggle" },
	{ XF86XK_AudioPlay,        "playerctl play-pause" },
	{ XF86XK_AudioPause,       "playerctl pause" },
	{ XF86XK_AudioStop,        "playerctl stop" },
	{ XF86XK_AudioNext,        "playerctl next" },
	{ XF86XK_AudioPrev,        "playerctl previous" },
};
#define FX(x) ((XFixed)((x) * 65536))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIC(w) ((w)->c && (w)->c->pic ? (w)->c->pic : 0)

enum { STACK, CUR, NDESK, DNAMES, DESK, STATE, HIDDEN, SKIPT, TYPE, DOCK, DESKTOP, NAME, UTF8, ICONA, ACTIVE, CLOSE, ROOTPM, TOGGLE, OWNER, NATOMS };
char *atomnames[] = { "_NET_CLIENT_LIST_STACKING", "_NET_CURRENT_DESKTOP", "_NET_NUMBER_OF_DESKTOPS", "_NET_DESKTOP_NAMES", "_NET_WM_DESKTOP", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_NAME", "UTF8_STRING", "_NET_WM_ICON", "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_XROOTPMAP_ID", "_OVERVIEW_TOGGLE", "_OVERVIEW_OWNER" };

typedef struct { Window frame; Pixmap pix; Picture pic; Damage dmg; int w, h; } Cache;
typedef struct { Window client, frame; Cache *c; Picture icon; int iw, ih, fx, fy, fw, fh, desk, hidden, x, y, w, h, row; float oa; char title[160]; } Win;

Display *D; Window R, W; Atom A[NATOMS]; Picture P, half; XftFont *font, *fonts[MAXF]; int nf; XftDraw *xd; XftColor white;
int S, sw, sh, mw, mh, mx, my, shown, daemon_, dmgbase, dirty, closehov = -1, pfrev, hoverd = -1, hov = -1, nrows, mode, grabbed, ndesk, cur, nw, ns, nc, sel = -1, scroll, top, ah, dragi = -1, dragging, ox, oy, px, py, dtw, dth, dx0;
long lastdraw, hovert; Window pfocus; Cache cache[MAXW], rootpm, *wp; Win wins[MAXW]; int show[MAXW]; char names[MAXD][64]; float da[MAXD];

int xerr(Display *d, XErrorEvent *e) { return 0; }

unsigned char *prop(Window w, Atom a, Atom t, unsigned long *n)
{
	Atom rt; int f; unsigned long b; unsigned char *p = 0; *n = 0;
	if (XGetWindowProperty(D, w, a, 0, 1 << 20, 0, t, &rt, &f, n, &b, &p) != Success) return 0;
	if (p && !*n) { XFree(p); p = 0; }
	return p;
}

unsigned long card(Window w, Atom a, unsigned long def) { unsigned long n, *p = (unsigned long*)prop(w, a, XA_CARDINAL, &n), v = p ? *p : def; if (p) XFree(p); return v; }

int has(Atom *p, unsigned long n, Atom v) { for (unsigned long i = 0; p && i < n; i++) if (p[i] == v) return 1; return 0; }

Window frameof(Window w) /* top-level ancestor (the xfwm4 frame) */
{
	Window r, p, *c; unsigned n;
	while (XQueryTree(D, w, &r, &p, &c, &n)) { if (c) XFree(c); if (p == R || !p) return w; w = p; }
	return 0;
}

Cache *cfind(Window f) { for (int i = 0; i < nc; i++) if (cache[i].frame == f) return &cache[i]; return 0; }

void cfree(Cache *c) { if (c->pic) XRenderFreePicture(D, c->pic); if (c->pix) XFreePixmap(D, c->pix); if (c->dmg) XDamageDestroy(D, c->dmg); c->pic = c->pix = c->dmg = 0; }

void cdel(Window f) { Cache *c = cfind(f); if (c) { cfree(c); *c = cache[--nc]; } }

Cache *cname(Window f) /* (re)name the frame's backing pixmap; only possible while viewable. The reference keeps the contents alive after unmap. */
{
	XWindowAttributes a; Window r; int x, y; unsigned w, h, b, d; Cache *c;
	if (!XGetWindowAttributes(D, f, &a) || a.map_state != IsViewable || a.override_redirect) return 0;
	Pixmap p = XCompositeNameWindowPixmap(D, f);
	if (!XGetGeometry(D, p, &r, &x, &y, &w, &h, &b, &d)) return 0;
	if (!(c = cfind(f))) { if (nc == MAXW) { XFreePixmap(D, p); return 0; } c = &cache[nc++]; *c = (Cache){f}; }
	cfree(c); c->pix = p; c->w = w; c->h = h; c->pic = XRenderCreatePicture(D, p, XRenderFindVisualFormat(D, a.visual), 0, 0);
	XRenderSetPictureFilter(D, c->pic, FilterBilinear, 0, 0); c->dmg = XDamageCreate(D, f, XDamageReportNonEmpty); return c;
}

Cache *cget(Window f) { Cache *c = cfind(f); return c ? c : cname(f); }

Picture mkicon(Window cw, int *iw, int *ih)
{
	unsigned long n, i, *p = (unsigned long*)prop(cw, A[ICONA], XA_CARDINAL, &n), *b = 0; if (!p) return 0;
	for (i = 0; i + 1 < n && i + 2 + p[i] * p[i + 1] <= n; i += 2 + p[i] * p[i + 1]) if (!b || labs((long)p[i] - ICON) < labs((long)b[0] - ICON)) b = p + i;
	if (!b) { XFree(p); return 0; }
	int w = *iw = b[0], h = *ih = b[1]; uint32_t *d = malloc((size_t)w * h * 4);
	for (i = 0; i < (unsigned long)w * h; i++) { unsigned long v = b[2 + i], a = v >> 24 & 255; d[i] = a << 24 | (v >> 16 & 255) * a / 255 << 16 | (v >> 8 & 255) * a / 255 << 8 | (v & 255) * a / 255; }
	XImage *im = XCreateImage(D, DefaultVisual(D, S), 32, ZPixmap, 0, (char*)d, w, h, 32, 0);
	Pixmap pm = XCreatePixmap(D, R, w, h, 32); GC g = XCreateGC(D, pm, 0, 0);
	XPutImage(D, pm, g, im, 0, 0, 0, 0, w, h); XFreeGC(D, g); XDestroyImage(im); XFree(p);
	Picture pic = XRenderCreatePicture(D, pm, XRenderFindStandardFormat(D, PictStandardARGB32), 0, 0);
	XFreePixmap(D, pm); XRenderSetPictureFilter(D, pic, FilterBilinear, 0, 0); return pic;
}

void layout(void) /* greedy left-to-right rows of TH-high thumbnails (never upscaled past 1:1) */
{
	dth = DH; dtw = DH * sw / sh; if (ndesk * (dtw + GAP) - GAP > mw - 2 * GAP) { dtw = (mw - (ndesk + 1) * GAP) / ndesk; dth = dtw * sh / sw; }
	dx0 = DESKLEFT ? GAP : (mw - ndesk * (dtw + GAP) + GAP) / 2; top = mode ? GAP : dth + font->height + 3 * GAP; ah = mh - top - GAP;
	int aw = mw - 2 * GAP, h = TH + STRIP, x = 0, r = 0;
	for (int i = 0; i < ns; i++)
	{
		Win *w = &wins[show[i]]; int cw = PIC(w) ? w->c->w : w->fw, ch = PIC(w) ? w->c->h : w->fh; double s = MIN((double)TH / MAX(ch, 1), 1.0);
		w->w = MAX(cw * s, 1); w->h = MAX(ch * s, 1);
		if (x && x + w->w > aw) { x = 0; r++; }
		w->row = r; x += w->w + GAP;
	}
	nrows = ns ? r + 1 : 0; int cont = nrows * (h + GAP) - GAP + SELW; scroll = MAX(0, MIN(scroll, cont - ah)); int yo = mode ? MAX(ah - cont, 0) / 2 : 0; /* switchers centre vertically while it fits, scroll once it doesn't */
	for (int i = 0, j; i < ns; i = j) /* overview: top-left aligned; switcher: centred both ways */
	{
		int r = wins[show[i]].row, rw = -GAP; for (j = i; j < ns && wins[show[j]].row == r; j++) rw += wins[show[j]].w + GAP;
		int x = GAP + (mode ? (aw - rw) / 2 : 0), y = top + yo + r * (h + GAP) - scroll;
		for (j = i; j < ns && wins[show[j]].row == r; j++) { Win *w = &wins[show[j]]; w->x = x; w->y = y; x += w->w + GAP; }
	}
	if (sel >= ns) sel = ns - 1;
}

void rebuild(void)
{
	unsigned long n, m, i, j; Window dc = dragi >= 0 ? wins[show[dragi]].client : 0, oc[MAXW], *cl = (Window*)prop(R, A[STACK], XA_WINDOW, &n); float oo[MAXW]; int onw = nw; unsigned char *dn = prop(R, A[DNAMES], A[UTF8], &m); Pixmap *pm;
	for (i = 0; i < (unsigned long)nw; i++) oc[i] = wins[i].client, oo[i] = wins[i].oa; /* keep fade state across rebuilds */
	cur = card(R, A[CUR], 0); ndesk = MIN(card(R, A[NDESK], 1), MAXD);
	for (i = 0, j = 0; i < (unsigned long)ndesk; i++) { snprintf(names[i], 64, "%lu", i + 1); if (dn && j < m) { snprintf(names[i], 64, "%s", (char*)dn + j); j += strlen((char*)dn + j) + 1; } }
	if (dn) XFree(dn);
	for (i = 0; i < (unsigned long)nw; i++) if (wins[i].icon) XRenderFreePicture(D, wins[i].icon);
	nw = ns = 0; wp = 0;
	for (i = 0; cl && i < n && nw < MAXW; i++)
	{
		Window cw = cl[i]; Atom *ty = (Atom*)prop(cw, A[TYPE], XA_ATOM, &m); int dock = has(ty, m, A[DOCK]), isdesk = has(ty, m, A[DESKTOP]); if (ty) XFree(ty);
		if (isdesk) wp = cget(frameof(cw));
		if (dock || isdesk) continue;
		Atom *st = (Atom*)prop(cw, A[STATE], XA_ATOM, &m); int skip = has(st, m, A[SKIPT]), hid = has(st, m, A[HIDDEN]); if (st) XFree(st);
		if (skip) continue;
		Win *w = &wins[nw]; *w = (Win){cw, frameof(cw)}; if (!w->frame) continue;
		unsigned long d = card(cw, A[DESK], cur); w->hidden = hid; w->desk = d == 0xFFFFFFFF ? -1 : (int)d;
		Window r; unsigned fw, fh, b, dp; if (!XGetGeometry(D, w->frame, &r, &w->fx, &w->fy, &fw, &fh, &b, &dp)) continue;
		w->fw = fw; w->fh = fh; w->c = cget(w->frame);
		w->icon = mkicon(cw, &w->iw, &w->ih); for (j = 0; j < (unsigned long)onw; j++) if (oc[j] == cw) w->oa = oo[j];
		unsigned char *t = prop(cw, A[NAME], A[UTF8], &m); if (!t) t = prop(cw, XA_WM_NAME, XA_STRING, &m); snprintf(w->title, sizeof w->title, "%s", t ? (char*)t : ""); if (t) XFree(t);
		XSelectInput(D, cw, PropertyChangeMask); nw++;
	}
	if (cl) XFree(cl);
	if (!wp && (pm = (Pixmap*)prop(R, A[ROOTPM], XA_PIXMAP, &m))) /* fallback wallpaper if no DESKTOP-type window found */
	{
		Window r; int x, y; unsigned w, h, b, d;
		if (rootpm.pix != *pm) { if (rootpm.pic) XRenderFreePicture(D, rootpm.pic); rootpm.pic = 0; if (XGetGeometry(D, *pm, &r, &x, &y, &w, &h, &b, &d)) { rootpm.pix = *pm; rootpm.w = w; rootpm.h = h; rootpm.pic = XRenderCreatePicture(D, *pm, XRenderFindVisualFormat(D, DefaultVisual(D, S)), 0, 0); XRenderSetPictureFilter(D, rootpm.pic, FilterBilinear, 0, 0); } }
		wp = rootpm.pic ? &rootpm : 0; XFree(pm);
	}
	for (i = nw; i-- > 0;) if (wins[i].desk == cur || wins[i].desk < 0) show[ns++] = i; /* alt-tab order: top of stack first, minimised windows keep their place */
	Window *ap = (Window*)prop(R, A[ACTIVE], XA_WINDOW, &m), act = ap ? *ap : 0; if (ap) XFree(ap);
	for (i = 1; act && i < (unsigned long)ns; i++) if (wins[show[i]].client == act) { int t = show[i]; memmove(show + 1, show, i * sizeof *show); show[0] = t; break; } /* the focused window leads, even if the WM has not restacked yet (e.g. the instant after a minimise) */
	for (dragi = -1, i = 0; dc && i < (unsigned long)ns; i++) if (wins[show[i]].client == dc) dragi = i;
	if (dragi < 0) dragging = 0;
	layout();
}

void blit(Picture src, int w, int h, double s, int x, int y, Picture mask)
{
	XTransform t = {{{FX(1 / s), 0, 0}, {0, FX(1 / s), 0}, {0, 0, FX(1)}}};
	XRenderSetPictureTransform(D, src, &t);
	XRenderComposite(D, PictOpOver, src, mask, P, 0, 0, 0, 0, x, y, w * s + 0.5, h * s + 0.5);
}

void fill(int x, int y, int w, int h, int r, int g, int b, int a) { XRenderColor c = {r, g, b, a}; XRenderFillRectangle(D, PictOpOver, P, &c, x, y, w, h); }

void box(int x, int y, int w, int h, int t, float a) /* outline at opacity a */
{
	XRectangle r[] = {{x - t, y - t, w + 2 * t, t}, {x - t, y + h, w + 2 * t, t}, {x - t, y, t, h}, {x + w, y, t, h}}; int v = a * 0xffff; XRenderColor c = {v, v, v, v};
	if (v > 0) XRenderFillRectangles(D, PictOpOver, P, &c, r, 4);
}

int fade(float *v, float t, long dt) { float d = (float)dt / FADEMS; *v = *v < t ? MIN(*v + d, t) : MAX(*v - d, t); return *v != t; } /* returns 1 while still animating */

void clip(int x, int y, int w, int h) { XRectangle r = {x, y, MAX(w, 0), MAX(h, 0)}; XRenderSetPictureClipRectangles(D, P, 0, 0, &r, 1); XftDrawSetClipRectangles(xd, 0, 0, &r, 1); }

void openfonts(void)
{
	FcPattern *p = FcNameParse((FcChar8*)FONT); FcResult r; FcConfigSubstitute(0, p, FcMatchPattern); XftDefaultSubstitute(D, S, p);
	FcFontSet *fs = FcFontSort(0, p, FcTrue, 0, &r);
	for (int i = 0; fs && i < fs->nfont && nf < MAXF; i++) { XftFont *f = XftFontOpenPattern(D, FcFontRenderPrepare(0, p, fs->fonts[i])); if (f) fonts[nf++] = f; }
	FcPatternDestroy(p); font = nf ? fonts[0] : 0; if (fs) FcFontSetDestroy(fs);
}

int text(char *s, int x, int y, int draw) /* draws (draw=1) or measures UTF-8 text with per-glyph font fallback; returns width */
{
	int w = 0, l; FcChar32 u; XGlyphInfo g;
	for (; *s && (l = FcUtf8ToUcs4((FcChar8*)s, &u, strlen(s))) > 0; s += l)
	{
		XftFont *f = font; for (int i = 0; i < nf; i++) if (XftCharExists(D, fonts[i], u)) { f = fonts[i]; break; }
		if (draw) XftDrawString32(xd, &white, f, x + w, y, &u, 1);
		XftTextExtents32(D, f, &u, 1, &g); w += g.xOff;
	}
	return w;
}

void drawdesk(int d, long dt)
{
	int x = dx0 + d * (dtw + GAP), y = GAP; double s = (double)dtw / sw;
	clip(x, y, dtw, dth); fill(x, y, dtw, dth, 0, 0, 0, 0xffff);
	if (wp && wp->pic) blit(wp->pic, wp->w, wp->h, s, x, y, 0);
	for (int i = 0; i < nw; i++) { Win *w = &wins[i]; if (!w->hidden && (w->desk == d || w->desk < 0) && PIC(w)) blit(PIC(w), w->c->w, w->c->h, s, x + w->fx * s, y + w->fy * s, 0); }
	clip(x - DESKW, y - DESKW, dtw + 2 * DESKW, dth + DESKW + GAP + font->height);
	dirty |= fade(&da[d], d == cur, dt); box(x, y, dtw, dth, DESKW, da[d]);
	text(names[d], x + (dtw - text(names[d], 0, 0, 0)) / 2, y + dth + GAP / 2 + font->ascent, 1);
}

long now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

int hitclose(int i, int x, int y) { Win *w = i >= 0 ? &wins[show[i]] : 0; return w && x >= w->x + w->w - STRIP && x < w->x + w->w && y >= w->y && y < w->y + STRIP; }

void draw(void)
{
	if (!shown) return;
	long t = now(), dt = MIN(t - lastdraw, 1000 / FPS); lastdraw = t; dirty = 0; int xw = text("\xc3\x97", 0, 0, 0);
	XRenderColor bg = {0, 0, 0, mode ? SWA : BGA}, tb = {0, 0, 0, TOPA}, k = {0, 0, 0, mode ? 0 : 0xffff}; XRenderFillRectangle(D, PictOpSrc, P, &k, 0, 0, mw, mh);
	if (!mode && wp && wp->pic) blit(wp->pic, wp->w, wp->h, 1, -mx, -my, 0); /* this monitor's part of the wallpaper as the backdrop */
	XRenderFillRectangle(D, PictOpOver, P, &bg, 0, 0, mw, mh);
	if (!mode) { XRenderFillRectangle(D, PictOpOver, P, &tb, 0, 0, mw, top - GAP); for (int d = 0; d < ndesk; d++) drawdesk(d, dt); }
	clip(0, top - SELW, mw, ah + SELW);
	for (int i = 0; i < ns; i++)
	{
		Win *w = &wins[show[i]]; if (w->y + w->h + STRIP < top || w->y > top + ah) continue;
		fill(w->x, w->y, w->w, STRIP, 0, 0, 0, 0xa000); /* title strip sits above the thumbnail */
		if (PIC(w)) blit(PIC(w), w->c->w, w->c->h, (double)w->w / w->c->w, w->x, w->y + STRIP, 0); else fill(w->x, w->y + STRIP, w->w, w->h, 0x3000, 0x3000, 0x3000, 0xffff);
		if (i == closehov) fill(w->x + w->w - STRIP, w->y, STRIP, STRIP, 0xc000, 0x1000, 0x1000, 0xffff);
		text("\xc3\x97", w->x + w->w - (STRIP + xw) / 2, w->y + (STRIP + font->ascent - font->descent) / 2, 1); /* × */
		if (w->icon) blit(w->icon, w->iw, w->ih, (double)ICON / w->iw, w->x + 4, w->y + (STRIP - ICON) / 2, 0);
		clip(w->x + ICON + 8, top - SELW, w->w - ICON - 12 - STRIP, ah + SELW); text(w->title, w->x + ICON + 8, w->y + (STRIP + font->ascent - font->descent) / 2, 1); clip(0, top - SELW, mw, ah + SELW);
		dirty |= fade(&w->oa, dragging ? 0 : i == sel ? 1 : i == hov ? HOVA : 0, dt); box(w->x, w->y, w->w, w->h + STRIP, SELW, w->oa);
	}
	clip(0, 0, mw, mh);
	if (dragging) { Win *w = &wins[show[dragi]]; if (PIC(w)) blit(PIC(w), w->c->w, w->c->h, (double)w->w / w->c->w / 2, px - w->w / 4, py - w->h / 4, half); }
	XFlush(D);
}

int hitwin(int x, int y) { if (y < top || y > top + ah) return -1; for (int i = 0; i < ns; i++) { Win *w = &wins[show[i]]; if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h + STRIP) return i; } return -1; }
int hitdesk(int x, int y) { int d = (x - dx0) / (dtw + GAP); return !mode && y >= GAP && y < GAP + dth && x >= dx0 && d < ndesk && x - dx0 - d * (dtw + GAP) < dtw ? d : -1; }

void msg(Window w, Atom a, long d0, long d1)
{
	XEvent e = {ClientMessage}; e.xclient.window = w; e.xclient.message_type = a; e.xclient.format = 32; e.xclient.data.l[0] = d0; e.xclient.data.l[1] = d1;
	XSendEvent(D, R, 0, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

void hide(int refocus) /* refocus: give focus back to what had it (cancel); when a window is being activated the WM sets focus itself and we must not fight it */
{ if (refocus && !mode) XSetInputFocus(D, pfocus, pfrev, CurrentTime); XUnmapWindow(D, W); XUngrabKey(D, AnyKey, AnyModifier, R); XFlush(D); shown = 0; if (!daemon_) exit(0); }

void activate(int i) { if (i >= 0 && i < ns) { msg(wins[show[i]].client, A[ACTIVE], 2, CurrentTime); hide(0); } else hide(1); }

void key(KeySym k, unsigned st);

void grabkeys(void) /* switchers: passive grabs on only the keys key() uses, so media keys etc. still reach xfce. Each modifier combination is grabbed separately,
                       so one that xfce already holds (e.g. Alt+Tab, KP_5) just fails on its own (xfce then re-runs ov.bin, which advances via cmd) */
{ /* only the modifiers that matter (Shift, ALTMOD, Ctrl) times the lock states (Caps, NumLock): a few hundred grabs instead of thousands, which made the switcher slow to appear */
	KeySym ks[] = { XK_Tab, XK_Escape, XK_Return, XK_KP_Enter, XK_space, XK_Left, XK_Right, XK_Up, XK_Down, XK_KP_4, XK_KP_6, XK_KP_8, XK_KP_2, XK_KP_5,
	                XK_1, XK_2, XK_3, XK_4, XK_5, XK_6, XK_7, XK_8, XK_9 };
	unsigned nl = XkbKeysymToModifiers(D, XK_Num_Lock), locks[] = { 0, LockMask, nl, LockMask | nl }, nav[] = { 0, ShiftMask, ALTMOD, ALTMOD | ShiftMask }, ctl[] = { ControlMask, ControlMask | ALTMOD }, alt[] = { ALTMOD, ALTMOD | ShiftMask };
	#define GRAB(kc, set) for (unsigned a = 0; a < sizeof set / sizeof *set; a++) for (unsigned l = 0; l < 4; l++) XGrabKey(D, kc, set[a] | locks[l], R, 0, GrabModeAsync, GrabModeAsync)
	for (unsigned i = 0; i < sizeof ks / sizeof *ks; i++) { KeyCode kc = XKeysymToKeycode(D, ks[i]); if (kc) GRAB(kc, nav); }
	KeyCode w = XKeysymToKeycode(D, XK_w); if (w) GRAB(w, ctl); /* Ctrl+W only, plain w stays free */
	for (unsigned i = 0; mode == 1 && i < sizeof media / sizeof *media; i++) { KeyCode kc = XKeysymToKeycode(D, media[i].k); if (kc) GRAB(kc, alt); }
	#undef GRAB
}

void grab(void) /* probe: succeeds once no other client holds the keyboard (xfce's shortcut grab lasts while the key that launched us is held); until then
                   our key grabs can't fire, so the idle loop polls the keys that matter. Released at once, so unrelated keys keep going where they normally would */
{ if ((grabbed = XGrabKeyboard(D, W, 0, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)) XUngrabKeyboard(D, CurrentTime); }

unsigned mods(void) { int rx, ry, wx, wy; unsigned m; Window a, b; XQueryPointer(D, R, &a, &b, &rx, &ry, &wx, &wy, &m); return m; }

void reveal(int m) /* fullscreen on the monitor under the pointer; m: 0 overview, 1 alt-tab, 2 sticky switcher */
{
	int n, rx, ry, wx, wy, x = 0, y = 0; unsigned mk; Window a, b; XineramaScreenInfo *si = XineramaQueryScreens(D, &n);
	mode = m; mw = sw; mh = sh; XQueryPointer(D, R, &a, &b, &rx, &ry, &wx, &wy, &mk);
	for (int i = 0; si && i < n; i++) if (rx >= si[i].x_org && rx < si[i].x_org + si[i].width && ry >= si[i].y_org && ry < si[i].y_org + si[i].height) { x = si[i].x_org; y = si[i].y_org; mw = si[i].width; mh = si[i].height; }
	if (si) XFree(si);
	mx = x; my = y; XMoveResizeWindow(D, W, x, y, mw, mh); scroll = 0; dragi = -1; dragging = 0; closehov = -1; hoverd = hov = -1; rebuild(); sel = mode ? MIN(1, ns - 1) : -1; shown = 1; XMapRaised(D, W);
	grabbed = 0; if (mode) { grabkeys(); grab(); } else { XGetInputFocus(D, &pfocus, &pfrev); XSetInputFocus(D, W, RevertToPointerRoot, CurrentTime); } /* switchers use key grabs (the idle loop watches ALTMOD); the overview takes focus */
}

void cmd(int m) /* IPC command: the mode to show; re-sending a switcher's command while it is open advances it */
{
	if (shown && mode == m && m) key(XK_Right, 0);
	else if (shown && mode == m) hide(1);
	else { if (shown) hide(0); reveal(m); }
}

void key(KeySym k, unsigned st)
{
	Win *s = sel >= 0 ? &wins[show[sel]] : 0;
	for (unsigned i = 0; i < sizeof media / sizeof *media; i++) if (k == media[i].k) { if (!fork()) { close(ConnectionNumber(D)); setsid(); execl("/bin/sh", "sh", "-c", media[i].cmd, (char*)0); _exit(127); } return; }
	if (k == XK_Tab || (mode && (k == XK_KP_5 || k == XK_KP_Begin))) k = st & ShiftMask ? XK_Left : XK_Right;
	k = k == XK_KP_8 || k == XK_KP_Up ? XK_Up : k == XK_KP_2 || k == XK_KP_Down ? XK_Down : k == XK_KP_4 || k == XK_KP_Left ? XK_Left : k == XK_KP_6 || k == XK_KP_Right ? XK_Right : k; /* numpad, with or without NumLock */
	if (k == XK_Escape) { hide(1); return; }
	else if (k == XK_w && st & ControlMask) { if (s) msg(s->client, A[CLOSE], CurrentTime, 2); return; }
	else if (k == XK_Return || k == XK_KP_Enter || k == XK_space) { activate(sel); return; }
	else if (k >= XK_1 && k <= XK_9 && (int)(k - XK_1) < ndesk) { msg(R, A[CUR], k - XK_1, CurrentTime); hide(0); return; }
	else if (k == XK_Right) sel = ns ? (sel + 1) % ns : -1;
	else if (k == XK_Left) sel = ns ? (sel + ns - 1) % ns : -1;
	else if ((k == XK_Up || k == XK_Down) && !s) sel = ns ? 0 : -1;
	else if (k == XK_Up || k == XK_Down) /* nearest window in the row above/below, wrapping */
	{
		int bd = 1 << 30, cx = s->x + s->w / 2, tr = (s->row + (k == XK_Down ? 1 : nrows - 1)) % nrows;
		for (int i = 0; i < ns; i++) { Win *w = &wins[show[i]]; int d = abs(w->x + w->w / 2 - cx); if (w->row == tr && d < bd) bd = d, sel = i; }
	}
	else return;
	hoverd = -1;
	if (sel >= 0) { s = &wins[show[sel]]; if (s->y < top) scroll -= top - s->y; else if (s->y + s->h + STRIP > top + ah) scroll += s->y + s->h + STRIP - top - ah; layout(); }
	draw();
}

void button(XButtonEvent *e)
{
	int d;
	if (e->button == 4 || e->button == 5) { scroll += e->button == 4 ? -80 : 80; layout(); draw(); }
	else if (e->button == 1)
	{
		int i = hitwin(e->x, e->y);
		if (hitclose(i, e->x, e->y)) msg(wins[show[i]].client, A[CLOSE], CurrentTime, 2);
		else if ((dragi = i) >= 0) { ox = e->x; oy = e->y; }
		else if ((d = hitdesk(e->x, e->y)) >= 0) { msg(R, A[CUR], d, CurrentTime); hide(0); }
		else hide(1);
	}
}

void release(XButtonEvent *e)
{
	if (e->button != 1 || dragi < 0) return;
	int d = hitdesk(e->x, e->y);
	if (!dragging) { activate(dragi); dragi = -1; return; }
	if (d >= 0 && d != cur) msg(wins[show[dragi]].client, A[DESK], d, 2); /* the WM's _NET_WM_DESKTOP change triggers a rebuild */
	dragging = 0; dragi = -1; draw();
}

void motion(XMotionEvent *e)
{
	px = e->x; py = e->y;
	if (dragi >= 0 && !dragging && abs(px - ox) + abs(py - oy) > 10) dragging = 1;
	int h = hitwin(px, py), ch = hitclose(h, px, py) ? h : -1, d = hitdesk(px, py);
	if (d != hoverd) { hoverd = d >= 0 && d != cur && !dragging ? d : -1; hovert = now(); } /* the idle loop switches desktop after HOVERMS */
	if (dragging || h != hov || ch != closehov) { hov = h; closehov = ch; draw(); } /* hovering never moves the active selection */
}

int down(char *km, KeySym s) { KeyCode k = XKeysymToKeycode(D, s); return km[k >> 3] >> (k & 7) & 1; }

void run(void)
{
	XEvent e; Cache *c; Atom a; int fd = ConnectionNumber(D);
	for (;;)
	{
		if (!XPending(D)) /* idle: block until an event arrives, or until the next frame is due if a thumbnail changed */
		{
			long t = now(), ms = 1L << 30;
			if (mode == 1 && shown && !(mods() & ALTMOD)) { activate(sel); continue; } /* polled, so the release is caught even before our grab/focus is in place */
			if (mode && shown && !grabbed) /* until our grabs can fire keys go elsewhere (xfce's shortcut grab, the old window), so poll the ones that matter */
			{
				char km[32]; XQueryKeymap(D, km); grab();
				if (down(km, XK_Return) || down(km, XK_KP_Enter)) { activate(sel); continue; } else if (down(km, XK_Escape)) { hide(1); continue; }
			}
			if (mode && shown && (mode == 1 || !grabbed)) ms = 5;
			if (hoverd >= 0) { if (t >= hovert + HOVERMS) { if (hoverd != cur) msg(R, A[CUR], hoverd, CurrentTime); hoverd = -1; continue; } ms = hovert + HOVERMS - t; }
			if (dirty && shown) { if (t >= lastdraw + 1000 / FPS) { draw(); continue; } ms = MIN(ms, lastdraw + 1000 / FPS - t); }
			fd_set s; FD_ZERO(&s); FD_SET(fd, &s); struct timeval tv = {ms / 1000, ms % 1000 * 1000}; select(fd + 1, &s, 0, 0, ms < 1L << 30 ? &tv : 0); continue;
		}
		XNextEvent(D, &e);
		if (e.type == dmgbase + XDamageNotify) { XDamageSubtract(D, ((XDamageNotifyEvent*)&e)->damage, None, None); dirty = 1; continue; }
		switch (e.type)
		{
		case Expose: if (!e.xexpose.count) draw(); break;
		case KeyPress: if (shown) key(XLookupKeysym(&e.xkey, 0), e.xkey.state); break;
		case ButtonPress: button(&e.xbutton); break;
		case ButtonRelease: release(&e.xbutton); break;
		case MotionNotify: while (XCheckTypedWindowEvent(D, W, MotionNotify, &e)); motion(&e.xmotion); break;
		case PropertyNotify:
			a = e.xproperty.atom;
			if (a == A[TOGGLE]) cmd(card(R, A[TOGGLE], 0));
			else if (shown && (a == A[STACK] || a == A[CUR] || a == A[NDESK] || a == A[DNAMES] || a == A[DESK] || a == A[STATE])) { rebuild(); if (!mode) XSetInputFocus(D, W, RevertToPointerRoot, CurrentTime); draw(); }
			break;
		case MapNotify: cname(e.xmap.window); break;
		case ConfigureNotify: if ((c = cfind(e.xconfigure.window)) && (c->w != e.xconfigure.width || c->h != e.xconfigure.height)) cname(c->frame); break;
		case DestroyNotify: cdel(e.xdestroywindow.window); break;
		}
	}
}

int main(int argc, char **argv)
{
	daemon_ = argc > 1 && !strcmp(argv[1], "-d"); long m = argc > 1 && !strcmp(argv[1], "-t") ? 1 : argc > 1 && !strcmp(argv[1], "-s") ? 2 : 0;
	if (!(D = XOpenDisplay(0))) return fprintf(stderr, "cannot open display\n"), 1;
	XSetErrorHandler(xerr); signal(SIGCHLD, SIG_IGN); /* no zombies from media commands */ S = DefaultScreen(D); R = RootWindow(D, S); sw = DisplayWidth(D, S); sh = DisplayHeight(D, S);
	XInternAtoms(D, atomnames, NATOMS, 0, A); int ee; if (!XDamageQueryExtension(D, &dmgbase, &ee)) return fprintf(stderr, "no XDamage\n"), 1;
	Window o = XGetSelectionOwner(D, A[OWNER]);
	if (o && daemon_) return fprintf(stderr, "ov already running\n"), 1;
	if (o) { XChangeProperty(D, R, A[TOGGLE], XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&m, 1); XSync(D, 0); return 0; }
	XVisualInfo vi; if (!XMatchVisualInfo(D, S, 32, TrueColor, &vi)) return fprintf(stderr, "no ARGB visual (is the compositor on?)\n"), 1;
	XSetWindowAttributes wa = {.override_redirect = 1, .background_pixel = 0, .border_pixel = 0, .colormap = XCreateColormap(D, R, vi.visual, AllocNone), .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask};
	W = XCreateWindow(D, R, 0, 0, 1, 1, 0, 32, InputOutput, vi.visual, CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &wa);
	P = XRenderCreatePicture(D, W, XRenderFindVisualFormat(D, vi.visual), 0, 0);
	XRenderColor hc = {0, 0, 0, 0xa000}, wc = {0xffff, 0xffff, 0xffff, 0xffff}; half = XRenderCreateSolidFill(D, &hc);
	openfonts(); if (!font) return fprintf(stderr, "cannot open font\n"), 1;
	xd = XftDrawCreate(D, W, vi.visual, wa.colormap); XftColorAllocValue(D, vi.visual, wa.colormap, &wc, &white);
	XSetSelectionOwner(D, A[OWNER], W, CurrentTime);
	XSelectInput(D, R, PropertyChangeMask | (daemon_ ? SubstructureNotifyMask : 0));
	if (daemon_) { Window r, p, *ch; unsigned n; if (XQueryTree(D, R, &r, &p, &ch, &n)) { for (unsigned i = 0; i < n; i++) cname(ch[i]); XFree(ch); } }
	else reveal(m);
	run();
}