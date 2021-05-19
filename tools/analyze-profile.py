#!/usr/bin/env python3

import argparse
import bisect
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('profile_path', type=str)
parser.add_argument('--aggregate-by',
	choices=['symbol', 'source'], default='symbol',
	help="aggregate samples by source line of code or by symbol inside the binary")
parser.add_argument('--line', action='store_true')
parser.add_argument('--isn', action='store_true')

args = parser.parse_args()

profile = dict()

if args.aggregate_by == 'symbol':
	nm = subprocess.check_output(
		[
			'nm', '-nC',
			'pkg-builds/managarm-kernel/kernel/thor/thor'
		],
		encoding='ascii');

	sym_table = []
	for line in nm.splitlines():
		start, attr, symbol = line.split(' ', 2)
		sym_table.append((int(start, 16), symbol))
	sym_index = [e[0] for e in sym_table]
else:
	addr2line = subprocess.Popen(
		[
			'addr2line', '-sfC',
			'-e', 'pkg-builds/managarm-kernel/kernel/thor/thor'
		],
		encoding='ascii',
		stdin=subprocess.PIPE, stdout=subprocess.PIPE)

n_user = 0
n_kernel = 0
n_resolved = 0

with open(args.profile_path, 'rb') as f:
	while True:
		rec = f.read(8)
		if not rec:
			break
		ip = struct.unpack('Q', rec)[0]
		if ip < (1 << 63):
			n_user += 1
			continue
		else:
			n_kernel += 1

		if args.aggregate_by == 'symbol':
			idx = bisect.bisect_left(sym_index, ip)
			if idx == 0:
				continue
			start, symbol = sym_table[idx - 1];
			assert ip >= start

			loc = symbol, 0
		else:
			addr2line.stdin.write(hex(ip) + '\n')
			addr2line.stdin.flush()
			func = addr2line.stdout.readline().rstrip()
			line = addr2line.stdout.readline().rstrip()
			if args.line:
				loc = (func, line)
			elif args.isn:
				loc = (func, line.split(':')[0] + ':' + hex(ip))
			else:
				loc = (func, line.split(':')[0])

		if loc in profile:
			profile[loc] += 1
		else:
			profile[loc] = 1
		n_resolved += 1

n_all = n_user + n_kernel

out = sorted(profile.keys(), key=lambda loc: profile[loc])
for loc in out:
	print("{:.2f}% ({} samples) in:".format(profile[loc]/n_kernel*100, profile[loc]))
	print("    {} in {}".format(loc[0], loc[1]))
print("{} (= {:.2f}% of all samples) in the kernel".format(n_kernel, n_kernel/(n_user + n_kernel)*100))
print("{:.2f}% of all kernel samples could be resolved".format(n_resolved/n_kernel*100))
