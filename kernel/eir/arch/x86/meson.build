eir_x86_sources = files(
	'arch.cpp')

eir_cpp_args += ['-mno-80387', '-mno-mmx', '-mno-sse', '-mno-sse2']
eir_c_args += ['-mno-80387', '-mno-mmx', '-mno-sse', '-mno-sse2']
eir_includes += include_directories('.')

eir32_sources = [
	files('load32.S', 'i386.cpp'),
	eir_generic_sources,
	eir_x86_sources]
eir32_c_args = [eir_c_args, '-m32']
eir32_cpp_args = [eir_cpp_args, '-m32']
# -lgcc here is a bit of a hack, as Meson attempts to resolve libgcc to it's
# non-multilibbed path, which fails to be useful here.
eir32_link_args = [eir_link_args, '-m32', '-Wl,-T,' + meson.current_source_dir() + '/generic32_link.x', '-lgcc']
eir32_dependencies = eir_dependencies
eir32_extra_objects = []
eir32_link_depends = files('generic32_link.x')

eir64_sources = [
	files('load64.S', 'x86_64.cpp'),
	eir_generic_sources,
	eir_x86_sources]
eir64_c_args = eir_c_args
eir64_cpp_args = eir_cpp_args
eir64_link_args_without_link_script = [eir_link_args, '-Wl,-z,max-page-size=0x1000']
eir64_link_args = ['-Wl,-T,' + meson.current_source_dir() + '/generic64_link.x', eir64_link_args_without_link_script]
eir64_dependencies = eir_dependencies
eir64_extra_objects = []
eir64_link_depends = files('generic64_link.x')

if not build_uefi
	subdir('multiboot2')
endif
