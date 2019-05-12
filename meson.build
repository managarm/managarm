
project('managarm', 'c', 'cpp')
add_project_arguments('--std=c++17', language: ['cpp'])
c_compiler = meson.get_compiler('cpp')
cpp_compiler = meson.get_compiler('cpp')

if get_option('build_kernel')
	protoc = find_program('protoc')
	frigg_pb = find_program('frigg_pb')

	cxxshim = subproject('cxxshim')
	frigg = subproject('frigg')
	libsmarter = subproject('libsmarter')
	cxxshim_dep = cxxshim.get_variable('cxxshim_dep')
	frigg_dep = frigg.get_variable('frigg_dep')
	libsmarter_dep = libsmarter.get_variable('libsmarter_dep')

	subdir('kernel/eir')
	subdir('kernel/thor')
endif

if get_option('build_drivers')
	protoc = find_program('protoc')

	proto_lite_dep = dependency('protobuf-lite')
	lib_cofiber_dep = cpp_compiler.find_library('cofiber')

	subdir('hel')

	subdir('libarch/')
	subdir('protocols/clock/')
	subdir('protocols/fs/')
	subdir('protocols/hw/')
	subdir('protocols/mbus/')
	subdir('protocols/usb/')
	subdir('protocols/kernlet/') # Depends on mbus.
	subdir('core/drm/')
	subdir('core/virtio/')
	subdir('mbus/')
	subdir('posix/subsystem/')
	subdir('posix/init/')
	subdir('drivers/libblockfs/')
	subdir('drivers/libevbackend/')
	subdir('drivers/block/ata')
	subdir('drivers/gfx/bochs/')
	subdir('drivers/gfx/intel/')
	subdir('drivers/gfx/virtio/')
	subdir('drivers/gfx/plainfb/')
	subdir('drivers/gfx/vmware/')
	subdir('drivers/kbd/')
	subdir('drivers/uart/')
	subdir('drivers/usb/hcds/uhci/')
	subdir('drivers/usb/hcds/ehci/')
	subdir('drivers/usb/devices/hid/')
	subdir('drivers/usb/devices/storage/')
	subdir('drivers/virtio/')
	subdir('drivers/kernletcc')
	subdir('utils/runsvr/')

	subdir('drivers/clocktracker')

	install_data('drivers/gfx/90-managarm-vga.rules',
		install_dir: 'lib/udev/rules.d')
endif

if get_option('build_tools')
	subdir('tools/frigg_pb')
endif

