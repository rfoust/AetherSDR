# Stage 4: engine TX coordinator and primary desktop paths

This is the first Stage 4 increment of RFC #3849, after the receive-control
milestone (#5563). It does **not** enable daemon transmission or complete the
RFC's multi-client TX arbiter.

The command-queue follow-up extends that foundation, not its authority:
primary Flex commands and CWX text now carry cancellation to the terminal
TCP writer. The desktop compatibility actor remains shared.

## Authority and ownership

`TxCoordinator` lives in `libaethercore`. Trusted engine composition registers
opaque actor handles with a transmit policy. Handles belong to one coordinator;
client strings, `PttSource` values and handles from another coordinator cannot
mint authority. Mutations are confined to the coordinator's owning thread.

There are at most 64 live actor registrations. An operation has one owner,
an opaque identity and a cancellation fence. Repeated acquisition by that owner
keeps the operation and its original duration limit; another actor is refused.
Zero maximum duration preserves the existing unbounded local-operator workflow.
The optional bounded policy is tested in isolation but is not granted to daemon
clients by this increment.

Cancellation, revocation and expiry invalidate key-on delivery before invoking
the stop callback. Recovery is published before that callback and remains an
admission barrier even if the callback acknowledges synchronously. A stale
operation cannot acknowledge or complete another operation. Connection reset
also invalidates queued cleanup, including when a transport object survives.
An idle cleanup fence can permit key-up delivery but cannot acquire, complete
or acknowledge an owned operation.

`RadioModel` registers one **transitional desktop compatibility actor**. Existing
desktop, CAT, TCI, MIDI, keyer and bridge calls still share their existing entry
points. This does not establish individual client identity. `PttSource` remains
display/preflight metadata, never the owner or a permission grant.

## Primary paths

| Intent | Engine route |
| --- | --- |
| MOX/PTT | TransmitModel admission, RadioModel, `IRadioBackend::setKeying` |
| TUNE / two-tone | TransmitModel admission, RadioModel, `IRadioBackend::setTune` |
| Internal ATU | TransmitModel admission, RadioModel, `IRadioBackend::setAtu` |
| Straight key / paddle / timed CW / CW PTT | RadioModel admission and existing backend or NetCW transport |
| CWX text, characters and macros | Actual-send admission before commands or local-keyer delivery |

MOX, TUNE and ATU no longer emit parallel raw keying commands from
`TransmitModel`. Flex encodes the same FlexLib 4.2.18 commands behind its typed
backend methods. The receive-only backend, receive-only mode, pan inhibit and
existing operator preflight checks remain in force. Admission refusals carry a
distinct operator message and notification key per `Refusal` reason; only
`Recovering` is reachable while a single desktop actor exists, but the per-client
actors of the next increment make the rest reachable. Key-up, bypass and abort
are not subject to key-on permission checks.

Admission precedes optimistic model state. The resulting fence is checked
again after synchronous notifications. Intent epochs prevent a reentrant new
key from being followed by the preceding intent's stale key-up. This does not
turn local intent into radio readback: existing Flex interlock, Icom PTT
readback and command-edge fallback provenance are unchanged.

Direct `TransmitModel::setMox(true)` now runs the same source-aware preflight
as the operator PTT path, before optimistic state or admission. This intentionally
closes the former direct-call bypass: non-DAX voice MOX needs an assigned TX
slice. A connection attempt is not a connected session; admission stays closed
until connection completion. Tests of dispatch use an injected backend with
the explicit slice/admission prerequisites instead of an unanswered connection.

The compatibility actor tracks active primary intent kinds. CWX queue drain
does not end a separately held MOX intent. QSK interlock gaps do not release
the CWX batch. CWX clear/reset fences remaining segments and late replies
through its existing drain epoch as well as the engine operation.
Unsupported radio-side CWX and tunerless ATU are refused before acquisition.
Terminal ATU status closes local intent even when an in-progress report was
missed; synchronous status observers cannot use that old completion to end a
new ATU command. This status has no operation ID and is not qualified readback
for multi-client arbitration.

CWX rejection or invalid reply cancels the current batch and disarms its drain
watch; stale replies are fenced by both epoch and operation. Cancelled speed
expansion restores the base WPM without sending later text. Neutral radio-side
text dispatch happens before sidetone notification, so a rejected batch does
not start a misleading local playback. Non-Flex acceptance completes the local
handoff after all segments, not RF transmission. An unsynced Flex macro likewise
completes only its local handoff: its text length is unknown, so it cannot arm
the indexed drain watch. Both remain explicitly unsuitable as another client's
TX-admission evidence.

## Normal release versus cancellation

Normal PTT release retains Quindar outro and RADE end-of-over sequencing.
Each deferred release carries its original atomic cancellation fence. New
key-on, explicit stop, reset or destruction invalidates it. Duplicate releases
do not truncate an in-flight outro. Release execution stays on the owning
thread; workers may inspect the fence but cannot mutate the model through it.

RADE completion carries the original request ID. Its queued audio-gate closure
and delayed PTT release check the original release fence. Re-engagement during
EOO also has an explicit intent notification, because optimistic MOX can remain
true throughout that interval and emit no state edge. The hardware/interlock
fallback may finish its own audio tail but gains no authority to release a
later carrier.

NetCW retains the existing timestamp, packet-count, dedup index, four UDP
copies and TCP backstop. UDP copies, the TCP backstop and the no-stream TCP
fallback capture the original transport and check their fences at dispatch.
Normal key-up retains the operation until both participating transport queues
consume it (including the final UDP copy), so a short element
does not lose its already-queued key-down. This is transport completion, not
proof of RF reception or radio-idle state.
Iambic producer-thread input captures a session generation before queueing onto
the model; reset/reconnect and the scheduled-time floor reject old-session edges
before either Flex or non-Flex delivery. This is session isolation, not yet
per-producer authorization within a shared desktop operation.

### Primary Flex and CWX command queues

Flex `setKeying`, `setTune`, `setAtu` and `abortCwText` use a dedicated TX
command sink, with no fallback to the generic command sink. Encoding remains
behind the backend seam; the sink's keying/cleanup flag is a classification,
not authority. RadioModel captures its original operation and transport and
checks them at the LAN writer or synchronous WAN dispatch.

Normal primary key-up retains local operation lifetime until the TCP queue
consumes it. This preserves both edges of a short key-down/key-up sequence;
disconnect cancels key-down immediately instead. Idle cleanup cannot unkey a
subsequently acquired operation, and a late queue callback cannot finish a
newer operation or a reengaged local intent. Queue consumption includes a
cancelled write and is **not** transport delivery, a command acknowledgement,
radio readback, or a multi-client handoff permission.

CWX text/macro commands additionally capture a worker-safe batch cancellation
fence. Clear/reset invalidates that fence immediately even while a separately
held MOX keeps the compatibility operation alive. Cancelled queued commands
retire their pending reply callbacks. Worker checks never read CwxModel state
or dereference the model's synchronous admission closure.

An unsynced macro retains its operation until queued local handoff completes.
Appending one to known text abandons the now-incomplete drain index without
cancelling either the text or macro waiting for transport. Clear/reset still
cancels both. Partial `cwx erase` keeps its existing, unqualified semantics;
this increment does not resolve the hardware-dependent drain-index question
documented in `CwxModel::erase`.

Disconnect, forced disconnect and backend replacement cancel before transport
reuse. Session admission closes before any cancellation, pending-command reply
or model-removal notification, even when no operation was active. It stays
closed through the disconnect gap and reopens only on the new connection edge.
Stop cleanup remains in recovery until transport loss/teardown is
acknowledged. Every stop source therefore needs a matching acknowledgment: an
unacknowledged stop keeps admission closed for the rest of the session. `reset()`
is the only production stop source in this increment and the disconnect/teardown
paths acknowledge it; `cancel()`, `revoke()`, `expire()` and `emergencyStop()`
have no production callers yet, so the increment that gives one of them a caller
must land its acknowledgment path in the same change. A refusal that reaches the
coordinator outside a disconnect gap is logged, because the session latch
short-circuits the normal case before admission is attempted. Destruction does
not call presentation observers while the aggregate is partially destroyed.

## Deliberate limits and next increment

### Bridge watchdog compatibility-operation fence

The watchdog captures the engine operation produced by an accepted bridge
action, not just a boolean claim. Pre-dispatch operation identity prevents
adoption of an existing batch during an RX/QSK readback gap. Nested requests
restore their caller's sample. A fresh local operation cannot inherit an older
watchdog, and cleanup rechecks the captured identity after each synchronous
notification. Cleanup covers MOX, TUNE, ATU, straight-key/CW PTT and CWX.

The existing 20-second default is unchanged. Its clock is monotonic, repeated
commands cannot extend it, and a pending operation or readback gap does not
disarm it. Normal operator duration remains unbounded. The diagnostic's per-key
observer receives pre-key identity/state after synchronous engine admission,
before its first event-loop wait; it no longer claims a transmission in advance.

Deferred bridge widget invocations capture their permission epoch, sample the
operation at actual execution and claim after admission. Revocation, an
observe-only transition, model replacement, bridge stop or destruction fences
queued TX actions; re-enabling permission does not resurrect them.

This is still the shared compatibility actor, not per-socket isolation or a
solution for arbitrary asynchronous widget/keyer continuations. A normal local
completion plus reported TX tail is not qualified stop evidence. The watchdog
continues to use the existing stop entry points, not the coordinator's future
cancel/expiry recovery path; independent client handoff remains disabled.

`automation_tx_watchdog_test` injects a socket-free backend and exercises real
engine admission, typed bridge requests, permission changes and deferred
callbacks. It covers replacement between polls, repeated commands, refused and
nested requests, RX gaps, CW/ATU cleanup and reentrant replacement during stop.
It also waits for the production timer to expire an owned operation after
initial permission enable and after disable/re-enable, without calling the poll
handler directly.

The daemon still advertises no TX permission or method. No credential
provisioning, remote listener, TLS policy, device discovery or transmit default
is changed. The bridge's existing permission gate and watchdog remain intact.

Desktop compatibility completion ends a local intent, **not** a qualified
radio-idle claim. It must not be reused to authorize another client's TX.
The remaining work must propagate per-client actors through all integration
and audio producers, bind bounded actor expiry to engine scheduling, complete
qualified stop/readback recovery and fence remaining queued audio/wire paths.
Only after that coverage is demonstrated can daemon TX grants be considered.
The separately tracked CW/TUNE UX interlock in #5513 is not replaced here.

Cross-tracker sequencing does not block this Stage 4 work (#3849's
cross-tracker prerequisites comment, 2026-09-11). Capability serialization
must not widen before #5594/M1 and the capability-record convention are ready.
The Flex RX PCM conversion belongs to #5468/#5598; this TX-command increment
does not perform that conversion or begin Stage 5 streaming.

## Verification boundary

`tx_coordinator_test` exercises actor isolation, duration, revocation, recovery,
stale handles, thread affinity, reentrancy and cleanup-only authority.
`tx_operation_integration_test` drives production model entry points with an
injected backend and terminal packet writer. It does not start a radio peer,
bind a socket, discover hardware or transmit RF. It covers typed dispatch,
refusals, deferred release, replacement, reentrant intent, Quindar, CWX and
queued NetCW, primary Flex and CWX delivery. The additional cases cover short
MOX/TUNE/ATU edges, reset-before-write, idle cleanup, reengagement, CWX
clear-and-replace while MOX is held, producer destruction, cancelled reply
retirement, and preservation of unknown-length macro tails. Existing model,
ATU, Icom, CAT/TUNE, applet and bridge
watchdog tests remain part of the targeted regression set.
The private test-only terminal writers in `PanadapterStream` and
`RadioConnection` allow these queue tests to exercise production dispatch without
initializing sockets or substituting synthetic radio firmware.

Native Demo/MCP checks with TX disabled can establish launch, identity,
receive-path and refusal behavior. They cannot establish over-the-air CW
timing, an amplifier's response, or RADE RF tail quality; those require
separately authorized hardware verification.
