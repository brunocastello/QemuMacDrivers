/* UTM RAVE Loader: CFM extension that makes Apple's "Classic RAVE" engine
 * (the RAVE engine on OpenGL) active on real Mac OS 9, without modifying any
 * Apple file.
 *
 * The QuickDraw 3D RAVE manager scans the Extensions folder in every process
 * for files of type 'shlb' / creator 'tnsl' and loads each with FragPrepare,
 * so this library registers the engine from its CFM initialization routine.
 * In Mac OS X's Classic environment (Gestalt 'bbox' bit 0) the manager loads
 * Classic RAVE itself; then this library does nothing.
 *
 * Otherwise it does what the manager does in Classic mode: GetSharedLibrary
 * "Classic RAVE", FindSymbol rgl_initialize / rgl_terminate, call
 * rgl_initialize (result ignored), and rgl_terminate + CloseConnection from
 * the termination routine.
 *
 * Afterwards (outside Classic) it makes Classic RAVE the default engine of
 * the QuickDraw 3D RAVE manager, which scores the enabled engines: every
 * engine that did not appear with rgl_initialize is disabled with
 * QAEngineDisable. The manager functions are resolved at run time
 * (GetSharedLibrary + FindSymbol; the Universal Interfaces ship no stub
 * library for "QuickDraw(TM) 3D Accelerator"), so the loader still loads
 * when the manager is absent.
 *
 * Logging: only when "OpenGLLibrary.debug" exists in the System Folder;
 * lines go to "UTM RAVE Loader.log" there. */
/* Built against Apple's Universal Interfaces (see build-ndrv.yml): Retro68's
 * Multiversal headers have no CodeFragments.h or Folders.h. */
#include <MacTypes.h>
#include <Files.h>
#include <Gestalt.h>
#include <Folders.h>
#include <CodeFragments.h>
#include <Quickdraw.h>
#include <QDOffscreen.h>
#include <RAVE.h>
#include <string.h>

#ifndef fnfErr
#define fnfErr (-43)
#endif

typedef void (*RGLProc)(void);

static int gDone;			/* the guard: runs once per process */
static int gActive;
static CFragConnectionID gConn;
static RGLProc gTerm;

/* Appends one line to the log (when logging is on): msg, then text (if any),
 * then the number (if hasNum). */
static void LogLine(const char *msg, const char *text, int hasNum, long err)
{
	static int checked, on;
	static short vref;
	static long dirID;
	char buf[160], num[16];
	int i = 0, n = 0;
	unsigned long v = err < 0 ? (unsigned long)-err : (unsigned long)err;
	FSSpec spec;
	short ref;
	long count;

	if (!checked) {
		checked = 1;
		on = FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vref, &dirID) == noErr &&
		     FSMakeFSSpec(vref, dirID, "\pOpenGLLibrary.debug", &spec) == noErr;
	}
	if (!on)
		return;
	while (*msg && n < 70)
		buf[n++] = *msg++;
	if (text) {
		int t;
		buf[n++] = ' ';
		for (t = 0; text[t] && t < 70; t++)
			buf[n++] = (unsigned char)text[t] >= ' ' ? text[t] : '?';
	}
	if (hasNum) {
		buf[n++] = ' ';
		if (err < 0)
			buf[n++] = '-';
		do {
			num[i++] = '0' + v % 10;
			v /= 10;
		} while (v);
		while (i)
			buf[n++] = num[--i];
	}
	buf[n++] = '\r';
	count = n;
	if (FSMakeFSSpec(vref, dirID, "\pUTM RAVE Loader.log", &spec) == fnfErr)
		FSpCreate(&spec, '????', 'TEXT', 0);
	if (FSpOpenDF(&spec, fsRdWrPerm, &ref))
		return;
	SetFPos(ref, fsFromLEOF, 0);
	FSWrite(ref, &count, buf);
	FSClose(ref);
}

static void Log(const char *msg, long err)
{
	LogLine(msg, 0, 1, err);
}

