=====
 dim
=====

---------------------------
drm-intel maintainer script
---------------------------

:Author: Daniel Vetter <daniel.vetter@ffwll.ch>
:Author: Jani Nikula <jani.nikula@intel.com>
:Date: 2014-05-15
:Copyright: 2012-2016 Intel Corporation
:Manual section: 1
:Manual group: maintainer tools

SYNOPSIS
========

**dim** [*option* ...] *command*

DESCRIPTION
===========

drm-intel maintainer script.

Used to maintain drm-intel_ and drm-misc_ git repositories.

.. _drm-intel: drm-intel.html
.. _drm-misc: drm-misc.html

QUICKSTART
==========

For getting started grab the latest drm (drm-intel-maintainer) script from::

    http://cgit.freedesktop.org/drm-intel/tree/dim?h=maintainer-tools

There's also a sample config file for ~/.dimrc::

    http://cgit.freedesktop.org/drm-intel/tree/dimrc.sample?h=maintainer-tools

Plus, there's bash completion in the same directory if you feel like using that.
Run::

    $ dim help

for tons of details about how this thing works. Also see the git repository
specific pages for details on the patch merging process for each tree. Adjust
your .dimrc to match your setup and then run::

    $ dim setup

This will also check out the latest maintainer-tools branches, so please replace
the dim you just downloaded with a symlink after this step. And by the way, if
you have improvements for dim, please submit them to intel-gfx.

If you have a freedesktop.org account and plan to push things on one of the
drm-xxx repos, you should use the ssh://git.freedesktop.org/git/drm-xxx urls
when adding a remote and, if it's not already done, you should add new entry in
~/.ssh/config::

    $ printf '\nHost git.freedesktop.org\n\tUser <username>' >> ~/.ssh/config

You should now have a main repository for patch application. The directory
corresponding to this repository is defined by DIM_DRM_INTEL in your .dimrc.
You should also have directories called maintainer-tools, drm-tip (for
rebuilding the tree), and drm-rerere for some dim-internal book-keeping.

If someone else has pushed patches first resync using::

   $ dim update-branches

Since dim supports lots of different branches in different repositories you
first need to check out the right branch using::

   $ dim checkout <branch>

Applying patches is done in the main repository with::

    $ cat patch.mbox | dim apply-branch <branch>

This works like a glorified version of git apply-mbox and does basic patch
checking and adds stuff like patchwork links of the merged patch. It is
preferred to use the patch email file instead of the original patch file since
it contains some interesting headers like the message ID. When you're happy
(remember that with a shared tree any mistake is permanent and there's no
rebasing) push out the new tree with::

    $ dim push-branch <branch>

This will also rebuild a new drm-tip integration tree. For historical reasons
there's shortcut for the drm-intel specific branches for most of these commands.

OPTIONS
=======

-f		Ignore some error conditions.
-d		Dry run.
-i		Interactive mode.

COMMANDS
========

setup *prefix*
--------------
Setup git maintainer branches in the given prefix.

update-branches
---------------
Updates all maintainer branches. Useful to synchronize all branches when other
maintainers and committers pushed patches meanwhile.

status
------

Lists all branches with unmerged patches, and how many patches are unmerged.

rebuild-tip
-----------
Rebuild and push the integration tree.

revert-rerere *rerere-cache-commit-ish*
---------------------------------------

When a stored conflict resolution in the integration tree is wrong, this command
can be used to fix up the mess. First figure out which commit in the
*rerere-cache* branch contains the bogus conflict resolution, then revert it
using this command. This ensures the resolution is also purged from any local
caches, to make sure it doesn't get resurrected. Then run *rebuild-tip* to
redo the merges, correctly.

cat-to-fixup
------------

Pipes stdin into the fixup patch file for the current drm-tip merge.

push-branch branch [*git push arguments*]
-----------------------------------------

push-fixes|pf [*git push arguments*]
------------------------------------

push-next-fixes|pnf [*git push arguments*]
------------------------------------------

push-queued|pq [*git push arguments*]
-------------------------------------

