eir_sources = [files('load64.S', 'arch.cpp', 'fault.S', 'fault.cpp', 'generic.cpp'), eir_generic_sources]
eir_includes += include_directories('.')
eir_cpp_args += ['-mgeneral-regs-only', '-mno-unaligned-access']
eir_c_args += ['-mgeneral-regs-only', '-mno-unaligned-access']

subdir('raspi4')
subdir('virt')
