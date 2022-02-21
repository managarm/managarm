# LSP setup
With [cbuildrt>=0.1.2](https://github.com/managarm/cbuildrt) and
[xbstrap>=0.26.0](https://github.com/managarm/xbstrap), it is now possible to
run [clangd](https://clangd.llvm.org/) server inside the build context.
This provides autocompletion, hover information, basic refactoring and other
features to your text editor of choice.

As the server runs inside a container or with a modified environment, which is
a nonstandard configuration, some editor configuration will be required.

## Common setup
Using the `scripts/managarm-lsp-launcher.sh` script in the `bootstrap-managarm`
repository, we conveniently launch an LSP server in our build environment.

This script discovers what package context you're currently in and runs clangd
appropriately.
It relies on a link to the compile commands database to detect what package
it's working in.
To create this link, run:

```sh
xbstrap -C /path/to/build-dir lsp <package> -- ln -s @THIS_BUILD_DIR@/compile_commands.json
```

The package, for instance, would be `managarm-system`, which will place
`compile_commands.json` in the root of the `managarm` source managed by
`xbstrap`.

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
  If there is no obvious issue, reach out for help.
