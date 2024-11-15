eir_uefi_sources = [
	'entry.cpp',
	'helpers.cpp',
]

eir_uefi_cpp_args = ['-DEIR_UEFI', '-ffreestanding', '-fshort-wchar', '-fno-stack-protector']
eir_uefi_ld_args = ['-nostdlib']
eir_uefi_link_depends = []

# not all architectures have a native PE toolchain (e.g. riscv64)
native_pe_toolchain = meson.get_external_property('native_pe_toolchain')

if native_pe_toolchain
	eir_uefi_cpp_args += ['-DEIR_NATIVE_PE', '-gdwarf']
	eir_uefi_ld_args += [
		'-Wl,-entry:eirUefiMain',
		'-Wl,-subsystem:efi_application',
		'-Wl,-lld-allow-duplicate-weak',
		'-gdwarf'
	]
	eir_uefi_exe_name = 'eir-uefi'
	eir_uefi_linker_script = files()
else
	eir_uefi_ld_args += [
		'-Wl,-entry,eirUefiMain',
		'-Wl,--defsym=PECOFF_HEADER_SIZE=' + meson.get_external_property('pecoff_header_size').to_string()
	]
	eir_uefi_exe_name = 'eir-uefi-elf'
	eir_uefi_linker_script = files(meson.current_source_dir() / 'non-pe-native-link.x')
endif

foreach script : eir_uefi_linker_script
	eir_uefi_ld_args += ['-Wl,-T' + script.full_path()]
	eir_uefi_link_depends += [script]
endforeach

eir_uefi_exe = executable(eir_uefi_exe_name, eir_uefi_sources + eir64_sources, eir_generic_main,
	include_directories : [eir_includes],
	dependencies : [freestnd_cxx_hdrs_dep, eir_dependencies],
	c_args: [eir_c_args, eir_uefi_cpp_args],
	cpp_args: [eir_cpp_args, eir_uefi_cpp_args],
	link_args: eir_uefi_ld_args,
	link_depends: eir_uefi_link_depends,
	pie: not native_pe_toolchain,
	install : true
)

if not native_pe_toolchain
	genfw = find_program('GenFw', required: true)

	# GenFw takes an ELF and produces a PE
	custom_target('eir-uefi',
		command: [genfw, '-e', 'UEFI_APPLICATION', '-o', '@OUTPUT@', '@INPUT@'],
		input: eir_uefi_exe,
		output: 'eir-uefi',
		install: true,
		install_dir: get_option('bindir')
	)
endif
