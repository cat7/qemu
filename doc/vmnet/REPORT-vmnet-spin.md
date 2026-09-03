# REPORT: g3beige + vmnet-bridged -- host thread spin (goal 1 done), dead guest RX (goal 2 fixed from code reading, awaiting the user's trace run)

Contract: `doc/vmnet/HANDOFF-vmnet-spin.md`. Worktree branch `vmnet-spin`
off `g3beige` (`b08253ace6`). Written 2026-09-03.

## 1. Evidence re-verified before changing anything

`/Users/hsp/.claude/jobs/886cc763/tmp/sample-26669.txt` (5 s `sample` of the
live incident): thread `DispatchQueue_23: org.qemu.vmnet.if_queue` 3088/3180
samples, of which 2961 in `__vmnet_interface_set_event_callback_block_invoke_3
-> getsockopt` and 99 in `qemu_bh_schedule -> event_notifier_set -> write`.
Main thread 2718/3180 in `main_loop_wait -> g_poll -> __select`, ~300 in the
timer/GUI path. Exactly the contract's reading: Apple's level-triggered event
source polls and re-invokes our callback, which only schedules a bottom half
that does nothing while a batch is parked.

Code reading confirmed in this tree (`net/vmnet-common.m` is identical to
upstream at the base):

- `vmnet_send_bh()` returns immediately when
  `packets_send_current_pos < packets_send_end_pos` (batch parked).
- `vmnet_write_packets_to_qemu()` returns on the first
  `qemu_send_packet_async() == 0`; the refused packet sits in the net queue
  with `vmnet_send_completed` as its callback.
- The `PACKETS_AVAILABLE` callback stayed registered while parked.
- `net/tap.c`: `tap_send()` -> `tap_read_poll(s, false)` on 0,
  `tap_send_completed()` -> `tap_read_poll(s, true)`.
- Additional finding: the last test in `vmnet_send_completed()` was inverted
  (`if (current_pos < end_pos) qemu_bh_schedule(...)`: schedules the bh only
  while STILL parked, when the bh does nothing, never when drained). It only
  worked upstream because the level-triggered event kept firing.

## 2. Goal 1 -- the fix (commit `1fb7b2500b`)

Files: `net/vmnet-common.m`, `net/vmnet_int.h`, `net/trace-events`
(97 insertions, 26 deletions).

- `VmnetState` gains `bool vm_running` (last run state seen by the change
  handler) and `bool rx_events_armed` (callback currently registered).
- New `vmnet_batch_parked(s)` = `current_pos < end_pos`.
- New `vmnet_update_rx_events(s)`: the single register/unregister site.
  `want = vm_running && !parked`; registers the same block as before (only
  `qemu_bh_schedule(s->send_bh)`) when `want` flips to true, unregisters
  (`NULL, NULL`) when it flips to false, no-op otherwise.
- `vmnet_vm_state_change_cb()` now records `vm_running` and calls the helper,
  so the VM-stopped behaviour from upstream `993f71ee33` is unchanged and no
  re-registration happens while the VM is not running.
- `vmnet_write_packets_to_qemu()`: on `size == 0` it emits
  `trace_vmnet_rx_parked(end_pos - current_pos)` and calls the helper
  (unregisters).
- `vmnet_send_completed()`: after draining, if the batch is no longer parked
  it calls the helper (re-registers when the VM runs), emits
  `trace_vmnet_rx_resumed()` and schedules the bh once so packets that
  accumulated in vmnet while parked are read now. This replaces the
  inverted test.
- `#include "trace.h"` and two events in `net/trace-events`:
  `vmnet_rx_parked(int pending) "pending=%d"`, `vmnet_rx_resumed(void) ""`.

Threading: every call to `vmnet_update_rx_events()` is on the main loop
(bh, completion, VM state change, `vmnet_if_create`, cleanup);
`vmnet_interface_set_event_callback()` is documented safe from any thread
and the pre-existing code already called it from the main loop. A block
already in flight on `if_queue` when we unregister at most schedules one
more bh, which returns at once. `qemu_purge_queued_packets()` in cleanup
invokes `vmnet_send_completed(nc, 0)` (see `qemu_net_queue_purge()`), which
advances the batch; with `vm_running == false` the helper stays a no-op so
nothing is re-registered on the stopped interface.

