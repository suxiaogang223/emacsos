# Guest networking

The network backend implements bounded outbound IPv4 TCP for the fixed QEMU
machine. Standard GNU Emacs connection objects remain the public interface;
there is no second networking language or application framework. Native and
pinned-Wasm Ethernet transport probes, native kernel TCP probes and actual
Emacs TCP/file acceptance have passed. Browser acceptance separately exercises
the pinned Wasm guest, automatic bridge, completed DATA checkpoint and restart.

## Transport and ownership

```text
Elisp connection / filter / sentinel
  -> static Emacs + musl -> bounded Linux ABI -> typed stream operations
  -> lwIP NO_SYS raw TCP/IPv4 -> NE2000 ISA port I/O
       native: QEMU user networking (SLIRP) -> host TCP
       browser: QEMU/Wasm socket -> same-origin WebSocket
                -> development server -> private headless QEMU SLIRP -> host TCP
```

`third_party/lwip/` pins the official upstream mirror's 2.2.1 release. Its
unchanged source and BSD license stay in the submodule; `third_party/lwip.json`
records the exact commit and compiled-source hashes. Driver, freestanding
configuration, stream ownership and ABI translation belong to the parent kernel.
The Emacs fork needs no networking policy changes.

The NIC uses ISA I/O base `0x300`, IRQ 9 with NIC interrupts masked, and MAC
`52:54:00:12:34:56`. Payload transfer uses port I/O to internal NIC memory, without
bus-master DMA into guest RAM. The single interface uses `10.0.2.15/24` with
SLIRP gateway `10.0.2.2`. That gateway reaches the machine running native QEMU or
the browser server; it does not mean the computer running the browser.

The kernel services bounded NIC batches and lwIP timers at syscall and wait
boundaries. PIT wakes waits at approximately 1 ms intervals; network callbacks
never execute on the interrupt stack. `NO_SYS=1` uses lwIP's raw API, without
threads or its socket layer. The ABI maps its typed streams to descriptors.
There is no new scheduler, protected network process or independent Lisp thread.

## Starting and disconnecting

`make run` enables the native NIC and QEMU user networking automatically.
`python3 scripts/qemu.py --no-network` deliberately starts without it; network
operations report an unavailable network, while files and Lisp remain usable.

`make web` and `make web-serve` automatically provide the browser bridge using
the existing `qemu-system-x86_64` host dependency. Each browser WebSocket owns a
separate headless QEMU SLIRP relay; it does not boot a second guest or share a
layer-2 network with other sessions. No manual proxy, TAP device or privileged
network setup is required. The WebSocket carries length-prefixed Ethernet
frames; actual TCP/IP processing remains in the guest. JavaScript does not
substitute a `fetch` download for guest networking.

The server permits at most four simultaneous network sessions, validates the
WebSocket origin and binary framing, bounds message/queue sizes and tears down
the session's relay on disconnect or server shutdown. The page reports link
connection and disconnect separately from Emacs boot and DATA status. A connected
link does not establish that a particular TCP destination is reachable. After a
link disconnect, restart the machine to reconnect; existing TCP streams are not
silently transferred to a new relay. Their guest operations retain their own
error/timeout behavior.

Static hosting or a missing host QEMU permits offline boot without a NIC.
An SSH tunnel needs only the existing web port: the network endpoint shares the
same origin. HTTPS hosting uses WSS automatically. A reverse proxy must forward
the `/network` WebSocket upgrade as well as the documented isolation headers.

The development server has no user authentication. Origin checks are not account
permissions, and SLIRP allows outbound access including services reachable on
the server host. Choose its listening address deliberately. Native QEMU, the
browser bridge, guest Emacs and its applications remain part of the same trusted
system; neither the bridge nor connection ownership supplies isolation.

## Supported API and behavior

The implemented runtime path targets these standard operations:

- `make-network-process` with a numeric IPv4 host, numeric TCP port and
  `:family 'ipv4`; `:nowait t` reports connection completion through the usual
  process state and sentinel.
- Plain `open-network-stream` with `:type 'plain`, `process-send-string`, filters,
  sentinels, `accept-process-output`, `process-status` and `delete-process`.
- `process-send-eof` shuts down the write direction while retaining reads, so a
  peer can send its final response before EOF.

A connection process object is a connection, not a kernel process. Filters and
sentinels run through the normal Emacs event loop and must return promptly.
`:nowait t` makes connection establishment asynchronous; a later large send can
still spend time in Emacs waiting for write capacity. Long Lisp computations or
C calls can delay networking, timers and editing.

| Property | Implemented bound or contract |
| --- | --- |
| Active streams | 16; duplicated descriptors share one stream and open-description flags. |
| Receive storage | 16 KiB ring per stream; capacity is advertised when the application reads. |
| TCP send capacity | 16 KiB per stream, additionally subject to bounded shared heap/queue capacity. |
| Stack heap | One 256 KiB dynamically allocated lwIP heap shared by its allocations. |
| Ethernet | MTU 1500, TCP MSS 1460; bounded NIC loops and receive batches. |
| Connection deadline | 8 seconds of guest monotonic time, evaluated when network service runs. |
| Partial progress | Positive write counts mean accepted bytes, not peer delivery; would-block is distinct from zero-byte progress. |
| EOF | A nonempty read returns zero only after queued bytes drain at peer EOF or local read shutdown. |
| Errors | Refusal, timeout, reset and other backend errors retain their original ABI/Emacs error where supported. |
| `SO_ERROR` | Reading clears the pending socket error; terminal read/write failure remains recorded. |
| Close | Final descriptor close consumes the stream even if cleanup reports failure; detached callbacks cannot target a reused slot. |

