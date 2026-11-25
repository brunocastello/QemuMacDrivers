/* Diagnostic frame trace (off unless triggered).
 *
 * Trigger: the application calls glFinish on TRACE_ARM consecutive frames
 * (GLQuake: type `gl_finish 1` in the console). The library then records every
 * message of the next whole frame (aglSwapBuffers to aglSwapBuffers) and writes
 * it once to "OpenGLLibrary.trace" in the default folder (the application's).
 *
 * File: "DGTR" magic, u32 count, u32 dropped, then count records of
 * TRACE_RECORD bytes, all big-endian as the guest stores them:
 *   u32 op, u32 payload bytes, u64 args[0..3] (host values before the LE
 *   header encoding), first 16 payload bytes (wire = little-endian order).
 */
#include <MacTypes.h>
#include <Files.h>
#include <MacMemory.h>
#include <string.h>
#include "dg_client.h"

#define TRACE_ARM 30
#define TRACE_MAX 60000u
#define TRACE_RECORD 56u
#define OP_FINISH 89u
#ifndef fnfErr
#define fnfErr (-43)
#endif

enum { IDLE, ARMED, RECORDING, DONE };
static int gState = IDLE;
static int gFinishFrames, gSawFinish;
static Handle gBuf;
static UInt32 gCount, gDropped;

void DGTraceMessage(const struct DGPacket *p)
{
	unsigned char *r;
	UInt32 n;

	if (p->message.op == OP_FINISH)
		gSawFinish = 1;
	if (gState != RECORDING)
		return;
	if (gCount >= TRACE_MAX) {
		gDropped++;
		return;
	}
	r = (unsigned char *)*gBuf + gCount * TRACE_RECORD;
	memcpy(r, &p->message.op, 4);
	memcpy(r + 4, &p->message.bytes, 4);
	memcpy(r + 8, p->message.args, 32);
	memset(r + 40, 0, 16);
	n = p->message.bytes < 16 ? p->message.bytes : 16;
	if (p->data && n)
		memcpy(r + 40, p->data, n);
	gCount++;
}

static void Dump(void)
{
	FSSpec spec;
	short ref;
	long len;
	UInt32 head[3];

	if (FSMakeFSSpec(0, 0, "\pOpenGLLibrary.trace", &spec) == fnfErr)
		FSpCreate(&spec, '????', 'BINA', 0);
	if (FSpOpenDF(&spec, fsRdWrPerm, &ref)) {
		DGLog("trace: cannot open OpenGLLibrary.trace");
		return;
	}
	SetEOF(ref, 0);
	head[0] = 'DGTR';
	head[1] = gCount;
	head[2] = gDropped;
	len = sizeof head;
	FSWrite(ref, &len, head);
	HLock(gBuf);
	len = gCount * TRACE_RECORD;
	FSWrite(ref, &len, *gBuf);
	HUnlock(gBuf);
	FSClose(ref);
	DGLog("trace: wrote %lu records (%lu dropped)", (unsigned long)gCount, (unsigned long)gDropped);
}

/* Called at the start of aglSwapBuffers: a frame boundary. */
void DGTraceFrame(void)
{
	OSErr err;

	switch (gState) {
	case IDLE:
		gFinishFrames = gSawFinish ? gFinishFrames + 1 : 0;
		gSawFinish = 0;
		if (gFinishFrames < TRACE_ARM)
			return;
		gBuf = TempNewHandle(TRACE_MAX * TRACE_RECORD, &err);
		if (!gBuf || err) {
			gBuf = NewHandle(TRACE_MAX * TRACE_RECORD);
			if (!gBuf) {
				DGLog("trace: no memory");
				gState = DONE;
				return;
			}
		}
		HLock(gBuf);
		gCount = gDropped = 0;
		gState = RECORDING;
		DGLog("trace: recording one frame");
		return;
	case RECORDING:
		gState = DONE;
		Dump();
		DisposeHandle(gBuf);
		gBuf = NULL;
		return;
	default:
		return;
	}
}
