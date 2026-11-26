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
 * Logging: only when "OpenGLLibrary.debug" exists in the System Folder;
 * lines go to "UTM RAVE Loader.log" there. */
/* Built against Apple's Universal Interfaces (see build-ndrv.yml): Retro68's
 * Multiversal headers have no CodeFragments.h or Folders.h. */
#include <MacTypes.h>
#include <Files.h>
#include <Gestalt.h>
#include <Folders.h>
#include <CodeFragments.h>
#include <string.h>

#ifndef fnfErr
#define fnfErr (-43)
#endif

typedef void (*RGLProc)(void);

static int gDone;			/* the guard: runs once per process */
static int gActive;
static CFragConnectionID gConn;
static RGLProc gTerm;

static void Log(const char *msg, long err)
{
	static int checked, on;
	static short vref;
	static long dirID;
	char buf[96], num[16];
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
	buf[n++] = ' ';
	if (err < 0)
		buf[n++] = '-';
	do {
		num[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i)
		buf[n++] = num[--i];
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
	Log("calling rgl_initialize", 0);
	init();
	Log("rgl_initialize returned", 0);
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
