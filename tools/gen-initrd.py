#!/usr/bin/python3

import os
import shutil
import subprocess
import tempfile
import sys
import argparse

parser = argparse.ArgumentParser(description = 'Generate a managarm initrd')
parser.add_argument('-t', '--triple', dest = 'arch',
		choices = ['x86_64-managarm', 'aarch64-managarm'], default = 'x86_64-managarm',
		help = 'Target system triple (default: x86_64-managarm)')

args = parser.parse_args()

class Entry:
	__slots__ = ('is_dir', 'source', 'strip')

	def __init__(self, is_dir=False, source=None, strip=False):
		self.is_dir = is_dir
		self.source = source
		self.strip = strip

file_dict = dict()

def add_dir(rel_path):
	file_dict[rel_path] = Entry(is_dir=True)

def add_file(src_prefix, tree_prefix, filename, rename_to=None, strip=False):
	dest_filename = filename if rename_to is None else rename_to
	src_path = os.path.join(src_prefix, filename)
	rel_path = os.path.join(tree_prefix, dest_filename)
	file_dict[rel_path] = Entry(source=src_path, strip=strip)

# Add all the files.

add_dir('bin')
add_dir('sbin')
add_dir('lib')
add_dir('managarm')
add_dir('managarm/server')

# Runtime libraries.
add_file('system-root/usr/lib', 'lib', 'libhelix.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libc.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libm.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'ld.so', rename_to='ld-init.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libz.so.1', strip=True)
add_file('system-root/usr/lib', 'lib', 'libz.so.1', strip=True)
add_file('system-root/usr/lib', 'lib', 'liblewis.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libz.so.1', strip=True)
add_file('system-root/usr/lib', 'lib', 'libprotobuf-lite.so.11', strip=True)
add_file('system-root/usr/lib', 'lib', 'libvirtio_core.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libarch.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libfs_protocol.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libhw_protocol.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libkernlet_protocol.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libmbus.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libostrace_protocol.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libsvrctl_protocol.so', strip=True)
add_file('system-root/usr/lib', 'lib', 'libusb_protocol.so', strip=True)
add_file(f'tools/system-gcc/{args.arch}/lib64/', 'lib', 'libgcc_s.so.1', strip=True)
add_file(f'tools/system-gcc/{args.arch}/lib64/', 'lib', 'libstdc++.so.6', strip=True)

# User-space core components.
add_file('system-root/usr/bin', 'sbin', 'mbus', strip=True)
add_file('system-root/usr/bin', 'sbin', 'kernletcc', strip=True)
add_file('system-root/usr/bin', 'sbin', 'clocktracker', strip=True)
add_file('system-root/usr/bin', 'sbin', 'posix-subsystem', strip=True)
add_file('system-root/usr/bin', 'sbin', 'posix-init', strip=True)

# Essential drivers.
add_file('system-root/usr/lib', 'lib', 'libblockfs.so', strip=True)
add_file('system-root/usr/bin', 'sbin', 'uhci', strip=True)
add_file('system-root/usr/bin', 'sbin', 'ehci', strip=True)
add_file('system-root/usr/bin', 'sbin', 'xhci', strip=True)
add_file('system-root/usr/bin', 'sbin', 'block-ata', strip=True)
add_file('system-root/usr/bin', 'sbin', 'block-ahci', strip=True)
add_file('system-root/usr/lib/managarm/server', 'lib', 'block-ahci.bin')
add_file('system-root/usr/bin', 'sbin', 'storage', strip=True)
add_file('system-root/usr/bin', 'sbin', 'virtio-block', strip=True)
add_file('system-root/usr/bin', 'sbin', 'virtio-console', strip=True)
add_file('system-root/usr/bin', 'sbin', 'uart', strip=True)

# Essential utilities.
add_file('system-root/usr/bin', 'bin', 'runsvr', strip=True)

# Server descriptions.
for fname in os.listdir('system-root/usr/lib/managarm/server'):
	if not fname.endswith('.bin'):
		continue
	add_file('system-root/usr/lib/managarm/server', 'managarm/server', fname)

# Copy (= hard link) the files to a temporary directory, run GNU cpio.

tree_path = tempfile.mkdtemp(prefix='initrd-', dir='.')

file_list = sorted(file_dict.keys())
for rel_path in file_list:
	entry = file_dict[rel_path]
	dest_path = os.path.join(tree_path, rel_path)

	if entry.is_dir:
		os.mkdir(dest_path)
	elif entry.strip:
		subprocess.check_call([f'{args.arch}-strip', '-o', dest_path, entry.source])
	else:
		os.link(entry.source, dest_path)

proc = subprocess.Popen(['cpio', '--create', '--format=newc',
			'-D', tree_path,
			'--file', 'initrd.cpio',
			'--quiet'],
		stdin=subprocess.PIPE,
		encoding='ascii')
proc.communicate(input='\n'.join(file_list))
if proc.returncode != 0:
	sys.exit(1)

shutil.rmtree(tree_path)
