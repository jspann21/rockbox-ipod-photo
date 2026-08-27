#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__ /\_ \
#                     \/            \/     \/    \/            \/
#

MJPEGSRCDIR := $(APPSDIR)/plugins/mjpegplayer
MJPEGBUILDDIR := $(BUILDDIR)/apps/plugins/mjpegplayer

ROCKS += $(MJPEGBUILDDIR)/mjpegplayer.rock

MJPEG_SRC := $(call preprocess, $(MJPEGSRCDIR)/SOURCES)
MJPEG_OBJ := $(call c2obj, $(MJPEG_SRC))

# mjpegplayer textually reuses the accelerated Image Viewer JPEG decoder.
# On iPod Color that decoder calls the classic-ARM vertical IDCT helpers, so
# build a private copy of their assembly object into this plugin as well.
MJPEG_IDCT_SRC := $(APPSDIR)/plugins/imageviewer/jpeg/jpeg_idct_arm.S
MJPEG_IDCT_OBJ := $(MJPEGBUILDDIR)/jpeg_idct_arm.o
MJPEG_OBJ += $(MJPEG_IDCT_OBJ)

# Add source files to OTHER_SRC to get automatic dependencies.
OTHER_SRC += $(MJPEG_SRC) $(MJPEG_IDCT_SRC)

$(MJPEGBUILDDIR)/mjpegplayer.rock: $(MJPEG_OBJ)

$(MJPEGBUILDDIR)/%.o: $(MJPEGSRCDIR)/%.c $(MJPEGSRCDIR)/mjpegplayer.make
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -I$(dir $<) $(PLUGINFLAGS) -c $< -o $@

$(MJPEG_IDCT_OBJ): $(MJPEG_IDCT_SRC) $(MJPEGSRCDIR)/mjpegplayer.make
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -I$(dir $<) $(PLUGINFLAGS) -c $< -o $@
