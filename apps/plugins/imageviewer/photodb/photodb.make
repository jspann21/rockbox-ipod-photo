#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

PHOTODBSRCDIR := $(IMGVSRCDIR)/photodb
PHOTODBBUILDDIR := $(IMGVBUILDDIR)/photodb

PHOTODB_SRC := $(call preprocess, $(PHOTODBSRCDIR)/SOURCES)
PHOTODB_OBJ := $(call c2obj, $(PHOTODB_SRC))

OTHER_SRC += $(PHOTODB_SRC)

ROCKS += $(PHOTODBBUILDDIR)/photodb.ovl

$(PHOTODBBUILDDIR)/photodb.refmap: $(PHOTODB_OBJ)
$(PHOTODBBUILDDIR)/photodb.link: $(PLUGIN_LDS) $(PHOTODBBUILDDIR)/photodb.refmap
$(PHOTODBBUILDDIR)/photodb.ovl: $(PHOTODB_OBJ)

$(PHOTODBBUILDDIR)/%.o: $(PHOTODBSRCDIR)/%.c $(PHOTODBSRCDIR)/photodb.make
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) -I$(dir $<) $(IMGDECFLAGS) -c $< -o $@
