custom_target('bakesvr-pb',
	command: [
		protoc,
		'--python_out=@OUTDIR@',
		'--proto_path=' + protos/'svrctl',
		'@INPUT@'
	],
	input: protos/'svrctl/svrctl.proto',
	output: 'svrctl_pb2.py',
	install: true,
	install_dir: get_option('datadir') / 'bakesvr'
)

install_data('bakesvr', install_dir: get_option('bindir'))