Not changed: `vmnet_send_bh()`, the read path, bmac, slirp.

## 3. Build

Worktree `build/`, configured with the same options as `build-g3`
(`--enable-cocoa --enable-slirp`) but `--target-list=ppc-softmmu` only
(`build-g3/config.log` line 2 also lists ppc64-softmmu; the project rule
since 2026-08-30 is ppc only). Clean full build of the unmodified tree:
1728 steps, exit 0 (`/Users/hsp/.claude/jobs/886cc763/tmp/build1.log`).
Incremental rebuild with the change: 19 steps, `net_vmnet-common.m.o` and
the other three vmnet objects recompiled, no warnings, exit 0
(`.../build2.log`). Third rebuild with the bmac requeue fix: 26 steps,
`hw_net_bmac.c.o` recompiled, no warnings, exit 0 (`.../build3.log`).
Configure summary: SDL 2.32.70 YES, Cocoa YES,
vmnet.framework YES, slirp 4.9.3 YES, PNG YES -- the display/network
backends the user's launcher needs.

Binary: `<worktree>/build/qemu-system-ppc`. Its QMP greeting reports
`v11.1.0-rc2-481-g3f6b3c4e3e-dirty`, i.e. linked on top of the contract
commit with the (then uncommitted) vmnet change in the tree.

## 4. Regression boots (protocol from the contract)

Both from a scratch cwd with no pre-existing nvram.img/pram.img, a qcow2
overlay whose read-only backing file is `/Volumes/Macdata/qemu/hd/9.2-G3.img`
(`qemu-img create -f qcow2 -b ... -F raw overlay.qcow2`), `-M g3beige -m 512`,
the V3 ROM, the ati_mach_gt romfile, `-display none`, `-qmp unix:./qmp.sock`,
`-audiodev none,id=snd -global awacs.audiodev=snd`, one guest at a time,
driven by `/Users/hsp/.claude/jobs/886cc763/tmp/qmp_drive.py` (screendumps,
then `quit`). Launcher: `/Users/hsp/.claude/jobs/886cc763/tmp/boot.sh`.
Only the agent's own pid was ever touched; no `pkill`. QMP `screendump`
without a `format` argument writes PPM; the files were converted with
`/Users/hsp/.claude/jobs/886cc763/tmp/ppm2png.py` (`*-conv.png`) and viewed.

### (b) `-nic user,model=bmac` (slirp) -- PASS

Scratch cwd `/Users/hsp/.claude/jobs/886cc763/tmp/boot-slirp/`, QEMU pid
32118, started 08:46:xx; the driver attached ~75 s after launch, so its
timestamps below are launch + ~75 s.

    greeting {"QMP": {"version": {"qemu": {"micro": 50, "minor": 1, "major": 11},
              "package": "v11.1.0-rc2-481-g3f6b3c4e3e-dirty"}, "capabilities": ["oob"]}}
    qmp_capabilities {} -> {"return": {}}
    [ 30.1s] screendump slirp-030s.png -> {"return": {}}      (~105 s after launch)
    [ 90.5s] screendump slirp-090s.png -> {"return": {}}
    [150.7s] screendump slirp-150s.png -> {"return": {}}
    [190.9s] screendump slirp-190s.png -> {"return": {}}
    [190.9s] event SHUTDOWN {"guest": false, "reason": "host-qmp-quit"}
    [190.9s] quit {} -> {"return": {}}
    qemu exit=0            (boot.sh `wait`), qemu.log (stderr) empty

Screendumps viewed: `boot-slirp/slirp-030s-conv.png` -- Finder desktop
(menu bar "Finder", volume "Mac OS 92-G3", Mail / new name / QuickTime
Player / Register with Apple / Sherlock 2 icons, Trash), guest clock
6:47 AM; `slirp-090s-conv.png` and `slirp-190s-conv.png` identical
desktop, clock 6:48 and 6:50 -- the guest kept running until `quit`.

