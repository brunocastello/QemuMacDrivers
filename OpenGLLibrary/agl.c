/* AGL 1.2 on top of the DG client: one pixel format, one renderer (reported
 * as accelerated), windowed drawables presented by reading the host's
 * drawable back straight into the window's area of the screen.
 *
 * Written from the public AGL API; the wire sequence is the one GLHello
 * proved: DG_HELLO (once), DG_CREATE, DG_RESIZE, GL calls, DG_READ_DRAWABLE,
 * DG_DROP_DRAWABLE, DG_DESTROY. Context make-current is client-side only.
 */
#include <MacTypes.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <string.h>
#include "agl.h"
#include "dg_client.h"
#include "dg_transport.h"
#include "direct-layout.h"		/* DG_LAYOUT, generated */

/* GL's origin is bottom-left: the executor returns drawable rows bottom-up
 * and the device flips them while writing the screen. If frames ever show up
 * upside down, this is the only switch. */
#define PRESENT_FLAGS (PV_FLAG_SWAP32 | PV_FLAG_FLIP)

#define AGL_RENDERER_ID_VALUE 0x00028000	/* fixed, unused by Apple's renderers */

struct __AGLPixelFormatRec { int depth, stencil; };
struct __AGLRendererInfoRec { int unused; };
struct __AGLContextRec {
	struct DGContext dg;			/* first: dg_current points here */
	AGLDrawable port;
	int width, height;
	uint32_t drawable, nextDrawable;
	GLint swapInterval;
	GLint enabled[8];			/* last AGL_ enable state, by (pname - 200) % 8 */
};

static struct __AGLPixelFormatRec gFormat = { 24, 0 };
static struct __AGLRendererInfoRec gRenderer;
static int gHello;			/* 1 done, -1 failed, 0 not yet */
static GLenum gAglError;

static int SetError(GLenum e) { gAglError = e; return 0; }

/* --- connection (DG_HELLO once per library) */
static int Hello(void)
{
	struct DGContext boot;
	struct DGPacket p;
	struct DGReply reply;
	struct DGHelloFeatures f;

	if (gHello)
		return gHello > 0 ? 0 : -1;
	gHello = -1;
	if (PvmacOpen())
		return -1;
	memset(&boot, 0, sizeof boot);
	boot.connection = &dg_connection;
	p = DGPacketNew(&boot, DG_HELLO);
	p.message.args[0] = DG_MAGIC;
	p.message.args[1] = DG_VERSION;
	p.message.args[3] = 0x4d4f5339;		/* "MOS9": resource namespace, stable per boot */
	p.message.args[5] = DG_HELLO_FEATURES_MAGIC;
	p.message.args[6] = DG_FEATURE_LARGE_IOSURFACE_WRITES;
	DGPacketPointerW(&p, 2, DG_LAYOUT, sizeof DG_LAYOUT, 0, 1);
	reply = DGSubmit(&boot, &p, 1);
	if (!p.valid_reply) {
		DGLog("DG_HELLO failed: error %lu (layout %s)", (unsigned long)reply.error, DG_LAYOUT);
		DGPacketFree(&p);
		return -1;
	}
	dg_connection.capabilities = reply.result;
	memset(&f, 0, sizeof f);
	DGPacketCopyW(&p, 2, &f, sizeof f, 4);
	if (f.magic == DG_HELLO_FEATURES_MAGIC) {
		dg_connection.features = f.features;
		if (f.magic2 == DG_HELLO_FEATURES2_MAGIC)
			dg_connection.features2 = f.features2;
		if (f.magic3 == DG_HELLO_FEATURES3_MAGIC)
			dg_connection.features3 = f.features3;
	}
	DGPacketFree(&p);
	DGLog("DG_HELLO ok: capabilities %lx%08lx features %lx", (unsigned long)(dg_connection.capabilities >> 32),
		(unsigned long)dg_connection.capabilities, (unsigned long)dg_connection.features);
	gHello = 1;
	return 0;
}

/* One message with a reply and no payload (DG_CREATE, DG_RESIZE, ...). */
static int Simple(struct DGContext *c, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, struct DGReply *reply)
{
	struct DGPacket p = DGPacketNew(c, op);

	p.message.args[0] = a0;
	p.message.args[1] = a1;
	p.message.args[2] = a2;
	*reply = DGSubmit(c, &p, 1);
	DGPacketFree(&p);
	return reply->error || dg_connection.dead ? -1 : 0;
}

