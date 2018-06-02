
$(call standard_dirs)
$c_EXTRA_SRCDIR := $(TREE_PATH)/$c/extra-src
$c_ACPICA_SRCDIR := $(ACPICA)/source
$(call define_objdir,ARCH_OBJ,$($c_OBJDIR)/arch/x86)
$(call define_objdir,SYSTEM_OBJ,$($c_OBJDIR)/system)
$(call define_objdir,PCI_OBJ,$($c_OBJDIR)/system/pci)
$(call define_objdir,ACPI_OBJ,$($c_OBJDIR)/system/acpi)
$(call define_objdir,GENERIC_OBJ,$($c_OBJDIR)/generic)
$(call define_objdir,ACPICA_OBJ,$(BUILD_PATH)/$c/acpica-obj)
$(call decl_targets,install-$c-headers)

ACPICA_SUBDIRS := common components/debugger components/dispatcher components/disassembler \
	components/events components/executer components/hardware components/namespace components/parser \
	components/resources components/tables components/utilities
$c_ACPICA_SUBDIR_PATHS := $(addprefix $($c_ACPICA_OBJDIR)/,$(ACPICA_SUBDIRS))

$($c_ACPICA_SUBDIR_PATHS):
	mkdir -p $@

$c_OBJECTS := frigg-debug.o frigg-libc.o \
	frigg-arch-gdt.o frigg-arch-idt.o frigg-arch-tss.o \
	arch/x86/early_stubs.o arch/x86/stubs.o \
	arch/x86/cpu.o arch/x86/entry.o \
	arch/x86/ints.o arch/x86/pic.o arch/x86/system.o arch/x86/paging.o \
	arch/x86/hpet.o arch/x86/rtc.o \
	arch/x86/embed_trampoline.o \
	generic/physical.o generic/main.o generic/service.o generic/hel.o \
	generic/cancel.o generic/core.o generic/fiber.o generic/ipc-queue.o generic/usermem.o \
	generic/schedule.o generic/futex.o generic/stream.o \
	generic/timer.o generic/thread.o generic/irq.o generic/io.o generic/service_helpers.o
$c_OBJECTS += generic/font-8x16.o system/boot-screen.o system/fb.o
$c_OBJECTS += system/pci/pci_io.o system/pci/pci_discover.o 
$c_OBJECTS += system/acpi/glue.o system/acpi/madt.o system/acpi/pm-interface.o
$c_OBJECT_PATHS := $(addprefix $($c_OBJDIR)/,$($c_OBJECTS))

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
	components/hardware/hwesleep.o \
	components/hardware/hwregs.o \
	components/hardware/hwvalid.o \
	components/hardware/hwgpe.o \
	components/hardware/hwpci.o \
	components/hardware/hwsleep.o \
	components/hardware/hwxface.o \
	components/hardware/hwxfsleep.o \
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

$c_HEADERS := thor.h
$c_HEADER_PATHS := $(addprefix $($c_HEADERDIR)/,$($c_HEADERS))

all-$c: $($c_BINDIR)/thor

install-$c-headers:
	install $($d_HEADER_PATHS) $(SYSROOT_PATH)/usr/include

$c_CC = x86_64-managarm-kernel-gcc
$c_CXX = x86_64-managarm-kernel-g++

$c_INCLUDES := -I$(TREE_PATH)/frigg/include -I$(TREE_PATH)/eir/include \
	-I$(TREE_PATH)/bragi/include -I$(TREE_PATH)/$c/include \
	-I$(TREE_PATH)/libarch/include -I$(CXXSHIM)/stage2/include -I$(FRIGG)/include \
	-I$($c_GENDIR)
$c_INCLUDES += -I$(TREE_PATH)/thor/kernel/c_headers -I$(ACPICA)/source/include
$c_INCLUDES += -iquote $($c_SRCDIR)

$c_CFLAGS := $(CFLAGS) $($c_INCLUDES)
$c_CFLAGS += -std=c11 -Wall -O2
$c_CFLAGS += -ffreestanding -mno-red-zone -mcmodel=kernel
$c_CFLAGS += -msoft-float -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx

