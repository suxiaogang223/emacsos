# Trusted application MVP

`lisp/emacsos-app.el` implements application policy for one trusted Emacs
runtime. `lisp/emacsos-counter.el` demonstrates two independent invocations.
The implementation is host-tested; it does not establish that Emacs, timers,
files or this UI execute in the emacsOS guest. Runtime-image integration and
guest functional acceptance are separate work.

## Try on host Emacs

From the repository root:

```sh
emacs -Q -L lisp -l emacsos-counter
```

Evaluate `(emacsos-counter-start)` twice, then use `M-x emacsos-processes`.
Each invocation gets a new identity and a separate `*Counter ID*` buffer.
The timer updates its display every second. In `*Processes*`, use:

| Key | Operation |
| --- | --- |
| `g` | Refresh snapshots. |
| `RET` | Inspect state, backend limits, resources, logs and exit diagnostics. |
| `s` | Request cooperative stop for the selected run. |
| `r` | Observe a terminal run and start a new invocation. |
| `v` | Visit its declared source with normal `find-file`. |

Stop one counter: the other continues. Restart creates a new identity and
buffer; late delivery to the old timer does nothing. Editing a counter buffer
marks it modified, pauses display replacement and preserves its contents on
stop. Closing an unmodified counter buffer explicitly makes its next tick
finish the instance. The UI displays tracked registrations, not per-app CPU or
heap accounting. Details identify the trusted shared-runtime backend.

## Small public management API

```elisp
(require 'emacsos-app)
(defun example-entry ()
  (let ((id emacsos-app-current))
    (emacsos-app-buffer id "*Example*")
    (emacsos-app-run-at-time id 1 nil #'emacsos-app-finish id "Completed")))

(setq example-run (emacsos-app-start "Example" #'example-entry))
(emacsos-app-list)
(emacsos-app-inspect example-run)
(emacsos-app-wait example-run) ; One poll: pending or complete, never pumps events.
(emacsos-app-request-stop example-run)
(emacsos-app-restart example-run) ; Requires observed terminal outcome.
```

Start takes a name, entry function, optional argument list and source file.
Entry return ends startup only; use `emacsos-app-finish` for successful completion.
Start and managed callback errors/quits record the original condition, initiate
cleanup and preserve the original signal. Arbitrary throws unwind context and
active counts, record incomplete execution and preserve their nonlocal exit.
Failed starts remain visible through `emacsos-app-list` even if start did not
return an identity.

`emacsos-app-wrap` explicitly captures context for an external callback.
`emacsos-app-run-at-time` registers both its callback and owned timer.
`emacsos-app-set-wait-reason` and `emacsos-app-log` provide diagnostics. Stop,
finish and error invalidate future managed delivery immediately. Resource
cleanup waits for every already-entered callback to unwind, including nested
entries. A stopped run is terminal only after cleanup completes. Inspect/wait
observe terminal outcomes; listing alone does not. Restart cannot interrupt or
reuse a live run.

`emacsos-app-adopt` explicitly registers a resource with a release function and
owned/borrowed mode. Borrowing never releases the resource. Release functions
receive the resource, must return or signal an ordinary condition, and are
called once after consuming the registration. Cleanup attempts all registered
releases even after one fails. Original execution failure, completion reason and
cleanup failures remain separate; any cleanup failure makes terminal state
`failed`. Release rules that violate the contract by throwing still unwind
bookkeeping and attempt remaining cleanup, but their nonlocal exit can replace
an already-unwinding exit under ordinary Elisp `unwind-protect` semantics.

`emacsos-app-share` adds an explicit reference; the original release runs only
after the final shared reference is released. `emacsos-app-transfer` validates
and updates both runs atomically; rejected transfers leave the original owner
responsible. Transfer and explicit release require the source to have no active
managed callback, avoiding release underneath execution. Resource transfer does
not retarget a timer's captured callback context. Use it for resources whose
ownership can actually move independently of their callbacks.

`emacsos-app-buffer` creates a new owned buffer. Existing buffers passed to
`emacsos-app-adopt-buffer` are borrowed unless ownership is explicitly adopted.
Modified buffers and buffers whose kill query vetoes cleanup return to user
ownership. No implicit `find-file` adoption or standard API interception occurs.
Custom release functions remain responsible for the safety of their resources.

## Bounds and limits

The registry retains at most 64 live/terminal records. A new start evicts the
oldest terminal record when full; it never evicts live instances. A lookup of a
retired identity reports `emacsos-app-expired`, and identities are never reused.
Each run has at most 64 resource registrations, including callback registrations.
A managed timer uses two slots; registrations remain until explicit release or
exit, even after a one-shot timer fires. Each run retains 32 log entries of up
to 512 characters each and exposes the dropped-entry count. Cleanup diagnostics
retain the latest 32 entries and expose a separate dropped-entry count, including
after repeated explicit resource release/acquisition. Inspection returns detached
diagnostic data rather than live resources or closures. These are bookkeeping
bounds, not heap quotas; trusted entry arguments/closures can retain arbitrary
shared objects.

This MVP has polling wait only. Message queues, completion subscriptions,
deadline waits, automatic service startup and protected kernel processes from
the broader [process model](process-model.md) remain future work. File/network
ownership can be adopted explicitly through custom releases when real backends
exist. There is no per-instance isolation, enforced resource access, precise
memory accounting or rollback of external effects. A non-yielding callback
blocks the UI and other callbacks, so cooperative stop has no latency guarantee.
Ordinary Emacs extensions and direct calls can bypass this tracking discipline.

## Validation

```sh
emacs -Q --batch -L lisp -l tests/emacsos-app-tests.el -f ert-run-tests-batch-and-exit
```

Host ERT exercises lifecycle, nested context, cleanup barriers, errors, quit,
arbitrary throw, partial acquisition, stale callbacks, explicit resource
sharing/transfer, bounded records/logs, modified buffers, the two-counter demo
and the management UI. The demo test drives actual timer callback functions
deterministically; it does not claim guest timer or scheduling acceptance.
