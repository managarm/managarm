project('managarm', ['c', 'cpp'],
	license : 'MIT',
	meson_version : '>=0.57.0',
	default_options : [
		'cpp_std=c++20',
		'c_std=c11',
		'warning_level=3'
	]
)

# declare constants that subdirs are going to use
c = meson.get_compiler('cpp')
cxx = meson.get_compiler('cpp')
arch = host_machine.cpu_family()
root = meson.project_source_root()
protos = root/'protocols'

if arch != 'aarch64' and arch != 'x86_64'
	error('unknown architecture ' + arch)
endif

# declare generators that subdirs are going to use
protoc = generator(find_program('protoc'),
	output : [ '@BASENAME@.pb.h', '@BASENAME@.pb.cc' ],
	arguments : [ '--cpp_out=@BUILD_DIR@', '@EXTRA_ARGS@', '@INPUT@' ]
)

bragi = find_program('bragi')

cxxbragi = generator(bragi,
	output : '@BASENAME@.bragi.hpp',
	arguments : [ '-l', 'stdc++', '--protobuf', '@INPUT@', '@OUTPUT@' ]
)

friggbragi = generator(bragi,
	output : '@BASENAME@.frigg_bragi.hpp',
	arguments : [ '-l', 'frigg', '--protobuf', '@INPUT@', '@OUTPUT@' ]
)

# declare dependencies that subdirs are going to use
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

	cralgo = subproject('cralgo')
	libarch = subproject('libarch', default_options: ['install_headers=false', 'header_only=true'])
	libarch_dep = libarch.get_variable('libarch_dep')
	lai = subproject('lai')
	cralgo_sources = cralgo.get_variable('sources')
	cralgo_includes = cralgo.get_variable('includes')
	cxxshim_dep = cxxshim.get_variable('cxxshim_dep')
	lai_sources = lai.get_variable('sources')
	lai_includes = lai.get_variable('includes')

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

	install_data(
		[ 'drivers/gfx/90-managarm-vga.rules', 'drivers/nic/90-managarm-nic.rules' ],
		install_dir: 'lib/udev/rules.d'
	)
endif

# when building these tools make sure they stay below everything else
# as they depend on parts above
if get_option('build_tools')
	foreach tool : [ 'ostrace', 'basesvr', 'pb2frigg' ]
		subdir('tools'/tool)
	endforeach
endif

subdir('docs')
