here = meson.current_source_dir()
kerncfg_pb = protocxx.process(here/'kerncfg.proto', 
	extra_args : [ '-I' + here ]
)

kerncfg_dep = declare_dependency(
    sources : kerncfg_pb
)
