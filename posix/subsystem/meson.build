gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@',
			'@EXTRA_ARGS@',
			'@INPUT@'])

fs_pb = gen.process(meson.current_source_dir() + '/../../protocols/fs/fs.proto',
	extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/fs'])
posix_pb = gen.process(meson.current_source_dir() + '/../../protocols/posix/posix.proto',
	extra_args: ['--proto_path=' + meson.current_source_dir() + '/../../protocols/posix'])

gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@',
			'--proto_path=@CURRENT_SOURCE_DIR@../../protocols/clock',
			'@INPUT@'])
clock_pb = gen.process('../../protocols/clock/clock.proto')

gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@',
			'--proto_path=@CURRENT_SOURCE_DIR@../../protocols/kerncfg',
			'@INPUT@'])
kerncfg_pb = gen.process('../../protocols/kerncfg/kerncfg.proto')

posix_subsystem_inc = include_directories('../../frigg/include')

executable('posix-subsystem',
		['src/clock.cpp',
		'src/device.cpp',
		'src/devices/helout.cpp',
		'src/drvcore.cpp',
		'src/epoll.cpp',
		'src/exec.cpp',
		'src/extern_fs.cpp',
		'src/extern_socket.cpp',
		'src/fifo.cpp',
		'src/file.cpp',
		'src/fs.cpp',
		'src/net.cpp',
		'src/inotify.cpp',
		'src/main.cpp',
		'src/nl-socket.cpp',
		'src/process.cpp',
		'src/procfs.cpp',
		'src/pts.cpp',
		'src/signalfd.cpp',
		'src/subsystem/block.cpp',
		'src/subsystem/drm.cpp',
		'src/subsystem/input.cpp',
		'src/subsystem/pci.cpp',
		'src/sysfs.cpp',
		'src/timerfd.cpp',
		'src/eventfd.cpp',
		'src/tmp_fs.cpp',
		'src/un-socket.cpp',
		'src/vfs.cpp',
		clock_pb,
		fs_pb,
		kerncfg_pb,
		posix_pb],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep,
		libfs_protocol_dep,
		clock_protocol_dep],
	include_directories: posix_subsystem_inc,
	install: true)