### (c) `-nic none` -- PASS

Scratch cwd `/Users/hsp/.claude/jobs/886cc763/tmp/boot-none/`, QEMU pid
32235; driver attached at launch + 1 s.

    [  1.0s] greeting ... "package": "v11.1.0-rc2-481-g3f6b3c4e3e-dirty" ...
    [  1.0s] qmp_capabilities {} -> {"return": {}}
    [ 90.4s] screendump desktop-090s.png -> {"return": {}}
    [150.8s] screendump desktop-150s.png -> {"return": {}}
    [210.4s] screendump desktop-210s.png -> {"return": {}}
    [240.6s] screendump desktop-240s.png -> {"return": {}}
    [240.6s] event SHUTDOWN {"guest": false, "reason": "host-qmp-quit"}
    [240.6s] quit {} -> {"return": {}}
    qemu exit=0, qemu.log (stderr) empty

Screendump viewed: `boot-none/none-090s-conv.png` -- the same Finder
desktop 90 s after launch, guest clock 6:52 AM.

### (b again) slirp boot on the binary with the bmac requeue fix (`e3d813717f`) -- PASS

Fresh overlay, same scratch cwd, QEMU pid 32630; driver attached at
launch + 1 s. This run exercises the changed `bmac_receive()` path
directly (every slirp packet that lands between two arms is requeued).

    [  1.0s] greeting ... "package": "v11.1.0-rc2-483-g045d8f8300-dirty" ...
    [  1.0s] qmp_capabilities {} -> {"return": {}}
    [ 90.6s] screendump desktop-090s.png -> {"return": {}}
    [150.1s] screendump desktop-150s.png -> {"return": {}}
    [210.5s] screendump desktop-210s.png -> {"return": {}}
    [240.7s] screendump desktop-240s.png -> {"return": {}}
    [240.7s] event SHUTDOWN {"guest": false, "reason": "host-qmp-quit"}
    [240.7s] quit {} -> {"return": {}}
    qemu exit=0, qemu.log (stderr) empty

Screendump viewed: `boot-slirp/slirp2-090s-conv.png` -- Finder desktop
90 s after launch, guest clock 7:00 AM.

