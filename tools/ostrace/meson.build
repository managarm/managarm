bragi_gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

executable('extract-ostrace',
	[
		'extract-ostrace.cpp',
		bragi_gen.process('../../protocols/ostrace/ostrace.bragi'),
	],
	override_options: ['cpp_std=c++20'],
	dependencies: [
		bragi_dep,
		frigg_dep,
	],
	install: true)
