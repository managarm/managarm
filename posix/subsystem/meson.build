posix_bragi = cxxbragi.process(protos/'posix/posix.bragi')

src = [
	'src/clock.cpp',
	'src/device.cpp',
	'src/devices/full.cpp',
	'src/devices/helout.cpp',
	'src/devices/null.cpp',
	'src/devices/zero.cpp',
	'src/drvcore.cpp',
	'src/epoll.cpp',
	'src/eventfd.cpp',
	'src/exec.cpp',
	'src/extern_fs.cpp',
	'src/extern_socket.cpp',
	'src/fifo.cpp',
	'src/file.cpp',
	'src/fs.cpp',
	'src/gdbserver.cpp',
	'src/inotify.cpp',
	'src/main.cpp',
	'src/net.cpp',
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
	'src/tmp_fs.cpp',
	'src/un-socket.cpp',
	'src/vfs.cpp',
	posix_bragi
]

executable('posix-subsystem', src,
	dependencies : [ mbus_dep, fs_dep, clock_dep, kerncfg_dep ],
	install : true
)
