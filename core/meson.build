inc = [ 'include' ]

headers = [
	'include/core/bpf.hpp',
	'include/core/cmdline.hpp',
	'include/core/device-path.hpp',
	'include/core/logging.hpp',
	'include/core/id-allocator.hpp',
	'include/core/kernel-logs.hpp',
	'include/core/queue.hpp',
]

core_lib_sources = files(
	'lib/bpf/bpf.cpp',
	'lib/cmdline.cpp',
	'lib/kernel-logs.cpp',
)

core_lib = static_library('core-lib', core_lib_sources,
	include_directories: [ inc ],
	dependencies: [ libarch, frigg, hel_dep, bragi_dep, mbus_proto_dep, kerncfg_proto_dep ]
)

core_dep = declare_dependency(
	include_directories : inc,
	link_with : core_lib,
)

install_headers(headers, subdir : 'core')