void aglGetVersion(GLint *major, GLint *minor)
{
	if (major)
		*major = 1;
	if (minor)
		*minor = 2;
}

/* --- pixel format and renderer */
AGLPixelFormat aglChoosePixelFormat(const AGLDevice *gdevs, GLint ndev, const GLint *attribs)
{
	const GLint *a = attribs;
	int depth = 0, stencil = 0;

	(void)gdevs;
	(void)ndev;
	if (PvmacOpen()) {
		SetError(AGL_BAD_PIXELFMT);
		return NULL;
	}
	while (a && *a != AGL_NONE) {
		GLint attr = *a++, value = 0;

		switch (attr) {
		case AGL_BUFFER_SIZE: case AGL_LEVEL: case AGL_AUX_BUFFERS:
		case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: case AGL_ALPHA_SIZE:
		case AGL_DEPTH_SIZE: case AGL_STENCIL_SIZE:
		case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE: case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE:
		case AGL_PIXEL_SIZE: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
		case AGL_RENDERER_ID: case AGL_VIRTUAL_SCREEN:
			value = *a++;
			break;
		default:
			break;
		}
		switch (attr) {
		case AGL_STEREO: case AGL_OFFSCREEN: case AGL_PBUFFER:
			SetError(AGL_BAD_ATTRIBUTE);
			return NULL;
		case AGL_AUX_BUFFERS: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
		case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE: case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE:
			if (value > 0) {
				SetError(AGL_BAD_ATTRIBUTE);
				return NULL;
			}
			break;
		case AGL_RENDERER_ID:
			if (value != AGL_RENDERER_ID_VALUE) {
				SetError(AGL_BAD_MATCH);
				return NULL;
			}
			break;
		case AGL_DEPTH_SIZE: depth = value; break;
		case AGL_STENCIL_SIZE: stencil = value; break;
		default:
			break;
		}
	}
	gFormat.depth = depth > 16 ? 24 : (depth ? 16 : 24);
	gFormat.stencil = stencil ? 8 : 0;
	return &gFormat;
}

void aglDestroyPixelFormat(AGLPixelFormat pix) { (void)pix; }

GLboolean aglDescribePixelFormat(AGLPixelFormat pix, GLint attrib, GLint *value)
{
	GLint v = 0;

	if (!pix || !value)
		return SetError(AGL_BAD_PIXELFMT);
	switch (attrib) {
	case AGL_RGBA: case AGL_DOUBLEBUFFER: case AGL_ACCELERATED: case AGL_NO_RECOVERY:
	case AGL_WINDOW: case AGL_COMPLIANT: case AGL_BACKING_STORE:
		v = 1; break;
	case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE: v = 32; break;
	case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: case AGL_ALPHA_SIZE: v = 8; break;
	case AGL_DEPTH_SIZE: v = pix->depth; break;
	case AGL_STENCIL_SIZE: v = pix->stencil; break;
	case AGL_RENDERER_ID: v = AGL_RENDERER_ID_VALUE; break;
	case AGL_LEVEL: case AGL_STEREO: case AGL_AUX_BUFFERS: case AGL_ACCUM_RED_SIZE:
	case AGL_ACCUM_GREEN_SIZE: case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE:
	case AGL_OFFSCREEN: case AGL_FULLSCREEN: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
	case AGL_ROBUST: case AGL_MP_SAFE: case AGL_MULTISCREEN: case AGL_PBUFFER: case AGL_VIRTUAL_SCREEN:
		v = 0; break;
	default:
		return SetError(AGL_BAD_ATTRIBUTE);
	}
	*value = v;
	return GL_TRUE;
}

AGLRendererInfo aglQueryRendererInfo(const AGLDevice *gdevs, GLint ndev)
{
	(void)gdevs;
	(void)ndev;
	if (PvmacOpen()) {
		SetError(AGL_BAD_RENDINFO);
		return NULL;
	}
	return &gRenderer;
}

void aglDestroyRendererInfo(AGLRendererInfo rend) { (void)rend; }

/* One renderer only: the list ends after the first. */
AGLRendererInfo aglNextRendererInfo(AGLRendererInfo rend)
{
	(void)rend;
	return NULL;
}

