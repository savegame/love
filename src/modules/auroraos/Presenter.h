/**
 * AuroraOS/SailfishOS rotation + content-scaling presenter.
 *
 * Owns an offscreen Canvas (logical game size). Every frame, user code draws
 * into this canvas; right before present we blit it to the default framebuffer
 * applying rotation (to compensate for native portrait/landscape) and a
 * scaling policy (fit/stretch/pixel).
 *
 * Touch and mouse coordinates from SDL arrive in native (un-rotated) window
 * space; convertCoords/convertDelta map them into the logical canvas space.
 *
 * Only compiled when LOVE_AURORAOS is defined.
 */

#ifndef LOVE_AURORAOS_PRESENTER_H
#define LOVE_AURORAOS_PRESENTER_H

#include "common/config.h"

#ifdef LOVE_AURORAOS

#include <SDL_video.h>

namespace love
{
namespace graphics
{
class Graphics;
class Canvas;
}

namespace auroraos
{

enum ContentScale
{
	// Default. Aspect-correct, no letterbox: the SHORTER axis of the canvas
	// matches the user's setMode request; the LONGER axis is expanded so the
	// canvas fills the entire window. Game's love.graphics.getWidth/Height
	// reports the expanded size (so game code positions things correctly).
	CONTENT_SCALE_EXPAND,
	// Aspect-correct with letterbox. Canvas equals the requested size; black
	// bars fill the rest of the window.
	CONTENT_SCALE_FIT,
	// Canvas equals requested; drawn to the entire window, deforming aspect.
	CONTENT_SCALE_STRETCH,
	// Canvas equals requested; drawn 1:1, centered. Letterbox or clip if
	// requested size doesn't match the window.
	CONTENT_SCALE_PIXEL,
};

enum OrientationPref
{
	ORIENT_PREF_ANY,
	ORIENT_PREF_LANDSCAPE,
	ORIENT_PREF_LANDSCAPE_FLIPPED,
	ORIENT_PREF_PORTRAIT,
	ORIENT_PREF_PORTRAIT_FLIPPED,
};

class Presenter
{
public:
	static Presenter &getInstance();

	// Called from Window::setWindow after SDL window is created (or recreated).
	// requestedW/requestedH are the logical size requested by setMode().
	void setupForWindow(SDL_Window *window, int requestedW, int requestedH);

	// Lua-side config (called from boot.lua via love._auroraosSetup).
	void setOrientationPref(const char *pref);
	void setContentScale(const char *mode);

	// SDL event hooks (called from Event::convert).
	void onDisplayOrientation(int sdlOrientation);
	void onDisplayChanged();

	// Present-time hooks (called from Graphics::present).
	void beforePresent(graphics::Graphics *gfx);

	// Frame-start hook: bind the internal canvas if not already bound.
	// Called from Graphics::origin() / Graphics::clear() so that the canvas
	// is only active during actual drawing -- not across event.pump().
	void ensureBound(graphics::Graphics *gfx);

	// If the internal canvas is the active canvas, unbind it (no-op otherwise).
	// Called from sites that throw "Canvas active" guards (pump/setMode/...).
	// Returns true if it actually unbound something.
	bool unbindIfOurs(graphics::Graphics *gfx);

	// Returns true if it handled the default-canvas request.
	bool interceptSetCanvasDefault(graphics::Graphics *gfx);

	// Coord conversion for touch/mouse (window pixels -> logical canvas).
	void convertCoords(double &x, double &y) const;
	void convertDelta(double &dx, double &dy) const;

	// Logical (canvas) dimensions for getWidth/getHeight overrides.
	int getLogicalWidth() const  { return logicalW; }
	int getLogicalHeight() const { return logicalH; }

	bool isEnabled() const { return enabled; }

private:
	Presenter();
	~Presenter();
	Presenter(const Presenter &) = delete;
	Presenter &operator=(const Presenter &) = delete;

	// True when our canvas should be transparent to the user (rotation needed
	// OR content scale is not stretch-to-window 1:1). Recomputed on every
	// orientation/display change.
	bool enabled = false;

	SDL_Window *window = nullptr;
	int displayIndex = 0;

	// Native display dims as reported by SDL_GetDisplayBounds (physical).
	int displayW = 0;
	int displayH = 0;
	bool nativeLandscape = false;

	// Window dims after creation (what SDL says).
	int windowW = 0;
	int windowH = 0;

	// Game's setMode request (the size the game asked for). Used as the base
	// for expand-mode canvas computation and as the actual canvas dims for
	// fit/stretch/pixel modes.
	int requestedW = 0;
	int requestedH = 0;

	// Effective canvas dimensions. Equal to requestedW/H for fit/stretch/pixel
	// modes; expanded to match the window aspect for expand mode.
	int logicalW = 1;
	int logicalH = 1;

	// Computed: rotation we apply to the canvas at blit-time, in 90deg steps
	// (0/1/2/3, CW). This is what the visual content needs to be rotated by
	// for it to appear upright to a user holding the device per lastSdlOrientation.
	int rotationSteps = 0;

	// Computed: wl_surface_set_buffer_transform value we send to the compositor.
	// In general NOT the same as rotationSteps -- it tells the compositor how
	// our buffer is oriented relative to the panel, which is also a function of
	// the panel's native orientation. (WL_OUTPUT_TRANSFORM_NORMAL=0, 90=1,
	// 180=2, 270=3.)
	int wlTransform = 0;

	// Computed destination rect on the actual window/backbuffer (post-rotation
	// composer space — i.e. in the visible content's coord system).
	float dstX = 0, dstY = 0, dstW = 0, dstH = 0;

	// Content scaling policy.
	ContentScale scale = CONTENT_SCALE_EXPAND;

	// Orientation request.
	OrientationPref orientPref = ORIENT_PREF_ANY;
	int lastSdlOrientation = 0; // SDL_ORIENTATION_UNKNOWN initially

	// Offscreen canvas the game renders into.
	graphics::Canvas *internalCanvas = nullptr;

	bool reentry = false;

	void recompute();
	void applyBufferTransform();
	void ensureCanvas(graphics::Graphics *gfx);
	void destroyCanvas();
	bool isInternalCanvasBound(graphics::Graphics *gfx) const;
};

} // auroraos
} // love

#endif // LOVE_AURORAOS
#endif // LOVE_AURORAOS_PRESENTER_H