static void LogText(const char *msg, const char *text)
{
	LogLine(msg, text, 0, 0);
}

/* The RAVE manager's engine functions, found at run time. */
typedef TQAEngine *(*GetFirstProc)(const TQADevice *);
typedef TQAEngine *(*GetNextProc)(const TQADevice *, const TQAEngine *);
typedef TQAError (*GestaltProc)(const TQAEngine *, TQAGestaltSelector, void *);
typedef TQAError (*DisableProc)(long, long);
typedef TQAError (*CheckProc)(const TQAEngine *, const TQADevice *);

static GetFirstProc pFirst;
static GetNextProc pNext;
static GestaltProc pGestalt;
static DisableProc pDisable;
static CheckProc pCheck;		/* optional, diagnostics only */

#define MAXENG 32
typedef struct {
	long vendor, engine;
} EngID;

static int FindMgr(void)
{
	CFragConnectionID mc;
	Ptr mainAddr, sym;
	Str255 errName;
	CFragSymbolClass cls;
	OSErr e;

	/* "QuickDraw(TM) 3D Accelerator": the TM sign is Mac Roman 0xAA */
	e = GetSharedLibrary("\pQuickDraw\252 3D Accelerator", kPowerPCCFragArch, kReferenceCFrag, &mc, &mainAddr, errName);
	if (e != noErr) {
		Log("GetSharedLibrary QuickDraw 3D Accelerator: err", e);
		return 0;
	}
	if (FindSymbol(mc, "\pQADeviceGetFirstEngine", &sym, &cls) != noErr)
		return 0;
	pFirst = (GetFirstProc)sym;
	if (FindSymbol(mc, "\pQADeviceGetNextEngine", &sym, &cls) != noErr)
		return 0;
	pNext = (GetNextProc)sym;
	if (FindSymbol(mc, "\pQAEngineGestalt", &sym, &cls) != noErr)
		return 0;
	pGestalt = (GestaltProc)sym;
	if (FindSymbol(mc, "\pQAEngineDisable", &sym, &cls) != noErr)
		return 0;
	pDisable = (DisableProc)sym;
	if (FindSymbol(mc, "\pQAEngineCheckDevice", &sym, &cls) == noErr)
		pCheck = (CheckProc)sym;
	return 1;
}

/* Lists the engines for one device (NULL = any) into ids; returns the count. */
static int EnumWith(const TQADevice *dev, EngID *ids, int verbose)
{
	TQAEngine *eng;
	int n = 0;

	for (eng = pFirst(dev); eng && n < MAXENG; eng = pNext(dev, eng)) {
		long vendor = -1, engid = -1, len = 0;
		unsigned long opt = 0, fast = 0;
		char name[256];

		name[0] = 0;
		pGestalt(eng, kQAGestalt_VendorID, &vendor);
		pGestalt(eng, kQAGestalt_EngineID, &engid);
		if (verbose) {
			pGestalt(eng, kQAGestalt_OptionalFeatures, &opt);
			pGestalt(eng, kQAGestalt_FastFeatures, &fast);
			pGestalt(eng, kQAGestalt_ASCIINameLength, &len);
			if (len > 0 && len < 200)
				pGestalt(eng, kQAGestalt_ASCIIName, name);
			Log(" engine vendor", vendor);
			Log("  engine id", engid);
			LogText("  name", name);
			Log("  optional features", (long)opt);
			Log("  fast features", (long)fast);
		}
		ids[n].vendor = vendor;
		ids[n].engine = engid;
		n++;
	}
	return n;
}

/* Engines of any device; if that finds none, those for the main GDevice. */
static int Enum(EngID *ids, int verbose)
{
	TQADevice dev;
	int n = EnumWith(0, ids, verbose);

	if (n == 0) {
		dev.deviceType = kQADeviceGDevice;
		dev.device.gDevice = GetMainDevice();
		n = EnumWith(&dev, ids, verbose);
	}
	return n;
}

