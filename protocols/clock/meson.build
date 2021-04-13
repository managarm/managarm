here = meson.current_source_dir()
clock_pb = protocxx.process(here/'clock.proto', 
	extra_args : [ '-I' + here ]
)

clock_dep = declare_dependency(
	sources : clock_pb,
	include_directories: 'include'
)

install_headers('include/protocols/clock/defs.hpp',
	subdir: 'protocols/clock'
)
