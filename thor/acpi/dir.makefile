
$(call standard_dirs)
$(call define_objdir,ACPICA_OBJ,$(BUILD_PATH)/$c/acpica-obj)

$c_INCLUDE := -I$($c_GENDIR)
$c_INCLUDE += -I$(ACPICA)/source/include -I$(TREE_PATH)/frigg/include

$c_CC := x86_64-managarm-gcc
$c_CFLAGS := $($c_INCLUDE)

$c_CXX := x86_64-managarm-g++
$c_CXXFLAGS := -std=c++14 $($c_INCLUDE)
$c_CXXFLAGS += -DFRIGG_HAVE_LIBC

$c_LDFLAGS :=
$c_LIBS := -lhelix -lprotobuf-lite -lcofiber -lmbus_protocol

$c_OBJECTS := main.o pci_io.o pci_discover.o glue-acpica.o hw.pb.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

# configure ACPICA paths
$c_ACPICA_SRCDIR = $(ACPICA)/source
$c_find_acpica = $(patsubst $($c_ACPICA_SRCDIR)/%.c,%.o,$(wildcard $($c_ACPICA_SRCDIR)/$x/*))

ACPICA_SUBDIRS := common components/debugger components/dispatcher components/disassembler \
	components/events components/executer components/hardware components/namespace components/parser \
	components/resources components/tables components/utilities
$c_ACPICA_SUBDIR_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$(ACPICA_SUBDIRS))

$c_ACPICA_OBJECTS := \
	components/events/evgpeinit.o \
	components/events/evgpe.o \
	components/events/evgpeblk.o \
	components/events/evgpeutil.o \
	components/events/evglock.o \
	components/events/evregion.o \
	components/events/evrgnini.o \
	components/events/evhandler.o \
	components/events/evmisc.o \
	components/events/evevent.o \
	components/events/evsci.o \
	components/events/evxface.o \
	components/events/evxfevnt.o \
	components/events/evxfregn.o \
	components/dispatcher/dsargs.o \
	components/dispatcher/dscontrol.o \
	components/dispatcher/dsutils.o \
	components/dispatcher/dsinit.o \
	components/dispatcher/dsfield.o \
	components/dispatcher/dsobject.o \
	components/dispatcher/dsopcode.o \
	components/dispatcher/dsdebug.o \
	components/dispatcher/dsmethod.o \
	components/dispatcher/dswload.o \
	components/dispatcher/dswload2.o \
	components/dispatcher/dswstate.o \
	components/dispatcher/dswexec.o \
	components/dispatcher/dswscope.o \
	components/dispatcher/dsmthdat.o \
	components/executer/exdebug.o \
	components/executer/exconfig.o \
	components/executer/exconvrt.o \
	components/executer/exconcat.o \
	components/executer/excreate.o \
	components/executer/exfield.o \
	components/executer/exfldio.o \
	components/executer/exmutex.o \
	components/executer/exnames.o \
	components/executer/exutils.o \
	components/executer/exresnte.o \
	components/executer/extrace.o \
	components/executer/exmisc.o \
	components/executer/exregion.o \
	components/executer/exoparg1.o \
	components/executer/exoparg2.o \
	components/executer/exoparg3.o \
	components/executer/exoparg6.o \
	components/executer/exstore.o \
	components/executer/exstoren.o \
	components/executer/exstorob.o \
	components/executer/exsystem.o \
	components/executer/exresop.o \
	components/executer/exresolv.o \
	components/executer/exprep.o \
	components/hardware/hwacpi.o \
	components/hardware/hwxface.o \
	components/hardware/hwregs.o \
	components/hardware/hwvalid.o \
	components/hardware/hwgpe.o \
	components/hardware/hwpci.o \
	components/namespace/nsalloc.o \
	components/namespace/nsaccess.o \
	components/namespace/nsarguments.o \
	components/namespace/nsconvert.o \
	components/namespace/nseval.o \
	components/namespace/nsload.o \
	components/namespace/nsinit.o \
	components/namespace/nsobject.o \
	components/namespace/nsnames.o \
	components/namespace/nsrepair.o \
	components/namespace/nsrepair2.o \
	components/namespace/nssearch.o \
	components/namespace/nsxfeval.o \
	components/namespace/nsxfname.o \
	components/namespace/nsxfobj.o \
	components/namespace/nsutils.o \
	components/namespace/nswalk.o \
	components/namespace/nsparse.o \
	components/namespace/nspredef.o \
	components/namespace/nsprepkg.o \
	components/parser/psargs.o \
	components/parser/psparse.o \
	components/parser/psloop.o \
	components/parser/psobject.o \
	components/parser/psopcode.o \
	components/parser/psopinfo.o \
	components/parser/psscope.o \
	components/parser/psutils.o \
	components/parser/pstree.o \
	components/parser/pswalk.o \
	components/parser/psxface.o \
	components/resources/rscreate.o \
	components/resources/rscalc.o \
	components/resources/rsxface.o \
	components/resources/rsinfo.o \
	components/resources/rsaddr.o \
	components/resources/rsirq.o \
	components/resources/rsio.o \
	components/resources/rsserial.o \
	components/resources/rsmisc.o \
	components/resources/rslist.o \
	components/resources/rsmemory.o \
	components/resources/rsutils.o \
	components/tables/tbdata.o \
	components/tables/tbfadt.o \
	components/tables/tbfind.o \
	components/tables/tbinstal.o \
	components/tables/tbxface.o \
	components/tables/tbxfload.o \
	components/tables/tbxfroot.o \
	components/tables/tbutils.o \
	components/tables/tbprint.o \
	components/utilities/utaddress.o \
	components/utilities/utalloc.o \
	components/utilities/utascii.o \
	components/utilities/utbuffer.o \
	components/utilities/utcache.o \
	components/utilities/utdecode.o \
	components/utilities/utcopy.o \
	components/utilities/utmath.o \
	components/utilities/utstrtoul64.o \
	components/utilities/utdelete.o \
	components/utilities/uteval.o \
	components/utilities/utexcep.o \
	components/utilities/uterror.o \
	components/utilities/uthex.o \
	components/utilities/utids.o \
	components/utilities/utinit.o \
	components/utilities/utglobal.o \
	components/utilities/utstring.o \
	components/utilities/utobject.o \
	components/utilities/utownerid.o \
	components/utilities/utlock.o \
	components/utilities/utmutex.o \
	components/utilities/utmisc.o \
	components/utilities/utpredef.o \
	components/utilities/utstate.o \
	components/utilities/utosi.o \
	components/utilities/utresrc.o \
	components/utilities/utxferror.o \
	components/utilities/utxface.o \
	components/utilities/utxfinit.o
$c_ACPICA_OBJECT_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$($c_ACPICA_OBJECTS))

$(call decl_targets,$c_ACPICA_OBJDIR/%)

all-$c: $($c_BINDIR)/acpi

$($c_ACPICA_SUBDIR_PATHS):
	mkdir -p $@

$($c_BINDIR)/acpi: $($c_OBJECT_PATHS) $($c_ACPICA_OBJECT_PATHS) | $($c_BINDIR)
	$($d_CXX) -o $@ $($d_LDFLAGS) $($d_OBJECT_PATHS) $($d_ACPICA_OBJECT_PATHS) $($d_LIBS)

$(call compile_cxx,$($c_SRCDIR),$($c_OBJDIR))

# compile ACPICA.
# debugger would require -DACPI_DEBUGGER -DACPI_DISASSEMBLER.
$($c_ACPICA_OBJDIR)/%.o: $($c_ACPICA_SRCDIR)/%.c | $($c_ACPICA_SUBDIR_PATHS)
	$($d_CC) -c -o $@ $($d_CFLAGS) -MD -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# generate protobuf
gen-$c: $($c_GENDIR)/hw.pb.tag

$($c_GENDIR)/%.pb.tag: $(TREE_PATH)/protocols/hw/%.proto | $($c_GENDIR)
	$(PROTOC) --cpp_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/protocols/hw $<
	touch $@