GLboolean aglDescribeRenderer(AGLRendererInfo rend, GLint prop, GLint *value)
{
	GLint v = 0;

	if (!rend || !value)
		return SetError(AGL_BAD_RENDINFO);
	switch (prop) {
	case AGL_RENDERER_ID: v = AGL_RENDERER_ID_VALUE; break;
	case AGL_ACCELERATED: case AGL_WINDOW: case AGL_COMPLIANT: case AGL_ROBUST: v = 1; break;
	case AGL_BUFFER_MODES: v = AGL_SINGLEBUFFER_BIT | AGL_DOUBLEBUFFER_BIT | AGL_MONOSCOPIC_BIT; break;
	case AGL_COLOR_MODES: v = AGL_RGB888_BIT | AGL_ARGB8888_BIT; break;
	case AGL_DEPTH_MODES: v = AGL_16_BIT | AGL_24_BIT; break;
	case AGL_STENCIL_MODES: v = AGL_0_BIT | AGL_8_BIT; break;
	case AGL_ACCUM_MODES: v = AGL_0_BIT; break;
	case AGL_MAX_AUX_BUFFERS: case AGL_MIN_LEVEL: case AGL_MAX_LEVEL: case AGL_FULLSCREEN:
	case AGL_OFFSCREEN: case AGL_PBUFFER: case AGL_MULTISCREEN: case AGL_STEREO:
		v = 0; break;
	case AGL_VIDEO_MEMORY: case AGL_TEXTURE_MEMORY: v = 64 * 1024 * 1024; break;
	default:
		return SetError(AGL_BAD_PROPERTY);
	}
	*value = v;
	return GL_TRUE;
}

/* --- contexts and drawables */
AGLContext aglCreateContext(AGLPixelFormat pix, AGLContext share)
{
	struct __AGLContextRec *c;
	struct DGReply reply;

	if (!pix) {
		SetError(AGL_BAD_PIXELFMT);
		return NULL;
	}
	if (Hello()) {
		SetError(AGL_BAD_CONNECTION);
		return NULL;
	}
	c = (struct __AGLContextRec *)NewPtrClear(sizeof *c);
	if (!c) {
		SetError(AGL_BAD_ALLOC);
		return NULL;
	}
	c->dg.connection = &dg_connection;
	c->dg.alignment[0] = c->dg.alignment[1] = 4;
	c->dg.profile = 0x1000;
	if (Simple(&c->dg, DG_CREATE, share ? share->dg.id : 0, 0, 0, &reply) || !reply.result) {
		DGLog("DG_CREATE failed: error %lu", (unsigned long)reply.error);
		DisposePtr((Ptr)c);
		SetError(AGL_BAD_CONTEXT);
		return NULL;
	}
	c->dg.id = (uint32_t)reply.result;
	return c;
}

static void DropDrawable(struct __AGLContextRec *c)
{
	struct DGReply reply;

	if (c->drawable)
		Simple(&c->dg, DG_DROP_DRAWABLE, c->drawable, 0, 0, &reply);
	c->drawable = 0;
	c->width = c->height = 0;
}

GLboolean aglDestroyContext(AGLContext ctx)
{
	struct DGReply reply;

	DGTraceDump();
	if (!ctx)
		return SetError(AGL_BAD_CONTEXT);
	DropDrawable(ctx);
	Simple(&ctx->dg, DG_DESTROY, 0, 0, 0, &reply);
	if (dg_current == &ctx->dg)
		dg_current = NULL;
	DisposePtr((Ptr)ctx);
	return GL_TRUE;
}

GLboolean aglSetCurrentContext(AGLContext ctx)
{
	dg_current = ctx ? &ctx->dg : NULL;
	return GL_TRUE;
}

/* dg is the first member of the context record, so the pointer maps back. */
AGLContext aglGetCurrentContext(void)
{
	return (AGLContext)dg_current;
}

GLenum aglGetError(void)
{
	GLenum e = gAglError;

	gAglError = 0;			/* AGL_NO_ERROR */
	return e;
}

