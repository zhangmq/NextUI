###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = minarch-gl

# minarch-gl = our GL hw-render/shader-chain work, split from the untouched
# upstream minarch target. No symlinks, no shared entries: this directory
# only tracks the REAL files we changed or added (generic_video.c/api.c/
# api.h/platform.c/ma_gl.c/ma_gl.h plus the ma_* TUs that differ from
# upstream). The wrapper below merges this directory over the upstream
# minarch into a throwaway build tree (MERGED_DIR), files here overriding
# upstream files with the same name, then builds inside that tree.

MERGED_DIR = ../.minarch-gl-build

ifeq ($(MERGED),1)

# ================= merged build tree (MERGED_DIR) =================
# Relative paths inside MERGED_DIR resolve like in minarch/ itself:
# ../common, ../../$(PLATFORM)/... and ../../hash.txt all match because
# MERGED_DIR sits beside minarch/ at workspace/all/.minarch-gl-build.

PRODUCT= build/$(PLATFORM)/$(TARGET).elf
INCDIR = -I. -I./libretro-common/include/ -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = minarch.c ma_cheats.c ma_rewind.c ma_audio.c ma_input.c \
         ma_options.c ma_frontend_opts.c ma_saves.c ma_video.c ma_core.c ma_game.c ma_environment.c ma_config.c ma_menu.c ma_runframe.c ma_gl.c \
         platform.c \
         ../common/scaler.c ../common/utils.c ../common/config.c api.c \
         ../common/notification.c

# RA support
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 h700 my355 desktop))
# RA source files
SOURCE += ../common/http.c ../common/ra_badges.c ../common/ra_offline.c ../common/ra_sync.c ../common/ra_event_queue.c ra_integration.c chd_reader.c
endif

CC = $(CROSS_COMPILE)gcc
CFLAGS  += $(OPT)
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS	 += -lmsettings -lsamplerate

ifeq ($(PROFILE), 1)
CFLAGS  += -pg
LDFLAGS += -pg
endif

ifeq ($(GPERFTOOLS), 1)
CFLAGS  += -g
LDFLAGS += -lprofiler
endif

# Rewind support
LDFLAGS  += -llz4

# SaveRAM support
ifeq ($(PLATFORM), desktop)
ifeq ($(UNAME_S),Linux)
CFLAGS += `pkg-config --cflags libzip`
LDFLAGS += `pkg-config --libs libzip` -lz
else
LDFLAGS += -lzip -lz
endif # Linux
else # all handheld platforms
LDFLAGS	 +=  -Llibretro-common -lsrm -lzip -lz
CFLAGS   += -DHAS_SRM
LDFLAGS +=  -lasound
endif

# RA support: rcheevos and libchdr linking
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 h700 my355 desktop))
LDFLAGS += -lrcheevos -lchdr -lcrypto
CFLAGS += -DRC_CLIENT_SUPPORTS_HASH -DHAS_CHEEVOS -DHAS_CHDR
ifeq ($(PLATFORM), desktop)
# Desktop needs rpath for local shared library lookup
LDFLAGS += -Wl,-rpath,'$$ORIGIN'
endif
endif

# Sanitizers
ifeq ($(PLATFORM), desktop)
ifeq ($(TSAN), 1)
CFLAGS  += -fsanitize=thread
LDFLAGS += -ltsan
endif
ifeq ($(ASAN), 1)
CFLAGS  += -fsanitize=address -fno-common -fno-omit-frame-pointer
LDFLAGS += -lasan
endif
endif

AR=$(CROSS_COMPILE)ar
ARFLAGS=rc

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

ifeq ($(PLATFORM), desktop)
all: clean libretro-common rcheevos libchdr $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
else ifneq (,$(filter $(PLATFORM),tg5040 tg5050 h700 my355))
# Platforms with RA support
all: clean libretro-common libchdr libsrm.a rcheevos $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libsamplerate.so.* build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libcrypto.so.* build/$(PLATFORM)
	chmod a+r build/$(PLATFORM)/libcrypto.so.*
