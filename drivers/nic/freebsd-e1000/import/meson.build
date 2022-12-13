inc = [ 'include', '../include/' ]

src_files = files(
	'src/e1000_mbx.c',
	'src/e1000_82542.c',
	'src/e1000_phy.c',
	'src/e1000_82543.c',
	'src/e1000_ich8lan.c',
	'src/e1000_82575.c',
	'src/e1000_82540.c',
	'src/e1000_i210.c',
	'src/e1000_82541.c',
	'src/e1000_api.c',
	'src/e1000_manage.c',
	'src/e1000_vf.c',
	'src/e1000_mac.c',
	'src/e1000_82571.c',
	'src/e1000_base.c',
	'src/e1000_nvm.c',
	'src/e1000_80003es2lan.c',
)

nic_freebsd_e1000_import_lib = static_library('nic-freebsd-e1000-import', src_files,
	include_directories : [ inc ],
	install : true,
	c_args : ['-Wno-implicit-fallthrough', '-Wno-unused-parameter']
)

nic_freebsd_e1000_import_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_freebsd_e1000_import_lib
)