Updates the named branch, or drm-intel-fixes, drm-intel-next-fixes or the
drm-intel-next-queued branch respectively. Complains if that's not the current
branch, assuming that patches got merged to the wrong branch. After pushing also
updates linux-next and drm-tip branches.

checkout|co *branch*
--------------------
Checks out the named branch.

conq
----

cof
---

conf
----
Checks out the drm-intel-fixes branch, dinf or dinq respectively for merging
patches.

apply-branch|ab|sob branch [*git am arguments*]
-----------------------------------------------
Applys a patch to the given branch, complaining if it is not
checked out yet.

apply-fixes|af [*git am arguments*]
-----------------------------------

apply-next-fixes|anf [*git am arguments*]
-----------------------------------------

apply-queued|aq [*git am arguments*]
------------------------------------
Applies a patch to -fixes, -next-fixes or -next-queued respectively, complains
if it's not the right branch. Additional arguments are passed to git am.

magic-patch|mp [-a]
-------------------
Apply a patch using patch and then wiggle in any conflicts. When passing the
option -a automatically changes the working directory into the git repository
used by the last previous branch-specific command. This is useful with the
per-branch workdir model.

magic-rebase-resolve|mrr
------------------------
Tries to resolve a rebase conflict by first resetting the tree
and the using the magic patch tool. Then builds the tree, adds
any changes with git add -u and continues the rebase.

cd
--
Changes the working directory into the git repository used by the last previous
branch-specific command. This is implemented as a bash-function to make it
useful in interactive shells and scripts. Only available when the bash
completion is sourced.

apply-resolved|ar
-----------------
Compile-test the current tree and if successfully resolve a
confilicted git am. Also runs the patch checker afterwards.

apply-igt|ai
------------
Apply a patch to the i-g-t repository.

tc *commit-ish*
---------------
Print the oldest Linux kernel release or -rc tag that contains the supplied
*commit-ish*, or, if none do, print the upstream branches that contain it.

cite *commit-ish*
-----------------
Cite the supplied *commit-ish* in format 'sha1 ("commit subject")'.

fixes *commit-ish*
------------------
Print the Fixes: and Cc: lines for the supplied *commit-ish* in the linux kernel
CodingStyle approved format.

cherry-pick *commit-ish* [*git cherry-pick arguments*]
------------------------------------------------------

Improved git cherry-pick version which also scans drm-tip picked, too. In
dry-run mode/-d only the patch list is generated.

cherry-pick-fixes
-----------------

cherry-pick-next-fixes
----------------------

Look for non-upstreamed fixes (commits tagged Cc: stable@vger.kernel.org or Cc:
drm-intel-fixes@lists.freedesktop.org) in drm-intel-next-queued, and try to
cherry-pick them to drm-intel-fixes or drm-intel-next-fixes.

pull-request *branch* *upstream*
--------------------------------
Fetch the *upstream* remote to make sure it's up-to-date, create and push a date
based tag for the *branch*, generate a pull request template with the specified
*upstream*, and finally start \$DIM_MUA with the template with subject and
recipients already set.

Since the tag for the *branch* is date based, the pull request can be
regenerated with the same commands if something goes wrong.

pull-request-fixes [*upstream*]
-------------------------------
This is a special case of **pull-request**, with *drm-intel-fixes* as the
branch and *origin/master* as the default upstream.

pull-request-next-fixes [*upstream*]
------------------------------------
This is a special case of **pull-request**, with *drm-intel-next-fixes* as
the branch and *\$DRM_UPSTREAM/drm-next* as the default upstream.

pull-request-next [*upstream*]
------------------------------
This is similar to **pull-request**, but for feature pull requests, with
*drm-intel-next* as the branch and *\$DRM_UPSTREAM/drm-next* as the default
upstream.

The difference to **pull-request** is that this command does not generate a
tag; this must have been done previously using **update-next**. This also means
that the pull request can be regenerated with the same commands if something
goes wrong.

update-next
-----------
Pushes out the latest dinq to drm-intel-next and tags it. Also
pushes out the latest drm-tip to drm-intel-testing. For an
overview a gitk view of the currently unmerged feature pile is
opened.

