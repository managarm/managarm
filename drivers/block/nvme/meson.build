src = [
    'src/main.cpp',
    'src/controller.cpp',
    'src/queue.cpp',
    'src/command.cpp',
    'src/namespace.cpp',
]

executable('block-nvme', src,
    dependencies : [ libarch, hw_proto_dep, mbus_proto_dep, libblockfs_dep ],
    install : true
)
