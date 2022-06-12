libsmarter = subproject('libsmarter').get_variable('libsmarter_dep')
link_args = [ '-nostdlib' ]

args = [ 
	'-DLIBASYNC_CUSTOM_PLATFORM',
	'-fno-threadsafe-statics'
]
c_args = [ ]
cpp_args = [ ]

link_depends = []
inc = [ cralgo_includes ]
link_with = []
want_acpi = false

src = [
	protofrg.process(protos/'mbus/mbus.proto', extra_args : [ '-I' + protos/'mbus' ]),
	protofrg.process(protos/'clock/clock.proto', extra_args : [ '-I' + protos/'clock' ]),
	protofrg.process(protos/'kerncfg/kerncfg.proto', extra_args : [ '-I' + protos/'kerncfg' ]),
	protofrg.process(protos/'kernlet/kernlet.proto', extra_args : [ '-I' + protos/'kernlet' ]),
	protofrg.process(protos/'svrctl/svrctl.proto', extra_args : [ '-I' + protos/'svrctl' ])
]

src += frgbragi.process([ 
	protos/'fs/fs.bragi', 
	protos/'hw/hw.bragi',
	protos/'ostrace/ostrace.bragi',
	protos/'posix/posix.bragi'
])

src += files(
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
	'generic/kernel-stack.cpp',
	'generic/main.cpp',
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
	'system/framebuffer/boot-screen.cpp',
	'system/framebuffer/fb.cpp',
	'system/pci/dmalog.cpp',
	'system/pci/pci_discover.cpp',
	'system/pci/pci_legacy.cpp',
	'system/pci/pcie_ecam.cpp',
	'system/pci/pcie_brcmstb.cpp'
)

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
	'../../subprojects/libarch/include',
	'../../tools/pb2frigg/include',
	'../../protocols/posix/include',
	'../../hel/include'
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
		'system/acpi/glue.cpp',
		'system/acpi/madt.cpp',
		'system/acpi/pm-interface.cpp',
		'system/pci/pci_acpi.cpp'
	)

	lai_lib = static_library('lai', lai_sources,
		include_directories : lai_includes,
		c_args : c_args + args,
		pic : false
	)

	inc += lai_includes
	link_with += lai_lib
endif

link_with += static_library('cralgo', cralgo_sources,
	include_directories : [klibc_inc, cralgo_includes],
	cpp_args : cpp_args + args,
	dependencies : cxxshim_dep,
	pic : false
)

deps = [ coroutines, cxxshim_dep, frigg, libsmarter, bragi_dep, libasync_dep ]

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