Also checks that the drm-intel-fixes|-next-queued are fully
merged into drm-tip to avoid operator error.

tag-next
--------

Pushes a new tag for the current drm-intel-next state after checking that the
remote is up-to-date. Useful if drm-intel-next has been changed since the last
run of the update-next command (e.g. to apply a hotfix before sending out the
pull request).

checkpatch|check-patch|cp [*commit-ish* [.. *commit-ish*]]
----------------------------------------------------------
Runs the given commit range commit-ish..commit-ish through the check tools.

If no commit-ish is passed, defaults to HEAD^..HEAD. If one commit-ish is passed
instead of a range, the range commit-ish..HEAD is used.

sparse [*commit-ish* [.. *commit-ish*]]
---------------------------------------
Run sparse on the files changed by the given commit range.

If no commit-ish is passed, defaults to HEAD^..HEAD. If one commit-ish is passed
instead of a range, the range commit-ish..HEAD is used.

checker
-------
Run sparse on drm/i915.

create-branch *branch* [*commit-ish*]
-------------------------------------

Create a new topic branch with the given name. Note that topic/ is not
automatically prepended. The branch starts at HEAD or the given commit-ish. Note
that by default the new branch is created in the drm-intel.git repository. If
you want to create a branch somewhere else, then you need to prepend the remote
name from nigthly.conf, e.g. "drm-misc/topic/branch".

remove-branch *branch*
----------------------

Remove the given topic branch.

create-workdir (*branch* | all)
-------------------------------

Create a separate workdir for the branch with the given name (requires that
git-new-workdir from git-core contrib is installed), or for all branches if
"all" is given.

for-each-workdir|fw *command*
-----------------------------

Run the given command in all active workdirs including the main repository under
\$DIM_DRM_INTEL.

list-aliases
------------
List all aliases for the subcommand names. Useful for autocompletion scripts.

See \$dim_alias_<alias> under ENVIRONMENT below on how to define aliases.

list-branches
-------------

List all branches (main and topic) managed by dim. Useful for autocompletion
scripts.

list-commands
-------------

List all subcommand names, including aliases. Useful for autocompletion scripts.

list-upstreams
--------------

List of all upstreams commonly used for pull requests. Useful for autocompletion
scripts.

uptodate
--------
Try to check if you're running an up-to-date version of **dim**.

help
----
Show this help. Install **rst2man(1)** for best results.

usage
-----

Short form usage help listening all subcommands. Run by default or if an unknown
subcommand was passed on the cmdline.

ENVIRONMENT
===========

DIM_CONFIG
----------
Path to the dim configuration file, \$HOME/.dimrc by default, which is sourced
if it exists. It can be used to set other environment variables to control dim.

DIM_PREFIX
----------
Path prefix for kernel repositories.

DIM_DRM_INTEL
-------------
The main maintainer repository under \$DIM_PREFIX.

DIM_DRM_INTEL_REMOTE
--------------------
Name of the $drm_intel_ssh remote within \$DIM_DRM_INTEL.

DIM_MUA
-------
Mail user agent. Must support the following subset of **mutt(1)** command line
options: \$DIM_MUA [-s subject] [-i file] [-c cc-addr] to-addr [...]

DIM_MAKE_OPTIONS
----------------
Additional options to pass to **make(1)**. Defaults to "-j20".

DIM_TEMPLATE_HELLO
------------------
Path to a file containing a greeting template for pull request mails.

DIM_TEMPLATE_SIGNATURE
----------------------
Path to a file containing a signature template for pull request mails.

dim_alias_<alias>
-----------------
Make **<alias>** an alias for the subcommand defined as the value. For example,
`dim_alias_ub=update-branches`. There are some built-in aliases. Aliases can be
listed using the **list-aliases** subcommand.

The alias functionality requires **bash(1)** version 4.3 or later to work.

CONTRIBUTING
============

Submit patches for any of the maintainer tools to
intel-gfx@lists.freedesktop.org with [maintainer-tools PATCH] prefix. Use

$ git format-patch --subject-prefix="maintainer-tools PATCH"

for that. Push them once you have
an ack from maintainers (Jani/Daniel).
