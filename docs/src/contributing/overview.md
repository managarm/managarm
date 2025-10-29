# Contributing

This section of the Managarm documentation helps you on contributing to Managarm.

## Communication

### Discord
Most of our communication happens on our Discord at [https://discord.gg/7WB6Ur3](https://discord.gg/7WB6Ur3).

### GitHub
Some of our communication takes place on GitHub at [https://github.com/managarm/managarm](https://github.com/managarm/managarm).

### IRC
We also have an IRC channel `#managarm` on `irc.libera.chat`, please keep in mind that our former channel found on freenode is no longer in use.


<!-- omit in toc -->
# Contributing to The Managarm Project

First off, thanks for taking the time to contribute! ❤️

All types of contributions are encouraged and valued. See the [Table of Contents](#table-of-contents) for different ways to help and details about how this project handles them. Please make sure to read the relevant section before making your contribution. It will make it a lot easier for us maintainers and smooth out the experience for all involved. The community looks forward to your contributions. 🎉

<!--
> And if you like the project, but just don't have time to contribute, that's fine. There are other easy ways to support the project and show your appreciation, which we would also be very happy about:
> - Star the project
> - Tweet about it
> - Mention the project at local meetups and tell your friends/colleagues
-->
<!-- omit in toc -->
## Table of Contents

- [Asking Questions](#i-have-a-question)
- [How to contribute](#i-want-to-contribute)
  - [Reporting Bugs](#reporting-bugs)
  - [Your First Code Contribution](#your-first-code-contribution)
  - [Preparing a PR](#preparing-a-pr)
  - [Improving The Documentation](#improving-the-documentation)
- [Styleguides](#styleguides)
  - [Coding Style](#coding-style)
  - [Commit Messages](#commit-messages)



## I Have a Question

Our primary way of communication is our [Discord server](https://discord.gg/7WB6Ur3), please don't hesitate to ask a question there, we are more then willing to help. We also use GitHub [Issues](https://github.com/managarm/managarm/issues). You can search here to see if a question has already been answered before.

## How do I contribute?

> ### Legal Notice
> When contributing to this project, you must agree that you have authored 100% of the content, that you have the necessary rights to the content and that the content you contribute may be provided under the project license.

### Reporting Bugs

<!-- omit in toc -->
#### Submitting a Bug Report

A good bug report shouldn't leave others needing to chase you up for more information. Therefore, we ask you to investigate carefully, collect information and describe the issue in detail in your report. Please complete the following steps in advance to help us fix any potential bug as fast as possible.

We use GitHub issues to track bugs and errors. If you run into an issue with the project:

- Open an [Issue](https://github.com/managarm/managarm/issues/new). (Since we can't be sure at this point whether it is a bug or not, we ask you not to talk about a bug yet and not to label the issue.)
- Explain the behavior you would expect and the actual behavior.
- Please provide as much context as possible and describe the *reproduction steps* that someone else can follow to recreate the issue on their own. This usually includes your code. For good bug reports you should isolate the problem and create a reduced test case.
- Collect information about the bug:
  - Your input and the output
  - Can the bug be reproduced with the latest nightly image and/or with current packages from our package repositories?

<!-- You might want to create an issue template for bugs and errors that can be used as a guide and that defines the structure of the information to be included. If you do so, reference it here in the description. -->

### Your First Code Contribution

> This section of the Guide is Work-In-Progress. Please come back later for more information.

For instructions on how to setup the build environment, how to build and how to run Managarm we refer you to the bootstrap-managarm repository [here](https://github.com/managarm/bootstrap-managarm).

There is also the option to [set up LSP support](lsp.md).

<!-- TODO
include Setup of env, IDE and typical getting started instructions?
-->

### Preparing a PR

When preparing your work for a PR, keep a few things in mind:

* Split your PR into reasonably small commits. Each commit should address one specific concern. Pick sensible and informative commit messages (see also ["Commit Messages"](commit-messages.md)).
* Commits must be self-contained. Each commit must build on its own (such that `git bisect` works as expected).
* Pick a descriptive branch name. The branch name appears in merge commits and is visible in the commit history.
* When you apply changes after code review, do not include fixes of early commits as separate commits in your PR. Instead, amend your earlier commits to include the fixes. Force push the amended commits to your branch.
  > `git commit --fixup <commit>` + `git rebase -i --autosquash origin/master`
  > are excellent tools to amend commits, regardless of where they appear
  > in a patch series.

  For the purposes of adding fixes to address reviews, it is advisable to not squash the fixup commits but to simply push them to the PR branch - this allows reviewers to trivially see the changes made, and the fixups can then still be squashed before merging.
* Do not include merge commits in your PR ("Merge branch master ..."). Managarm strives for a linear (or almost linear) commit history on all repositories.
  > Merge commits can accidentally be created by running `git pull origin master`
  > after `master` and `origin/master` have diverged. This can be avoided by
  > passing the `--rebase` option to `git pull`.
  > We strongly recommend to set `git config --global pull.rebase true` to
  > make `--rebase` the default.
* Signing your commits is optional, but welcome.
* Feel free to use [`Co-authored-by:`](https://docs.github.com/en/pull-requests/committing-changes-to-your-project/creating-and-editing-commits/creating-a-commit-with-multiple-authors) lines for co-authors of the commit.

### Handling bootstrap-managarm and managarm dependencies

Sometimes, a situation can arise where changes to both the [managarm/managarm repo](https://github.com/managarm/managarm) and [managarm/bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) are needed. If these changes depend on each other, our GitHub Actions jobs may fail for managarm. This is obviously suboptimal, as we would like to ensure that a PR builds correctly even if, and especially if, it depends on [bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) changes.

In order to fix this, you can specify a [bootstrap-managarm](https://github.com/managarm/bootstrap-managarm) PR that the GitHub Actions jobs for managarm should build against. In order to do this, include the following line as a separate line (as in, it should be at the beginning of a line) in your PR description body:

```md
Depends on managarm/bootstrap-managarm#<PR>.
```

Obviously, replace `<PR>` with your PR number.

### Improving The Documentation

> This section of the Guide is Work-In-Progress. Please come back later for more information.

<!-- TODO
Updating, improving and correcting the documentation

-->

## Styleguides
### Coding Style

For the general coding style used in The Managarm Project we refer you to the [coding style](coding-style.md).


### Commit Messages

For the general commit message style used in The Managarm Project we refer you to the [commit message](commit-messages.md) style.

<!-- TODO

-->

<!-- omit in toc -->
## Attribution
This guide is based on the **contributing-gen**. [Make your own](https://github.com/bttger/contributing-gen)!
