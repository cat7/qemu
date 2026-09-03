# HANDOFF: g3beige + vmnet-bridged -- host thread spin and dead guest RX

Contract for a reader with zero shared context. Read it whole before
touching code. Copy this file into your worktree as
`doc/vmnet/HANDOFF-vmnet-spin.md` in your first commit so the tree carries
it.

Written 2026-09-03 by the main session from a LIVE incident on the user's
machine (pid 26669, Mac OS 9.1 guest, `-nic vmnet-bridged,ifname=en0,
model=bmac`, QEMU run under sudo from `/Applications/qemu-system-ppc-g3-mac-os/`).

## Evidence gathered (all measured, none inferred)

1. `ps -M`: one non-main thread at 99% CPU with almost all of it SYSTEM
   time, hot since process start (7:48 system time after 8:40 wall).
   The vCPU thread was separately at 47-82% user time (guest executing).
   The main thread at 18-25%.
2. `top` deltas: ~240,000 BSD syscalls/s and ~31,000 context switches/s
   for the process, sustained.
3. `sudo sample 26669 5` (file: `/Users/hsp/.claude/jobs/886cc763/tmp/sample-26669.txt`,
   read it): thread `DispatchQueue_23: org.qemu.vmnet.if_queue` has 3088 of
   3180 samples inside Apple's `__vmnet_interface_set_event_callback_block_invoke_3`,
   of which 2961 are in `getsockopt` and 99 in QEMU's callback ->
   `qemu_bh_schedule` -> `event_notifier_set` -> `write`. The main thread
   has 2718/3180 samples in `g_poll` -> `__select` (returning
   immediately, i.e. woken continuously) and ~300 in the timer/GUI path.
4. `netstat -I en0 -w 1`: the LAN is QUIET, 3-24 packets/s. This is not a
   broadcast flood; one unread packet is enough.
5. `arp -an`: the guest MAC `0:5:2:12:34:56` is present with address
   169.254.132.119, i.e. the guest's TCP/IP stack came up and
   self-assigned because no DHCP offer arrived. `ping` from the host to
   that address: 100% loss. The user confirms: "bridged vmnet did not get
   me a ip address from my dhcp server". Guest TX works (its ARP reached
   the host); guest RX does not.
6. The same machine record with `-nic user,model=bmac` (slirp) works and
   has worked for weeks (DHCP from slirp, no spin).
7. The guest also showed "The Finder has unexpectedly quit" and the
   governor printed its 5%-paced warning during this run; both are
   plausibly downstream of a main loop woken 200k times a second, and
   are NOT in scope beyond noting whether they recur after the fix.

## Code reading (verify, do not trust)

`net/vmnet-common.m` in the g3beige tree (identical to upstream here):

- `vmnet_vm_state_change_cb()` registers a `VMNET_INTERFACE_PACKETS_AVAILABLE`
  callback on `s->if_queue`; the block only calls `qemu_bh_schedule(s->send_bh)`.
- `vmnet_send_bh()` returns immediately if `packets_send_current_pos <
  packets_send_end_pos` (a batch is "parked"), otherwise `vmnet_read()`s
  a batch and calls `vmnet_write_packets_to_qemu()`.
- `vmnet_write_packets_to_qemu()` stops at the first packet for which
  `qemu_send_packet_async()` returns 0 (the NIC's `can_receive` said no;
  the packet is queued in the net layer with `vmnet_send_completed` as
  its callback). Nothing is read from vmnet until that callback fires.
- While parked the event source stays registered. Apple's source is
  level-triggered on "packets available" (its block polls `getsockopt`,
  per the sample), so it fires continuously, each time waking the main
  loop for a bh that does nothing. That is the spin.
- `net/tap.c` handles the identical situation correctly: `tap_send()`
  calls `tap_read_poll(s, false)` when `qemu_send_packet_async()` returns
  0 and `tap_send_completed()` calls `tap_read_poll(s, true)`.
- Upstream commit `993f71ee33` ("vmnet: stop recieving events when VM is
  stopped", already in this tree) fixed the sibling case (event flood
  while the VM is stopped) by unregistering the callback; the parked-batch
  case is unfixed upstream as of this tree's base (2026-08-16).

`hw/net/bmac.c`: `bmac_can_receive()` returns `RXCFG_ENABLE && rx_dma_waiting`;
`bmac_rx_dma_rw()` sets `rx_dma_waiting = true` and calls
`qemu_flush_queued_packets()` (line ~954), which is what should deliver
the parked packet and fire `vmnet_send_completed`. Trace events exist:
`bmac_rx_dma_armed`, `bmac_rx_receive`, `bmac_can_receive`, `bmac_reg_write`,
`bmac_irq_update`, `bmac_dma_irq`.

## Goals, in order

1. **Stop the spin (vmnet backend).** While a batch is parked, the
   PACKETS_AVAILABLE callback must be unregistered; re-register it from
   `vmnet_send_completed()` once the parked batch has fully drained (or
   as soon as the completion arrives and `vmnet_write_packets_to_qemu()`
   returns with the buffer empty). Factor the register/unregister into
   one helper used by `vmnet_vm_state_change_cb()` too, and keep the
   VM-stopped behaviour intact (do not re-register while the VM is not
   running). Mind the threads: the callback runs on `if_queue`, the bh
   and the completion on the main loop; `vmnet_interface_set_event_callback`
   is documented as safe to call from any thread, and the existing code
   already calls it from the main loop. Add two trace events,
   `vmnet_rx_parked(int pending)` and `vmnet_rx_resumed(void)`, in
   `net/trace-events`, so the user's run can show the transitions.
