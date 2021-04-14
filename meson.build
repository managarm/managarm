project('managarm', ['c', 'cpp'],
	license : 'MIT',
	meson_version : '>=0.57.0',
	default_options : [
		'cpp_std=c++20',
		'c_std=c11',
		'warning_level=3'
	]
)

# build documentation
if get_option('only_docs')
	subdir('docs')
	
	doxyfile = configure_file(
		input : 'hel/Doxyfile.in',
		output : 'Doxyfile',
		configuration : {
			'ROOTDIR': meson.current_source_dir()
		}
	)

	doxygen = find_program('doxygen', required : false)
	if doxygen.found()
		custom_target('hel-doxygen',
			input : doxyfile,
			output : 'doc',
			depend_files : 'hel/include/hel.h',
			command : [ doxygen, doxyfile ]
		)
	endif

	subdir_done()
endif

# declare constants that subdirs are going to use
c = meson.get_compiler('cpp')
cxx = meson.get_compiler('cpp')
arch = host_machine.cpu_family()
protos = meson.project_source_root()/'protocols'
server = get_option('libdir')/'managarm/server'
kasan = get_option('kernel_kasan')
ubsan = get_option('kernel_ubsan')
log_alloc = get_option('kernel_log_allocations')

if arch != 'aarch64' and arch != 'x86_64'
	error('unknown architecture ' + arch)
endif

proto = find_program('protoc')
pb2frigg = find_program('pb2frigg')
bragi = find_program('bragi')

# declare generators that subdirs are going to use
protocxx = generator(proto,
	output : [ '@BASENAME@.pb.h', '@BASENAME@.pb.cc' ],
	arguments : [ '--cpp_out=@BUILD_DIR@', '@EXTRA_ARGS@', '@INPUT@' ]
)

protofrg = generator(proto,
	output : '@BASENAME@.frigg_pb.hpp',
	arguments : [ 
		'--plugin=protoc-gen-frigg=' + pb2frigg.full_path(),
		'--frigg_out=@BUILD_DIR@',
		'@EXTRA_ARGS@', '@INPUT@'
	]
)

cxxbragi = generator(bragi,
	output : '@BASENAME@.bragi.hpp',
	arguments : [ '-l', 'stdc++', '--protobuf', '@INPUT@', '@OUTPUT@' ]
)

frgbragi = generator(bragi,
	output : '@BASENAME@.frigg_bragi.hpp',
	arguments : [ '-l', 'frigg', '--protobuf', '@INPUT@', '@OUTPUT@' ]
)

# declare dependencies that subdirs are going to use
bragi_dep = declare_dependency(include_directories : 'subprojects/bragi/include')

cxxshim = subproject('cxxshim')
coroutines = cxxshim.get_variable('clang_coroutine_dep')

frigg = subproject('frigg').get_variable('frigg_dep')

libarch = subproject('libarch').get_variable('libarch_dep')

if get_option('build_kernel')
	cralgo = subproject('cralgo')
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

	proto_lite_dep = dependency('protobuf-lite')
	libudev_dep = dependency('libudev')

	# this produces the helix dependency which
	# all protocols depend on
	subdir('hel')

	protocols = [ 'clock', 'fs', 'hw', 'mbus', 'usb', 'svrctl', 'kerncfg', 'kernlet', 'ostrace' ]
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

	rules = [ 'drivers/gfx/90-managarm-vga.rules', 'drivers/nic/90-managarm-nic.rules' ]

	install_data(rules, install_dir : 'lib/udev/rules.d')
endif

# when building these tools make sure they stay below everything else
# as they depend on parts above
if get_option('build_tools')
	foreach tool : [ 'ostrace', 'basesvr', 'pb2frigg' ]
		subdir('tools'/tool)
	endforeach
endif
