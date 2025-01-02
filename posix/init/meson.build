executable('posix-init', 'src/stage1.cpp',
	dependencies: [core_dep, frigg, hel_dep, bragi_dep, mbus_proto_dep],
	install : true,
)

executable('init-stage2', 'src/stage2.cpp',
	dependencies : libudev_dep,
	install : true
)