2. **Explain and, if it is in QEMU, fix dead guest RX.** With the spin
   gone, the guest must still get its DHCP lease. Hypotheses to test
   with the trace run (below), most likely first:
   a. The parked packet is delivered into the FIRST RX descriptor the
      driver arms, i.e. during driver init, and something about that
      early delivery (an RX completion before the driver expects one,
      or a stale `rx_dma_waiting` from the ROM's own use of bmac) leaves
      the driver's ring or the model's state wedged so nothing later is
      accepted. Look at `bmac_rx_receive` accepted/waiting/has_io flags
      and `bmac_rx_dma_armed` around the first packets.
   b. `qemu_flush_queued_packets()` delivers one packet per arm, but the
      completion callback in vmnet immediately pushes the next packet
      while `rx_dma_waiting` is already false -> returns 0 -> parks
      again -> fine in principle; check that no packet is dropped
      silently in `bmac_receive()` when `!rx_dma_waiting` (line ~485:
      it returns size, i.e. CONSUMES the packet without delivering it,
      if `can_receive` was bypassed). `qemu_send_packet_async` calls
      `can_receive` first, so this should not happen from the net queue,
      but `bmac.c:910` calls `bmac_receive()` directly (loopback path).
   c. A mismatch between vmnet's frame format and what bmac expects
      (bmac's RX path expects the frame plus a 4-byte CRC? check
      `bmac_receive` size handling and `net_peer_needs_padding`).
   If the cause is in the guest driver or in Apple's bridged mode
   (e.g. bridged mode not delivering host-originated packets), say so
   with evidence and stop; do not build workarounds.

## The trace run (needs root, the USER runs it, you prepare it)

You cannot run vmnet (root/entitlement). Produce
`doc/vmnet/vmnet-trace.command` in your worktree: a copy of
`/Users/hsp/Qemu-GUI-Machines/MacOS91/run.command` (read it; it is the
user's real launcher, generated by the Qemu-GUI project) with the binary
path replaced by YOUR worktree build's `qemu-system-ppc`, and these
additions before the drive lines:

    -trace 'bmac_rx_*' -trace bmac_can_receive -trace bmac_reg_write \
    -trace bmac_irq_update -trace 'vmnet_*' \

and `2> /Users/hsp/.claude/jobs/886cc763/tmp/vmnet-trace.log` appended to
the QEMU line (keep the `sudo` prefix and the `chown` tail). The file
must remain double-clickable (`chmod +x`). The main session hands it to
the user; the user boots to the desktop, waits 60 s, shuts down from
inside the guest. You then analyse the log. If the log exceeds 200 MB
tell the user to stop early; delete it when done (disk-fill rule).

## Rules (binding)

- Work in the git worktree you were given (branch off `g3beige`); never
  touch `/Users/hsp/src/claude-code/qemu-master-g3` itself, never touch
  `/Applications/...`, never open any image under `/Volumes/Macdata/qemu/hd/`
  read-write (a qcow2 overlay with the master as read-only backing file
  is the accepted method for a scratch boot).
- Build: read the configure line at the top of
  `/Users/hsp/src/claude-code/qemu-master-g3/build-g3/config.log` and use
  the same options for a `build` dir inside your worktree, target list
  `ppc-softmmu` only. `ninja qemu-system-ppc`.
- Regression before calling goal 1 done: (a) clean build; (b) one headless
  scratch boot with `-nic user,model=bmac` over an overlay of
  `/Volumes/Macdata/qemu/hd/9.2-G3.img` (`-display none -qmp unix:...`,
  `-M g3beige -m 512 -bios /Applications/qemu-system-ppc-g3-mac-os/PowerMacG3v3.ROM
  -global ati-mach64-gt.romfile=/Applications/qemu-system-ppc-g3-mac-os/ati_mach_gt.rom`),
  run from a scratch cwd with NO pre-existing nvram.img/pram.img, reaches
  the Finder desktop (QMP `screendump`, look at the PNG) within 4 minutes
  and QMP `quit`s cleanly; (c) `-nic none` boots likewise. The slirp path
  is untouched by the change; this is the control that the build is not
  broken. One guest at a time; the user's own guest may be running, never
  `pkill` by pattern, kill only your own pid.
- Commit in the worktree with full messages and these trailers:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01WofRUkRsK1ynoHkTKpsk2q`.
  Do not push. Do not merge into `g3beige`; report the branch name.
- Report: `doc/vmnet/REPORT-vmnet-spin.md` in the worktree: the diff
  summary, build + regression evidence (screendump filenames, QMP
  transcript excerpts), the prepared trace command's path, and, once
  the user's log arrives (a second dispatch will bring it), the goal-2
  analysis. Your final message to the main session must contain the
  worktree path, branch, commit hashes, the regression evidence, and the
  exact path of `vmnet-trace.command`.
