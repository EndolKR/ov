/* ov.c — Win+Tab style overview for xfwm4 on X11 (xfwm4 compositor must be enabled)
 * build:  gcc -O2 -o ov.bin ov.c $(pkg-config --cflags --libs x11 xcomposite xrender xft xinerama xdamage)
 * usage:  ov.bin          toggle the overview (talks to the daemon if one is running, otherwise runs one-shot)
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

#define MAXW 256   /* max windows / cached pixmaps */
#define MAXD 32    /* max desktops */
#define GAP 16     /* spacing */
#define DH 180     /* desktop thumbnail height (strip height = DH + font height + 3*GAP) */
#define DESKLEFT 1 /* 1 = left-align the desktop strip, 0 = centre it */
#define SELW 1     /* thickness of the selected-window outline */
#define DESKW 1    /* thickness of the current-desktop outline */
#define FPS 60     /* max redraw rate for live thumbnail updates and fades */
#define FADEMS 150 /* outline fade in/out time */
#define BGA 0xa800 /* background opacity of the window area (0 = transparent, 0xffff = opaque black) */
#define TOPA 0xf400 /* background opacity of the desktop strip */
#define TH 220     /* window thumbnail height (constant; windows are never upscaled past 1:1) */
#define HOVERMS 0   /* hover time over a desktop before switching to it */
#define STRIP 26   /* title strip height */
#define ICON 18    /* icon size */
#define FONT "Fixed-10"
#define FX(x) ((XFixed)((x) * 65536))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIC(w) ((w)->c && (w)->c->pic ? (w)->c->pic : 0)

