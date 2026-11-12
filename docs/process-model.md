# Applications, instances and processes

This is the broader execution/management contract. The [application MVP](application-mvp.md)
implements trusted instances; [guest acceptance](mvp.md) exercises its initial
subset. Bounded messaging, completion subscriptions, deadline waits and
protected kernel processes remain unimplemented.
[The architecture](design.md) retains standard Elisp APIs and the small PAL;
this model adds ownership and execution concepts without a new application
framework or a second file/network API.

## Terms and execution models

| Term | Meaning |
| --- | --- |
| Application | Code and an entry contract for a user activity; not necessarily a buffer, service or separate executable. |
| Application instance | One invocation with a stable run identity, lifecycle, context and owned resources. Restart creates a new identity. |
| Kernel process | A future kernel-owned execution/address-space unit with scheduling, resource identity, enforced limits and exit reclamation. |
| Service | A policy for long-lived application work, potentially with restart/startup rules; a service manager is deferred. |
| Buffer | Editable data or a view/controller for one or more instances/resources; closing a buffer does not inherently exit an instance. |
| GNU Emacs process object | An upstream object that can describe a network connection or subprocess; its name/type alone does not establish a kernel process. |

A0 first introduces trusted Elisp instances inside the main Emacs runtime.
They share its heap, globals, buffers and privilege; a run record is not an
independent execution context. Use short callbacks and cooperative cancellation.
Existing extensions may remain ordinary libraries/commands. Do not require every
extension to be packaged as an instance or run in a separate process.

The later protected model associates independently executing applications with
kernel process identities and address spaces. The kernel schedules execution,
enforces implemented limits and reclaims owned kernel resources on exit/fault.
One kernel process may host several cooperating Lisp instances; mapping an
instance to a process is an explicit execution choice, not a universal rule.
Lisp threads and connection filters/sentinels do not create this protection.
Kernel termination/fault containment operates at process granularity: killing a
process ends all logical instances it hosts. Keeping the main Emacs alive while
forcibly terminating an application requires a separate protected worker process.

## Minimal management contract

Expose a small management API with these operations; exact names/types are A0
implementation work. It is separate from standard file/network operations.

| Operation | Contract |
| --- | --- |
| Start | Create a new immutable run identity/generation, establish context and invoke the declared entry under lifecycle/error handling. |
| List/inspect | Return detached state: application/source, execution model, lifecycle, wait reason, owned/tracked resources, logs and exit/errors. |
| Request stop | Record an idempotent cooperative cancellation request, invalidate future instance callback delivery and begin cleanup when control returns. |
| Finish | The application explicitly declares completion; reject future managed callbacks and reach terminal exit after the active-callback/cleanup barrier. |
| Wait | Observe exit by inspection/polling or a bounded completion subscription; never block/pump the shared command loop. Monotonic deadline expiry does not terminate the instance. |
| Restart | Start a new run with a new identity after the caller has explicitly observed the old run's outcome; never reuse its callbacks/resources. |

Entry return completes startup, not the instance: it may have registered future
callbacks. The application explicitly reports finish when its activity is done.
Finish/cancel/error invalidates future managed entry and reaches a cleanup
barrier only after all already-entered managed callbacks unwind. Reentrant stop
or finish must not release resources underneath the callback requesting it.

The lifecycle distinguishes starting, running, stop-requested/completing and
terminal exited/failed outcomes. Waiting is a reason attached to a live run,
not proof of an independently scheduled thread. Entry/callback errors that
unwind normally record the original cause and initiate cleanup. Preserve exit reason/status and
cleanup failures separately; attempt remaining registered cleanup after one
failure. Terminal completion cannot be advertised before that defined cleanup
sequence ends. Failure does not silently restart the application. Restart cannot
proceed while the old run still executes or cleans up; concurrent start of a
different instance must be an explicit choice. Exit/cleanup does not roll back
completed file writes or other external effects.

Stop request and completed exit are different facts. A running callback that
never yields prevents cooperative stop, waiting callbacks and even management
interaction. The initial model offers no forced termination or latency bound;
independently scheduled kernel execution is required to stop a hung application
while the main environment survives.

## Context, callbacks and ownership

At each managed entry/callback, install the intended run context, validate its
identity/generation and restore the caller's prior context on every normal,
error or quit unwind. Capture context when registering a callback; do not infer
ownership later from the selected buffer or a mutable global current-instance
value. Nested entries restore the outer context. Timers, network filters and
message handlers only inherit context through explicit managed registration.