Both scratch dirs got a fresh `nvram.img`/`pram.img` created by the ROM
(none existed before); the master image was never opened read-write (the
overlay grew to 1.6-1.7 MB, the master's mtime is unchanged). No other
guest was running during either boot (`ps` checked before the first). The
vmnet path itself cannot be exercised by the agent (root/entitlement); its
runtime proof is the user's trace run in section 5.

## 5. The prepared trace run (user, root)

`<worktree>/doc/vmnet/vmnet-trace.command` (mode 755, double-clickable).
It is the user's `/Users/hsp/Qemu-GUI-Machines/MacOS91/run.command` with:

- the binary replaced by `<worktree>/build/qemu-system-ppc`;
- `-trace 'bmac_rx_*' -trace bmac_can_receive -trace bmac_reg_write
  -trace bmac_irq_update -trace 'vmnet_*'` before the drive lines;
- `2> /Users/hsp/.claude/jobs/886cc763/tmp/vmnet-trace.log` on the QEMU line;
- `sudo` prefix and `chown` tail kept; cwd set explicitly to the machine
  directory so the guest keeps its usual nvram/pram (the original used
  `cd "$(dirname "$0")"`, which would now point into the worktree).

The user boots to the desktop, waits 60 s, shuts down from inside the guest.
If the log passes 200 MB (`bmac_can_receive` fires on every
`qemu_can_send_packet()` and can be chatty) stop early. Delete the log when
the analysis is done.

## 6. Goal 2 -- dead guest RX: cause found by code reading, fixed (commit `e3d813717f`)

The main session confirmed the leading hypothesis below independently
and directed that it be acted on before the trace log. The fix, in
`hw/net/bmac.c` `bmac_receive()`: the early return is split -- a packet
the address filter rejects (or RX disabled) still returns `size`
(consumed), but "no RX descriptor armed" (`!rx_dma_waiting || !io`) now
returns 0 so the net layer requeues it. Verified in `net/net.c`:
`qemu_deliver_packet_iov()` sets `nc->receive_disabled = 1` on a 0 return
(line ~878) and `qemu_net_queue_flush()` re-inserts the packet at the head
and stops; `qemu_flush_or_purge_queued_packets()` clears
`receive_disabled` (line ~706) and is what `bmac_rx_dma_rw()` calls on
every arm. The loopback caller `bmac_tx_dma_rw()` (bmac.c ~910) ignores
the return value, so a self-addressed frame with no descriptor armed is
dropped as before. The stale comment in `bmac_can_receive()` was
corrected. New trace event `bmac_rx_requeue(size_t size)` in
`hw/net/trace-events` ("no RX descriptor armed, requeued size=%zu"),
covered by the launcher's `-trace 'bmac_rx_*'`.

Regression after the change: see section 4 (second slirp boot). The
slirp path exercises the changed code directly -- every packet slirp
hands over between two arms is now requeued instead of consumed.

What the user's trace run should now show: `bmac_rx_requeue` lines
instead of `bmac_rx_receive ... waiting=0 has_io=0` drops, one
`bmac_rx_dma_armed` + delivery per packet, `vmnet_rx_parked`/`resumed`
sparse, and the guest obtaining a DHCP lease (TCP/IP control panel shows
a LAN address, host `ping` answers).

### The cause: the contract's hypothesis (b), whose "should not happen" clause was wrong

`qemu_net_queue_flush()` (net/queue.c) delivers queued packets with
`qemu_net_queue_deliver()` -> `qemu_deliver_packet_iov()` -> `bmac_receive()`
directly. Neither consults `can_receive`; the only `can_receive` check is in
`qemu_net_queue_send()`, i.e. for NEW packets. And `bmac_receive()`
(hw/net/bmac.c ~485) returns `size` -- consumes the packet without
delivering it -- when `!rx_dma_waiting || !io`. Chain per RX arm:

1. Driver arms one INPUT descriptor -> `bmac_rx_dma_rw()`:
   `rx_dma_waiting = true`, `qemu_flush_queued_packets()`.
2. Flush delivers the head packet: `bmac_receive()` writes the DMA buffer,
   `rx_dma_waiting = false`, `io->dma_end()`, returns size. Flush then
   calls its `sent_cb` = `vmnet_send_completed()`.
3. `vmnet_send_completed()` -> `vmnet_write_packets_to_qemu()` ->
   `qemu_send_packet_async(packet N+1)` -> `qemu_net_queue_send()`:
   `qemu_can_send_packet()` -> `bmac_can_receive()` = false (waiting is
   false) -> appended to the queue, returns 0 -> vmnet parks again
   (`vmnet_rx_parked pending=K`).
4. Control returns into the STILL-RUNNING flush loop of step 1. The queue
   is not empty (packet N+1), so it is delivered straight to
   `bmac_receive()` with `rx_dma_waiting == false` -> returns size ->
   silently dropped -> `sent_cb` -> `vmnet_send_completed()` -> packet N+2
   queued -> dropped -> ... until the whole parked batch is gone.
5. DBDMA only presents the next INPUT descriptor after `dma_end` from the
   DBDMA bh (`channel_run` stops while `io.processing`), so exactly one
   packet is delivered per arm and every other packet in the vmnet batch
   is lost.

Why slirp works: slirp hands QEMU one packet per socket event, no batch; a
refused packet is requeued and delivered on the next arm. Why vmnet fails:
`vmnet_read()` returns up to 200 packets, and while parked the kernel side
accumulates more, so batches are multi-packet whenever the driver is slower
than the LAN -- the DHCP OFFER/ACK and ARP replies are then dropped unless
they happen to be at the head of a batch. The spin made the driver slower
still.

Trace signature that CONFIRMS it: a `bmac_rx_receive size=... accepted=1
waiting=1 has_io=1` (the delivery) followed, with no intervening
`bmac_rx_dma_armed`, by one or more `bmac_rx_receive ... accepted=1
waiting=0 has_io=0` lines (the drops), bracketed by `vmnet_rx_parked
pending=K` and preceded by `bmac_can_receive enabled=1 waiting=0`. If K
packets were parked and K-1 drop lines follow, the hypothesis is proven.
Refutes it: every parked batch is followed by K separate
`bmac_rx_dma_armed` + delivery pairs.

This is the fix applied in `e3d813717f` (details at the top of this
section).

### Contract hypothesis (a): first-descriptor / stale-arm wedge (still worth a glance in the log)

Check the first `bmac_rx_dma_armed` after the driver's `BMAC_RXRST` write
(`bmac_reg_write`; the RXRST handler clears `rx_dma_waiting`, commit
`ac61f691a5`). Signature: a `bmac_rx_receive` delivery BEFORE the driver's
RXCFG enable + first arm, or a `bmac_rx_dma_armed` address outside the
driver's ring, then no further `bmac_rx_dma_armed` for the rest of the run
while `bmac_irq_update` shows RX status stuck. Refutes it: arms keep coming
at a steady cadence after each delivery.

### Contract hypothesis (c): frame format / CRC

Unlikely: vmnet delivers plain Ethernet frames without FCS, exactly like
slirp, and `bmac_receive()` appends a CRC only when the driver sets
`RXCFG_CRCNOSTRIP`, identically for both backends; the slirp path works.
`net_peer_needs_padding()` pads short frames to 60 bytes for both.
Signature: `bmac_rx_receive size=` values from vmnet in the 42..1514 range
with `accepted=1`; anything > 1518 would hit the oversize guard.

### Outside-QEMU possibility

If the log shows every packet from the parked batches delivered (no drop
lines) and a healthy `bmac_rx_dma_armed` cadence, yet no DHCP OFFER ever
appears as a `bmac_rx_receive` at all, then the OFFER never reached
vmnet -- Apple bridged mode on that `en0` (Wi-Fi bridging needs the AP to
accept foreign source MACs) or the DHCP server. Then stop, per the contract.

### Analysis steps when the log arrives

1. `ls -l` the log (200 MB rule); `wc -l`; `grep -c` per event name.
2. `grep -n 'vmnet_rx_parked\|vmnet_rx_resumed'`: confirm goal 1's
   transitions are sparse (one park per refused packet, not thousands per
   second) -- the positive control that the fixed binary is what ran.
3. Extract the window from the driver's RXRST to +30 s; list
   `bmac_rx_dma_armed` / `bmac_rx_receive` / `vmnet_rx_parked` in order;
   count deliveries vs drops per arm.
4. Count `bmac_rx_requeue` vs `bmac_rx_dma_armed`: with the fix every
   requeued packet must be followed by an arm and a delivery; any
   `bmac_rx_receive ... accepted=1 waiting=0 has_io=0` line without a
   matching `bmac_rx_requeue` would mean a path not covered. Then check
   hypothesis (a)'s signature and, above all, whether the guest got its
   lease (ask the user; TCP/IP control panel, host `ping`).
5. Delete the log.

## 7. Out of scope but noted for the re-run

"The Finder has unexpectedly quit" and the governor's 5%-paced warning in
the incident run: record whether either recurs in the user's trace run
with the spin gone.

---

# A/B: does networking still work?

Added 2026-09-03 by a second agent. Question put by the main session: the
earlier regression only proved the guest reached the Finder desktop; it
never proved networking still WORKS. `e3d813717f` changes the net-layer
return contract (returning 0 sets `nc->receive_disabled` until
`qemu_flush_queued_packets()` clears it), so it could plausibly have
broken RX for every backend, slirp included. Settled before anything else.

## Method

Two headless boots of the SAME disk, identical in every respect except the
binary. Raw artifacts in `doc/vmnet/ab-slirp/` (traces, decoded pcaps, and
the exact harness: `ab_boot.sh`, `ab_drive.py`, `pcapdump.py`).

- Arm A = FIXED: `<worktree>/build/qemu-system-ppc`
  (`v11.1.0-rc2-483-g045d8f8300-dirty`, both fixes; `strings` confirms
  `bmac_rx_requeue` and `vmnet_rx_parked` are present).
- Arm B = UNFIXED: `/Users/hsp/src/claude-code/qemu-master-g3/build-g3/qemu-system-ppc`
  (`v11.1.0-rc2-479-g28e54ed2d3-dirty`; neither new trace event present).
  `git diff 28e54ed2d3 b08253ace6 -- hw/net/bmac.c net/ include/hw/net/bmac.h`
  is EMPTY, i.e. the two builds' networking code differs by exactly the two
  commits under test and nothing else.
- Each arm: own scratch cwd, no pre-existing nvram.img/pram.img, a fresh
  qcow2 overlay whose backing file is `/Volumes/Macdata/qemu/hd/9.2-G3.img`
  (`-F raw`, read-only, never opened rw). Master mtime `2026-08-31 22:07:55`
  and size 2147483648 before AND after all runs -- unchanged.
- `-M g3beige -m 512`, V3 ROM, ati_mach_gt romfile, `-display none`,
  `-qmp unix:./qmp.sock,server=on,wait=off`,
  `-nic user,model=bmac,mac=00:05:02:12:34:56,id=n0`,
  `-object filter-dump,id=f0,netdev=n0,file=./net.pcap` (the `id=` on `-nic`
  is what makes the netdev addressable by filter-dump),
  `-trace 'bmac_rx_*' -trace bmac_tx_send -trace bmac_can_receive
  -trace bmac_reg_write -trace bmac_irq_update -trace 'vmnet_*'`.
  `bmac_tx_send` was missing from the user's vmnet run and is essential.
- Screendump at t+120 s (desktop check) and t+210 s (90 s later, so the
  TCP/IP stack has had time to finish DHCP), then QMP `quit`. Both PNGs
  viewed. One guest at a time; only own pids; no `pkill`.

## Verdict: BOTH arms get a lease. The two commits are clean.

### Arm A (FIXED) -- PASS

`ab-fixed/A-fixed-120s.png` and `-210s.png`: Mac OS 9.2 Finder desktop,
menu bar, disk/QuickTime/Sherlock icons, clock running (9:58 -> 10:00).
QMP: greeting, two `screendump` returns, `SHUTDOWN host-qmp-quit`,
`qemu exit=0`.

pcap (`doc/vmnet/ab-slirp/ab-fixed.pcap.txt`), 10 frames:

    1 00:05:02:12:34:56 -> bcast  DHCP DISCOVER
    2 52:55:0a:00:02:02 -> bcast  DHCP OFFER  yiaddr=10.0.2.15
    3 00:05:02:12:34:56 -> bcast  DHCP REQUEST
    4 52:55:0a:00:02:02 -> bcast  DHCP ACK    yiaddr=10.0.2.15
    5-8 ARP probes  spa=0.0.0.0 tpa=10.0.2.15   (duplicate-address detection)
    9-10 ARP         spa=10.0.2.15 tpa=10.0.2.15 (gratuitous: address taken)

The guest's own frames carry source 10.0.2.15 after the ACK -- it took the
lease. Not an inference: the address is on the wire.

### Arm B (UNFIXED) -- PASS

`ab-unfixed/B-unfixed-120s.png` and `-210s.png`: same desktop (clock 10:01
-> 10:03). Same 10 frames, same DHCP exchange, same 10.0.2.15.

### Side-by-side diff, arm A vs arm B

| measure | A (fixed) | B (unfixed) |
| --- | --- | --- |
| Finder desktop by t+120 s | yes | yes |
| slirp DHCP lease | 10.0.2.15 (DISCOVER/OFFER/REQUEST/ACK) | 10.0.2.15 (identical) |
| pcap frames | 10 | 10, byte-identical decode |
| trace lines | 445 | 445 |
| `bmac_reg_write` | 409 | 409 |
| `bmac_irq_update` | 20 | 20 |
| `bmac_tx_send` | 8 (372, 378, 6x42) | 8 (372, 378, 6x42) |
| `bmac_rx_dma_armed` | 3 | 3 |
| `bmac_can_receive` | 3 (enabled=0/waiting=0 once, then 2x enabled=1 waiting=1) | 3, same |
| `bmac_rx_receive` | 2, both `size=590 accepted=1 waiting=1 has_io=1` | 2, same |
| `bmac_rx_requeue` | **0** | n/a (event does not exist) |
| bmac init sequences | 1, ending RXCFG=0xb01 (ENABLE\|CRCNOSTRIP\|REJOWN\|HASHFILT) + TXCFG=0x1, never disabled again | 1, same |

`diff A/trace.log B/trace.log` is ONE line: `addr=0x540` (the random-seed
register) `0x5d84` vs `0x49ee`. Everything else is identical.

**Why `bmac_rx_requeue` is 0 matters.** The requeue branch never executes on
slirp, because slirp hands over one packet per socket event and only ever
does so when `bmac_can_receive()` already said yes. `e3d813717f` is
therefore INERT on slirp -- which is a stronger result than "it did no
harm": it cannot regress the slirp path because it is not on it. The
corresponding limit of this experiment, stated plainly: this A/B does NOT
positively exercise the requeue-and-redeliver contract. That contract's
recovery hook (a later `qemu_flush_queued_packets()`) is exactly what the
next section shows was missing.

## What the vmnet run actually shows, and where it diverges

`/Users/hsp/.claude/jobs/886cc763/tmp/vmnet-trace.log`, 1097 lines:
644 `bmac_reg_write`, 203 `bmac_rx_receive`, 203 `vmnet_rx_parked`,
40 `bmac_irq_update`, 5 `bmac_can_receive`, 1 `vmnet_rx_resumed`,
**1 `bmac_rx_dma_armed`**, 0 `bmac_rx_requeue`.
`bmac_rx_receive` splits 200x `accepted=0 waiting=0 has_io=0` and
3x `accepted=0 waiting=1 has_io=1`.

Correcting the main session's reading: the two init sequences are NOT
"device left disabled" throughout -- each one DOES reach
`RXCFG=0xb01` + `TXCFG=0x1` and only then tears down
(`RXCFG=0`, `XIFC=0`, `TXCFG=0`, `INTDISABLE=0xffff`). The driver opens the
device, gets nothing, gives up, retries, gives up again, and Open Transport
self-assigns 169.254.132.119.

Filtering out the MII/SROM bit-bang (`addr=0x190`) and IRQ noise, the two
runs are **identical instruction-for-instruction** through driver init:

    slirp (arm A)                        vmnet (user)
    ...                                  ...
    0x210=0xfcff                         0x210=0xfcff       INTDISABLE
    0x700..0x730 = 0                     0x700..0x730 = 0   hash table
    0x630=0xb00   RX cfg, ENABLE CLEAR   0x630=0xb00        <- same
    0x0=0x1       XIFC                   0x0=0x1
    bmac_rx_dma_armed 0x73d790 len=1536  bmac_rx_dma_armed 0xbf6790 len=1536
    -- nothing arrives --                bmac_rx_receive 86  accepted=0 waiting=1
                                         bmac_rx_receive 106 accepted=0 waiting=1
                                         bmac_rx_receive 101 accepted=0 waiting=1
                                         vmnet_rx_parked pending=200   <-- DIVERGENCE
    0x680/0x670/0x660 MADD               0x680/0x670/0x660 MADD
    0x760/0x750/0x740/0x770 hash         0x760/0x750/0x740/0x770 hash
    0x630=0xb01   RX ENABLE              0x630=0xb01        <- same
    0x430=0x1     TX ENABLE              0x430=0x1
    bmac_tx_send 372 (DHCP DISCOVER)     (tx not traced in the user's run)
    bmac_rx_receive 590 accepted=1  <-- OFFER
    bmac_rx_dma_armed (re-arm)           ... nothing, for the rest of the run ...
    bmac_rx_receive 590 accepted=1  <-- ACK
    bmac_rx_dma_armed (re-arm)
    -> lease 10.0.2.15                   -> teardown, retry, teardown, 169.254.x

The divergence is one window: **between arming the first RX descriptor and
setting `RXCFG_ENABLE`**. slirp is silent in that window (it sends nothing
until the guest speaks). A real LAN is not -- the user measured 3-24
packets/s on `en0`, and one is enough.

### Root cause (best-supported hypothesis)

1. Packets that land in that window are rejected by
   `bmac_can_receive_packet()` (RXCFG=0xb00, `RXCFG_ENABLE` clear) and are
   consumed WITHOUT completing the armed descriptor -- correct behaviour for
   a disabled RX MAC, but it leaves `rx_dma_waiting == true` forever.
2. The next one is refused by `bmac_can_receive()` (`enabled=0`), so the net
   layer queues it and `qemu_send_packet_async()` returns 0 -- vmnet parks
   its 200-packet batch and (with `1fb7b2500b`) unregisters its event source.
3. The driver then sets `RXCFG_ENABLE`. `bmac_can_receive()` is now true --
   but nothing re-offers the packet the net layer is already holding.
   **`bmac`'s only `qemu_flush_queued_packets()` call site is
   `bmac_rx_dma_rw()`**, i.e. a NEW DBDMA arm; and the driver will not arm
   again, because the descriptor it armed in step 1 never completed and so
   never raised an RX interrupt.
4. Deadlock. vmnet waits for a send-completion that needs a flush; bmac
   waits for a packet that needs vmnet. The trace's single
   `bmac_rx_dma_armed` in 1097 lines, and the total absence of RX between
   the park and the teardown, are that deadlock.

`1fb7b2500b` is not the cause -- before it, the level-triggered event kept
firing but `vmnet_send_bh()` returned early while parked, so no packet moved
either. It only makes the stall silent instead of spinning. And
`e3d813717f` never fires here (`bmac_rx_requeue` = 0) because the packets
are rejected by the filter, not by the missing descriptor.

### Fix under test: flush on the `RXCFG_ENABLE` 0->1 edge

`hw/net/bmac.c`: new `case BMAC_RXCFG:` in `bmac_write()` that stores the
value and, on a 0->1 transition of `RXCFG_ENABLE`, calls
`qemu_flush_queued_packets()`. New trace event
`bmac_rx_enable_flush(waiting)` (matched by the `bmac_rx_*` pattern already
in `doc/vmnet/vmnet-trace.command`). The address-filter and hash registers
need no hook of their own: this driver always clears `RXCFG_ENABLE` before
touching them and sets it again after, so the edge covers those too.

Slirp regression (arm C, same harness, three boots -- two clean, one
intermittent guest crash, see below): desktop at t+120 s and t+210 s,
lease 10.0.2.15, same 10 pcap frames, trace identical to arm A apart from
the random seed and two new
`bmac_rx_enable_flush ... waiting=1` events at the two enable edges --
`waiting=1` being precisely the state the vmnet deadlock needs.

One of three arm-C boots died with a guest "bus error" during extension
load. Its trace stops after 181 lines of pure SROM bit-banging with **no
`0x630` write at all**, so the changed code had not executed when the guest
crashed; the change cannot be the cause. Recorded, not explained -- the
image has a history of intermittent boot faults.

**This fix is UNVERIFIED on vmnet** (vmnet needs root; the agent cannot run
it). `doc/vmnet/vmnet-trace.command` already points at the worktree build,
so re-running it now tests exactly this hypothesis.

### What the next vmnet run should show if the hypothesis is right

- `bmac_rx_enable_flush ... waiting=1` right after each `0x630=0xb01`.
- `bmac_rx_dma_armed` MANY times, not once.
- `bmac_rx_receive ... accepted=1` for a 342-590 byte frame (DHCP
  OFFER/ACK) shortly after the first enable.
- A lease from the user's own DHCP server instead of 169.254.x.

If instead the trace still shows one arm and no accepted frame, the next
suspect is Apple's bridged mode itself (the OFFER never reaching vmnet),
which is outside QEMU -- stop there, per the contract.