ifeq ($(PLATFORM), my355)
	cp -L $(PREFIX)/lib/libsqlite3.so build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libffi.so.7 build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libtinyalsa.so.2 build/$(PLATFORM)
endif
	cp -L $(PREFIX)/lib/libsamplerate.so.0 build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libzip.so.5 build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libbz2.so.1.0 build/$(PLATFORM)
	cp -L $(PREFIX)/lib/liblzma.so.5 build/$(PLATFORM)
	cp -L $(PREFIX)/lib/libzstd.so.1 build/$(PLATFORM)
	cp -L $(PREFIX)/lib/liblz4.so.1 build/$(PLATFORM)
	cp -L libchdr/build/$(PLATFORM)/libchdr.so.0 build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
endif

libretro-common:
	git clone https://github.com/libretro/libretro-common

rcheevos: rcheevos/makefile $(PREFIX_LOCAL)/lib/librcheevos.a

libchdr: libchdr/CMakeLists.txt $(PREFIX_LOCAL)/lib/libchdr.so

rcheevos/makefile:
	rm -rf rcheevos
	mkdir -p rcheevos
	cp rcheevos.makefile rcheevos/makefile

$(PREFIX_LOCAL)/lib/librcheevos.a:
	cd rcheevos && make build install PLATFORM=$(PLATFORM)

libchdr/CMakeLists.txt:
	rm -rf libchdr
	git clone https://github.com/rtissera/libchdr
	cp libchdr.makefile libchdr/makefile

$(PREFIX_LOCAL)/lib/libchdr.so:
	cd libchdr && make build install PLATFORM=$(PLATFORM)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../../$(PLATFORM)/libmsettings && make

### libsrm stuff
OBJECTS = streams/rzip_stream.o streams/file_stream.o vfs/vfs_implementation.o file/file_path.o file/file_path_io.o compat/compat_strl.o time/rtime.o string/stdstring.o encodings/encoding_utf.o streams/trans_stream.o streams/trans_stream_pipe.o streams/trans_stream_zlib.o

$(OBJECTS) :
	cd libretro-common && $(CC) $(CFLAGS) -DHAVE_ZLIB -c -o $@ $(subst .o,.c,$@) -I include/

libsrm.a: $(OBJECTS)
	cd libretro-common && $(AR) $(ARFLAGS) $@ $?

clean:
	rm -f $(PRODUCT)
	rm -f $(OBJECTS) $(LIBRARY)

else

# ================= wrapper: merge + build =================
# 1) copy the untouched upstream minarch into MERGED_DIR,
# 2) copy this directory's files over it (override 1:1 by name),
# 3) build inside MERGED_DIR (recursive make with MERGED=1),
# 4) bring the elf back to build/$(PLATFORM)/.

all:
	rm -rf $(MERGED_DIR)
	mkdir -p $(MERGED_DIR)
	# Exclude only the TOP-LEVEL build dir and nested .git repos; sub-builds
	# like libchdr/build are kept (the inner make copies
	# libchdr/build/$(PLATFORM)/libchdr.so.0 from them).
	tar -C ../minarch --exclude=./build --exclude=.git -cf - . | tar -C $(MERGED_DIR) -xf -
	cp -f *.c *.h makefile $(MERGED_DIR)/
	$(MAKE) -C $(MERGED_DIR) -f makefile MERGED=1 PLATFORM=$(PLATFORM) \
		UNION_PLATFORM=$(UNION_PLATFORM) all
	mkdir -p build/$(PLATFORM)
	cp -f $(MERGED_DIR)/build/$(PLATFORM)/$(TARGET).elf build/$(PLATFORM)/$(TARGET).elf

clean:
	rm -rf $(MERGED_DIR)
	rm -rf build

endif