lewis_dep = cxx.find_library('lewis')

executable('kernletcc', [ 'src/main.cpp', 'src/fafnir.cpp' ],
	dependencies: [ lewis_dep, mbus_dep, kernlet_dep ],
	install: true
)
