=========
 drm-misc
=========

-------------------------------------------------------------
drm-misc patch and upstream merge flow and timeline explained
-------------------------------------------------------------

This document describes the flow and timeline of misc drm and gpu patches to
various upstream trees. For a detailed list of what's all maintained in drm-misc
grep for "drm-misc" in MAINTAINERS.

Rule No. 1
==========

This document is an eternal draft and simply tries to explain the reality of how
drm-misc is maintained. If you observe a difference between these rules and
reality, it is your assumed responsibility to update the rules.

The workflow is heavily based upon the one used to maintain the Intel drm
driver, see `drm-intel <drm-intel.html>`_:

Getting Started
===============

First you need a `freedesktop.org account with the drm-misc group permission
<https://www.freedesktop.org/wiki/AccountRequests/>`_. Then you need to setup the
branches and tooling, see the `getting started with dim <dim.html#quickstart>`_
guide.

Branches
========

All branches are maintained in `git://anongit.freedesktop.org/drm-misc`.

drm-misc-next
~~~~~~~~~~~~~

This is the main feature branch where most of the patches land. This branch is
always open to "hide" the merge window from developers. To avoid upsetting
linux-next and causing mayhem in the merge window, in general no pull requests
are sent to upstream after rc6 of the current kernel release. Outside of that
feature freeze period, pull requests are sent to upstream roughly every 1-2
weeks, to avoid too much coordination pains. See the timeline below for a
visualization of patch flow.

If you're unsure, apply your patch here, it can always be cherry-picked to one
of the -fixes branches later on. But in contrast to the drm-intel flow
cherry-picking is not the default.

drm-misc-next-fixes
~~~~~~~~~~~~~~~~~~~

This branch is only relevant between rc6 of the current kernel version (X) and
rc1 of the next (X+1). This is the feature freeze period mentioned above in the
drm-misc-next section. During this time, drm-misc-next will roll over to target
kernel version X+2, and drm-misc-fixes will still be on kernel version X, so
drm-misc-next-fixes is used for fixes that target X+1.

See the timeline below for a visualization of patch flow.

drm-misc-fixes
~~~~~~~~~~~~~~

This is for bugfixes which target the current -rc cycle.

drm-tip
~~~~~~~

This is the overall integration tree for drm, and lives in
`git://anongit.freedesktop.org/drm-tip`. Every time one of the above branches is
updated drm-tip gets rebuilt. If there's a conflict see section on `resolving
conflicts when rebuilding drm-tip
<drm-intel.html#resolving-conflicts-when-rebuilding-drm-tip>`_.

Where Do I Apply My Patch?
~~~~~~~~~~~~~~~~~~~~~~~~~~

Consult this handy flowchart to determine the best branch for your patch. If in
doubt, apply to drm-misc-next or ask your favorite maintainer on IRC.

.. image:: drm-misc-commit-flow.svg

Merge Timeline
~~~~~~~~~~~~~~

This chart describes the merge timelines for various branches in terms of one
kernel release cycle. Worth noting is that we're working on two or three kernel
releases at the same time. Big features take a long time to hit a kernel
release. There are no fast paths.

.. include:: drm-misc-timeline.rst


Merge Criteria
==============

Right now the only hard merge criteria are:

* Patch is properly reviewed or at least Ack, i.e. don't just push your own
  stuff directly. This rule holds even more for bugfix patches - it would be
  embarrassing if the bugfix contains a small gotcha that review would have
  caught.

* drm-misc is for drm core (non-driver) patches, subsystem-wide refactorings,
  and small trivial patches all over (including drivers). For a detailed list of
  what's all maintained in drm-misc grep for "drm-misc" in MAINTAINERS.

* Larger features can be merged through drm-misc too, but in some cases
  (especially when there are cross-subsystem conflicts) it might make sense to
  merge patches through a dedicated topic tree. The dim_ tooling has full
  support for them, if needed.

* Any non-linear actions (backmerges, merging topic branches and sending out
  pull requests) are only done by the official drm-misc maintainers (currently
  Daniel, Jani and Sean, see MAINTAINERS), and not by committers. See the
  `examples section in dim <dim.html#examples>`_ for more info

* All the x86, arm and arm64 DRM drivers need to still compile. To simplify this
  we track defconfigs for all three platforms in the `drm-intel-rerere` branch.

