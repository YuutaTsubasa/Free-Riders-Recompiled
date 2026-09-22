# Original blend control and native pipeline state

The verified native entry at commit97215e3 reaches824E9218/LR827F5F68 with
device71600000, target0 and packed07010706. The original setter stores one
RB_BLENDCONTROL word and ORs its exact dirty bits into device+10; it submits no
GPU work and assigns no return value.

Decode the audited Xenos blend fields to a backend-neutral typed state. Retain
four independently initialized target slots, keep write masks separate and
require an explicit write mask when producing a Plume pipeline description.
Reject reserved/unsupported encodings before any guest or native mutation.
In particular, do not map RGB constant-alpha to RGB constant-color without
the required extra state; leave uncertain variants blocked.

GuestGraphics will own the actual retained state, preflight both original guest
writes before publishing it, then mirror the exact original cache and dirty
effects. The strong original entry hook forwards the reached caller context
without supplying a result; other native consumers remain guarded. Test typed
semantics, per-target isolation, original writes, guard failures and real entry.
The next actual state/resource boundary drives further work. This completes a
state dependency only: no title, game draw or PSO consumption is claimed yet.