const GLubyte *aglErrorString(GLenum code)
{
	switch (code) {
	case 0: return (const GLubyte *)"no error";
	case AGL_BAD_ATTRIBUTE: return (const GLubyte *)"invalid pixel format attribute";
	case AGL_BAD_PROPERTY: return (const GLubyte *)"invalid renderer property";
	case AGL_BAD_PIXELFMT: return (const GLubyte *)"invalid pixel format";
	case AGL_BAD_RENDINFO: return (const GLubyte *)"invalid renderer info";
	case AGL_BAD_CONTEXT: return (const GLubyte *)"invalid context";
	case AGL_BAD_DRAWABLE: return (const GLubyte *)"invalid drawable";
	case AGL_BAD_GDEV: return (const GLubyte *)"invalid graphics device";
	case AGL_BAD_STATE: return (const GLubyte *)"invalid context state";
	case AGL_BAD_VALUE: return (const GLubyte *)"invalid numerical value";
	case AGL_BAD_MATCH: return (const GLubyte *)"invalid share context";
	case AGL_BAD_ENUM: return (const GLubyte *)"invalid enumerant";
	case AGL_BAD_OFFSCREEN: return (const GLubyte *)"invalid offscreen drawable";
	case AGL_BAD_FULLSCREEN: return (const GLubyte *)"invalid fullscreen drawable";
	case AGL_BAD_WINDOW: return (const GLubyte *)"invalid window";
	case AGL_BAD_POINTER: return (const GLubyte *)"invalid pointer";
	case AGL_BAD_MODULE: return (const GLubyte *)"invalid code module";
	case AGL_BAD_ALLOC: return (const GLubyte *)"memory allocation failure";
	case AGL_BAD_CONNECTION: return (const GLubyte *)"invalid CoreGraphics connection";
	default: return (const GLubyte *)"unknown error";
	}
}

/* Port content size in pixels. */
static int AglPortSize(AGLDrawable port, int *w, int *h)
{
	Rect r;

	if (!port)
		return -1;
	r = port->portRect;
	*w = r.right - r.left;
	*h = r.bottom - r.top;
	return *w >= 1 && *h >= 1 && *w <= 8192 && *h <= 8192 ? 0 : -1;
}

/* Tell the executor the drawable size: a new drawable id per size, the old
 * one dropped afterwards (VIM's DGMakeCurrentMode does the same). */
static GLboolean ApplyDrawable(struct __AGLContextRec *c, int force)
{
	struct DGReply reply;
	uint32_t old = c->drawable, id;
	int w, h;

	if (AglPortSize(c->port, &w, &h))
		return SetError(AGL_BAD_DRAWABLE);
	if (!force && old && w == c->width && h == c->height)
		return GL_TRUE;
	id = ++c->nextDrawable;
	if (Simple(&c->dg, DG_RESIZE, w, h, id, &reply)) {
		DGLog("DG_RESIZE %dx%d failed: error %lu", w, h, (unsigned long)reply.error);
		return SetError(AGL_BAD_DRAWABLE);
	}
	if (old)
		Simple(&c->dg, DG_DROP_DRAWABLE, old, 0, 0, &reply);
	c->drawable = id;
	c->width = w;
	c->height = h;
	return GL_TRUE;
}

GLboolean aglSetDrawable(AGLContext ctx, AGLDrawable draw)
{
	if (!ctx)
		return SetError(AGL_BAD_CONTEXT);
	if (!draw) {
		DropDrawable(ctx);
		ctx->port = NULL;
		return GL_TRUE;
	}
	ctx->port = draw;
	return ApplyDrawable(ctx, 1);
}

GLboolean aglUpdateContext(AGLContext ctx)
{
	if (!ctx)
		return SetError(AGL_BAD_CONTEXT);
	if (!ctx->port)
		return GL_TRUE;
	return ApplyDrawable(ctx, 0);
}

/* Full screen is not supported: Apple's AGL takes over a display here. A
 * program should fall back to a drawable (windowed, or a DrawSprocket
 * front-buffer port through aglSetDrawable, which this library presents). */
GLboolean aglSetFullScreen(AGLContext ctx, GLsizei width, GLsizei height, GLsizei freq, GLint device)
{
	(void)ctx;
	(void)freq;
	(void)device;
	DGLog("aglSetFullScreen(%d x %d) refused: use aglSetDrawable", (int)width, (int)height);
	return SetError(AGL_BAD_FULLSCREEN);
}

static const GLint kEnableKnown[] = {
	AGL_SWAP_RECT, AGL_BUFFER_RECT, AGL_SWAP_LIMIT, AGL_COLORMAP_TRACKING, AGL_COLORMAP_ENTRY,
	AGL_RASTERIZATION, AGL_SWAP_INTERVAL, AGL_STATE_VALIDATION, AGL_BUFFER_NAME,
	AGL_ORDER_CONTEXT_TO_FRONT, AGL_CONTEXT_SURFACE_ID, AGL_CONTEXT_DISPLAY_ID, AGL_SURFACE_ORDER,
	AGL_SURFACE_OPACITY, AGL_CLIP_REGION, AGL_FS_CAPTURE_SINGLE, 0
};

