# minarch-gl -- GL hw-render/shader-chain frontend, kept out of the upstream
# minarch target.
#
# Design (see workspace/tmp/minarch-gl-rewrite/PLAN.md):
#   * upstream all/minarch/ stays byte-identical; nothing here edits it.
#   * this directory holds ONLY files that must differ from upstream.  With no
#     source files in it, the target is exactly upstream minarch built through
#     this wrapper -- that is the step-1 baseline the rewrite starts from.
#   * the build merges upstream minarch/ into MERGED_DIR, copies this
#     directory's files over it, and builds there, so relative paths inside the
#     merged tree (../common, ../../$(PLATFORM)/..., ../../hash.txt) resolve as
#     they do in minarch/ itself.
ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif
ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=h700 make)
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

MERGED_DIR = ../.minarch-gl-build
UPSTREAM    = ../minarch
TARGET      = minarch-gl

# Upstream's SOURCE list, verbatim from all/minarch/makefile.  The wrapper hands
# it to the recursive make on the COMMAND LINE, which is how the overlay swaps
# an upstream translation unit for one of ours without editing upstream or
# shipping a makefile into the merged tree: replace the path here (api.c,
# platform.c, ...) and drop the corresponding file into this directory.
# generic_video.c needs no entry -- h700 platform.c #includes it, so overriding
# platform.c is what selects our copy.
UPSTREAM_SOURCE = minarch.c ma_cheats.c ma_rewind.c ma_audio.c ma_input.c \
	ma_options.c ma_frontend_opts.c ma_saves.c ma_video.c ma_core.c ma_game.c \
	ma_environment.c ma_config.c ma_menu.c ma_runframe.c \
	../common/scaler.c ../common/utils.c ../common/config.c ../common/api.c \
	../common/notification.c ../../h700/platform/platform.c \
	../common/http.c ../common/ra_badges.c ../common/ra_offline.c \
	../common/ra_sync.c ../common/ra_event_queue.c ra_integration.c chd_reader.c

all:
	rm -rf $(MERGED_DIR)
	mkdir -p $(MERGED_DIR)
	tar -C $(UPSTREAM) --exclude=./build --exclude=.git -cf - . | tar -C $(MERGED_DIR) -xf -
	@# Overlay sources only.  The merged tree keeps UPSTREAM's makefile so the
	@# recursive make below builds upstream minarch; copying this wrapper in
	@# would make it recurse into itself.  (When the overlay eventually needs a
	@# different SOURCE list, that is done with the MERGED=1 switch this file
	@# used to carry, not by shipping this file into the tree.)
	@files="$$(ls *.c *.h 2>/dev/null)"; [ -n "$$files" ] && cp -f $$files $(MERGED_DIR)/ || true
	$(MAKE) -C $(MERGED_DIR) -f makefile PLATFORM=$(PLATFORM) UNION_PLATFORM=$(UNION_PLATFORM) \
		SOURCE="$(UPSTREAM_SOURCE)"
	mkdir -p build/$(PLATFORM)
	cp -f $(MERGED_DIR)/build/$(PLATFORM)/minarch.elf build/$(PLATFORM)/$(TARGET).elf
	rm -rf $(MERGED_DIR)

clean:
	rm -rf build $(MERGED_DIR)

.PHONY: all clean
