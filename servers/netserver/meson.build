src = [ 
	'src/main.cpp',
	'src/nic.cpp',
	'src/ip/ip4.cpp',
	'src/ip/checksum.cpp',
	'src/ip/arp.cpp',
	'src/ip/udp4.cpp',
	'src/ip/tcp4.cpp'
]

executable('netserver', src,
	dependencies : [ fs_dep, mbus_dep, svrctl_dep, nic_virtio_dep ],
	include_directories : 'include',
	install : true
)

custom_target('netserver-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'netserver.bin',
	input: 'netserver.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server'
)