enum { STACK, CUR, NDESK, DNAMES, DESK, STATE, HIDDEN, SKIPT, TYPE, DOCK, DESKTOP, NAME, UTF8, ICONA, ACTIVE, CLOSE, ROOTPM, TOGGLE, OWNER, NATOMS };
char *atomnames[] = { "_NET_CLIENT_LIST_STACKING", "_NET_CURRENT_DESKTOP", "_NET_NUMBER_OF_DESKTOPS", "_NET_DESKTOP_NAMES", "_NET_WM_DESKTOP", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_NAME", "UTF8_STRING", "_NET_WM_ICON", "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_XROOTPMAP_ID", "_OVERVIEW_TOGGLE", "_OVERVIEW_OWNER" };

typedef struct { Window frame; Pixmap pix; Picture pic; Damage dmg; int w, h; } Cache;
typedef struct { Window client, frame; Cache *c; Picture icon; int iw, ih, fx, fy, fw, fh, desk, hidden, x, y, w, h, row; float oa; char title[160]; } Win;

Display *D; Window R, W; Atom A[NATOMS]; Picture P, half; XftFont *font; XftDraw *xd; XftColor white;
int S, sw, sh, mw, mh, shown, daemon_, dmgbase, dirty, closehov = -1, xw, pfrev, hoverd = -1, kb, ndesk, cur, nw, ns, nc, sel = -1, scroll, top, aw, ah, cont, dragi = -1, dragging, ox, oy, px, py, dtw, dth, dx0;
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

void cname(Window f) /* (re)name the frame's backing pixmap; only possible while viewable. The reference keeps the contents alive after unmap. */
{
	XWindowAttributes a; Window r; int x, y; unsigned w, h, b, d; Cache *c;
	if (!XGetWindowAttributes(D, f, &a) || a.map_state != IsViewable || a.override_redirect) return;
	Pixmap p = XCompositeNameWindowPixmap(D, f);
	if (!XGetGeometry(D, p, &r, &x, &y, &w, &h, &b, &d)) return;
	if (!(c = cfind(f))) { if (nc == MAXW) { XFreePixmap(D, p); return; } c = &cache[nc++]; *c = (Cache){f}; }
	cfree(c); c->pix = p; c->w = w; c->h = h; c->pic = XRenderCreatePicture(D, p, XRenderFindVisualFormat(D, a.visual), 0, 0);
	XRenderSetPictureFilter(D, c->pic, FilterBilinear, 0, 0); c->dmg = XDamageCreate(D, f, XDamageReportNonEmpty);
}

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

int pack(int h) /* greedy left-to-right rows at cell height h (= STRIP + thumbnail, never upscaled past 1:1); returns row count */
{
	int rows = 0, x = 0;
	for (int i = 0; i < ns; i++)
	{
		Win *w = &wins[show[i]]; int cw = PIC(w) ? w->c->w : w->fw, ch = PIC(w) ? w->c->h : w->fh; double s = MIN((double)(h - STRIP) / MAX(ch, 1), 1.0);
		w->w = MAX(cw * s, 1); w->h = MAX(ch * s, 1);
		if (x && x + w->w > aw) { x = 0; rows++; }
		w->row = rows; x += w->w + GAP;
	}
	return ns ? rows + 1 : 0;
}

void layout(void)
{
	dth = DH; dtw = DH * sw / sh; if (ndesk * (dtw + GAP) - GAP > mw - 2 * GAP) { dtw = (mw - (ndesk + 1) * GAP) / ndesk; dth = dtw * sh / sw; }
	dx0 = DESKLEFT ? GAP : (mw - ndesk * (dtw + GAP) + GAP) / 2; top = dth + font->height + 3 * GAP; aw = mw - 2 * GAP; ah = mh - top - GAP;
	int h = TH + STRIP, rows = pack(h);
	cont = rows * (h + GAP) - GAP + SELW; scroll = MAX(0, MIN(scroll, cont - ah));
	for (int i = 0, x = 0, r = -1; i < ns; i++) { Win *w = &wins[show[i]]; if (w->row != r) { r = w->row; x = GAP; } w->x = x; w->y = top + r * (h + GAP) - scroll; x += w->w + GAP; } /* top-left aligned */
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
		if (isdesk) { Window f = frameof(cw); if (!(wp = cfind(f))) { cname(f); wp = cfind(f); } }
		if (dock || isdesk) continue;
		Atom *st = (Atom*)prop(cw, A[STATE], XA_ATOM, &m); int skip = has(st, m, A[SKIPT]), hid = has(st, m, A[HIDDEN]); if (st) XFree(st);
		if (skip) continue;
		Win *w = &wins[nw]; *w = (Win){cw, frameof(cw)}; if (!w->frame) continue;
		unsigned long d = card(cw, A[DESK], cur); w->hidden = hid; w->desk = d == 0xFFFFFFFF ? -1 : (int)d;
		Window r; unsigned fw, fh, b, dp; if (!XGetGeometry(D, w->frame, &r, &w->fx, &w->fy, &fw, &fh, &b, &dp)) continue;
		w->fw = fw; w->fh = fh; if (!(w->c = cfind(w->frame))) { cname(w->frame); w->c = cfind(w->frame); }
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
	for (int h = 0; h < 2; h++) for (i = nw; i-- > 0;) if (wins[i].hidden == h && (wins[i].desk == cur || wins[i].desk < 0)) show[ns++] = i; /* alt-tab order: top of stack first, minimised last */
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

int fade(float *v, int on, long dt) { float t = on ? 1 : 0, d = (float)dt / FADEMS; *v = *v < t ? MIN(*v + d, t) : MAX(*v - d, t); return *v != t; } /* returns 1 while still animating */

void clip(int x, int y, int w, int h) { XRectangle r = {x, y, MAX(w, 0), MAX(h, 0)}; XRenderSetPictureClipRectangles(D, P, 0, 0, &r, 1); XftDrawSetClipRectangles(xd, 0, 0, &r, 1); }

void text(char *s, int x, int y) { XftDrawStringUtf8(xd, &white, font, x, y, (FcChar8*)s, strlen(s)); }

void drawdesk(int d, long dt)
{
	int x = dx0 + d * (dtw + GAP), y = GAP; double s = (double)dtw / sw; XGlyphInfo g;
	clip(x, y, dtw, dth); fill(x, y, dtw, dth, 0, 0, 0, 0xffff);
	if (wp && wp->pic) blit(wp->pic, wp->w, wp->h, s, x, y, 0);
	for (int i = 0; i < nw; i++) { Win *w = &wins[i]; if (!w->hidden && (w->desk == d || w->desk < 0) && PIC(w)) blit(PIC(w), w->c->w, w->c->h, s, x + w->fx * s, y + w->fy * s, 0); }
	clip(x - DESKW, y - DESKW, dtw + 2 * DESKW, dth + DESKW + GAP + font->height);
	dirty |= fade(&da[d], d == cur, dt); box(x, y, dtw, dth, DESKW, da[d]);
	XftTextExtentsUtf8(D, font, (FcChar8*)names[d], strlen(names[d]), &g); text(names[d], x + (dtw - (int)g.width) / 2, y + dth + GAP / 2 + font->ascent);
}

long now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

int hitclose(int i, int x, int y) { Win *w = i >= 0 ? &wins[show[i]] : 0; return w && x >= w->x + w->w - STRIP && x < w->x + w->w && y >= w->y && y < w->y + STRIP; }

void draw(void)
{
	if (!shown) return;
	long t = now(), dt = MIN(t - lastdraw, 1000 / FPS); lastdraw = t; dirty = 0; XGlyphInfo g; XftTextExtentsUtf8(D, font, (FcChar8*)"\xc3\x97", 2, &g); xw = g.width;
	XRenderColor bg = {0, 0, 0, BGA}, tb = {0, 0, 0, TOPA}; XRenderFillRectangle(D, PictOpSrc, P, &bg, 0, 0, mw, mh); XRenderFillRectangle(D, PictOpSrc, P, &tb, 0, 0, mw, top - GAP);
	for (int d = 0; d < ndesk; d++) drawdesk(d, dt);
	clip(0, top - SELW, mw, ah + SELW);
	for (int i = 0; i < ns; i++)
	{
		Win *w = &wins[show[i]]; if (w->y + w->h + STRIP < top || w->y > top + ah) continue;
		fill(w->x, w->y, w->w, STRIP, 0, 0, 0, 0xa000); /* title strip sits above the thumbnail */
		if (PIC(w)) blit(PIC(w), w->c->w, w->c->h, (double)w->w / w->c->w, w->x, w->y + STRIP, 0); else fill(w->x, w->y + STRIP, w->w, w->h, 0x3000, 0x3000, 0x3000, 0xffff);
		if (i == closehov) fill(w->x + w->w - STRIP, w->y, STRIP, STRIP, 0xc000, 0x1000, 0x1000, 0xffff);
		text("\xc3\x97", w->x + w->w - (STRIP + xw) / 2, w->y + (STRIP + font->ascent - font->descent) / 2); /* × */
		if (w->icon) blit(w->icon, w->iw, w->ih, (double)ICON / w->iw, w->x + 4, w->y + (STRIP - ICON) / 2, 0);
		clip(w->x + ICON + 8, top - SELW, w->w - ICON - 12 - STRIP, ah + SELW); text(w->title, w->x + ICON + 8, w->y + (STRIP + font->ascent - font->descent) / 2); clip(0, top - SELW, mw, ah + SELW);
		dirty |= fade(&w->oa, i == sel && !dragging, dt); box(w->x, w->y, w->w, w->h + STRIP, SELW, w->oa);
	}
	clip(0, 0, mw, mh);
	if (dragging) { Win *w = &wins[show[dragi]]; if (PIC(w)) blit(PIC(w), w->c->w, w->c->h, (double)w->w / w->c->w / 2, px - w->w / 4, py - w->h / 4, half); }
	XFlush(D);
}

int hitwin(int x, int y) { if (y < top || y > top + ah) return -1; for (int i = 0; i < ns; i++) { Win *w = &wins[show[i]]; if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h + STRIP) return i; } return -1; }
int hitdesk(int x, int y) { int d = (x - dx0) / (dtw + GAP); return y >= GAP && y < GAP + dth && x >= dx0 && d < ndesk && x - dx0 - d * (dtw + GAP) < dtw ? d : -1; }

void msg(Window w, Atom a, long d0, long d1)
{
	XEvent e = {ClientMessage}; e.xclient.window = w; e.xclient.message_type = a; e.xclient.format = 32; e.xclient.data.l[0] = d0; e.xclient.data.l[1] = d1;
	XSendEvent(D, R, 0, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

void hide(void) { XSetInputFocus(D, pfocus, pfrev, CurrentTime); XUnmapWindow(D, W); XFlush(D); shown = 0; if (!daemon_) exit(0); }

void activate(int i) { if (i >= 0 && i < ns) msg(wins[show[i]].client, A[ACTIVE], 2, CurrentTime); hide(); }

void reveal(void) /* fullscreen on the monitor under the pointer */
{
	int n, rx, ry, wx, wy, x = 0, y = 0; unsigned m; Window a, b; XineramaScreenInfo *si = XineramaQueryScreens(D, &n);
	mw = sw; mh = sh; XQueryPointer(D, R, &a, &b, &rx, &ry, &wx, &wy, &m);
	for (int i = 0; si && i < n; i++) if (rx >= si[i].x_org && rx < si[i].x_org + si[i].width && ry >= si[i].y_org && ry < si[i].y_org + si[i].height) { x = si[i].x_org; y = si[i].y_org; mw = si[i].width; mh = si[i].height; }
	if (si) XFree(si);
	XMoveResizeWindow(D, W, x, y, mw, mh); sel = -1; kb = 0; scroll = 0; dragi = -1; dragging = 0; closehov = -1; hoverd = -1; rebuild(); shown = 1; XMapRaised(D, W);
	XGetInputFocus(D, &pfocus, &pfrev); XSetInputFocus(D, W, RevertToPointerRoot, CurrentTime);
}

void key(KeySym k, unsigned st)
{
	Win *s = sel >= 0 ? &wins[show[sel]] : 0;
	if (k == XK_Escape) { hide(); return; }
	else if (k == XK_w && st & ControlMask) { if (s) msg(s->client, A[CLOSE], CurrentTime, 2); return; }
	else if (k == XK_Return || k == XK_space) { activate(sel); return; }
	else if (k >= XK_1 && k <= XK_9 && (int)(k - XK_1) < ndesk) { msg(R, A[CUR], k - XK_1, CurrentTime); hide(); return; }
	else if (k == XK_Right || k == XK_Tab) sel = ns ? (sel + 1) % ns : -1;
	else if (k == XK_Left) sel = ns ? (sel + ns - 1) % ns : -1;
	else if ((k == XK_Up || k == XK_Down) && !s && ns) sel = 0;
	else if (k == XK_Up || k == XK_Down)
	{
		int best = -1, bd = 1 << 30, cx = s->x + s->w / 2, tr = s->row + (k == XK_Down ? 1 : -1);
		for (int i = 0; i < ns; i++) { Win *w = &wins[show[i]]; int d = abs(w->x + w->w / 2 - cx); if (w->row == tr && d < bd) bd = d, best = i; }
		if (best >= 0) sel = best;
	}
	else return;
	kb = 1; /* keyboard navigation: from now on the outline stays put when the pointer leaves */
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
		else if ((d = hitdesk(e->x, e->y)) >= 0) { msg(R, A[CUR], d, CurrentTime); hide(); }
		else hide();
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
	if (dragging || (h >= 0 && h != sel) || (h < 0 && !kb && sel >= 0) || ch != closehov) { if (h >= 0 || !kb) sel = h; closehov = ch; draw(); }
}

void run(void)
{
	XEvent e; Cache *c; Atom a; int fd = ConnectionNumber(D);
	for (;;)
	{
		if (!XPending(D)) /* idle: block until an event arrives, or until the next frame is due if a thumbnail changed */
		{
			long t = now(), ms = 1L << 30;
			if (hoverd >= 0) { if (t >= hovert + HOVERMS) { if (hoverd != cur) msg(R, A[CUR], hoverd, CurrentTime); hoverd = -1; continue; } ms = hovert + HOVERMS - t; }
			if (dirty && shown) { if (t >= lastdraw + 1000 / FPS) { draw(); continue; } ms = MIN(ms, lastdraw + 1000 / FPS - t); }
			fd_set s; FD_ZERO(&s); FD_SET(fd, &s); struct timeval tv = {ms / 1000, ms % 1000 * 1000}; select(fd + 1, &s, 0, 0, ms < 1L << 30 ? &tv : 0); continue;
		}
		XNextEvent(D, &e);
		if (e.type == dmgbase + XDamageNotify) { XDamageSubtract(D, ((XDamageNotifyEvent*)&e)->damage, None, None); dirty = 1; continue; }
		switch (e.type)
		{
		case Expose: if (!e.xexpose.count) draw(); break;
		case KeyPress: key(XLookupKeysym(&e.xkey, 0), e.xkey.state); break;
		case ButtonPress: button(&e.xbutton); break;
		case ButtonRelease: release(&e.xbutton); break;
		case MotionNotify: while (XCheckTypedWindowEvent(D, W, MotionNotify, &e)); motion(&e.xmotion); break;
		case PropertyNotify:
			a = e.xproperty.atom;
			if (a == A[TOGGLE]) { if (shown) hide(); else reveal(); }
			else if (shown && (a == A[STACK] || a == A[CUR] || a == A[NDESK] || a == A[DNAMES] || a == A[DESK] || a == A[STATE])) { rebuild(); XSetInputFocus(D, W, RevertToPointerRoot, CurrentTime); draw(); }
			break;
		case MapNotify: cname(e.xmap.window); break;
		case ConfigureNotify: if ((c = cfind(e.xconfigure.window)) && (c->w != e.xconfigure.width || c->h != e.xconfigure.height)) cname(c->frame); break;
		case DestroyNotify: cdel(e.xdestroywindow.window); break;
		}
	}
}

int main(int argc, char **argv)
{
	daemon_ = argc > 1 && !strcmp(argv[1], "-d");
	if (!(D = XOpenDisplay(0))) return fprintf(stderr, "cannot open display\n"), 1;
	XSetErrorHandler(xerr); S = DefaultScreen(D); R = RootWindow(D, S); sw = DisplayWidth(D, S); sh = DisplayHeight(D, S);
	XInternAtoms(D, atomnames, NATOMS, 0, A); int ee; if (!XDamageQueryExtension(D, &dmgbase, &ee)) return fprintf(stderr, "no XDamage\n"), 1;
	Window o = XGetSelectionOwner(D, A[OWNER]);
	if (o && daemon_) return fprintf(stderr, "ov already running\n"), 1;
	if (o) { long v = 1; XChangeProperty(D, R, A[TOGGLE], XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&v, 1); XSync(D, 0); return 0; }
	XVisualInfo vi; if (!XMatchVisualInfo(D, S, 32, TrueColor, &vi)) return fprintf(stderr, "no ARGB visual (is the compositor on?)\n"), 1;
	XSetWindowAttributes wa = {.override_redirect = 1, .background_pixel = 0, .border_pixel = 0, .colormap = XCreateColormap(D, R, vi.visual, AllocNone), .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask};
	W = XCreateWindow(D, R, 0, 0, 1, 1, 0, 32, InputOutput, vi.visual, CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &wa);
	P = XRenderCreatePicture(D, W, XRenderFindVisualFormat(D, vi.visual), 0, 0);
	XRenderColor hc = {0, 0, 0, 0xa000}, wc = {0xffff, 0xffff, 0xffff, 0xffff}; half = XRenderCreateSolidFill(D, &hc);
	if (!(font = XftFontOpenName(D, S, FONT))) return fprintf(stderr, "cannot open font\n"), 1;
	xd = XftDrawCreate(D, W, vi.visual, wa.colormap); XftColorAllocValue(D, vi.visual, wa.colormap, &wc, &white);
	XSetSelectionOwner(D, A[OWNER], W, CurrentTime);
	XSelectInput(D, R, PropertyChangeMask | (daemon_ ? SubstructureNotifyMask : 0));
	if (daemon_) { Window r, p, *ch; unsigned n; if (XQueryTree(D, R, &r, &p, &ch, &n)) { for (unsigned i = 0; i < n; i++) cname(ch[i]); XFree(ch); } }
	else reveal();
	run();
}