static int KnownOption(GLenum pname)
{
	int i;

	for (i = 0; kEnableKnown[i]; i++)
		if ((GLenum)kEnableKnown[i] == pname)
			return 1;
	return 0;
}

GLboolean aglEnable(AGLContext ctx, GLenum pname)
{
	if (!ctx)
		return SetError(AGL_BAD_CONTEXT);
	if (!KnownOption(pname))
		return SetError(AGL_BAD_ENUM);
	ctx->enabled[pname & 7] = 1;
	return GL_TRUE;
}

GLboolean aglDisable(AGLContext ctx, GLenum pname)
{
	if (!ctx)
		return SetError(AGL_BAD_CONTEXT);
	if (!KnownOption(pname))
		return SetError(AGL_BAD_ENUM);
	ctx->enabled[pname & 7] = 0;
	return GL_TRUE;
}

/* The values are stored or ignored: frames are always presented at swap. */
GLboolean aglSetInteger(AGLContext ctx, GLenum pname, const GLint *params)
{
	if (!ctx)
		return SetError(AGL_BAD_CONTEXT);
	if (!KnownOption(pname))
		return SetError(AGL_BAD_ENUM);
	if (!params)
		return SetError(AGL_BAD_POINTER);
	if (pname == AGL_SWAP_INTERVAL)
		ctx->swapInterval = params[0];
	return GL_TRUE;
}

/* --- presentation */
struct Target {
	UInt8 *base;			/* screen base address */
	long rowBytes;
	int depth;			/* pixel size in bits */
	int screenW, screenH;
	int gx, gy;			/* window content origin in global coordinates */
};

static void GetTarget(AGLDrawable port, struct Target *t)
{
	GDHandle gd = GetMainDevice();
	PixMapHandle pm = (**gd).gdPMap;
	GrafPtr save;
	Point tl;
	Rect r;

	t->base = (UInt8 *)(**pm).baseAddr;
	t->rowBytes = (**pm).rowBytes & 0x3FFF;
	t->depth = (**pm).pixelSize;
	t->screenW = (**gd).gdRect.right - (**gd).gdRect.left;
	t->screenH = (**gd).gdRect.bottom - (**gd).gdRect.top;
	GetPort(&save);
	SetPort((GrafPtr)port);
	r = port->portRect;
	tl.v = r.top;
	tl.h = r.left;
	LocalToGlobal(&tl);
	SetPort(save);
	t->gx = tl.h - (**gd).gdRect.left;
	t->gy = tl.v - (**gd).gdRect.top;
}

/* Source rows are bottom-up BGRA bytes (a band of n rows, `src`); screen row
 * of source row s is (h - 1 - s) of the window. CPU path for windows that are
 * partly off screen; the device does the common case. */
static void BlitBand(const struct Target *t, const UInt8 *src, int w, int h, int y0, int n)
{
	int r, x, x0 = 0, x1 = w;

	if (t->gx < 0)
		x0 = -t->gx;
	if (t->gx + w > t->screenW)
		x1 = t->screenW - t->gx;
	for (r = 0; r < n; r++) {
		int sy = t->gy + (h - 1 - (y0 + r));
		const UInt8 *s = src + (long)r * w * 4;
		UInt8 *d;

		if (sy < 0 || sy >= t->screenH)
			continue;
		d = t->base + (long)sy * t->rowBytes;
		if (t->depth == 32) {
			d += (long)(t->gx + x0) * 4;
			for (x = x0; x < x1; x++, d += 4) {
				d[0] = 0;
				d[1] = s[x * 4 + 2];
				d[2] = s[x * 4 + 1];
				d[3] = s[x * 4];
			}
		} else {
			d += (long)(t->gx + x0) * 2;
			for (x = x0; x < x1; x++, d += 2) {
				UInt16 v = (UInt16)(((s[x * 4 + 2] >> 3) << 10) | ((s[x * 4 + 1] >> 3) << 5) | (s[x * 4] >> 3));
				d[0] = (UInt8)(v >> 8);
				d[1] = (UInt8)v;
			}
		}
	}
}

/* Read the drawable (all rows, in bands that fit the reply area) and show it
 * at the window's place on the main screen. */
