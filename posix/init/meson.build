
executable('posix-init', ['src/stage1.cpp'],
	install: true)

executable('init-stage2', ['src/stage2.cpp'],
	dependencies: [
		libudev_dep
	],
	install: true)

