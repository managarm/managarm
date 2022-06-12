project('managarm', ['c', 'cpp'],
	license : 'MIT',
	meson_version : '>=0.57.0',
	default_options : [
		'cpp_std=gnu++20',
		'c_std=gnu11',
		'warning_level=2'
	]
)

add_project_arguments('-Wimplicit-fallthrough', language: ['c', 'cpp'])

# build documentation
if get_option('build_docs')
	subdir('docs')

	doxyfile = configure_file(
		input : 'hel/Doxyfile.in',
		output : 'Doxyfile',
		configuration : {
			'ROOTDIR': meson.current_source_dir()/'hel'
		}
	)

	doxygen = find_program('doxygen', required : false)
	if doxygen.found()
		custom_target('hel-doxygen',
			input : doxyfile,
			output : 'hel-api',
			depend_files : 'hel/include/hel.h',
			command : [ doxygen, doxyfile ]
		)
	endif
endif

build_kernel = get_option('build_kernel')
build_drivers = get_option('build_drivers')
build_tools = get_option('build_tools')

any_userspace = build_drivers or build_tools

if build_kernel and any_userspace
	error('Cannot build kernel and userspace components at the same time')
endif

# Set flavor = {'kernel', 'userspace', 'none'}.
# Use this variable below if we need conflicting settings in each of the cases.
if build_kernel
	flavor = 'kernel'
elif any_userspace
	flavor = 'userspace'
else
	flavor = 'none'
endif

summary('Build flavor', flavor, section : 'Configuration')

if flavor == 'kernel'
	if not meson.is_cross_build()
		error('Kernel can only be cross-compiled')
	endif
elif flavor == 'userspace'
	summary({'System' : build_drivers,
			'Tools' : build_tools},
		section : 'Userspace Components', bool_yn : true)
endif

if not build_kernel and not build_drivers and not build_tools
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
frame_pointers = get_option('kernel_frame_pointers')

supported_archs = [
	'aarch64',
	'riscv64',
	'x86_64'
]
if not supported_archs.contains(arch)
	error('unknown architecture ' + arch)
endif

protoc = find_program('protoc')
bragi = find_program('bragi')

frigg = dependency('frigg',
	default_options : [ 'frigg_no_install=true' ],
	fallback: ['frigg', 'frigg_dep'],
)

if build_tools
	subdir('tools/pb2frigg')
else
	pb2frigg = find_program('pb2frigg')
endif

# declare generators that subdirs are going to use
protocxx = generator(protoc,
	output : [ '@BASENAME@.pb.h', '@BASENAME@.pb.cc' ],
	arguments : [ '--cpp_out=@BUILD_DIR@', '@EXTRA_ARGS@', '@INPUT@' ]
)

protofrg = generator(protoc,
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

# If we are building the kernel, we need the freestanding subset of libarch.
if flavor == 'kernel'
	libarch_opts = [ 'install_headers=false', 'header_only=true' ]
elif flavor == 'userspace'
	libarch_opts = [ 'install_headers=false' ]
endif

if build_kernel or build_drivers
	cxxshim_dep = dependency('cxxshim', fallback: ['cxxshim', 'cxxshim_dep'])
	coroutines = dependency('cxxshim-coroutine-std', fallback: ['cxxshim', 'std_coroutine_dep'])

	libarch = subproject('libarch',
		default_options : libarch_opts
	).get_variable('libarch_dep')
endif

if build_kernel
	cralgo = subproject('cralgo')
	lai = subproject('lai')
	libasync = subproject('libasync', default_options : [ 'install_headers=false' ])
	cralgo_sources = cralgo.get_variable('sources')
	cralgo_includes = cralgo.get_variable('includes')
	lai_sources = lai.get_variable('sources')
	lai_includes = lai.get_variable('includes')
	libasync_dep = libasync.get_variable('libasync_dep')

	subdir('kernel/eir')
	subdir('kernel/thor')
endif

if build_drivers
	bakesvr = find_program('bakesvr')

	proto_lite_dep = dependency('protobuf-lite')
	libudev_dep = dependency('libudev')
	cli11_dep = dependency('CLI11')

	# this produces the helix dependency which
	# all protocols depend on
	subdir('hel')

	protocols = [ 'posix', 'clock', 'fs', 'hw', 'mbus', 'usb', 'svrctl', 'kerncfg', 'kernlet', 'ostrace' ]
	core = [ 'core/drm', 'core/virtio', 'mbus' ]
	posix = [ 'subsystem', 'init' ]
	drivers = [
		# libraries
		'libblockfs', 'libevbackend',
		# storage
		'block/ata', 'block/virtio-blk', 'block/ahci', 'block/nvme',
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
	testsuites = [ 'kernel-bench', 'kernel-tests', 'posix-torture', 'posix-tests', 'virt-test' ]

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
if build_tools
	cli11_dep = dependency('CLI11')

	foreach tool : [ 'ostrace', 'bakesvr' ]
		subdir('tools'/tool)
	endforeach
endif
