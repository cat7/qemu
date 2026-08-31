# Session-start rule for the Radeon 9800 / mac99 GPU project

Before ANY work on the ati-radeon9800 / mac99 GPU project — including "pick up
where you left off" or resuming from a compacted-session summary:

1. Read the **STATE OF PLAY** section at the top of
   `/Users/hsp/.claude/projects/-Users-hsp-src-claude-code-qemu-master-g3/memory/project_gpu_offload_quartz_extreme.md`.
   Stop at the `LOG` divider — the dated log below it is archaeology whose
   entries correct each other; never resume from "the latest entry".
2. Before running anything, report the ledger back to the user in at most 6
   lines: the open claims you inherit, the next action you will take, and
   which traps apply to it.
3. Binding while working:
   - DEAD ENDS in the ledger are not retried. NEXT ACTIONS are taken in order
     unless the user redirects.
   - Every device change passes the Regression protocol in that section
     before being called done.
   - A negative result ("never happens", "nothing changed") counts only with
     a positive control proving the instrument could have detected the effect.
4. Precedence: STATE OF PLAY beats compaction summaries and older log entries
   wherever they conflict. Update STATE OF PLAY **in place** at session end —
   claim statuses change, consumed next-actions come off the list; raw detail
   goes to the log below.