Unsupported socket families, options and operations return errors. DNS, UDP,
listening/accepting, IPv6, DHCP, IP-fragment reassembly, TLS/HTTPS and secure
randomness are not implemented. No cryptographic entropy or unpredictable TCP
sequence-number guarantee is provided. The fixed address/profile does not imply
general Linux compatibility or general device support.

## Controlled HTTP-to-file example

This manual example illustrates the numeric-address TCP and existing file APIs;
it is not a general HTTP client. It assumes a trusted, small HTTP/1.0 response
which closes the connection, and deliberately does not load downloaded code.
On the host running QEMU or the web server, prepare a local fixture:

```sh
mkdir -p build/http-fixture
printf '(message "Downloaded from the controlled fixture")\n' > build/http-fixture/example.el
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/http-fixture
```

Evaluate this in guest Emacs:

```elisp
(require 'network-stream)
(let* ((buffer (generate-new-buffer " *http-fixture*"))
       (process nil)
       (deadline (+ (float-time) 10)))
  (unwind-protect
      (progn
        (with-current-buffer buffer (set-buffer-multibyte nil))
        (setq process
              (open-network-stream "http-fixture" buffer "10.0.2.2" 8000
                                   :type 'plain :coding 'binary))
        (set-process-query-on-exit-flag process nil)
        (set-process-sentinel process #'ignore)
        (set-process-coding-system process 'binary 'binary)
        (process-send-string
         process "GET /example.el HTTP/1.0\r\nHost: fixture\r\nConnection: close\r\n\r\n")
        (while (and (process-live-p process) (< (float-time) deadline))
          (accept-process-output process 0.1)
          (when (> (buffer-size buffer) 65536)
            (error "Fixture response exceeds the example limit")))
        (when (process-live-p process) (error "Fixture response timed out"))
        (when (> (buffer-size buffer) 65536)
          (error "Fixture response exceeds the example limit"))
        (with-current-buffer buffer
          (goto-char (point-min))
          (unless (looking-at "HTTP/1\\.[01] 200 ")
            (error "Fixture did not return HTTP 200"))
          (unless (search-forward "\r\n\r\n" nil t)
            (error "Fixture response has no header terminator"))
          (let ((body-start (point))
                (case-fold-search t)
                (coding-system-for-write 'binary))
            (goto-char (point-min))
            (unless (re-search-forward
                     "\r\nContent-Length: *\\([0-9]+\\)\r\n" body-start t)
              (error "Fixture response has no Content-Length"))
            (unless (= (string-to-number (match-string 1))
                       (- (point-max) body-start))
              (error "Fixture response is incomplete"))
            (write-region body-start (point-max)
                          "/home/user/projects/downloaded.el" nil 'silent))))
    (when (processp process) (delete-process process))
    (kill-buffer buffer)))
```

Inspect the saved file before explicitly loading it. The `Host` header is merely
request text; the numeric destination performs no DNS lookup. Ordinary guest
save/sync and [DATA retention](storage.md) still apply: the browser must complete
a checkpoint after the save, and an unsaved buffer is never retained. This
numeric HTTP/file loop is separate from the still-deferred DNS part of N2.

## Validation boundaries

| Command or evidence | Establishes |
| --- | --- |
| Native and pinned-Wasm N0 probes | Real bidirectional Ethernet; ARP through SLIRP, including the explicit browser bridge. |
| `make test-network` | Host ABI behavior using a controlled fake stream PAL; no NIC or guest proof. |
| `make test-probes` | Includes actual native kernel TCP probes: 64 KiB echo, partial writes/backpressure, half-close/EOF, refusal, reset, timeout, cancellation/reuse and absent NIC. |
| `make test-network-guest` | Actual Emacs numeric TCP, filters/sentinels, timers, HTTP save/load and separate-boot DATA acceptance; requires a clean matching source/image. |
| `make test-browser-network` | Actual pinned-Wasm guest TCP through the automatic bridge, timer progress, DATA save, completed checkpoint and restart. Requires the already-running web server and optional Playwright tools. |

Native Emacs acceptance has exercised a 64 KiB asynchronous echo, write
half-close and timers; original refusal/timeout errors; DATA writes while a
connection waits; plain `open-network-stream` HTTP to a controlled numeric
fixture; response hashing, save and explicit load; and a COM2-frozen DATA copy
opened in a new guest, with source retained but never automatically loaded.
`build/image/network/results.json` binds these observations to source, runtime,
image and checkpoint identities.

Actual browser acceptance has exercised asynchronous fragmented TCP echo,
timer progress, a saved response, a completed DATA checkpoint and exact file
contents after restart. Its separate report records the clean source, fetched
SYSTEM hash and build manifest; all three must match. Host relay tests also
exercise real SLIRP, TLS WebSocket transport, queue limits, disconnect and helper
cleanup. TLS here protects the browser-to-server WebSocket; it does not provide
guest TLS/HTTPS. Compilation, a link-status label or webpage HTTP success alone
does not establish guest TCP. DNS remains deferred even when numeric HTTP succeeds.
