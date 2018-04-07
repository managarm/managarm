
project('managarm', 'cpp')
add_project_arguments('--std=c++17', language: ['cpp'])
compiler = meson.get_compiler('cpp')

proto_lite_dep = dependency('protobuf-lite')

protoc = find_program('protoc')

lib_helix_dep = compiler.find_library('helix')
lib_cofiber_dep = compiler.find_library('cofiber')

subdir('libarch/')
subdir('protocols/fs/')
subdir('protocols/hw/')
subdir('protocols/mbus/')
subdir('protocols/usb/')
subdir('core/drm/')
subdir('core/virtio/')
subdir('mbus/')
subdir('posix/subsystem/')
subdir('posix/init/')

