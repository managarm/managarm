gen = generator(protoc, arguments: [
		'--plugin=protoc-gen-frigg=' + pb2frigg.path(),
		'--frigg_out=@BUILD_DIR@',
		'@EXTRA_ARGS@',
		'@INPUT@'
	],
	output: '@BASENAME@.frigg_pb.hpp')

pb_sources = [
	gen.process(
			meson.current_source_dir() + '/../../bragi/proto/mbus.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../bragi/proto']),
	gen.process(
			meson.current_source_dir() + '/../../bragi/proto/fs.proto',
			meson.current_source_dir() + '/../../bragi/proto/posix.proto',
		extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../bragi/proto']),
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

thor_sources = files(
	'../../frigg/src/debug.cpp',
	'../../frigg/src/libc.cpp',
	'../../frigg/src/arch_x86/gdt.cpp',
	'../../frigg/src/arch_x86/idt.cpp',
	'../../frigg/src/arch_x86/tss.cpp',
	'../common/font-8x16.cpp',
	'arch/x86/early_stubs.S',
	'arch/x86/stubs.S',
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
	'arch/x86/vmx.cpp',
	'generic/physical.cpp',
	'generic/main.cpp',
	'generic/service.cpp',
	'generic/hel.cpp',
	'generic/cancel.cpp',
	'generic/core.cpp',
	'generic/fiber.cpp',
	'generic/ipc-queue.cpp',
	'generic/usermem.cpp',
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
		'@INPUT@'],
	input: 'extra-src/trampoline.S',
	output: 'trampoline.bin')

embed_trampoline = custom_target('embed-trampoline',
	command: ['x86_64-managarm-objcopy', '-I', 'binary', '-O', 'elf64-x86-64',
			'-B', 'i386:x86-64', '@INPUT@', '@OUTPUT@'],
	input: trampoline,
	output: 'embed-trampoline.o')

lai_lib = static_library('lai', lai_sources,
	include_directories: lai_includes,
	c_args: [
		'-mcmodel=kernel',
		'-mno-red-zone'
	],
	pic: false
)

executable('thor', pb_sources, thor_sources, embed_trampoline,
	include_directories: [
		include_directories(
			'c_headers/',
			'../include/',
			'../common',
			'../../frigg/include',
			'../../libarch/include',
			'../../tools/pb2frigg/include',
			'../../subprojects/acpica/source/include'
		),
		lai_includes
	],
	dependencies: [
		clang_coroutine_dep,
		cxxshim_dep,
		frigg_dep,
		libsmarter_dep
	],
	cpp_args: [
		'-std=c++17',
		'-mcmodel=kernel',
		'-mno-red-zone',
		'-DCXXSHIM_INTEGRATE_GCC',
		'-DFRIGG_NO_LIBC',
		'-Wall',
		'-Wno-non-virtual-dtor'
	],
	link_with: lai_lib,
	link_args: [
		'-nostdlib',
		'-z', 'max-page-size=0x1000',
		'-Wl,-T,' + meson.current_source_dir() + '/arch/x86/link.x'
	],
	link_depends: files('arch/x86/link.x'),
	install: true
)
