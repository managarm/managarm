import argparse
import collections
import os
import re
import shlex
import struct
import subprocess

# Indices for e_ident.
EI_CLASS = 4
EI_DATA = 5

# Values for e_machine.
EM_386 = 3
EM_X86_64 = 62
EM_AARCH64 = 183
EM_RISCV = 243

ehdr_tuple = collections.namedtuple("Ehdr", ["e_ident", "e_type", "e_machine"])
ehdr_struct = struct.Struct("16sHH")

# Regexp to extract build ID from readelf output.
build_id_re = re.compile(r"^\s*Build ID: ([a-f0-9]+)$", re.MULTILINE)

# Mappings of e_machine to architecture.
em_32bit_to_arch = {EM_386: "i386"}
em_64bit_to_arch = {
    EM_X86_64: "x86_64",
    EM_AARCH64: "aarch64",
    EM_RISCV: "riscv64",
}

verbose = False


def walk_regular(path):
    for entry in os.scandir(path):
        if entry.is_dir(follow_symlinks=False):
            yield from walk_regular(entry.path)
        elif entry.is_file(follow_symlinks=False):
            yield entry.path


def get_build_id(elf_path):
    args = ["readelf", "-n", elf_path]
    process = subprocess.run(args, check=True, stdout=subprocess.PIPE, text=True)
    build_ids = build_id_re.findall(process.stdout)
    if len(build_ids) > 1:
        raise RuntimeError("More than one build ID in ELF")
    if len(build_ids):
        return build_ids[0]
    return None


def extract_debug(elf_path, *, arch, build_id, destdir):
    if len(build_id) < 32:
        raise RuntimeError("Expected build IDs to be >= 32 hex digits")
    debug_dir = os.path.join(destdir, "usr/lib/debug/.build-id", build_id[:2])
    debug_path = os.path.join(debug_dir, build_id[2:] + ".debug")
    os.makedirs(debug_dir, exist_ok=True)
    args = [
        # TODO: Alternatively support {arch}-{system}-objcopy.
        "llvm-objcopy",
        "--only-keep-debug",
        elf_path,
        debug_path,
    ]
    print(f"SPLIT {elf_path} -> {debug_path}")
    if verbose:
        print("Execute:", " ".join(shlex.quote(s) for s in args))
    subprocess.run(args, check=True)


def strip_debug(elf_path, *, arch):
    args = [
        # TODO: Alternatively support {arch}-{system}-strip.
        "llvm-strip",
        "--strip-debug",
        elf_path,
    ]
    print(f"STRIP {elf_path}")
    if verbose:
        print("Execute:", " ".join(shlex.quote(s) for s in args))
    subprocess.run(args, check=True)


def main():
    global verbose

    parser = argparse.ArgumentParser(
        description="Utility to split debug information from ELF files"
    )
    parser.add_argument("path", type=str)
    parser.add_argument("-v", "--verbose", action="store_true")

    args = parser.parse_args()
    if args.verbose:
        verbose = True

    paths = walk_regular(args.path)
    for p in paths:
        with open(p, "rb") as f:
            # Read the ELF header.
            buf = f.read(ehdr_struct.size)
            if len(buf) != ehdr_struct.size:
                continue

            ehdr = ehdr_tuple._make(ehdr_struct.unpack(buf))
            if buf[:4] != b"\x7fELF":
                continue
            if ehdr.e_ident[EI_DATA] != 1:
                print(f"ELF file {p} is not little-endian")
                continue
            if ehdr.e_ident[EI_CLASS] == 1:
                arch = em_32bit_to_arch.get(ehdr.e_machine)
            elif ehdr.e_ident[EI_CLASS] == 2:
                arch = em_64bit_to_arch.get(ehdr.e_machine)
            else:
                print(f"ELF file {p} is neither 32-bit nor 64-bit")
                continue
            if arch is None:
                print(f"ELF file {p} has unsupported machine type")
                continue

        build_id = get_build_id(p)
        if build_id is None:
            print(f"ELF file {p} has no build ID")
            continue

        extract_debug(
            p,
            arch=arch,
            build_id=build_id,
            destdir=args.path,
        )
        strip_debug(
            p,
            arch=arch,
        )


if __name__ == "__main__":
    main()
