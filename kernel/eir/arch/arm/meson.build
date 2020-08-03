eir_sources = [files('load64.S', 'arch.cpp'), eir_generic_sources]
eir_includes += include_directories('.')
eir_cpp_args += ['-mgeneral-regs-only', '-mno-unaligned-access', '-DCXXSHIM_INTEGRATE_GCC']
eir_c_args += ['-mgeneral-regs-only', '-mno-unaligned-access']

subdir('raspi4/')
