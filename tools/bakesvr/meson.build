src = files(
	'src/main.cpp',
)

src += cxxbragi.process([protos/'svrctl/svrctl.bragi'])

yaml_cpp_subproject = import('cmake').subproject('yaml-cpp')
yaml_cpp_dep = yaml_cpp_subproject.dependency('yaml-cpp')

executable('bakesvr', src,
	dependencies: [yaml_cpp_dep, cli11_dep, bragi_dep],
	install_dir : get_option('bindir'),
	native: true,
	install: true
)
