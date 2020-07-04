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
			meson.current_source_dir() + '/../../protocols/fs/fs.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/fs']),
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
]

thor_sources = files(
	'../../frigg/src/debug.cpp',
	'../../frigg/src/libc.cpp',
	'../../frigg/src/arch_x86/gdt.cpp',
	'../../frigg/src/arch_x86/idt.cpp',
	'../../frigg/src/arch_x86/tss.cpp',
	'../common/font-8x16.cpp',
	'arch/x86/early_stubs.S',
	'arch/x86/stubs.S',
	'arch/x86/user-access.S',
	'arch/x86/vmx_stubs.S',
	'arch/x86/cpu.cpp',
	'arch/x86/entry.S',
	'arch/x86/ints.cpp',
	'arch/x86/pic.cpp',
	'arch/x86/system.cpp',
	'arch/x86/paging.cpp',
	'arch/x86/hpet.cpp',
	'arch/x86/rtc.cpp',
	'arch/x86/ept.cpp',
	'arch/x86/pmc-amd.cpp',
	'arch/x86/pmc-intel.cpp',
	'arch/x86/vmx.cpp',
	'generic/address-space.cpp',
	'generic/physical.cpp',
	'generic/main.cpp',
	'generic/memory-view.cpp',
	'generic/service.cpp',
	'generic/hel.cpp',
	'generic/cancel.cpp',
	'generic/core.cpp',
	'generic/fiber.cpp',
	'generic/ipc-queue.cpp',
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
	'generic/service_helpers.cpp',
	'generic/work-queue.cpp',
	'system/boot-screen.cpp',
	'system/fb.cpp',
	'system/pci/pci_io.cpp',
	'system/pci/pci_discover.cpp',
	'system/acpi/glue.cpp',
	'system/acpi/madt.cpp',
	'system/acpi/pm-interface.cpp',
	'system/legacy-pc/ata.cpp'
)

trampoline = custom_target('trampoline',
	command: c_compiler.cmd_array() + ['-o', '@OUTPUT@',
		'-nostdlib', '-Wl,-Ttext,0', '-Wl,--oformat,binary',
		'@INPUT@'] + meson.get_cross_property('cpp_args'),
	input: 'extra-src/trampoline.S',
	output: 'trampoline.bin')

embed_trampoline = custom_target('embed-trampoline',
	command: ['x86_64-managarm-objcopy', '-I', 'binary', '-O', 'elf64-x86-64',
			'-B', 'i386:x86-64', '@INPUT@', '@OUTPUT@'],
	input: trampoline,
	output: 'embed-trampoline.o')

extra_c_args = []
extra_cpp_args = []

if get_option('kernel_log_allocations')
	extra_c_args = ['-fno-omit-frame-pointer']
	extra_cpp_args = ['-fno-omit-frame-pointer', '-DKERNEL_LOG_ALLOCATIONS']
endif

cralgo_lib = static_library('cralgo',
	cralgo_sources,
	include_directories: cralgo_includes,
	cpp_args: [
		'-mcmodel=kernel',
		'-mno-red-zone',
		'-DCXXSHIM_INTEGRATE_GCC',
		extra_cpp_args
	],
	dependencies: cxxshim_dep,
	pic: false
)

lai_lib = static_library('lai', lai_sources,
	include_directories: lai_includes,
	c_args: [
		'-mcmodel=kernel',
		'-mno-red-zone',
		extra_c_args
	],
	pic: false
)

executable('thor', pb_sources, bragi_sources, thor_sources, embed_trampoline,
	include_directories: [
		include_directories(
			'private-include/',
			'generic/',
			'../include/',
			'../common',
			'../../frigg/include',
			'../../subprojects/libarch/include',
			'../../subprojects/libasync/include',
			'../../tools/pb2frigg/include'
		),
		cralgo_includes,
		lai_includes
	],
	dependencies: [
		clang_coroutine_dep,
		cxxshim_dep,
		frigg_dep,
		libsmarter_dep,
		bragi_dep
	],
	cpp_args: [
		'-mcmodel=kernel',
		'-mno-red-zone',
		'-DLIBASYNC_CUSTOM_PLATFORM',
		'-DCXXSHIM_INTEGRATE_GCC',
		'-DFRIGG_NO_LIBC',
		'-Wall',
		'-Wno-non-virtual-dtor',
		extra_cpp_args
	],
	link_with: [
		cralgo_lib,
		lai_lib,
	],
	link_args: [
		'-nostdlib',
		'-z', 'max-page-size=0x1000',
		'-Wl,-T,' + meson.current_source_dir() + '/arch/x86/link.x'
	],
	link_depends: files('arch/x86/link.x'),
	install: true
)
