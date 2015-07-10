
SUBDIRS = thor zisa eir

ALL_SUBDIRS = $(SUBDIRS:%=all-%)
CLEAN_SUBDIRS = $(SUBDIRS:%=clean-%)

.PHONY: all clean $(ALL_SUBDIRS) $(CLEAN_SUBDIRS)

all: $(ALL_SUBDIRS)

clean: $(CLEAN_SUBDIRS)

$(ALL_SUBDIRS):
	make -C $(@:all-%=%)

$(CLEAN_SUBDIRS):
	make -C $(@:clean-%=%) clean

# "special" dependencies: eir has to be build last
all-eir: all-thor all-zisa

