/**
 * AuroraOS/SailfishOS rotation + content-scaling presenter (impl).
 * See Presenter.h for design overview.
 */

#include "Presenter.h"

#ifdef LOVE_AURORAOS

#include "graphics/Graphics.h"
#include "graphics/Canvas.h"
#include "common/Matrix.h"

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

	logicalW = requestedW > 0 ? requestedW : 1;
	logicalH = requestedH > 0 ? requestedH : 1;

	displayIndex = SDL_GetWindowDisplayIndex(window);

	SDL_Rect db;
	if (SDL_GetDisplayBounds(displayIndex, &db) == 0)
	{
		displayW = db.w;
		displayH = db.h;
	}
	nativeLandscape = displayW > displayH;

	SDL_GetWindowSize(window, &windowW, &windowH);

	lastSdlOrientation = SDL_GetDisplayOrientation(displayIndex);

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

void Presenter::recompute()
{
	if (!window)
		return;

	SDL_GetWindowSize(window, &windowW, &windowH);

	bool contentLandscape = wantLandscape(orientPref, logicalW, logicalH);

	// Determine rotation. The compositor reports SDL orientation relative to
	// the device's natural orientation. We must end up with content visually
	// matching the user's hold direction.
	//
	// Strategy: compute a rotation in 90deg steps such that after rotating our
	// canvas, its width axis is along the visible-content "width" direction.

	// "Window axis is landscape" iff windowW > windowH (post-transform compositor
	// hands us a backbuffer of native size — windowW/H reflect that).
	bool windowAxisLandscape = windowW > windowH;

	// If content orientation matches window axis, no rotation needed (just
	// fit/stretch into window). Otherwise rotate 90 degrees.
	int baseSteps = (contentLandscape == windowAxisLandscape) ? 0 : 1;

	// Flip variants add 180.
	bool flipped = (orientPref == ORIENT_PREF_LANDSCAPE_FLIPPED ||
	                orientPref == ORIENT_PREF_PORTRAIT_FLIPPED);

	// SDL_DisplayOrientation provides hardware-side hint; for "any" follow it.
	if (orientPref == ORIENT_PREF_ANY)
	{
		// SDL_ORIENTATION_LANDSCAPE_FLIPPED=2, PORTRAIT_FLIPPED=4
		if (lastSdlOrientation == 2 || lastSdlOrientation == 4)
			flipped = true;
	}

	rotationSteps = (baseSteps + (flipped ? 2 : 0)) & 3;

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

	// We're "enabled" — always, on AuroraOS, since fullscreen surface and
	// rotation/scale handling is a constant requirement.
	enabled = true;
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
	switch (rotationSteps)
	{
	case 0: t = WL_OUTPUT_TRANSFORM_NORMAL; break;
	case 1: t = WL_OUTPUT_TRANSFORM_90;     break;
	case 2: t = WL_OUTPUT_TRANSFORM_180;    break;
	case 3: t = WL_OUTPUT_TRANSFORM_270;    break;
	default: t = WL_OUTPUT_TRANSFORM_NORMAL; break;
	}
	wl_surface_set_buffer_transform(wm.info.wl.surface, t);

	// SDL also looks at this hint for QtWayland integration.
	const char *h = "primary";
	switch (rotationSteps)
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
	if (internalCanvas)
	{
		internalCanvas->release();
		internalCanvas = nullptr;
	}
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
	return true;
}

void Presenter::ensureBound(graphics::Graphics *gfx)
{
	if (!enabled || reentry) return;
	ensureCanvas(gfx);
	if (!internalCanvas) return;
	if (isInternalCanvasBound(gfx)) return;
	// Don't steal an unrelated user canvas.
	if (gfx->isCanvasActive()) return;
	reentry = true;
	graphics::Graphics::RenderTarget rt(internalCanvas, 0, 0);
	gfx->setCanvas(rt, 0);
	reentry = false;
}

void Presenter::beforePresent(graphics::Graphics *gfx)
{
	if (!enabled) return;
	if (!isInternalCanvasBound(gfx)) return;

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
