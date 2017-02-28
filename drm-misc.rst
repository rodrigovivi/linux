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
linux-next and causing mayhem in the merge window in general no pull requests
are sent to upstream 1-2 weeks before the merge window opens. Outside of that
feature freeze period pull requests are sent to upstream roughly every week, to
avoid too much coordination pains.

If you're unsure apply your patch here, it can always be cherry-picked to one of
the -fixes patches later on. But in contrast to the drm-intel flow
cherry-picking is not the default.

drm-misc-next-fixes
~~~~~~~~~~~~~~~~~~~

This is for bugfixes to drm-misc-next after feature freeze, but before -rc1 is
tagged.

drm-misc-fixes
~~~~~~~~~~~~~~

This is for bugfixes which target the current -rc cycle.

drm-tip
~~~~~~~

This is the overall integration tree for drm, and lives in
`git://anongit.freedesktop.org/drm-tip`. Every time one of the above branches is
update drm-tip gets rebuild. If there's a conflict see section on `resolving
conflicts when rebuilding drm-tip
<drm-intel.html#resolving-conflicts-when-rebuilding-drm-tip>`_.

Merge Criteria
==============

Right now the only hard merge criteria are:

* Patch is properly reviewed or at least Ack, i.e. don't just push your own
  stuff directly.

* drm-misc is for drm core (non-driver) patches, subsystem-wide refactorings,
  and small trivial patches all over (including drivers). For a detailed list of
  what's all maintained in drm-misc grep for "drm-misc" in MAINTAINERS.

* Larger features can be merged through drm-misc too, but in some cases
  (especially when there are cross-subsystem conflicts) it might make sense to
  merge patches through a dedicated topic tree. The dim_ tooling has full
  support for them, if needed.

* Any non-linear actions (backmerges, merging topic branches and sending out
  pull requests) are only done by the official drm-misc maintainers (currently
  Daniel, Jani and Sean, see MAINTAINERS), and not by committers.

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
now it's just an experiment with a few drivers to figure out a working process.
Slightly different rules apply:

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
  more involved rework in follow-up work. This way lenghty review cycles get
  avoided, which are a drag for both reviewer and author.

Tooling
=======

drm-misc git repositories are managed with dim_:

.. _dim: dim.html

