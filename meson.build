project('managarm', ['c', 'cpp'],
	license : 'MIT',
	meson_version : '>=0.57.0',
	default_options : [
		'cpp_std=c++20',
		'c_std=c11',
		'warning_level=3'
	]
)

c = meson.get_compiler('cpp')
cxx = meson.get_compiler('cpp')
arch = host_machine.cpu_family()

if arch != 'aarch64' and arch != 'x86_64'
	error('unknown architecture ' + arch)
endif

protoc = find_program('protoc')
bragi = find_program('bragi')
bragi_dep = declare_dependency(include_directories: 'subprojects/bragi/include')

cxxshim = subproject('cxxshim')
coroutines = cxxshim.get_variable('clang_coroutine_dep')

frigg = subproject('frigg').get_variable('frigg_dep')

# clang needs an additional flag to enable sized allocation/deallocation.
if cxx.get_id() == 'clang'
	add_project_arguments('-fsized-deallocation', language: 'cpp')
endif

if get_option('build_kernel')
	pb2frigg = find_program('pb2frigg')

	add_global_arguments(['-fno-rtti', '-fno-exceptions'], language : 'cpp')

	cralgo = subproject('cralgo')
	libarch = subproject('libarch', default_options: ['install_headers=false', 'header_only=true'])
	libarch_dep = libarch.get_variable('libarch_dep')
	lai = subproject('lai')
	libsmarter = subproject('libsmarter')
	cralgo_sources = cralgo.get_variable('sources')
	cralgo_includes = cralgo.get_variable('includes')
	cxxshim_dep = cxxshim.get_variable('cxxshim_dep')
	lai_sources = lai.get_variable('sources')
	lai_includes = lai.get_variable('includes')
	libsmarter_dep = libsmarter.get_variable('libsmarter_dep')

	subdir('kernel/eir')
	subdir('kernel/thor')
endif

if get_option('build_drivers')
	bakesvr = find_program('bakesvr')

	add_project_arguments('-DLIBASYNC_FORCE_USE_EXPERIMENTAL', language: 'cpp')

	proto_lite_dep = dependency('protobuf-lite')
	libudev_dep = dependency('libudev')

	libarch = subproject('libarch', default_options: 'install_headers=true')
	libarch_dep = libarch.get_variable('libarch_dep')
endif

subdir('hel')

if get_option('build_drivers')
	# kernlet depends on mbus, so must appear laster on this list
	protocols = [ 'clock', 'fs', 'hw', 'mbus', 'usb', 'svrctl', 'kernlet', 'ostrace' ]
	core = [ 'core/drm', 'core/virtio', 'mbus' ]
	posix = [ 'subsystem', 'init' ]
	drivers = [ 
		# libraries
		'libblockfs', 'libevbackend', 
		# storage
		'block/ata', 'block/virtio-blk', 'block/ahci', 
		# gfx
		'gfx/bochs', 'gfx/intel', 'gfx/virtio', 'gfx/plainfb', 'gfx/vmware',
		# io
		'kbd', 'tty/virtio-console', 'uart',
		# usb
		'usb/hcds/uhci', 'usb/hcds/ehci', 'usb/hcds/xhci',
		'usb/devices/hid', 'usb/devices/storage',
		# misc
		'kernletcc'
	]
	utils = [ 'runsvr', 'lsmbus' ]
	testsuites = [ 'kernel-tests', 'posix-torture', 'posix-tests' ]
	
	# delay these dirs until last as they require other libs
	# to already be built
	delay = [ 'drivers/nic/virtio', 'servers/netserver', 'drivers/clocktracker' ]

	foreach dir : protocols
		subdir('protocols'/dir)
	endforeach

	foreach dir : core
		subdir(dir)
	endforeach

	foreach dir : posix
		subdir('posix'/dir)
	endforeach

	foreach dir : drivers
		subdir('drivers'/dir)
	endforeach

	foreach dir : utils
		subdir('utils'/dir)
	endforeach

	foreach dir : testsuites
		subdir('testsuites'/dir)
	endforeach

	foreach dir : delay
		subdir(dir)
	endforeach
'''
	subdir('protocols/clock')
	subdir('protocols/fs')
	subdir('protocols/hw')
	subdir('protocols/mbus')
	subdir('protocols/usb')
	subdir('protocols/svrctl')
	subdir('protocols/kernlet') # Depends on mbus.
	subdir('protocols/ostrace')
	subdir('core/drm') # depends on fs
	subdir('core/virtio')
	subdir('mbus')
	subdir('posix/subsystem')
	subdir('posix/init')
	subdir('drivers/libblockfs')
	subdir('drivers/libevbackend')
	subdir('drivers/block/ata')
	subdir('drivers/block/virtio-blk')
	subdir('drivers/block/ahci')
	subdir('drivers/gfx/bochs')
	subdir('drivers/gfx/intel')
	subdir('drivers/gfx/virtio')
	subdir('drivers/gfx/plainfb')
	subdir('drivers/gfx/vmware')
	subdir('drivers/kbd')
	subdir('drivers/tty/virtio-console')
	subdir('drivers/uart')
	subdir('drivers/usb/hcds/uhci')
	subdir('drivers/usb/hcds/ehci')
	subdir('drivers/usb/hcds/xhci')
	subdir('drivers/usb/devices/hid')
	subdir('drivers/usb/devices/storage')
	subdir('drivers/kernletcc')
	subdir('utils/runsvr')
	subdir('utils/lsmbus')
	subdir('testsuites/kernel-tests')
	subdir('testsuites/posix-torture')
	subdir('testsuites/posix-tests')

	subdir('drivers/nic/virtio')
	subdir('servers/netserver')

	subdir('drivers/clocktracker')
'''
	install_data(
		[
			'drivers/gfx/90-managarm-vga.rules',
			'drivers/nic/90-managarm-nic.rules',
		],
		install_dir: 'lib/udev/rules.d')
endif

if get_option('build_tools')
	foreach tool : [ 'ostrace', 'basesvr', 'pb2frigg' ]
		subdir('tools'/tool)
	endforeach
endif

subdir('docs')