static int Present(struct __AGLContextRec *c)
{
	struct Target t;
	struct DGReply reply;
	int w = c->width, h = c->height, y0, n, rowsMax, direct;
	UInt32 flags;
	UInt32 bytes, rowBytes = (UInt32)w * 4;

	GetTarget(c->port, &t);
	if (t.depth != 32 && t.depth != 16) {
		DGLog("present: %d-bit screen not supported", t.depth);
		return -1;
	}
	rowsMax = (int)(gPv.replySize / rowBytes);
	if (rowsMax < 1 || rowBytes > 65536) {
		DGLog("present: window %d wide does not fit the reply area", w);
		return -1;
	}
	/* The device writes rows straight into VRAM when the whole window is on
	 * the main 32- or 16-bit screen (aligned, below the reserved ring); 16-bit
	 * uses the device's RGB555 conversion. */
	direct = t.gx >= 0 && t.gy >= 0 && t.gx + w <= t.screenW && t.gy + h <= t.screenH &&
		(UInt32)t.base >= gPv.vramBase && !((UInt32)t.base & 1) && !(t.rowBytes & 1) &&
		(UInt32)t.base - gPv.vramBase + (UInt32)(t.gy + h) * (UInt32)t.rowBytes <= gPv.ringOffset;
	if (t.depth == 32 && (((UInt32)t.base & 3) || (t.rowBytes & 3)))
		direct = 0;
	flags = t.depth == 32 ? PRESENT_FLAGS : (PV_FLAG_RGB555 | PV_FLAG_FLIP);
	for (y0 = 0; y0 < h; y0 += n) {
		struct DGPacket p;
		int whole;

		n = h - y0 < rowsMax ? h - y0 : rowsMax;
		whole = n == h;
		bytes = (UInt32)n * rowBytes;
		if (!whole && !(dg_connection.capabilities & DG_CAP_DRAWABLE_READ_RECTS)) {
			DGLog("present: %dx%d needs banded reads, host lacks DG_CAP_DRAWABLE_READ_RECTS", w, h);
			return -1;
		}
		p = DGPacketNew(&c->dg, whole ? DG_READ_DRAWABLE : DG_READ_DRAWABLE_RECT);
		p.message.flags = DG_OUTPUT_ONLY;
		p.message.bytes = bytes;
		p.message.args[0] = w;
		p.message.args[1] = n;
		if (!whole) {
			p.message.args[2] = 0;		/* x */
			p.message.args[3] = y0;		/* y, from the bottom */
		}				/* else args[2] = 0: BGRA8 */
		if (DGSubmitHeader(&c->dg, &p, &reply) || reply.error || reply.bytes != bytes) {
			DGLog("present: read failed (error %lu bytes %lu of %lu)", (unsigned long)reply.error,
				(unsigned long)reply.bytes, (unsigned long)bytes);
			DGPacketFree(&p);
			if (!dg_connection.dead && reply.bytes && reply.bytes <= gPv.replySize)
				PvmacDoorbell(reply.bytes);	/* keep the stream in step */
			return -1;
		}
		DGPacketFree(&p);
		if (direct) {
			UInt32 dest = (UInt32)t.base - gPv.vramBase + (UInt32)(t.gy + h - y0 - n) * (UInt32)t.rowBytes +
				(UInt32)t.gx * (t.depth == 32 ? 4 : 2);
			int bad;

			PvmacRegWr(PV_REG_REPLY_DEST, dest);
			PvmacRegWr(PV_REG_REPLY_PITCH, (UInt32)t.rowBytes);
			PvmacRegWr(PV_REG_REPLY_ROW, rowBytes);
			PvmacRegWr(PV_REG_REPLY_FLAGS, flags | PV_FLAG_PRESENT);
			bad = PvmacDoorbell(bytes);
			PvmacRegWr(PV_REG_REPLY_FLAGS, 0);
			PvmacRegWr(PV_REG_REPLY_DEST, 0);
			if (bad)
				return -1;
		} else {
			if (PvmacDoorbell(bytes))
				return -1;
			BlitBand(&t, PvmacReplyArea(), w, h, y0, n);
		}
	}
	return 0;
}

void aglSwapBuffers(AGLContext ctx)
{
	DGTraceFrame();
	if (!ctx || !ctx->port || !ctx->drawable) {
		SetError(ctx ? AGL_BAD_DRAWABLE : AGL_BAD_CONTEXT);
		return;
	}
	if (!ApplyDrawable(ctx, 0))		/* the window was resized */
		return;
	if (dg_connection.dead)
		return;
	if (Present(ctx))
		SetError(AGL_BAD_DRAWABLE);
}
