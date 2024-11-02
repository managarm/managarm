# LSP setup
With [cbuildrt>=0.1.2](https://github.com/managarm/cbuildrt) and
[xbstrap>=0.26.0](https://github.com/managarm/xbstrap), it is now possible to
run [clangd](https://clangd.llvm.org/) server inside the build context.
This provides autocompletion, hover information, basic refactoring and other
features to your text editor of choice.

As the server runs inside a container or within a modified environment, which
is a nonstandard configuration, some editor configuration will be required.

## Common setup
Using the `scripts/managarm-lsp-launcher.sh` script in the `bootstrap-managarm`
repository, we can conveniently launch an LSP server in our build environment.

This script discovers what package context you're currently in and runs clangd
appropriately.
It relies on the `compile_commands.json` link to figure out what package you're
currently in.
To create this link, run:

```sh
xbstrap -C /path/to/build-dir lsp <package> -- ln -s @THIS_BUILD_DIR@/compile_commands.json
```

For instance, if `<package>` is `managarm-system`, it will place a link in
`<source dir>/managarm/compile_commands.json`, as this is where the xbstrap
`managarm` source is located.

### Note on "multi build" sources
Some code, such as the `managarm` kernel and servers, is built under two
distinct configurations from the same source.
For these, the above procedure will not be sufficient (which one of the two
compile databases do you pick?).
As a workaround, we can instruct clangd to conditionally pick a compile
database.

In the managarm source directory, drop in a snippet like this one into a file
called `.clangd` at the root:

```yaml
---
CompileFlags:
  CompilationDatabase: /var/lib/managarm-buildenv/build/pkg-builds/managarm-system
---
If:
  PathMatch: kernel/.*
CompileFlags:
  CompilationDatabase: /var/lib/managarm-buildenv/build/pkg-builds/managarm-kernel
---
If:
  PathMatch: kernel/eir/protos/uefi/.*
CompileFlags:
  CompilationDatabase: /var/lib/managarm-buildenv/build/pkg-builds/managarm-kernel-uefi

# vim: set ft=yaml :
```

Your paths might vary, and the LSP launcher relies on a compile database link
existing (even if broken), so the above step with `ln -s` is still necessary.

## Editor specific setup

### vim (`vim-lsp`)
To configure [vim-lsp](https://github.com/prabirshrestha/vim-lsp), we recommend
using a conditional on your working directory based in your Vim startup file:

```vim
if executable('xbstrap') && getcwd() =~ '/path/to/bootstrap-managarm/.*'
    au User lsp_setup call lsp#register_server({
        \ 'name': 'xbstrap-lsp-managarm',
        \ 'cmd': {server_info->['/path/to/managarm-lsp-launcher.sh', '/path/to/managarm/build/']},
        \ 'whitelist': ['c', 'cpp', 'objc', 'objcpp'],
        \ })
endif
```

### GNU Emacs (`eglot`)
In GNU Emacs, you can use directory local variables to adjust
[Eglot](https://elpa.gnu.org/packages/eglot.html) in your clone of Managarm
alone, by placing a file like the following into it:

```emacs-lisp
;;; Directory Local Variables            -*- no-byte-compile: t -*-
;;; For more information see (info "(emacs) Directory Variables")

((nil . ((eglot-server-programs
          . (((c-mode c++-mode) . ("/.../managarm-lsp-launcher.sh" "/.../build"))
)))))
```

Which translates, roughly, to: for each file in this project, associate
`c-mode` and `c++-mode` with `managarm-lsp-launcher.sh`.  This is done like so
in order to have consistent Eglot variables across the entire project.

### VSCodium (LLVM clangd)
The [LLVM clangd
plugin](https://open-vsx.org/extension/llvm-vs-code-extensions/vscode-clangd)
allows you to change the path to clangd.
We can use this to launch our wrapper:

```json
{
    "clangd.path": "/path/to/managarm-lsp-launcher.sh",
    "clangd.arguments": ["/path/to/managarm/build/"],
    "clangd.onConfigChanged": "restart"
}
```

## Troubleshooting
In case of trouble, please invoke the script directly like so:

```sh
/path/to/managarm-lsp-launcher.sh path/to/build; echo "$?"
```

- If the script exits with no output and exit code `1`, your build directory is
  incorrect.
- If the script exits with no output and exit code `2`, there is no
  `compile_commands.json`.
- If you see the `clangd` banner, but you are seeing issues in the editor,
  enable the LSP logs in your editor (for instance, `g:lsp_log_verbose` and
  `g:lsp_log_file` with `vim-lsp`), and check the logs.
- Export `CLANGD_FLAGS="--log=verbose"` before running your text editor to
  increase log verbosity and try again.
- LLVM 13 changed the meaning of `.clangd`.
  It used to be a directory containing the indexer cache, but it was moved to
  `$XDG_CACHE_HOME/clangd`.
  If `.clangd` is a directory, remove it.
- If there is no obvious solution, reach out for help.