$c_CXXFLAGS := $(CXXFLAGS) $($c_INCLUDES)
$c_CXXFLAGS += -std=c++17 -Wall -O2
$c_CXXFLAGS += -fno-exceptions -fno-rtti
$c_CXXFLAGS += -ffreestanding -mno-red-zone -mcmodel=kernel
$c_CXXFLAGS += -msoft-float -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx
$c_CXXFLAGS += -DFRIGG_NO_LIBC

$c_AS := x86_64-managarm-kernel-as
$c_ASFLAGS :=

$c_LD := x86_64-managarm-kernel-ld
$c_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T $($c_SRCDIR)/arch/x86/link.x

$c_OBJCOPY := x86_64-managarm-kernel-objcopy

$($c_BINDIR)/thor: $($c_OBJECT_PATHS) $($c_ACPICA_OBJECT_PATHS)
$($c_BINDIR)/thor: $($c_SRCDIR)/arch/x86/link.x | $($c_BINDIR)
	$($d_LD) $($d_LDFLAGS) -o $@ $($d_OBJECT_PATHS) $($d_ACPICA_OBJECT_PATHS)

$($c_GENDIR)/frigg-%.cpp: $(TREE_PATH)/frigg/src/%.cpp | $($c_GENDIR)
	install $< $@

$($c_GENDIR)/frigg-arch-%.cpp: $(TREE_PATH)/frigg/src/arch_x86/%.cpp | $($c_GENDIR)
	install $< $@

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.cpp | $($c_GENERIC_OBJDIR) $($c_ARCH_OBJDIR) $($c_PCI_OBJDIR) $($c_ACPI_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_GENDIR)/%.cpp | $($c_OBJDIR)
	$($d_CXX) -c -o $@ $($d_CXXFLAGS) $<
	$($d_CXX) $($d_CXXFLAGS) -MM -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

$($c_OBJDIR)/%.o: $($c_SRCDIR)/%.asm | $($c_ARCH_OBJDIR)
	$($d_AS) -o $@ $($d_ASFLAGS) $<

# Compile the x86 AP trampoline code. 
$($c_ARCH_OBJDIR)/trampoline.o: $($c_EXTRA_SRCDIR)/trampoline.asm | $($c_ARCH_OBJDIR)
	$($d_AS) -o $@ $($d_ASFLAGS) $<

$($c_ARCH_OBJDIR)/trampoline.bin: $($c_ARCH_OBJDIR)/trampoline.o
	$($d_LD) -o $@ -Ttext 0 --oformat binary $<

$($c_ARCH_OBJDIR)/embed_trampoline.o: $($c_ARCH_OBJDIR)/trampoline.bin
	$($d_OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

# Compile ACPICA.
$(call decl_targets,$c_ACPICA_OBJDIR/%)

$($c_ACPICA_OBJDIR)/%.o: $($c_ACPICA_SRCDIR)/%.c | $($c_ACPICA_SUBDIR_PATHS)
	$($d_CC) -c -o $@ $($d_CFLAGS) -MD -MP -MF $(@:%.o=%.d) -MT "$@" -MT "$(@:%.o=%.d)" $<

# Generate protobuf files.
gen-$c: $($c_GENDIR)/posix.frigg_pb.hpp \
	$($c_GENDIR)/fs.frigg_pb.hpp $($c_GENDIR)/mbus.frigg_pb.hpp \
	$($c_GENDIR)/hw.frigg_pb.hpp $($c_GENDIR)/clock.frigg_pb.hpp

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/bragi/proto/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/bragi/proto $<

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/thor/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/thor $<

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/protocols/hw/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/protocols/hw $<

$($c_GENDIR)/%.frigg_pb.hpp: $(TREE_PATH)/protocols/clock/%.proto | $($c_GENDIR)
	$(PROTOC) --plugin=protoc-gen-frigg=$(BUILD_PATH)/tools/frigg_pb/bin/frigg_pb \
			--frigg_out=$($d_GENDIR) --proto_path=$(TREE_PATH)/protocols/clock $<

