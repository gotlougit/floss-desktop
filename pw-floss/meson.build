project(
  'pw-floss',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
  ],
)

pipewire = dependency('libpipewire-0.3', version: '>= 0.3')
dbus = dependency('dbus-1', version: '>= 1.12')

executable(
  'pw-floss',
  'src/pw-floss.c',
  dependencies: [pipewire, dbus],
  install: true,
)

install_data('PROTOCOL.md', install_dir: get_option('datadir') / 'doc' / 'pw-floss')

summary({
  'PipeWire': pipewire.version(),
  'D-Bus': dbus.version(),
}, section: 'Dependencies')
