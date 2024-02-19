src = [
	'src/ip/arp.cpp',
	'src/ip/checksum.cpp',
	'src/ip/ip4.cpp',
	'src/ip/tcp4.cpp',
	'src/ip/udp4.cpp',
	'src/main.cpp',
	'src/nic.cpp',
	'src/netlink/netlink.cpp',
	'src/netlink/packets.cpp',
	'src/netlink/queries.cpp',
	'src/netlink/utils.cpp',
	posix_bragi
]

inc = [ 'include', 'src' ]

executable('netserver', src,
	dependencies : [ fs_proto_dep, mbus_proto_dep, svrctl_proto_dep, nic_virtio_dep, core_dep ],
	include_directories : inc,
	install : true
)

custom_target('netserver-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'netserver.bin',
	input : 'netserver.yml',
	install : true,
	install_dir : server
)
