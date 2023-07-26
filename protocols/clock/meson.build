clock_bragi = cxxbragi.process('clock.bragi')

clock_proto_dep = declare_dependency(
	sources : clock_bragi,
	include_directories : 'include'
)

install_headers('include/protocols/clock/defs.hpp',
	subdir : 'protocols/clock'
)
