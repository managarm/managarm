inc = [ 'include' ]

headers = [
	'include/core/align.hpp',
	'include/core/bpf.hpp',
	'include/core/cancel-events.hpp',
	'include/core/clock.hpp',
	'include/core/cmdline.hpp',
	'include/core/device-path.hpp',
	'include/core/dispatch.hpp',
	'include/core/logging.hpp',
	'include/core/id-allocator.hpp',
	'include/core/kernel-logs.hpp',
	'include/core/netlink.hpp',
	'include/core/process-data.hpp',
	'include/core/queue.hpp',
	'include/core/smbios.hpp',
	'include/core/tty.hpp',
	'include/core/mount.hpp',
]

core_lib_sources = files(
	'lib/bpf/bpf.cpp',
	'lib/clock.cpp',
	'lib/cmdline.cpp',
	'lib/kernel-logs.cpp',
	'lib/process-data.cpp',
	'lib/smbios.cpp',
)

arch_inc = include_directories('arch/include')

core_lib = static_library('core-lib', core_lib_sources,
	include_directories: [ inc, arch_inc ],
	dependencies: [ libarch_base, frigg, hel_dep, bragi_dep, mbus_proto_dep, kerncfg_proto_dep, clock_proto_dep, posix_extra_dep ]
)

core_dep = declare_dependency(
	include_directories : inc,
	link_with : core_lib,
)

install_headers(headers, subdir : 'core')