static int HasID(const EngID *ids, int n, const EngID *id)
{
	int i;

	for (i = 0; i < n; i++)
		if (ids[i].vendor == id->vendor && ids[i].engine == id->engine)
			return 1;
	return 0;
}

/* Diagnostics: what an application asking for the main screen gets, and
 * whether each engine accepts that device (EngineCheckDevice). */
static void CheckMainDevice(void)
{
	TQADevice dev;
	TQAEngine *eng;

	dev.deviceType = kQADeviceGDevice;
	dev.device.gDevice = GetMainDevice();
	Log("main GDevice", (long)dev.device.gDevice);
	for (eng = pFirst(&dev); eng; eng = pNext(&dev, eng)) {
		long vendor = -1, engid = -1;

		pGestalt(eng, kQAGestalt_VendorID, &vendor);
		pGestalt(eng, kQAGestalt_EngineID, &engid);
		Log(" engine for main GDevice: vendor", vendor);
		Log("  engine id", engid);
		if (pCheck)
			Log("  QAEngineCheckDevice", pCheck(eng, &dev));
	}
	Log("end of engines for main GDevice", 0);
}

/* CFM initialization routine (named by the PEF loader header: initSection /
 * initOffset, set by set-pef-init-term.py). Always noErr. */
OSErr UTMRAVEInit(const CFragInitBlock *block)
{
	long bbox;
	CFragConnectionID conn;
	Ptr mainAddr, sym;
	Str255 errName;
	CFragSymbolClass cls;
	RGLProc init, term;
	OSErr e;
	EngID before[MAXENG], after[MAXENG];
	int nBefore = 0, nAfter, i, nNew = 0, haveMgr;

	(void)block;
	if (gDone)
		return noErr;
	gDone = 1;
	if (Gestalt('bbox', &bbox) == noErr && (bbox & 1)) {
		Log("in Classic: manager loads Classic RAVE itself", 0);
		return noErr;
	}
	e = GetSharedLibrary("\pClassic RAVE", kPowerPCCFragArch, kLoadCFrag, &conn, &mainAddr, errName);
	if (e != noErr) {
		Log("GetSharedLibrary Classic RAVE: err", e);
		return noErr;
	}
	if (FindSymbol(conn, "\prgl_initialize", &sym, &cls) != noErr) {
		Log("rgl_initialize not found", 0);
		CloseConnection(&conn);
		return noErr;
	}
	init = (RGLProc)sym;
	if (FindSymbol(conn, "\prgl_terminate", &sym, &cls) != noErr) {
		Log("rgl_terminate not found", 0);
		CloseConnection(&conn);
		return noErr;
	}
	term = (RGLProc)sym;
	gConn = conn;
	gTerm = term;
	gActive = 1;
	haveMgr = FindMgr();
	if (haveMgr) {
		Log("engines before rgl_initialize", 0);
		nBefore = Enum(before, 1);
		Log("engine count", nBefore);
	}
	Log("calling rgl_initialize", 0);
	init();
	Log("rgl_initialize returned", 0);
	if (!haveMgr)
		return noErr;
	Log("engines after rgl_initialize", 0);
	nAfter = Enum(after, 1);
	Log("engine count", nAfter);
	for (i = 0; i < nAfter; i++)
		if (!HasID(before, nBefore, &after[i]))
			nNew++;
	Log("Classic RAVE engines found", nNew);
	if (nNew == 0)
		return noErr;
	for (i = 0; i < nAfter; i++) {
		if (!HasID(before, nBefore, &after[i]))
			continue;
		Log("disabling vendor", after[i].vendor);
		Log("  engine id", after[i].engine);
		Log("  QAEngineDisable result", pDisable(after[i].vendor, after[i].engine));
	}
	CheckMainDevice();
	return noErr;
}

/* CFM termination routine (termSection / termOffset). */
void UTMRAVETerm(void)
{
	if (!gActive)
		return;
	gActive = 0;
	Log("calling rgl_terminate", 0);
	gTerm();
	CloseConnection(&gConn);
	Log("terminated", 0);
}
