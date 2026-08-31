common_ss.add(files('accel-common.c'))
# always-on MMU/TB instrumentation counters (see
# include/exec/tcg-mmu-stats.h); common so a non-TCG build links too
common_ss.add(files('tcg/tcg-mmu-stats.c'))
system_ss.add(files('accel-system.c', 'accel-blocker.c', 'accel-qmp.c', 'accel-irq.c'))
user_ss.add(files('accel-user.c'))

subdir('tcg')
if have_system
  subdir('hvf')
  subdir('whpx')
  subdir('qtest')
  subdir('kvm')
  subdir('xen')
  subdir('stubs')
  subdir('mshv')
  subdir('nitro')
endif

# qtest
system_ss.add(files('dummy-cpus.c'))
