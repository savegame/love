/**
 * AuroraOS/SailfishOS rotation + content-scaling presenter (impl).
 * See Presenter.h for design overview.
 */

#include "Presenter.h"

#ifdef LOVE_AURORAOS

#include "graphics/Graphics.h"
#include "graphics/Canvas.h"
#include "common/Matrix.h"
#include "common/Module.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace love
{
namespace auroraos
{

Presenter::Presenter() {}
Presenter::~Presenter() { destroyCanvas(); }

Presenter &Presenter::getInstance()
{
	static Presenter inst;
	return inst;
}

static OrientationPref parsePref(const char *s)
{
	if (!s) return ORIENT_PREF_ANY;
	if (!strcmp(s, "landscape"))         return ORIENT_PREF_LANDSCAPE;
	if (!strcmp(s, "landscapeflipped"))  return ORIENT_PREF_LANDSCAPE_FLIPPED;
	if (!strcmp(s, "portrait"))          return ORIENT_PREF_PORTRAIT;
	if (!strcmp(s, "portraitflipped"))   return ORIENT_PREF_PORTRAIT_FLIPPED;
	return ORIENT_PREF_ANY;
}

static ContentScale parseScale(const char *s)
{
	if (!s) return CONTENT_SCALE_FIT;
	if (!strcmp(s, "stretch")) return CONTENT_SCALE_STRETCH;
	if (!strcmp(s, "pixel"))   return CONTENT_SCALE_PIXEL;
	return CONTENT_SCALE_FIT;
}

void Presenter::setOrientationPref(const char *pref)
{
	orientPref = parsePref(pref);
	recompute();
	applyBufferTransform();
}

void Presenter::setContentScale(const char *mode)
{
	scale = parseScale(mode);
	recompute();
}

void Presenter::setupForWindow(SDL_Window *w, int requestedW, int requestedH)
{
	window = w;
	if (!window)
	{
		enabled = false;
		destroyCanvas();
		return;
	}

	displayIndex = SDL_GetWindowDisplayIndex(window);

	SDL_Rect db;
	if (SDL_GetDisplayBounds(displayIndex, &db) == 0)
	{
		displayW = db.w;
		displayH = db.h;
	}
	nativeLandscape = displayW > displayH;

	SDL_GetWindowSize(window, &windowW, &windowH);

	// If the caller passed (0, 0) (game asked for "use current screen"), pick
	// dimensions that fit the screen rather than a 1x1 canvas. Default to the
	// window's longer axis going to the game's longer axis.
	if (requestedW <= 0 || requestedH <= 0)
	{
		if (logicalW > 0 && logicalH > 0)
		{
			// Reuse the previous logical size — game probably wants no change.
		}
		else
		{
			logicalW = windowW;
			logicalH = windowH;
		}
	}
	else
	{
		logicalW = requestedW;
		logicalH = requestedH;
	}

	lastSdlOrientation = SDL_GetDisplayOrientation(displayIndex);

	SDL_Log("[AURORAOS] setupForWindow: requested=%dx%d -> logical=%dx%d displayIdx=%d displayBounds=%dx%d nativeLandscape=%d windowSize=%dx%d sdlOrient=%d",
		requestedW, requestedH, logicalW, logicalH, displayIndex, displayW, displayH, (int)nativeLandscape, windowW, windowH, lastSdlOrientation);

	// Force-unbind whatever canvas (ours or the user's) was active. After a
	// Window recreate the GL context is gone and any bound canvas is stale.
	// This also clears the graphics module's renderTargets StrongRef so
	// subsequent isCanvasActive() guards see a clean state.
	{
		auto *gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
		if (gfx && gfx->isCanvasActive())
		{
			reentry = true;
			gfx->setCanvas();
			reentry = false;
			SDL_Log("[AURORAOS] setupForWindow: force-unbound active canvas");
		}
	}

	// Drop existing canvas — its size or the rotation policy may have changed.
	destroyCanvas();

	recompute();
	applyBufferTransform();
}

void Presenter::onDisplayOrientation(int sdlOrientation)
{
	lastSdlOrientation = sdlOrientation;
	if (!window) return;
	recompute();
	applyBufferTransform();
}

void Presenter::onDisplayChanged()
{
	if (!window) return;
	int newIdx = SDL_GetWindowDisplayIndex(window);
	if (newIdx == displayIndex)
		return;
	displayIndex = newIdx;
	SDL_Rect db;
	if (SDL_GetDisplayBounds(displayIndex, &db) == 0)
	{
		displayW = db.w;
		displayH = db.h;
	}
	nativeLandscape = displayW > displayH;
	lastSdlOrientation = SDL_GetDisplayOrientation(displayIndex);
	SDL_GetWindowSize(window, &windowW, &windowH);
	recompute();
	applyBufferTransform();
}

// Decide whether logical content is landscape or portrait. Returns true if
// the visible (rotated) content should be landscape.
static bool wantLandscape(OrientationPref pref, int logicalW, int logicalH)
{
	switch (pref)
	{
	case ORIENT_PREF_LANDSCAPE:
	case ORIENT_PREF_LANDSCAPE_FLIPPED:
		return true;
	case ORIENT_PREF_PORTRAIT:
	case ORIENT_PREF_PORTRAIT_FLIPPED:
		return false;
	case ORIENT_PREF_ANY:
	default:
		return logicalW > logicalH;
	}
}

// Direct table from the legacy SailfishOS/AuroraOS Love port (empirically
// correct). Inputs: contentLandscape (game wants landscape), effSdl (the
// SDL_DisplayOrientation as if the panel were portrait-native). Outputs:
// rotationSteps (canvas draw rotation, 90deg CW per step) and wlTransform
// (WL_OUTPUT_TRANSFORM_* value sent to compositor).
//
// SDL values: 1=LANDSCAPE 2=LANDSCAPE_FLIPPED 3=PORTRAIT 4=PORTRAIT_FLIPPED
// WL values:  0=NORMAL    1=90               2=180     3=270
static void rotationTable(bool contentLandscape, int effSdl, int &rotSteps, int &wlt)
{
	if (contentLandscape)
	{
		switch (effSdl)
		{
		case 1: rotSteps = 3; wlt = 1; break; // LANDSCAPE        -> 270CW canvas + WL_90
		case 2: rotSteps = 1; wlt = 3; break; // LANDSCAPE_FLIPPED -> 90CW canvas + WL_270
		case 4: rotSteps = 3; wlt = 3; break; // PORTRAIT_FLIPPED -> 270CW + WL_270
		case 3:
		default: rotSteps = 1; wlt = 1; break; // PORTRAIT/UNKNOWN -> 90CW + WL_90
		}
	}
	else // portrait content
	{
		switch (effSdl)
		{
		case 2: rotSteps = 2; wlt = 2; break; // LANDSCAPE_FLIPPED -> 180 + WL_180
		case 4: rotSteps = 2; wlt = 2; break; // PORTRAIT_FLIPPED  -> 180 + WL_180
		case 1: rotSteps = 0; wlt = 0; break; // LANDSCAPE         -> none
		case 3:
		default: rotSteps = 0; wlt = 0; break;
		}
	}
}

void Presenter::recompute()
{
	if (!window)
		return;

	SDL_GetWindowSize(window, &windowW, &windowH);

	bool contentLandscape = wantLandscape(orientPref, logicalW, logicalH);

	// Re-map SDL orientation so the table above can be portrait-native only.
	// On a landscape-native panel SDL says LANDSCAPE when held naturally, but
	// from the rotation logic's perspective that's the same as PORTRAIT on a
	// portrait-native panel.
	int effSdl = lastSdlOrientation;
	if (nativeLandscape)
	{
		switch (effSdl) {
		case 1: effSdl = 3; break;
		case 3: effSdl = 1; break;
		case 2: effSdl = 4; break;
		case 4: effSdl = 2; break;
		default: break;
		}
	}

	int rs = 0, wlt = 0;
	rotationTable(contentLandscape, effSdl, rs, wlt);
	rotationSteps = rs;
	wlTransform = wlt;

	// Compute destination rect in the *visible* (post-rotation) coord system,
	// then we'll map through rotation at draw time.
	int visW = (rotationSteps & 1) ? windowH : windowW;
	int visH = (rotationSteps & 1) ? windowW : windowH;

	switch (scale)
	{
	case CONTENT_SCALE_STRETCH:
		dstX = 0; dstY = 0;
		dstW = (float)visW; dstH = (float)visH;
		break;
	case CONTENT_SCALE_PIXEL:
		dstW = (float)logicalW;
		dstH = (float)logicalH;
		dstX = (visW - dstW) * 0.5f;
		dstY = (visH - dstH) * 0.5f;
		break;
	case CONTENT_SCALE_FIT:
	default:
	{
		float sx = (float)visW / (float)logicalW;
		float sy = (float)visH / (float)logicalH;
		float s  = sx < sy ? sx : sy;
		dstW = logicalW * s;
		dstH = logicalH * s;
		dstX = (visW - dstW) * 0.5f;
		dstY = (visH - dstH) * 0.5f;
		break;
	}
	}

	enabled = true;

	SDL_Log("[AURORAOS] recompute: logical=%dx%d window=%dx%d contentLandscape=%d nativeLandscape=%d sdlOrient=%d(eff=%d) rotationSteps=%d wlTransform=%d dst=(%.1f,%.1f %.1fx%.1f) scale=%d orientPref=%d",
		logicalW, logicalH, windowW, windowH, (int)contentLandscape, (int)nativeLandscape,
		lastSdlOrientation, effSdl, rotationSteps, wlTransform, dstX, dstY, dstW, dstH, (int)scale, (int)orientPref);
}

void Presenter::applyBufferTransform()
{
	if (!window) return;

	SDL_SysWMinfo wm;
	SDL_VERSION(&wm.version);
	if (!SDL_GetWindowWMInfo(window, &wm))
		return;
	if (wm.subsystem != SDL_SYSWM_WAYLAND)
		return;

	// Tell the compositor about our buffer orientation so swipes/top-menu
	// edges align with the user's perception of "up".
	uint32_t t;
	switch (wlTransform)
	{
	case 0: t = WL_OUTPUT_TRANSFORM_NORMAL; break;
	case 1: t = WL_OUTPUT_TRANSFORM_90;     break;
	case 2: t = WL_OUTPUT_TRANSFORM_180;    break;
	case 3: t = WL_OUTPUT_TRANSFORM_270;    break;
	default: t = WL_OUTPUT_TRANSFORM_NORMAL; break;
	}
	wl_surface_set_buffer_transform(wm.info.wl.surface, t);
	SDL_Log("[AURORAOS] applyBufferTransform: wl_transform=%u (rotationSteps=%d)", t, rotationSteps);

	// SDL also looks at this hint for QtWayland integration.
	const char *h = "primary";
	switch (wlTransform)
	{
	case 0: h = "primary";   break;
	case 1: h = "landscape"; break;
	case 2: h = "inverted";  break;
	case 3: h = "inverted-landscape"; break;
	}
	SDL_SetHint(SDL_HINT_QTWAYLAND_CONTENT_ORIENTATION, h);
}

void Presenter::ensureCanvas(graphics::Graphics *gfx)
{
	if (internalCanvas)
	{
		if (internalCanvas->getPixelWidth() == logicalW &&
		    internalCanvas->getPixelHeight() == logicalH)
			return;
		destroyCanvas();
	}
	graphics::Canvas::Settings s;
	s.width = logicalW;
	s.height = logicalH;
	// Stencil depth — most games expect it on the default render target.
	internalCanvas = gfx->newCanvas(s);
}

void Presenter::destroyCanvas()
{
	if (!internalCanvas) return;

	// If our canvas is currently bound in the graphics module's render-target
	// state, unbind it first -- otherwise the graphics module's StrongRef
	// keeps the (now-stale) canvas alive and isCanvasActive() keeps returning
	// true for it, which breaks subsequent setMode/pump/etc. guards.
	auto *gfx = Module::getInstance<graphics::Graphics>(Module::M_GRAPHICS);
	if (gfx && isInternalCanvasBound(gfx))
	{
		reentry = true;
		gfx->setCanvas();
		reentry = false;
		SDL_Log("[AURORAOS] destroyCanvas: unbound from graphics state");
	}

	internalCanvas->release();
	internalCanvas = nullptr;
}

bool Presenter::isInternalCanvasBound(graphics::Graphics *gfx) const
{
	if (!internalCanvas) return false;
	auto rts = gfx->getCanvas();
	if (rts.colors.empty()) return false;
	return rts.colors[0].canvas == internalCanvas;
}

bool Presenter::interceptSetCanvasDefault(graphics::Graphics *gfx)
{
	if (!enabled || reentry) return false;
	ensureCanvas(gfx);
	if (!internalCanvas) return false;
	reentry = true;
	graphics::Graphics::RenderTarget rt(internalCanvas, 0, 0);
	gfx->setCanvas(rt, 0);
	reentry = false;
	SDL_Log("[AURORAOS] interceptSetCanvasDefault: substituted internalCanvas=%p", (void*)internalCanvas);
	return true;
}

void Presenter::ensureBound(graphics::Graphics *gfx)
{
	if (!enabled || reentry) return;
	ensureCanvas(gfx);
	if (!internalCanvas) { SDL_Log("[AURORAOS] ensureBound: no internalCanvas"); return; }
	if (isInternalCanvasBound(gfx)) return;
	// Don't steal an unrelated user canvas.
	if (gfx->isCanvasActive()) {
		SDL_Log("[AURORAOS] ensureBound: user canvas already bound, skipping");
		return;
	}
	reentry = true;
	graphics::Graphics::RenderTarget rt(internalCanvas, 0, 0);
	gfx->setCanvas(rt, 0);
	reentry = false;
	SDL_Log("[AURORAOS] ensureBound: bound internalCanvas=%p", (void*)internalCanvas);
}

bool Presenter::unbindIfOurs(graphics::Graphics *gfx)
{
	bool active = gfx ? gfx->isCanvasActive() : false;
	bool ours = isInternalCanvasBound(gfx);
	SDL_Log("[AURORAOS] unbindIfOurs: enabled=%d reentry=%d internalCanvas=%p isCanvasActive=%d isOurs=%d",
		(int)enabled, (int)reentry, (void*)internalCanvas, (int)active, (int)ours);
	if (!enabled || reentry) return false;
	if (!internalCanvas) return false;
	if (!ours) return false;
	reentry = true;
	gfx->setCanvas();
	reentry = false;
	SDL_Log("[AURORAOS] unbindIfOurs: unbound");
	return true;
}

void Presenter::beforePresent(graphics::Graphics *gfx)
{
	if (!enabled) return;
	if (!isInternalCanvasBound(gfx)) {
		static int skipLog = 0;
		if ((skipLog++ % 60) == 0)
			SDL_Log("[AURORAOS] beforePresent: skipped (internalCanvas not bound, %p)", (void*)internalCanvas);
		return;
	}
	static int frameLog = 0;
	if ((frameLog++ % 60) == 0)
		SDL_Log("[AURORAOS] beforePresent: rotationSteps=%d window=%dx%d dst=(%.1f,%.1f %.1fx%.1f)",
			rotationSteps, windowW, windowH, dstX, dstY, dstW, dstH);

	// Keep reentry set for the whole blit: setCanvas / origin / clear all run
	// through hooks that would otherwise re-attach the internal canvas.
	reentry = true;

	gfx->setCanvas(); // truly unbind — clear() / draw() below go to default FBO

	// Save state we mutate.
	gfx->push(graphics::Graphics::STACK_ALL);
	gfx->origin();
	gfx->setColor(Colorf(1, 1, 1, 1));

	graphics::OptionalColorf bg(Colorf(0, 0, 0, 1));
	OptionalInt    st;
	OptionalDouble dp;
	gfx->clear(bg, st, dp);

	// Build transform: rotate around origin, then translate so the rotated
	// dst rect lands at (dstX,dstY) in the post-rotation space.
	float angle = (float)(rotationSteps * (M_PI * 0.5));
	float sx = dstW / (float)logicalW;
	float sy = dstH / (float)logicalH;

	// Translation in window (pre-rotation) coords. Rotation moves the canvas
	// origin; we precompute the correct offset for each step.
	float tx = 0, ty = 0;
	switch (rotationSteps)
	{
	case 0: tx = dstX;             ty = dstY;             break;
	case 1: tx = (float)windowW - dstY; ty = dstX;        break;
	case 2: tx = (float)windowW - dstX; ty = (float)windowH - dstY; break;
	case 3: tx = dstY;             ty = (float)windowH - dstX; break;
	}

	Matrix4 m(tx, ty, angle, sx, sy, 0, 0, 0, 0);
	gfx->draw(internalCanvas, m);

	gfx->pop();

	reentry = false;
}

// ---------------------------------------------------------------------------
// Coordinate conversion: SDL gives us (x, y) in *window* pixel space, which on
// AuroraOS is the un-rotated compositor surface. Our visible content lives in
// the rotated dst rect. Map: window px -> post-rotation visible px -> subtract
// dst rect offset -> divide by scale -> logical canvas coord.
// ---------------------------------------------------------------------------

void Presenter::convertCoords(double &x, double &y) const
{
	if (!enabled) return;

	double vx = x, vy = y;
	switch (rotationSteps)
	{
	case 0: vx = x;            vy = y;            break;
	case 1: vx = y;            vy = windowW - x;  break;
	case 2: vx = windowW - x;  vy = windowH - y;  break;
	case 3: vx = windowH - y;  vy = x;            break;
	}

	float sx = dstW > 0 ? (float)logicalW / dstW : 1.0f;
	float sy = dstH > 0 ? (float)logicalH / dstH : 1.0f;

	x = (vx - dstX) * sx;
	y = (vy - dstY) * sy;
}

void Presenter::convertDelta(double &dx, double &dy) const
{
	if (!enabled) return;

	double vx = dx, vy = dy;
	switch (rotationSteps)
	{
	case 0: vx = dx;   vy = dy;   break;
	case 1: vx = dy;   vy = -dx;  break;
	case 2: vx = -dx;  vy = -dy;  break;
	case 3: vx = -dy;  vy = dx;   break;
	}

	float sx = dstW > 0 ? (float)logicalW / dstW : 1.0f;
	float sy = dstH > 0 ? (float)logicalH / dstH : 1.0f;

	dx = vx * sx;
	dy = vy * sy;
}

} // auroraos
} // love

#endif // LOVE_AURORAOS
