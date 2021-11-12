===========================================
Matthew Brost's XE driver notes / questions
===========================================

Adding my initial notes for Jason's XE PoC driver. Some of this is just my
opinion and obviously open to discussion. Some of it just spitballing for
features that likely are not going to be implemented anytime soon.

TL;DR - Great start, let's align on a few things before moving forward.

Code sharing
============

* Lots of code could potentially be shared between the i915 and XE
	* e.g. Downloading + authenticating GuC firmware
* My opinion is just duplicate the code in XE aside from display
	* Probably not worth the effort initially to share code
	* Will be faster to just duplicate rather than share
	* Can revisit later if needed

Structures
==========

Notes on the XE structures

General notes on structures 
---------------------------

* Let's try to pull in the GuC backend ASAP
	* This will help us put the structures together in a way where the
	  backends are not leaking into common code
	* Also help us figure out where we need vfuncs, etc...
	* Large pitfalls of the i915 was execlists bled everywhere in
	  the driver, let's avoid that 
* I think kzalloc'ing everything is probably fine for now, but at some point we
  may need to start using slabs for certain objects (sched_job comes to mind).
  We need to keep this in mind going forward so we don't code ourselves into a
  bad position.
	* Helpers for all object allocation / free from the start?

struct xe_hw_engine
-------------------

* Should be a physical engine only, decoupled from submission
	* intel_engine_cs in the i915 is used both for physical engine state and
	  some submission state. This really breaks down in our virtual engine
	  implementation where we have to create a dummy intel_engine_cs because
	  parts of the structure are used in submission while other huge parts
	  of the structure are completely unused.
	* 'struct xe_engine' should contain everything related to submission
* 'struct xe_hw_fence_irq' probably should be a pointer with a ref count
	* With GuC virtual engines we have no idea which physical engine, within
	  a scheduling class (more on this below), a sched_job will get run on.
	  So when we get an interrupt from a physical engine indicating a user
	  ring interrupt (sched_job complete) we need to search a list which
	  contains all the sched_job across multiple physical engines.
	* Having a pointer allows each physical engine to point to a common list
	* The execlists virtual engines implementation likely won't know which
	  physical engine an individual sched_job is run on either
	* In general once we start making assumptions like this sched_job is
	  tied to this physical engine (other structures in the XE driver)
	  things start to go bad
	* The aforementioned 'scheduling class' is slightly different than
	  'engine class'. Future GuC versions are going to add support for
	  instances of the same engine class to be split out into different
	  scheduling classes. A scheduling class is basically a queue of
	  runnable contexts. The reasoning behind this is of instances of class
	  might have specical capabilities that result in very high priority use
	  cases (e.g. an engine instance can program page tables). These
	  instances should have there own scheduling queue. We should build in a
	  disconnect from 'engine class' to 'scheduling class' from the start
	  and perhaps even give this knowledge to the user (more below). 
