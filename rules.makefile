
c :=
DIRECTORIES :=

define include_dir_aux
c := $1
DIRECTORIES += $1

include $(TREE_PATH)/$1/dir.makefile

$$($1_TARGETS): d := $1

c := $c
endef
#include_dir = $(eval $(call include_dir_aux,$(if $c,$c/$1,$1)))
include_dir = $(eval $(call include_dir_aux,$1))