A retired generation must reject late callbacks, stale readiness and queued
messages, including work delivered after resource/handle reuse. Canceling a timer
alone is insufficient if a callback was already queued. Use unwind-safe entry
bookkeeping for partial startup and arbitrary nonlocal exits: restore context
and active-entry counts even when a throw cannot be caught as an ordinary
condition, retaining the original exit and recording incomplete execution.

Resources are explicitly owned, borrowed or shared. An owner registers each
resource and its cleanup/release rule; failed acquisition unwinds partial state.
Borrowed resources are never destroyed by the borrower. Shared resources need
explicit reference/transfer rules and are released only at final ownership
release; aliased descriptors share their open description. Do not close unrelated
user buffers or connections merely because they were current during entry.
`find-file` may return an existing buffer: that is not a new acquisition.
Existing/user buffers remain borrowed unless ownership is explicitly adopted.
Never discard modified buffers on exit; preserve or return them to user
ownership and report the outcome. Specify managed-buffer policy separately.

Explicit transfer validates the destination run and updates both ownership and
accounting together; failed transfer leaves the source responsible. Shared
release removes one reference, not the resource. Bookkeeping must reflect a
consumed handle even if close reports an error.

Cleanup invalidates managed callbacks before releasing resources and attempts
registered releases exactly once, with errors retained. A failed handle close
still consumes that handle under the PAL contract. Track what was adopted;
standard file/network APIs do not automatically become instance-owned merely
because a current context exists. Unregistered activity and direct Lisp mutation
can bypass this discipline in a trusted shared runtime.

Ownership tracking supports diagnosis and cleanup, not security or hard quotas.
Do not claim per-instance heap isolation, precise retained-memory accounting or
enforcement over ambient Emacs APIs. Initially kernel resources belong to the
main Emacs runtime; A0 tracks adopted Lisp-level resources, not independent
kernel ownership for each instance. Correct kernel process ownership must
later cover handles, mappings, queued messages and termination independently of
whether a Lisp cleanup handler returns.

## Communication and management UI

Use explicit destinations and bounded queues/messages with value-based payloads.
The managed API copies supported data rather than sharing mutable payloads by
accident. Specify size/depth/capacity limits and full-queue/closed-destination
errors; discard or reject delivery to retired runs. Do not pass closures, live
buffers or raw resource handles as portable messages. Any shared object or
resource transfer is explicit and confined to its supported execution model.

Choose an explicit serialization schema before crossing future protected
address spaces; shared-heap Lisp identity is not a wire format. Switching to a
protected backend cannot automatically migrate live closures, buffers or shared
objects; a new execution/communication contract and new instance are required.
A0 may deliver local bounded messages cooperatively without a generic bus or
process broker.
Direct calls/ambient data remain possible for trusted extensions and are outside
this managed communication contract.

A future `*Processes*` buffer uses the same list/inspect/stop/wait API as scripts.
Show application/run identity, execution backend and its limits, state/wait
reason, source, logs/errors and stop outcome. Distinguish tracked resources/callback counts from
kernel-accounted CPU/memory, which are unavailable in A0. Do not invent per-app
memory or CPU guarantees from global heap/elapsed-time readings. A buffer is a
management client, not the executing process.

Bound logs/history, tracked registrations and pending completion subscriptions.
A0 chooses numeric caps and terminal-record eviction rules; report truncation
or expired history instead of silently retargeting an identity. Reject an excess
completion subscription explicitly; bounded log eviction must expose lost older
entries. Do not evict a live run merely to conceal exhausted tracking capacity.

## Phasing and future checks

A0 follows E0 and can begin with timers/buffers; file/network ownership extends
only when those backends exist. Check two instance identities, context restore,
nested/error exits, partial-start cleanup, late callback/message rejection,
borrowed/shared ownership/transfer accounting, explicit finish versus entry
return, the cleanup barrier, bounded queues and stop-versus-exit results. Include
an explicitly bounded observation of a non-yielding callback in later testing;
never claim cooperative cancellation can force it to stop.

K0 later implements kernel process execution/protection after the runtime and
ownership contracts. Validate separate mappings, checked entry/access, real
scheduling/limits, process faults/exit and final-resource reclamation. A worker
must not write kernel/main-Emacs memory or execute privileged operations; the
kernel validates caller identity and owned or explicitly delegated resources,
rather than treating separate mappings alone as isolation. Forced
termination of a hung worker must leave kernel/main Emacs progressing. Define
supported spawn/communication operations without promising Unix fork/exec or
all extensions in separate processes. F0/F1 and N0/N1 do not depend on A0 or K0.
Service policy, automatic startup, durable app snapshots and quota UI remain
separate later decisions; no prior implementations are restored by this design.
