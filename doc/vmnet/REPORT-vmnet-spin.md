# REPORT: g3beige + vmnet-bridged -- host thread spin (goal 1 done), dead guest RX (goal 2 pending log)

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
(`.../build2.log`). Configure summary: SDL 2.32.70 YES, Cocoa YES,
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

## 6. Goal 2 -- dead guest RX: hypotheses and analysis plan (log not yet received)

Nothing in bmac was changed. The following is a code reading done while
preparing goal 1; it ranks the contract's hypotheses and gives the trace
signature that would confirm or refute each.

### Leading hypothesis: the contract's (b), whose "should not happen" clause is wrong

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

Candidate fix if confirmed (NOT applied; needs the log first): in
`bmac_receive()` split the early return -- a packet the MAC filter rejects
(`!bmac_can_receive_packet`) is consumed (`return size`), but a packet that
arrives with no RX descriptor armed (`!rx_dma_waiting || !io`) must
`return 0` so the net layer requeues it (`qemu_net_queue_flush` re-inserts
it at the head and stops; `qemu_deliver_packet_iov` sets `receive_disabled`,
which the next `bmac_rx_dma_rw()` flush clears). The loopback caller at
bmac.c:910 ignores the return value, so it is unaffected. The regression
protocol (slirp + none boots) must be re-run after that change.

### Contract hypothesis (a): first-descriptor / stale-arm wedge

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
4. Decide among the hypotheses with the signatures above; if the leading
   one is confirmed, apply the `bmac_receive()` return-0 split, rebuild,
   re-run the regression protocol, and hand the user a second trace run
   to prove the DHCP lease.
5. Delete the log.

## 7. Out of scope but noted for the re-run

"The Finder has unexpectedly quit" and the governor's 5%-paced warning in
the incident run: record whether either recurs in the user's trace run
with the spin gone.
