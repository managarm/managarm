gen = generator(protoc, arguments: [
		'--plugin=protoc-gen-frigg=' + pb2frigg.path(),
		'--frigg_out=@BUILD_DIR@',
		'@EXTRA_ARGS@',
		'@INPUT@'
	],
	output: '@BASENAME@.frigg_pb.hpp')

bragi_gen = generator(bragi, arguments: [
		'-l', 'frigg',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.frigg_bragi.hpp')

pb_sources = [
	gen.process(
			meson.current_source_dir() + '/../../protocols/mbus/mbus.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/mbus']),
	gen.process(
			meson.current_source_dir() + '/../../protocols/clock/clock.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/clock']),
	gen.process(
			meson.current_source_dir() + '/../../protocols/hw/hw.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/hw']),
	gen.process(
			meson.current_source_dir() + '/../../protocols/kerncfg/kerncfg.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/kerncfg']),
	gen.process(
			meson.current_source_dir() + '/../../protocols/kernlet/kernlet.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/kernlet']),
	gen.process(
			meson.current_source_dir() + '/../../protocols/svrctl/svrctl.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/svrctl']),
]

bragi_sources = [
	bragi_gen.process('../../protocols/posix/posix.bragi'),
	bragi_gen.process('../../protocols/fs/fs.bragi'),
]

thor_sources = [files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'generic/address-space.cpp',
	'generic/physical.cpp',
	'generic/main.cpp',
	'generic/memory-view.cpp',
	'generic/service.cpp',
	'generic/hel.cpp',
	'generic/cancel.cpp',
	'generic/core.cpp',
	'generic/debug.cpp',
	'generic/fiber.cpp',
	'generic/ipc-queue.cpp',
	'generic/kasan.cpp',
	'generic/schedule.cpp',
	'generic/futex.cpp',
	'generic/stream.cpp',
	'generic/timer.cpp',
	'generic/thread.cpp',
	'generic/event.cpp',
	'generic/irq.cpp',
	'generic/io.cpp',
	'generic/kerncfg.cpp',
	'generic/kernlet.cpp',
	'generic/profile.cpp',
	'generic/random.cpp',
	'generic/servers.cpp',
	'generic/ubsan.cpp',
	'generic/work-queue.cpp',
	'generic/kernel-stack.cpp',
	'system/framebuffer/boot-screen.cpp',
	'system/framebuffer/fb.cpp',
	'system/pci/pci_discover.cpp'
)]

thor_includes = [include_directories(
	'private-include/',
	'generic/',
	'system/acpi/',
	'system/framebuffer/',
	'system/legacy-pc/',
	'system/pci/',
	'../include/',
	'../common',
	'../../frigg/include',
	'../../subprojects/libarch/include',
	'../../subprojects/libasync/include',
	'../../tools/pb2frigg/include'
)]

extra_c_args = []
extra_cpp_args = ['-DCXXSHIM_INTEGRATE_GCC']
extra_link_args = []
extra_link_depends = []
extra_includes = []
extra_link_with = []
want_acpi = false

if get_option('kernel_ubsan')
	# -fsanitize=function requires RTTI.
	extra_c_args += ['-fsanitize=undefined', '-fno-sanitize=function']
	extra_cpp_args += ['-fsanitize=undefined', '-fno-sanitize=function']
endif
if get_option('kernel_kasan')
	# The shadow memory offset is calculated as:
	#     ShadowMemoryBase - (StartOfHigherHalf/ShadowScale).
	# This ensures that the first shadow byte of the higher half ends up at SharedMemoryBase.
	# Lower half addresses become non-canonical by design
	# (and KASAN should not instrument lower-half access anyway).
	extra_c_args += [
		'-DTHOR_KASAN',
		'-fsanitize=kernel-address',
		'-mllvm', '-asan-mapping-offset=0xdfffe00000000000',
		'-mllvm', '-asan-globals=false',
	]
	extra_cpp_args += [
		'-DTHOR_KASAN',
		'-fsanitize=kernel-address',
		'-mllvm', '-asan-mapping-offset=0xdfffe00000000000',
		'-mllvm', '-asan-globals=false',
	]
endif
if get_option('kernel_log_allocations')
	extra_c_args = ['-fno-omit-frame-pointer']
	extra_cpp_args = ['-fno-omit-frame-pointer', '-DKERNEL_LOG_ALLOCATIONS']
endif

if host_machine.cpu_family() == 'aarch64'
	subdir('arch/arm')
elif host_machine.cpu_family() == 'x86_64'
	subdir('arch/x86')
else
	error('Unknown architecture')
endif

if want_acpi
	thor_sources += files(
		'system/acpi/glue.cpp',
		'system/acpi/madt.cpp',
		'system/acpi/pm-interface.cpp',
		'system/pci/pci_acpi.cpp'
	)

	lai_lib = static_library('lai', lai_sources,
		include_directories: lai_includes,
		c_args: extra_c_args,
		pic: false
	)

	extra_includes += lai_includes
	extra_link_with += lai_lib
endif

cralgo_lib = static_library('cralgo',
	cralgo_sources,
	include_directories: [
		'private-include/',
		cralgo_includes,
	],
	cpp_args: extra_cpp_args,
	dependencies: cxxshim_dep,
	pic: false
)

executable('thor', pb_sources, bragi_sources, thor_sources,
	include_directories: [
		thor_includes,
		cralgo_includes,
		extra_includes
	],
	dependencies: [
		clang_coroutine_dep,
		cxxshim_dep,
		frigg_dep,
		libsmarter_dep,
		bragi_dep
	],
	cpp_args: [
		# For now, we only use static initializers for initgraph stages.
		# If we decide to use them for something more complicated, we have to remove this.
		'-fno-threadsafe-statics',
		'-DLIBASYNC_CUSTOM_PLATFORM',
		'-DFRIGG_NO_LIBC',
		'-Wall',
		'-Wno-non-virtual-dtor',
		extra_cpp_args
	],
	link_with: [
		cralgo_lib,
		extra_link_with
	],
	link_args: [
		'-nostdlib',
		extra_link_args
	],
	link_depends: extra_link_depends,
	install: true
)
