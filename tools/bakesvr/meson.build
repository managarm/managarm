
custom_target('bakesvr-pb',
	command: [
		protoc,
		'--python_out=@OUTDIR@',
		'--proto_path=' + meson.current_source_dir() / '../../protocols/svrctl',
		'@INPUT@'
	],
	input: meson.current_source_dir() / '../../protocols/svrctl/svrctl.proto',
	output: 'svrctl_pb2.py',
	install: true,
	install_dir: get_option('datadir') / 'bakesvr')

install_data('bakesvr',
	install_dir: get_option('bindir'))

