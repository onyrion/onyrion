project(
  'onyrion',
  'c',
  version: '0.1.0',
  meson_version: '>= 1.8.0',
  default_options: [
    'warning_level=3',
    'werror=false',
    'c_std=c23',
  ],
)

installed_config_path = join_paths(
  get_option('prefix'),
  get_option('datadir'),
  'onyrion',
  'config',
  'compositor.kdl',
)

add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-DONYRION_INSTALLED_CONFIG_PATH="' + installed_config_path + '"',
  language: 'c',
)

cc = meson.get_compiler('c')

libm = cc.find_library(
  'm',
  required: true,
)

ckdl_inc = include_directories(
  'third_party/ckdl/include',
  'third_party/ckdl/src',
)

ckdl_lib = static_library(
  'ckdl',
  [
    'third_party/ckdl/src/bigint.c',
    'third_party/ckdl/src/compat.c',
    'third_party/ckdl/src/emitter.c',
    'third_party/ckdl/src/parser.c',
    'third_party/ckdl/src/str.c',
    'third_party/ckdl/src/tokenizer.c',
    'third_party/ckdl/src/utf8.c',
  ],
  include_directories: ckdl_inc,
  c_args: [
    '-DBUILDING_KDL=1',
    '-DKDL_STATIC_LIB=1',
  ],
  override_options: [
    'c_std=c11',
    'warning_level=0',
    'werror=false',
  ],
)

ckdl_dep = declare_dependency(
  link_with: ckdl_lib,
  include_directories: ckdl_inc,
  dependencies: libm,
  compile_args: [
    '-DKDL_STATIC_LIB=1',
  ],
)

wayland_server = dependency('wayland-server')
wlroots = dependency('wlroots-0.20')
wlr_protocols = dependency('wlr-protocols')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')

wayland = import('wayland')
wayland_scanner = find_program('wayland-scanner')

wlr_protocols_dir = wlr_protocols.get_variable(
  pkgconfig: 'pkgdatadir',
)

layer_shell_protocol_header = custom_target(
  'wlr-layer-shell-unstable-v1-protocol-header',
  input: wlr_protocols_dir + '/unstable/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [
    wayland_scanner,
    'server-header',
    '@INPUT@',
    '@OUTPUT@',
  ],
)

shell_protocol = wayland.scan_xml(
  'protocol/onyrion-shell-unstable-v1.xml',
  client: true,
  server: true,
  public: false,
  include_core_only: true,
)

inc = include_directories('include')

sources = files(
  'src/config.c',
  'src/config_runtime.c',
  'src/config_foundation.c',
  'src/control.c',
  'src/fallback.c',
  'src/action.c',
  'src/recovery.c',
  'src/main.c',
  'src/server.c',
  'src/shell_protocol.c',
  'src/session_lock.c',
  'src/output.c',
  'src/group.c',
  'src/group_surface.c',
  'src/input.c',
  'src/layer_shell.c',
  'src/layout.c',
  'src/window.c',
  'src/workspace.c',
)

executable(
  'onyrion',
  sources,
  shell_protocol,
  layer_shell_protocol_header,
  include_directories: inc,
  dependencies: [
    wayland_server,
    wlroots,
    xkbcommon,
    libinput,
    ckdl_dep,
  ],
  install: true,
)

install_data(
  'config/compositor.kdl',
  install_dir: join_paths(
    get_option('datadir'),
    'onyrion',
    'config',
  ),
)

if get_option('test_shell')
  wayland_client = dependency('wayland-client')
  wayland_protocols = dependency('wayland-protocols')

  xdg_shell_protocol = wayland.scan_xml(
    join_paths(
      wayland_protocols.get_variable(pkgconfig: 'pkgdatadir'),
      'stable',
      'xdg-shell',
      'xdg-shell.xml',
    ),
    client: true,
    server: false,
    public: false,
    include_core_only: true,
  )

  executable(
    'onyrion-test-shell',
    'tools/test-shell.c',
    shell_protocol,
    dependencies: [
      wayland_client,
    ],
    install: false,
  )

  executable(
    'onyrion-test-group-surface-configure',
    'tools/test-group-surface-configure.c',
    shell_protocol,
    xdg_shell_protocol,
    dependencies: [
      wayland_client,
    ],
    install: false,
  )
endif
