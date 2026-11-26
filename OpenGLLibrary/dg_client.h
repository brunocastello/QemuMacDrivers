/* OS 9 client layer for VirtualIntelMac's PassthroughOpenGL wire.
 *
 * Packet API as the generated client code (direct-client-generated.inc)
 * expects it. Wire layout, opcodes and the DG_ constants follow
 * passthrough-opengl-wire.h of VirtualIntelMac (MIT, nfzerox, pinned commit
 * 2861da9e265e557aef90319e4451487ed1fed553). The transport is the PVMAC v2
 * ring (dg_transport.c) instead of VIM's mach/IOKit stream, and every value
 * that crosses the wire is converted from PowerPC (big-endian) to the
 * little-endian wire format here.
 */
#ifndef DG_CLIENT_H
#define DG_CLIENT_H
#include <string.h>
#include "glmin.h"

/* --- wire constants (VIM passthrough-opengl-wire.h, MIT) */
#define DG_MAGIC      0x31474c44u
#define DG_VERSION    8u
#define DG_MAX_BYTES  (64u*1024u*1024u)
#define DG_CREATE         0x10000u
#define DG_DESTROY        0x10001u
#define DG_RESIZE         0x10002u
#define DG_HELLO          0x10003u
#define DG_DRAW           0x10006u
#define DG_DROP_DRAWABLE  0x10007u
#define DG_READ_DRAWABLE  0x1000au
#define DG_READ_DRAWABLE_RECT 0x10019u
#define DG_REPLY       1u
#define DG_OUTPUT_ONLY 2u
#define DG_INPUT_ONLY  4u
#define DG_HELLO_FEATURES_MAGIC  0x584d4956u
#define DG_HELLO_FEATURES2_MAGIC 0x324d4956u
#define DG_HELLO_FEATURES3_MAGIC 0x334d4956u
#define DG_FEATURE_LARGE_IOSURFACE_WRITES 16u
#define DG_FEATURE_TIMER_QUERY 262144u
#define DG_FEATURE2_ES2_COMPAT 16u
#define DG_FEATURE2_PROGRAM_BINARY 64u
#define DG_FEATURE2_VERTEX_BLEND 1048576u
#define DG_FEATURE2_ADDITIONAL_STATE 268435456u
#define DG_CAP_DRAWABLE_READ_RECTS 256u
#define DG_CAP_CALL_LISTS (UINT64_C(1)<<44)
#define DG_CAP_QUERY_REPLIES (UINT64_C(1)<<45)
#define DG_CAP_COMPACT_SCALARS (UINT64_C(1)<<55)
#define DG_CAP_APPLE_VIDEO_TEXTURES (UINT64_C(1)<<58)
#define DG_CAP_BIG_ENDIAN_ARRAYS (UINT64_C(1)<<60)
#define DG_FEATURE2_ARRAY_ELEMENT 16384u
#define DG_COMPACT_SCALARS 0x10057u
#define DG_BATCH_BYTES (64u*1024u)

struct DGMessage { uint32_t op,context,bytes,flags;uint64_t args[16]; };
#define DG_MESSAGE_BYTES 144u
struct DGReply { uint32_t bytes,error;uint64_t result; };
struct DGHelloFeatures { uint32_t magic,features,magic2,features2,magic3,features3; };

struct DGConnection {
	int dead;
	unsigned features,features2,features3;
	uint64_t capabilities;
};
struct DGVertexState { GLuint name; };
/* Client vertex arrays (dg_arrays.c, after VIM's passthrough-opengl-arrays-client.c):
 * slots 0 vertex, 1 normal, 2 color, 3 secondary color, 4 fog coord,
 * 8..15 texture coordinates per client texture unit. */
#define DG_ARRAY_SLOTS 16u
struct DGClientArray {
	int enabled;
	GLint size;
	GLenum type;
	GLsizei stride;
	const void *pointer;
};
struct DGContext {
	struct DGConnection *connection;
	uint32_t id;
	GLenum error;
	int alignment[2],row_length[2],skip_rows[2],skip_pixels[2],row_bytes[2];
	int image_height[2],skip_images[2];
	unsigned profile;			/* 0x1000: legacy profile */
	GLuint pixel_buffer[2];
	struct DGVertexState *vertex;
	int common_state_blocked;		/* between glBegin and glEnd */
	int client_texture;			/* glClientActiveTexture unit */
	struct DGClientArray arrays[DG_ARRAY_SLOTS];
};
extern struct DGContext *dg_current;

extern struct DGConnection dg_connection;

#define DG_INLINE_BYTES 128u
struct DGPacket {
	struct DGMessage message;		/* host byte order until DGSubmit */
	unsigned char *data;			/* payload, wire (little-endian) element order */
	size_t capacity;
	int failed,valid_reply,oom;
	unsigned char inline_data[DG_INLINE_BYTES];
};
/* dg_trace.c: one-frame diagnostic trace (see dg_trace.c) */
void DGTraceMessage(const struct DGPacket *p);
void DGTraceFrame(void);
void DGTraceDump(void);

/* Value bits of a scalar argument, zero-extended like VIM's DGBits on a
 * little-endian client (a GLint -1 is 0x00000000ffffffff). Floats and doubles
 * keep their IEEE bit patterns. */
static inline uint64_t DGBits(const void *value,size_t size)
{
	switch(size) {
	case 1: return *(const uint8_t *)value;
	case 2: return *(const uint16_t *)value;
	case 4: return *(const uint32_t *)value;
	case 8: return *(const uint64_t *)value;
	default: return 0;
	}
}

struct DGPacket DGPacketNew(struct DGContext *c,uint32_t op);
void DGPacketFree(struct DGPacket *p);
/* Payload helpers: `width` is the element size in bytes (1, 2, 4, 8; 0 =
 * opaque, copied verbatim); elements are stored little-endian in the packet. */
