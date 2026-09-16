# ==============================================================================
# Cgoct — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dcactlib=../CactLibc-x86_32
#   ninja -C build-meson            # build-meson/cgoct
#
# A PIE linked against CactLibc with /lib/ld.so as PT_INTERP, exactly as the
# Makefile built it (which then copied the binary to the repo root — the
# integrator now takes it from build-meson/ instead).
# ==============================================================================

project('cgoct', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',        # -Wall -Wextra, as in the Makefile
    'buildtype=plain',
    'b_staticpic=false',
  ])

fs = import('fs')
ld = find_program('ld')

src_root = meson.project_source_root()

_cactlib = get_option('cactlib')
cactlib = _cactlib.startswith('/') ? _cactlib : src_root / _cactlib

start_o = cactlib / 'build-meson' / 'start.o'
clibc_so = cactlib / 'build-meson' / 'clibc.so'

if not fs.exists(start_o) or not fs.exists(clibc_so)
  error('CactLibc is not built: expected ' + start_o +
        '  —  build it first:  ninja -C ' + cactlib / 'build-meson')
endif

# Every object in this repo is position independent: the supervisor is a PIE.
add_project_arguments('-fPIE', language: 'c')

c_args = [
  '-ffreestanding',
  '-fno-stack-protector',
  '-nostdlib',
  '-I' + (cactlib / 'include'),
]

cgoct_lib = static_library('cgoct', 'src/main.c', c_args: c_args)

cgoct = custom_target('cgoct',
  output: 'cgoct',
  input: cgoct_lib,
  command: [ld, '-m', 'elf_i386', '-pie', '--dynamic-linker=/lib/ld.so',
            '--hash-style=both', '-nostdlib',
            '-T', src_root / 'link.ld', '-o', '@OUTPUT@',
            start_o,
            '--whole-archive', cgoct_lib, '--no-whole-archive',
            clibc_so],
  depend_files: files('link.ld'),
  build_by_default: true)
