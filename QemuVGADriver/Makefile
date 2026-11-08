# Retro68 build of QemuVGADriver (qemu_vga.ndrv).
#
# The original build is the CodeWarrior project QemuVGADriver.mcp.xml; this
# Makefile reproduces its release target ("qemu_vga.ndrv": SharedLibrary,
# type 'ndrv', creator 'PSSD', main symbol DoDriverIO, exports from
# VideoDriver.exp) with the Retro68 PowerPC toolchain and Apple's Universal
# Interfaces 3.x (the open-source Multiversal Interfaces lack Video.h and
# VideoServices.h).
#
# Build:   PATH=/path/to/Retro68-build/toolchain/bin:$PATH make
# CI:      .github/workflows/build-ndrv.yml (ghcr.io/autc04/retro68 container)
#
# The link recipe (ndrv.lds, -bE:, --gc-keep-exported, MakePEF) follows
# elliotnunn/classicvirtio's Makefile (MIT).

CC      = powerpc-apple-macos-gcc
MAKEPEF = MakePEF

OUT     = build
SRCS    = src/DriverDoDriverIO.c \
          src/DriverGestaltHandler.c \
          src/DriverQDCalls.c \
          src/QemuEdid.c \
          src/QemuVga.c \
          ../shared/MacDriverUtils.c

CFLAGS  = -Os -ffunction-sections -fdata-sections \
          -Isrc -I../shared \
          -Wall -Wno-unknown-pragmas -Wno-multichar -Wno-pointer-sign \
          $(EXTRA_CFLAGS)

LIBS    = -lDriverServicesLib -lNameRegistryLib -lPCILib -lVideoServicesLib \
          -lDriverLoaderLib -lInterfaceLibForDriverServices

all: $(OUT)/qemu_vga.ndrv

$(OUT)/qemu_vga.so: $(SRCS) src/*.h ../shared/*.h ndrv.lds src/VideoDriver.exp
	mkdir -p $(OUT)
	$(CC) -o $@ $(CFLAGS) \
		-T ndrv.lds -Wl,-bE:src/VideoDriver.exp -Wl,--gc-sections -Wl,--gc-keep-exported \
		$(SRCS) $(LIBS)

$(OUT)/qemu_vga.ndrv: $(OUT)/qemu_vga.so
	$(MAKEPEF) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
