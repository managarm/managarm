inc = [ 'include' ]

headers = [
	'include/core/bpf.hpp',
	'include/core/logging.hpp',
	'include/core/id-allocator.hpp',
	'include/core/queue.hpp',
]

core_lib_sources = files(
	'lib/bpf/bpf.cpp',
)

core_lib = static_library('core-lib', core_lib_sources,
	include_directories: [ inc ],
	dependencies: [ libarch, frigg ]
)

core_dep = declare_dependency(
	include_directories : inc,
	link_with : core_lib,
)

install_headers(headers, subdir : 'core')