* The goal is to also pre-check everything with CI. Unfortunately neither the
  arm side (using kernelci.org and generic i-g-t tests) nor the Intel side
  (using Intel CI infrastructure and the full i-g-t suite) isn't yet fully ready
  for production.

* No rebasing out mistakes, because this is a shared tree.

* See also the extensive `committer guidelines for drm-intel
  <drm-intel.html#committer-guidelines>`_.

Small Drivers
=============

Small drivers, where a full tree is overkill, can be maintained in drm-misc. For
now there are just a few drivers maintained in drm-misc, but we can slowly add
more to figure out how to make this scale. Slightly different rules apply:

* Small is measured in patches merged per kernel release. The occasional big
  patch series is still acceptable if it's not a common thing (e.g. new hw
  enabling once a year), and if the series is really big (more than 20 patches)
  it should probably be managed through a topic branch in drm-misc and with a
  separate pull request to drm maintainer. dim_ supports this with the
  create-branch command. Everything that doesn't justify a topic branch goes
  into the normal drm-misc branches directly.

* Group maintainership is assumed, i.e. all regular contributors (not just
  the primary maintainer) will get commit rights.

* Since even a broken driver is more useful than no driver minimal review
  standards are a lot lower. The default should be some notes about what could
  be improved in follow-up work and accepting patches by default. Maintainer
  group for drivers can agree on stricter rules, especially when they have a
  bigger user base that shouldn't suffer from regressions.

* Minimal peer-review is also expected for drivers with just one contributor,
  but obviously then only focuses on best practices for the interaction with drm
  core and helpers. Plus a bit looking for common patterns in dealing with the
  hardware, since display IP all has to handle the same issues in the end. In
  most cases this will just along the lines of "Looks good, Ack".  drm-misc
  maintainers will help out with getting that review market going.

* Best practice for review: When you have some suggestions and comments for
  future work, please make sure you don't forget your Ack tag to unblock the
  original patch. And if you think something really must be fixed before
  merging, please give a conditional Ack along the lines of "Fix
  $specific_thing, with that addressed, Ack". The goal is to always have a clear
  and reasonable speedy path towards getting the patch merged. For authors on
  the other side, just do the minimal rework and push the patch, and do any
  more involved rework in follow-up work. This way lengthy review cycles get
  avoided, which are a drag for both reviewer and author.

Maintainer's Duties
===================

Maintainers mostly provide services to keep drm-misc running smoothly:

* Coordinate cross-subsystem depencies and handle topic branches, sending out
  pull request and merging topic pull requests from other subsystems.

* At least once per week check for pending bugfixes (using ``dim status``) and
  if there are any (either in `-fixes` or `-next-fixes`), send out the pull
  request.

* Fast-forward (when possible) `-fixes` to each released -rc kernel tag, to
  keep it current. We try to avoid backmerges for bugfix branches, and rebasing
  isn't an option with multiple committers.

* Pull requests become noisy if `-fixes` has been fast-forwarded to Linus'
  latest -rc tag but drm-upstream hasn't done the same yet: The shortlog
  will contain not just the queued fixes but also anything else that has
  landed in Linus' tree in the meantime. The best practice is then to base
  the pull request on Linus' master branch (rather than drm-upstream) by
  setting the `upstream` argument for ``dim pull-request`` accordingly.
  Upstream should be warned that they haven't fast-forwarded yet.

* During the merge-windo blackout, i.e. from -rc6 on until the merge window
  closes with the release of -rc1, try to track `drm-next` with the
  `-next-fixes` branch. Do not advance past -rc1, otherwise the automagic in
  the scripts will push the wrong patches to the linux-next tree.

* Between -rc1 and -rc6 send pull requests for the `-next` branch every 1-2
  weeks, depending upon how much is queued up.

* Backmerge `drm-next` into the `-next` branch when needed, properly recording
  that reason in the merge commit message. Do a backmerge at least once per
  month to avoid conflict chaos, and specifically merge in the main drm feature
  pull request, to resync with all the late driver submissions during the merge
  window.

* Last resort fallback for applying patches, in case all area expert committers
  are somehow unavailable.

* Take the blame when something goes wrong. Maintainers interface and represent
  the entire group of committers to the wider kernel community.

Tooling
=======

drm-misc git repositories are managed with dim_:

.. _dim: dim.html