* kernel_lrc
	* Makes sense to have kernel contexts stored in 'xe_hw_engine' unless we
	  want to allow virtual engine kernel contexts (e.g. we want to migrate
	  an object via copy engine and we don't care which instance).
		* If we want VEs for kernel context we might need to rethink
		  how / where we stick kernel contexts
	* Should this be pointer? In i915 we have multiple kernel contexts per
	  phyical engine and pointer for each one we use (e.g. 1 for heartbeat /
	  kernel context switches, 1 for binding, etc...). Do we need multiple?
	* Should this be an 'xe_engine', probably as the xe_engine will have
	  backend specific info in it
* vfuncs
	* Are we going to have backend specific vfuncs in this structure,
	  probably
	* Likely can be organically added over time
* Need logical instance to speak with GuC / parallel submit uAPI (more below on
  logical instances)

struct xe_engine
----------------

* union execlists / guc rather than OO
	* OO is great but might be overkill to start
	* unions of backend specific fields is great place to start
	* if a common layer is touching something in backend specific fields,
	  we've done something wrong
* vfuncs
	* e.g. Probably need vfunc to emit batch, almost certainly more
* Mask (likely logical) of xe_hw_engines where it can be scheduled
* Array of xe_lrc for parallel submit? More on that below.
	* xe_lrc lrc[0]
	* kzalloc(sizeof(xe_engine) + width * sizeof(xe_lrc)) 
* Dynamic size allocation conflicts with slab allocation
	* Could have 2 slab allocators, 1 for non-parallel + 1 for parallel
	  based on max width

struct xe_execlist
------------------

* s/xe_execlist/xe_execlists_engine
* want a name that indicates this part of the xe_engine specific to the backend

struct xe_gt
------------

* Likely need a GT structure right away even if only single GT initially

struct xe_execlist_port
-----------------------

* s/xe_execlist_port/xe_execlists_port
* Right now, 1 to 1 with xe_hw_engine
* If we want to do VE like the GuC this becomes 1 per 'schedulng class'
	* e.g. multiple xe_hw_engine where each xe_engine can be placed
* Each xe_engine has mask of where it can be placed
* Simple algorithm, if HoQ can be scheduled place xe_engine on xe_hw_engine
	* this is what GuC does

struct xe_sched_job
-------------------

* Array of user_batch_addr for parallel submit? More on parallel submit below.
	* u64 user_batch_addr[0]
	* kzalloc(sizeof(xe_sched_job) * width * sizeof(u64))
* Same as xe_engine for slab allocation
* dma_fence becomes a dma_fence_array of xe_hw_fence for parallel submit
	* hierarchy of enabling signalling just works, right?

struct xe_hw_fence
------------------

* Love this structure and fits perfectly with DRM scheduler / DMA buf subsystem
* May need a hook to disconnect signalled from seqno, we do this in the i915
  when cancel / skip requests
	* Minor detail that we can likely work out later
* Good candidate for a slab allocations as we allocate one of these every
  execbuf IOCTL

uAPI
====

General notes on uAPI
---------------------

* Love seperation between execbuf IOCTL + address binding
* Love 'GEM_CONTEXT' concept dropped - xe_engines only
* I'm told compute uses the userptr interface, do we need something like this in
  XE?
	* My understanding is this takes user malloc'd buffer and then creates
	  graphics BO based on the user malloc 

struct drm_xe_engine_class_instance
-----------------------------------

* Include GT instance?
	* i915 we made a choice not to expose the GT, was that a mistake?
	* Initial done because multi-GT was a secret, but no longer the case
	* Thinking we are ok to expose GTs to the user
* engine_instance make this the logical instance?
	* in i915 instance is an opaque handle (neither physical or logical)
	* Parallel submission is logical space per GuC interface / how HW works
	* Had to add an interface to expose logical instance to i915 for
	  parallel submission
	* Having everything in logical space from the start makes sense to me
* Add 'scheduling class'? 
	* 'scheduing class' discussed above / below
	* I don't see down side of reporting as much information possible in
	  the engine tuple
* If we have GT + scheduling class VE / parallel submits are allowed to be
  created one matching GTs + scheduling classes

struct drm_xe_gem_create
------------------------

* s/drm_xe_gem_create/drm_xe_bo_create
	* gem is confusing because i915 it means 'graphic execution manager'
	* what does gem mean in this context?
	* IMO BO (buffer object) vs GEM (no sure what this means), BO is more
	  clear

struct drm_xe_engine_create
---------------------------

* Add width field (parallel submit)
* Add number placements fields (parallel submit / virtual engines)
* engine_id is an dynamically sized array
	* not sure if this works with how the DRM IOCTLs work (no dynamic size
	  for base IOCTLs), array might need to be an extension?
	* could the DRM core copy the first entry and our IOCTL copy the rest of
	  the array if paralle submit / virtual engine?

struct drm_xe_exec
------------------

* Array of address for parallel submit
	* same as above, not sure dynamic sized arrays work with the DRM IOCTLs
* For implicit object conflict tracking, the user passes in the read / writes as
  a DRM_XE_SYNC_DMA_BUF, right?

Parallel submission / virtual engines
=====================================

* 1 xe_engine per parallel submit
	* Just add width + array of xe_lrc
* Parallel submit fixed width
	* User can only submit N (width) BB in execbuf IOCTL (same as i915)
	* Number of BB per execbuf IOCTL implicit based on xe_engine (same as i915)
* 1 xe_sched_job per parallel submit
	* Differs from i915 where we have N (width) i915_requests per parallel submit 
* 1 xe_engine per virtual engine 
	* Just a mask of where sched_jobs can be placed

Execlists backend
=================

* As mentioned above, xe_execlist_port, should likely transition to a queue for
  each 'scheduling class'
	* Parallel submit / Virtual engines more or less just work then
	* Aligns with GuC implementation
* Likely should use CSBs for functionality if we can't get the execbuf status
  working
* Productizing execlists is much more than just the backend
	* e.g. Reset flows, error capture, PM stats, OA, power management,
	  etc...
	* As such, I think we keep this simple as possible just for driver
	  bringup and to have 2 backends from the start so we code the driver
	  correctly ensuring it is easy to add more backends in the future 
* Should we implement virtual engines?
	* I think yes as this is part of the uAPI
* Should we implement parallel submit?
	* I think yes as this is part of the uAPI
* Should we implement timeslicing?
	* I think no as this not being productized and not part of the uAPI
* Should we implmeent preemption?
	* I think no as this not being productized and not part of the uAPI

GuC backend
===========

* Likely can refactor to have a more or less a big dumb mutex for everything
	* Protects submission, sending H2G, processing G2H, and GT resets
	* CPU are orders of magnitude faster than the GuC, likely can block for
	  bit when processing things related to the GuC
	* Can bring back in fine grain locking if needed
	* Much easier to start with big lock -> go to fine grained as needed
* guc_id perma-pinned at xe_engine creation time
	* Released at destroy
	* If no guc_id available user is out luck (e.g. fail xe_engine create)
	* Exposes a DoS but not worse than existing DoS in i915
* 1 GPU scheduler for all of GuC submission vs. 1 GPU scheduler per xe_engine?
	* Execlists current implementation does 1 GPU scheduler per xe_engine
	* Likely can start with 1 per xe_engine, can switch to 1 GPU scheduler
	  per GuC if this doesn't scale
	* Keep this mind that we may switch, don't code ourselves into a corner
	* 1 scheduler per xe_engine makes per-context reset easier
 	* 1 scheduler per xe_engine makes timing out xe_sched_jobs easier
* No entry into backend from IRQ contexts (e.g. we can use mutexes)
* This is backend in general comment - avoid at all costs entering the backend
  anyway aside from the DRM scheduler. One of the hardest things in the i915 is
  the backend could be entered from like 10 different places all at the same
  and from IRQ contexts. If funnel almost everything through the DRM scheduler
  it will makes everyones lives so much easier. If this doesn't scale, we can
  fix it but until then this should be the goal. Certain things will hard to
  funnel throught the scheduler - e.g. the final put on xe_engine but we should
  at least make an effort to minimize exposer as much as possible. 

Priority inheritance
====================

* Basically i915_scheduler.c in the i915
* Used for page flips to make anything the page flips depends on MAX priority
* Also used for high priority user requests which depends on low priority user
  requests to boost there priority
	* Prevents priority inversion
	* Is this a real use case?
* Do we need this?
	* AFAIK this feature isn't implement in any other DRM driver
	* Let's do some research and find out if this needed
* If we do need this, should be implemented in drm scheduler core
	* Ported this over into a drm scheduler PoC branch of mine
	* Not very painful at all

Resets
======

* Only support GT reset (no per-engine done by XE)
	* GuC handles per-engine reset
	* Execlists not being productized, not a lot of ROI implementing
	  per-engine resets just for execlists
* How to detect a GuC hang
	* i915 uses heartbeat on each physical engine
	* same idea for XE or something else?

Driver load order
=================

* Huge discussions around driver load order as it related to LMEM / GuC / SRIOV
  in the i915
* Turning into a huge mess / issue / rework
* We need to keep a pulse of the work in DII / drm-tip so we can get the driver
  load order correct from the start in the new driver
* Will dig up some Docs on this for a reference

Context persistence
===================

* e.g. Can an xe_engine's existing jobs complete after the xe_engine is
  destroyed or do we kill all sched_jobs immediately
* Another huge mess in the i915 that needs rework
* Need to keep a pulse on this in DII / upstream
* Will dig up some Docs on this for a reference

Selftests
=========

* Do you want a selftest infrastructure in XE?
* In the i915 we have spent so much time debugging selftest failures because the
  selftests are so tightly coupled to certain i915 (e.g. execlists scheduling)
  or HW platforms (e.g. LMEM vs. SMEM) behaviors
	* One small change is made to the i915 and then selftests fail
	* Selftests run a new platform with slightly different behavior and then
	  selftests fail
	* In i915 no one really understanding the whole stack so debugging these
	  takes forever (or only certain engineers can actually understand the
	  failures) and is massive time suck
	* Selftests can wild and crazy things that user could never do which can
	  be good (stress driver) or bad (rathole on debugging non-issues) 
	* I'd say 95% of selftest failures I've looked are test issues
	* Massive waste of time and energy
	* If we use selftests we really need to avoid these pitfalls
* All of this being said, selftests can be useful
