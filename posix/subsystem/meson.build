gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@',
			'@EXTRA_ARGS@',
			'@INPUT@'])

bragi_gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = bragi_gen.process('../../protocols/fs/fs.bragi')
posix_bragi = bragi_gen.process('../../protocols/posix/posix.bragi')

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
		fs_bragi,
		kerncfg_pb,
		posix_bragi],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep,
		libfs_protocol_dep,
		bragi_dep,
		clock_protocol_dep],
	install: true)
