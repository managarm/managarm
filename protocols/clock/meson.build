
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@/../../protocols/clock/', 
			'@INPUT@']) 

clock_pb = custom_target('clock_pb',
	command: [protoc, '--cpp_out=@OUTDIR@',
		'-I@CURRENT_SOURCE_DIR@', '@INPUT@'],
	input: 'clock.proto',
	output: ['clock.pb.h', 'clock.pb.cc'],
	install: true,
	install_dir: ['include/protocols/clock', false])

#clock_pb = gen.process('../../protocols/clock/clock.proto')
clock_proto_inc = include_directories('include/')
#libclock_protocol = shared_library('clock_protocol', [clock_pb],
#	dependencies: [lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
#	include_directories: hw_proto_inc,
#	install: true)

clock_protocol_dep = declare_dependency(
#	link_with: libhw_protocol,
	include_directories: clock_proto_inc)

install_headers('include/protocols/clock/defs.hpp',
	subdir: 'protocols/clock')

