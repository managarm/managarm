if build_uefi
	subdir_done()
endif

libsmarter = subproject('libsmarter').get_variable('libsmarter_dep')
link_args = [ '-nostdlib' ]

args = [
	'-DLIBASYNC_CUSTOM_PLATFORM',
	'-DFRG_DONT_USE_LONG_DOUBLE',
	'-fno-threadsafe-statics'
]
c_args = [ ]
cpp_args = [ ]

cxx = meson.get_compiler('cpp')
libgcc = cxx.find_library('gcc')
link_depends = []
inc = [ cralgo_includes ]
link_with = []
want_acpi = false

src = files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'generic/address-space.cpp',
	'generic/cancel.cpp',
	'generic/credentials.cpp',
	'generic/core.cpp',
	'generic/debug.cpp',
	'generic/event.cpp',
	'generic/fiber.cpp',
	'generic/gdbserver.cpp',
	'generic/hel.cpp',
	'generic/irq.cpp',
	'generic/io.cpp',
	'generic/ipc-queue.cpp',
	'generic/kasan.cpp',
	'generic/kerncfg.cpp',
	'generic/kernlet.cpp',
	'generic/kernel-io.cpp',
	'generic/kernel-log.cpp',
	'generic/kernel-stack.cpp',
	'generic/main.cpp',
	'generic/mbus.cpp',
	'generic/memory-view.cpp',
	'generic/ostrace.cpp',
	'generic/physical.cpp',
	'generic/profile.cpp',
	'generic/random.cpp',
	'generic/service.cpp',
	'generic/schedule.cpp',
	'generic/stream.cpp',
	'generic/timer.cpp',
	'generic/thread.cpp',
	'generic/servers.cpp',
	'generic/ubsan.cpp',
	'generic/universe.cpp',
	'generic/work-queue.cpp',
	'generic/asid.cpp',
	'system/framebuffer/boot-screen.cpp',
	'system/framebuffer/fb.cpp',
	'system/pci/dmalog.cpp',
	'system/pci/pci_discover.cpp',
	'system/pci/pci_legacy.cpp',
	'system/pci/pci_quirks.cpp',
	'system/pci/pcie_ecam.cpp',
	'system/pci/pcie_brcmstb.cpp'
)

src += frgbragi.process([
	protos/'fs/fs.bragi',
	protos/'hw/hw.bragi',
	protos/'mbus/mbus.bragi',
	protos/'ostrace/ostrace.bragi',
	protos/'posix/posix.bragi',
	protos/'clock/clock.bragi',
	protos/'kernlet/kernlet.bragi',
	protos/'kerncfg/kerncfg.bragi',
	protos/'svrctl/svrctl.bragi'
])

klibc_inc = include_directories('../klibc')

inc += klibc_inc
inc += include_directories(
	'generic',
	'system/acpi',
	'system/dtb',
	'system/framebuffer',
	'system/legacy-pc',
	'system/pci',
	'../common',
	'../../protocols/posix/include',
)

# For now, we only use static initializers for initgraph stages.
# If we decide to use them for something more complicated, we have to remove this.

if ubsan
	# -fsanitize=function requires RTTI.
	args += [ '-fsanitize=undefined', '-fno-sanitize=function' ]
endif

if kasan
	# The shadow memory offset is calculated as:
	#     ShadowMemoryBase - (StartOfHigherHalf/ShadowScale).
	# This ensures that the first shadow byte of the higher half ends up at SharedMemoryBase.
	# Lower half addresses become non-canonical by design
	# (and KASAN should not instrument lower-half access anyway).
	args += [
		'-DTHOR_KASAN',
		'-fsanitize=kernel-address',
		'-mllvm', '-asan-mapping-offset=0xdfffe00000000000',
		'-mllvm', '-asan-globals=false',
	]
endif

if log_alloc
	args += [ '-fno-omit-frame-pointer', '-DKERNEL_LOG_ALLOCATIONS', '-DTHOR_HAS_FRAME_POINTERS' ]
endif

if frame_pointers
	args += [ '-fno-omit-frame-pointer', '-DTHOR_HAS_FRAME_POINTERS' ]
endif

if arch == 'aarch64'
	subdir('arch/arm')
elif arch == 'x86_64'
	subdir('arch/x86')
endif

if want_acpi
	src += files(
		'system/acpi/acpi.cpp',
		'system/acpi/glue.cpp',
		'system/acpi/madt.cpp',
		'system/acpi/ec.cpp',
		'system/acpi/pm-interface.cpp',
		'system/acpi/battery.cpp',
		'system/acpi/ps2.cpp',
		'system/pci/pci_acpi.cpp'
	)

	uacpi_args = [ '-DUACPI_OVERRIDE_LIBC' ]

	uacpi_lib = static_library('uacpi', uacpi_sources,
		include_directories : [uacpi_includes, 'system/acpi'],
		c_args : c_args + args,
		pic : false
	)

	inc += uacpi_includes
	link_with += uacpi_lib
endif

link_with += static_library('cralgo', cralgo_sources,
	include_directories : [klibc_inc, cralgo_includes],
	cpp_args : cpp_args + args,
	pic : false
)

deps = [ libgcc, frigg, libsmarter, bragi_dep, libasync_dep, libarch, hel_dep ]

# TODO: also build thor on RISC-V.
if arch != 'riscv64'
	executable('thor', src,
		include_directories : inc,
		dependencies : deps,
		cpp_args : cpp_args + args,
		link_with : link_with,
		link_args : link_args,
		link_depends : link_depends,
		install : true
	)
endif
