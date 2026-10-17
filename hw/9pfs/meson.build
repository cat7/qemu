fs_ss = ss.source_set()
if host_os == 'windows'
  fs_ss.add(files(
    '9p-local-win32.c',
    '9p-macos-compat.c',
    '9p-synth.c',
    '9p-util-generic.c',
    '9p-util-win32.c',
    '9p-xattr-user.c',
    '9p-xattr.c',
    '9p.c',
    'codir.c',
    'cofile.c',
    'cofs.c',
    'coth.c',
    'coxattr.c',
  ))
else
  fs_ss.add(files(
    '9p-local.c',
    '9p-macos-compat.c',
    '9p-posix-acl.c',
    '9p-synth.c',
    '9p-util-generic.c',
    '9p-xattr-user.c',
    '9p-xattr.c',
    '9p.c',
    'codir.c',
    'cofile.c',
    'cofs.c',
    'coth.c',
    'coxattr.c',
  ))
  if host_os == 'darwin'
    fs_ss.add(files('9p-util-darwin.c'))
  elif host_os == 'freebsd'
    fs_ss.add(files('9p-util-freebsd.c'))
  elif host_os == 'linux'
    fs_ss.add(files('9p-util-linux.c'))
  endif
endif
fs_ss.add(when: 'CONFIG_XEN_BUS', if_true: files('xen-9p-backend.c'))
system_ss.add_all(when: 'CONFIG_FSDEV_9P', if_true: fs_ss)

system_ss.add(when: 'CONFIG_VIRTIO_9P', if_true: files('virtio-9p-device.c'))
