# Updating Packages

This section describes how to keep an existing Managarm
build up to date after upstream repositories have merged
changes.

## Updating system packages

To keep up with updates of Managarm's kernel and drivers,
it is usually enough to keep the packages `managarm-kernel`,
`managarm-system`, `mlibc` and `mlibc-headers` up to date.

If you have local changes to these system packages, it is usually
advisable to update the packages via `git`
([see the following section](#updating-via-git-or-other-version-control-tools)).
In other cases, `xbstrap` can be used to update these packages using the command:
```sh
xbstrap install -u --deps-of managarm-system --deps-of managarm-kernel --deps-of mlibc mlibc-headers
```

> It is important to update `mlibc-headers` when `mlibc` modifies its public
> headers. If `mlibc-headers` is out of date, `mlibc` will still build fine
> but ports will not see any updates in C library headers.

## Updating via `git` (or other version control tools)

`xbstrap` manages source repositories by using each package's upstream
version control tool. In most cases, upstreams use `git` (although `hg` and `svn` are
also supported by `xbstrap`). To update a package via `git` (or any other VCS),
simply pull a new revision of the package and use `xbstrap` to rebuild it.

For example, pulling a new revision of the `managarm` repository
and rebuilding the `managarm-system` and `managarm-kernel` packages
can be achived via:
```sh
cd ~/managarm/src/managarm
git pull origin master
cd ~/managarm/build
xbstrap install --rebuild managarm-system managarm-kernel
```

> **Note**: When updating packages through VCS, make sure
> to also keep their dependencies up to date. For the
> [system packages mentioned above](#updating-system-packages)
> these dependencies include
> `frigg`, `libasync`, `libsmarter`, `fafnir` and `lewis`.

## Updating ports

Updating ports via `xbstrap` works similarly as updating system packages.
However, ports usually build from fixed versions (and not from branches).
If local changes have been applied to such a fixed version
(e.g., by patches), `xbstrap` refuses to
automatically check out a different commit,
as doing a `git checkout` (or similar) would *risk loss
of local commits and/or uncommitted changes*.
To override this behavior, pass `--reset` to discard
local commits (or `--hard-reset` to discard uncommitted changes and local commits).

For example, to update `bash`, run
```sh
# Do a dry-run first. Make sure to verify that no local changes would be discarded.
xbstrap install -u -n bash
xbstrap install -u --reset bash
```

> **Note**: It is safe to use `--reset` to remove patches that have been applied
> by `xbstrap` itself. However, care should be taken to not update
> repositories with important local modifications that you have applied
> yourself.
>
> In case of `git`, local commits can usually be recovered after
> `--reset` by inspecting `git reflog`; uncommitted changes that
> are discarded by `--hard-reset` cannot easily be restored.