void DGPacketPointerW(struct DGPacket *p,int index,const void *pointer,size_t bytes,int output,unsigned width);
void DGPacketOutputW(struct DGPacket *p,struct DGContext *context,int index,void *pointer,size_t bytes,unsigned width);
void DGPacketQueryInputW(struct DGPacket *p,int index,const void *pointer,size_t bytes,unsigned width);
void DGPacketCopyW(struct DGPacket *p,int index,void *pointer,size_t bytes,unsigned width);
/* sync: send, ring the doorbell and read the reply (and its payload into
 * p->data). Without sync the message only joins the ring. */
struct DGReply DGSubmit(struct DGContext *context,struct DGPacket *p,int sync);
int DGSubmitHeader(struct DGContext *context,struct DGPacket *p,struct DGReply *reply);
void DGFlushTransport(struct DGContext *context);
/* Emit the pending compact scalar run (if any) into the ring, in order. */
void DGCompactFlush(void);

/* --- helpers copied from VIM (dg_wire_subset.h) and stubs for the features
 * this client does not negotiate (the host sees plain per-call packets). */
#include "dg_wire_subset.h"
#include "direct-element-width.inc"	/* DGGLTypeWidth (generated) */

struct DGLargeTextureLayout { int unused; };
struct DGLargeTextureInput { int unused; };
static inline struct DGLargeTextureLayout DGLargeTextureLayout(struct DGContext *c,unsigned dimensions,
	GLsizei width,GLsizei height,GLsizei depth,GLenum format,GLenum type)
{ struct DGLargeTextureLayout l={0};(void)c;(void)dimensions;(void)width;(void)height;(void)depth;(void)format;(void)type;return l; }
static inline int DGCompactTextureInput(struct DGContext *c,struct DGPacket *p,int index,
	const void *pixels,const struct DGLargeTextureLayout *layout)
{ (void)c;(void)p;(void)index;(void)pixels;(void)layout;return 0; }
static inline int DGLargeTextureBegin(struct DGContext *c,const void *pixels,
	const struct DGLargeTextureLayout *layout,struct DGLargeTextureInput *input)
{ (void)c;(void)pixels;(void)layout;(void)input;return 0; }
static inline void DGLargeTextureEnd(struct DGContext *c,struct DGLargeTextureInput *input)
{ (void)c;(void)input; }
static inline void DGPBOPixel(struct DGContext *c,unsigned kind,GLenum target,GLint level,GLint internal,
	GLint x,GLint y,GLsizei width,GLsizei height,GLint border,GLenum format,GLenum type,const void *pixels)
{ (void)c;(void)kind;(void)target;(void)level;(void)internal;(void)x;(void)y;(void)width;(void)height;
  (void)border;(void)format;(void)type;(void)pixels; }
static inline int DGTightTexSubImage2D(struct DGContext *c,GLenum target,GLint level,GLint x,GLint y,
	GLsizei width,GLsizei height,GLenum format,GLenum type,const void *pixels)
{ (void)c;(void)target;(void)level;(void)x;(void)y;(void)width;(void)height;(void)format;(void)type;(void)pixels;return 0; }
static inline int DGLargeReadPixels(struct DGContext *c,GLint x,GLint y,GLsizei width,GLsizei height,
	GLenum format,GLenum type,void *pixels)
{ (void)c;(void)x;(void)y;(void)width;(void)height;(void)format;(void)type;(void)pixels;return 0; }
static inline int DGTrySubmitImmediateScalars(struct DGContext *c,uint32_t op,const uint64_t *args,unsigned words)
{ (void)c;(void)op;(void)args;(void)words;return 0; }
static inline int DGCommonStateGet(struct DGContext *c,GLenum key,void *out,int floating)
{ (void)c;(void)key;(void)out;(void)floating;return 0; }
static inline int DGPublicCoreVersion(struct DGContext *c,GLenum key,GLint *out)
{ (void)c;(void)key;(void)out;return 0; }
static inline uint64_t DGCoreExtensionMask(struct DGContext *c) { (void)c;return 0; }
static inline int DGClientWeightUnits(struct DGContext *c) { (void)c;return 0; }
static inline int DGClientArrayInteger(struct DGContext *c,GLenum key,GLint *out)
{ (void)c;(void)key;(void)out;return 0; }
static inline void DGPrepareArrayElement(struct DGContext *c) { (void)c; }

/* dg_arrays.c: client vertex array state and draw calls */
void DGClientTexture(struct DGContext *c,GLenum unit);
void DGArrayPointer(struct DGContext *c,unsigned slot,GLint size,GLenum type,GLsizei stride,
	const void *pointer,GLboolean normalized);
void DGEnableClientArray(struct DGContext *c,GLenum array,int enabled);
void DGDraw(struct DGContext *c,GLenum mode,GLint first,GLsizei count,GLenum type,const void *indices);
void DGArrayElement(struct DGContext *c,unsigned op,GLint index);

/* Pixel transfer sizes under the context's pixel-store state. */
static inline size_t DGPixelBytes(struct DGContext *c,int w,int h,GLenum format,GLenum type,int pack)
{
	if(!c || c->pixel_buffer[pack]) return SIZE_MAX;
	return DGPixelSpan(w,h,format,type,c->row_length[pack],c->alignment[pack],c->skip_rows[pack],
		c->skip_pixels[pack],c->row_bytes[pack]);
}

/* Logging (dg_log.c): appends to "OpenGLLibrary.log" in the default folder,
 * capped at 16 KiB. Format subset: %d %u %x %s %c %% with optional l. */
void DGLog(const char *fmt,...);
#endif
