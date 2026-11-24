/* PVMAC v2 ring transport; protocol proven by GLHello on Mac OS 9. */
#include <MacTypes.h>
#include <Files.h>
#include <Devices.h>
#include <Quickdraw.h>
#include <string.h>
#include "dg_transport.h"
#include "dg_client.h"

PvmacInfo gPv;
static int gOpen;			/* 0 never tried, 1 ok, -1 failed */
static UInt32 gHead;			/* local copy of RING_HEAD */
static UInt32 gPending;			/* bytes appended since the last doorbell */

/* The device queues asynchronous spans; keep them moderately sized so the
 * executor overlaps with the guest. */
#define FLUSH_THRESHOLD (48u*1024u)

UInt32 PvmacRegRd(UInt32 off)
{
	UInt32 v;
	__asm__ volatile("lwbrx %0,%1,%2" : "=r"(v) : "b"(gPv.regBase), "r"(off) : "memory");
	return v;
}

void PvmacRegWr(UInt32 off, UInt32 v)
{
	__asm__ volatile("stwbrx %0,%1,%2" : : "r"(v), "b"(gPv.regBase), "r"(off) : "memory");
	__asm__ volatile("eieio; sync" : : : "memory");
}

int PvmacOpen(void)
{
	ParamBlockRec pb;
	GDHandle gd;
	PvmacInfo *ip = &gPv;
	OSErr err;

	if (gOpen)
		return gOpen > 0 ? 0 : -1;
	gOpen = -1;
	gd = GetMainDevice();
	memset(&pb, 0, sizeof pb);
	pb.cntrlParam.ioCRefNum = (**gd).gdRefNum;
	pb.cntrlParam.csCode = PVMAC_CS_GET_INFO;
	memcpy(pb.cntrlParam.csParam, &ip, sizeof ip);
	err = PBStatusSync(&pb);
	if (err) {
		DGLog("PVMAC: csCode 0x8300 failed: %d (not the PVMAC qemu_vga.ndrv?)", (int)err);
		return -1;
	}
	if (!(gPv.caps & PVMAC_CAP_GL_STREAM) || !gPv.regBase || !gPv.ringSize) {
		DGLog("PVMAC: no GL stream (caps %lx): needs mac-accel=on and a v2 device", gPv.caps);
		return -1;
	}
	gHead = PvmacRegRd(PV_REG_RING_HEAD);
	gPending = (gHead + gPv.ringSize - PvmacRegRd(PV_REG_RING_TAIL)) % gPv.ringSize;
	DGLog("PVMAC v%lu caps %lx ring %lu reply %lu", gPv.version, gPv.caps, gPv.ringSize, gPv.replySize);
	gOpen = 1;
	return 0;
}

UInt32 PvmacPending(void) { return gPending; }

const UInt8 *PvmacReplyArea(void) { return (const UInt8 *)(gPv.vramBase + gPv.replyOffset); }

int PvmacReserve(UInt32 n)
{
	if (n >= gPv.ringSize) {
		DGLog("PVMAC: message of %lu bytes does not fit the %lu byte ring", n, gPv.ringSize);
		return -1;
	}
	if (n > gPv.ringSize - 1 - gPending)
		PvmacFlush();			/* the device queues the span: ring is free again */
	return 0;
}

void PvmacAppend(const void *p, UInt32 n)
{
	UInt8 *ring = (UInt8 *)(gPv.vramBase + gPv.ringOffset);
	UInt32 first = gPv.ringSize - gHead < n ? gPv.ringSize - gHead : n;

	memcpy(ring + gHead, p, first);
	memcpy(ring, (const UInt8 *)p + first, n - first);
	gHead = (gHead + n) % gPv.ringSize;
	gPending += n;
}

void PvmacFlush(void)
{
	if (!gPending)
		return;
	__asm__ volatile("sync" : : : "memory");
	PvmacRegWr(PV_REG_RING_HEAD, gHead);
	PvmacRegWr(PV_REG_REPLY_EXPECT, 0);
	PvmacRegWr(PV_REG_DOORBELL, 1);
	gPending = 0;
}

int PvmacDoorbell(UInt32 expect)
{
	__asm__ volatile("sync" : : : "memory");
	PvmacRegWr(PV_REG_RING_HEAD, gHead);
	PvmacRegWr(PV_REG_REPLY_EXPECT, expect);
	PvmacRegWr(PV_REG_DOORBELL, 1);
	gPending = 0;
	if (expect && PvmacRegRd(PV_REG_REPLY_LEN) != expect) {
		DGLog("PVMAC: reply short: wanted %lu got %lu status %lx", expect,
			PvmacRegRd(PV_REG_REPLY_LEN), PvmacRegRd(PV_REG_GL_STATUS));
		return -1;
	}
	return 0;
}

/* Called by the generated code after glFlush: queue the work now. */
void DGFlushTransport(struct DGContext *context)
{
	(void)context;
	if (gOpen > 0)
		PvmacFlush();
}

/* After a no-reply message: keep spans bounded. */
void PvmacMaybeFlush(void)
{
	if (gPending >= FLUSH_THRESHOLD)
		PvmacFlush();
}
