
project('managarm', 'c', 'cpp')
add_project_arguments('--std=c++17', language: 'cpp')
c_compiler = meson.get_compiler('cpp')
cpp_compiler = meson.get_compiler('cpp')

protoc = find_program('protoc')

# clang needs an additional flag to enable sized allocation/deallocation.
if cpp_compiler.get_id() == 'clang'
	add_project_arguments('-fsized-deallocation', language: 'cpp')
endif

if get_option('build_kernel')
	frigg_pb = find_program('frigg_pb')

	add_global_arguments(
		[
			'-ffreestanding',
			'-mno-80387',
			'-mno-mmx',
			'-mno-sse',
			'-mno-sse2',
		],
		language: ['c', 'cpp'])
	add_global_arguments(['-fno-exceptions', '-fno-rtti'], language: 'cpp')

	cxxshim = subproject('cxxshim')
	frigg = subproject('frigg')
	lai = subproject('lai')
	libsmarter = subproject('libsmarter')
	cxxshim_dep = cxxshim.get_variable('cxxshim_dep')
	frigg_dep = frigg.get_variable('frigg_dep')
	lai_sources = lai.get_variable('sources')
	lai_includes = lai.get_variable('includes')
	libsmarter_dep = libsmarter.get_variable('libsmarter_dep')

	subdir('kernel/eir')
	subdir('kernel/thor')
endif

if get_option('build_drivers')
	bakesvr = find_program('bakesvr')

	add_project_arguments('-fcoroutines-ts', language: 'cpp')

	cxxshim = subproject('cxxshim')
	clang_coroutine_dep = cxxshim.get_variable('clang_coroutine_dep')

	proto_lite_dep = dependency('protobuf-lite')
	libudev_dep = dependency('libudev')

	subdir('hel')

	subdir('libarch/')
	subdir('protocols/clock/')
	subdir('protocols/fs/')
	subdir('protocols/hw/')
	subdir('protocols/mbus/')
	subdir('protocols/usb/')
	subdir('protocols/svrctl/')
	subdir('protocols/kernlet/') # Depends on mbus.
	subdir('core/drm/')
	subdir('core/virtio/')
	subdir('mbus/')
	subdir('posix/subsystem/')
	subdir('posix/init/')
	subdir('drivers/libblockfs/')
	subdir('drivers/libevbackend/')
	subdir('drivers/block/ata')
	subdir('drivers/block/virtio-blk/')
	subdir('drivers/gfx/bochs/')
	subdir('drivers/gfx/intel/')
	subdir('drivers/gfx/virtio/')
	subdir('drivers/gfx/plainfb/')
	subdir('drivers/gfx/vmware/')
	subdir('drivers/kbd/')
	subdir('drivers/nic/virtio/')
	subdir('drivers/uart/')
	subdir('drivers/usb/hcds/uhci/')
	subdir('drivers/usb/hcds/ehci/')
	subdir('drivers/usb/hcds/xhci/')
	subdir('drivers/usb/devices/hid/')
	subdir('drivers/usb/devices/storage/')
	subdir('drivers/kernletcc')
	subdir('utils/runsvr/')
	subdir('utils/lsmbus/')
	subdir('testsuites/posix-torture/')

	subdir('drivers/clocktracker')

	install_data(
		[
			'drivers/gfx/90-managarm-vga.rules',
			'drivers/nic/90-managarm-nic.rules',
		],
		install_dir: 'lib/udev/rules.d')
endif

if get_option('build_tools')
	subdir('tools/bakesvr')
	subdir('tools/legacy-frigg_pb')
	subdir('tools/pb2frigg')
endif

