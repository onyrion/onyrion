project(
    'onyrion-shell',
    'c',
    version: '0.1.0',
    default_options: [
        'c_std=c23',
        'warning_level=3',
        'werror=true',
    ],
)

add_project_arguments(
    '-D_POSIX_C_SOURCE=200809L',
    language: 'c',
)

wayland_client = dependency(
    'wayland-client',
    version: '>=1.25.0',
)

glib = dependency(
    'glib-2.0',
)

xcb = dependency(
    'xcb',
)

json_glib = dependency(
    'json-glib-1.0',
)

gio_unix = dependency(
    'gio-unix-2.0',
    include_type: 'system',
)

pipewire = dependency(
    'libpipewire-0.3',
    version: '>=1.6.0',
    include_type: 'system',
)

gtk4 = dependency(
    'gtk4',
    include_type: 'system',
)

gtk4_layer_shell = dependency(
    'gtk4-layer-shell-0',
    required: false,
    include_type: 'system',
)

if not gtk4_layer_shell.found()
    gtk4_layer_shell = dependency(
        'gtk-layer-shell-0',
        include_type: 'system',
    )
endif

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

wayland_scanner = find_program(
    'wayland-scanner',
)

protocol_xml = files(
    'protocol/onyrion-shell-unstable-v1.xml',
)

protocol_header = custom_target(
    'onyrion-shell-client-protocol',
    input: protocol_xml,
    output: 'onyrion-shell-unstable-v1-client-protocol.h',
    command: [
        wayland_scanner,
        'client-header',
        '@INPUT@',
        '@OUTPUT@',
    ],
)

protocol_code = custom_target(
    'onyrion-shell-protocol',
    input: protocol_xml,
    output: 'onyrion-shell-unstable-v1-protocol.c',
    command: [
        wayland_scanner,
        'private-code',
        '@INPUT@',
        '@OUTPUT@',
    ],
)

executable(
    'onyrion-shell',
    [
        'src/main.c',
        'src/state.c',
        'src/config.c',
        'src/config_editor.c',
        'src/config_runtime.c',
        'src/provider.c',
        'src/provider_runtime.c',
        'src/ui_runtime.c',
        'src/ui_daily_v11.c',
        'src/ui_actions_v11.c',
        'src/widget_model.c',
        'src/ewwii_adapter.c',
        'src/power_actions.c',
        'src/tray_sni.c',
        protocol_header,
        protocol_code,
    ],
    include_directories: include_directories(
        'include',
        '.',
    ),
    dependencies: [
        wayland_client,
        gio_unix,
        glib,
        json_glib,
        ckdl_dep,
    ],
    install: true,
)

install_symlink(
    'onyrionctl',
    pointing_to: 'onyrion-shell',
    install_dir: get_option('bindir'),
)

widget_model_test = executable(
    'onyrion-widget-model-test',
    [
        'tests/widget_model_test.c',
        'src/widget_model.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [
        glib,
        json_glib,
    ],
    install: false,
)

test(
    'widget-model-bar-projection',
    widget_model_test,
)

onyrion_libexec_dir = get_option('libexecdir') / 'onyrion'
onyrion_data_dir = get_option('datadir') / 'onyrion'

cargo = find_program(
    'cargo',
)

onyrion_ewwii_plugin_builder = find_program(
    'plugins/onyrion-ewwii-plugin/build.sh',
)

onyrion_ewwii_plugin_sources = files(
    'plugins/onyrion-ewwii-plugin/Cargo.toml',
    'plugins/onyrion-ewwii-plugin/Cargo.lock',
    'plugins/onyrion-ewwii-plugin/build.rs',
    'plugins/onyrion-ewwii-plugin/src/lib.rs',
    'plugins/onyrion-ewwii-plugin/src/notifications.rs',
    'plugins/onyrion-ewwii-plugin/src/audio_renderer.rs',
    'plugins/onyrion-ewwii-plugin/src/wifi_renderer.rs',
    'plugins/onyrion-ewwii-plugin/src/bluetooth_renderer.rs',
    'plugins/onyrion-ewwii-plugin/src/tray_renderer.rs',
    'plugins/onyrion-ewwii-plugin/src/scalar_projection.rs',
    'plugins/onyrion-ewwii-plugin/src/bar_renderer.rs',
    'plugins/onyrion-ewwii-plugin/src/tabgroup_model.rs',
    'plugins/onyrion-ewwii-plugin/src/tabgroup_renderer.rs',
    'plugins/onyrion-ewwii-plugin/src/group_surface_bridge.c',
    'plugins/onyrion-ewwii-plugin/src/group_surface_role.rs',
)

onyrion_ewwii_plugin = custom_target(
    'onyrion-ewwii-plugin',
    input: onyrion_ewwii_plugin_sources,
    output: 'onyrion-ewwii-plugin.so',
    command: [
        onyrion_ewwii_plugin_builder,
        cargo,
        join_paths(
            meson.current_source_dir(),
            'plugins/onyrion-ewwii-plugin/Cargo.toml',
        ),
        join_paths(
            meson.current_build_dir(),
            'onyrion-ewwii-plugin-cargo',
        ),
        '@OUTPUT@',
    ],
    build_by_default: true,
    install: true,
    install_dir: onyrion_data_dir / 'ui' / 'ewwii' / 'plugins',
)

executable(
    'onyrion-control',
    [
        'src/control_cli.c',
        'src/control_plane.c',
        'src/control_watch.c',
        'src/wifi_extended.c',
        'src/bluetooth_extended.c',
        'src/audio_control.c',
        'src/audio_extended.c',
    ],
    include_directories: include_directories(
        'include',
        '.',
    ),
    dependencies: [
        gio_unix,
        glib,
        json_glib,
        pipewire,
    ],
    install: true,
)

executable(
    'onyrion-wifi-prompt',
    'src/wifi_secret_prompt.c',
    dependencies: [
        gio_unix,
        glib,
        gtk4,
    ],
    install: true,
    install_dir: onyrion_libexec_dir,
)

executable(
    'onyrion-launcher',
    'src/launcher.c',
    dependencies: [
        gio_unix,
        gtk4,
        gtk4_layer_shell,
    ],
    install: true,
    install_dir: onyrion_libexec_dir,
)

executable(
    'onyrion-session',
    'src/session.c',
    dependencies: [
        wayland_client,
        xcb,
    ],
    install: true,
    install_dir: onyrion_libexec_dir,
)

install_data(
    'config/shell.kdl',
    'config/hyprlock.conf',
    install_dir: onyrion_data_dir / 'config',
)

install_data(
    'ui/ewwii/ewwii.nbcl',
    'ui/ewwii/ewwii.scss',
    'ui/ewwii/default-wallpaper.svg',
    install_dir: onyrion_data_dir / 'ui' / 'ewwii',
)

install_data(
    'ui/ewwii/start-ewwii.sh',
    install_dir: onyrion_data_dir / 'ui' / 'ewwii',
    install_mode: 'rwxr-xr-x',
)

install_data(
    'data/onyrion.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
    'data/onyrion-portals.conf',
    install_dir: get_option('datadir') / 'xdg-desktop-portal',
)
