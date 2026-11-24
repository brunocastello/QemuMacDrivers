/* Exports that VirtualIntelMac's generator leaves to a hand-written client
 * ("custom-client" slots in opengl-codegen-ppc.json). Only the ones a listed
 * application imports are written: today glGetString (GLQuake). */
#include "dg_client.h"

/* Static answers, not a round trip to the host: Mac OS 9 applications only
 * need to know the GL 1.2 level and the two multitexture extensions below,
 * both of which the host executor runs natively (its GL 2.1 context has
 * ARB multitexture and the ADD texture environment). Never list more than
 * the executor supports. */
static const char kVendor[] = "UTM";
static const char kRenderer[] = "UTM PassthroughOpenGL";
static const char kVersion[] = "1.2";
static const char kExtensions[] = "GL_ARB_multitexture GL_EXT_texture_env_add";

const GLubyte *glGetString(GLenum name)
{
	if (!dg_current)
		return NULL;
	switch (name) {
	case 0x1F00: return (const GLubyte *)kVendor;		/* GL_VENDOR */
	case 0x1F01: return (const GLubyte *)kRenderer;		/* GL_RENDERER */
	case 0x1F02: return (const GLubyte *)kVersion;		/* GL_VERSION */
	case 0x1F03: return (const GLubyte *)kExtensions;	/* GL_EXTENSIONS */
	default:
		dg_current->error = GL_INVALID_ENUM;
		return NULL;
	}
}
