use std::{
    env,
    ffi::OsStr,
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

fn run(mut cmd: Command, what: &str) {
    let status = cmd
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .status()
        .unwrap_or_else(|error| panic!("{what}: failed to spawn: {error}"));

    if !status.success() {
        panic!("{what}: exited with {status}");
    }
}

fn words(output: &[u8]) -> Vec<String> {
    String::from_utf8_lossy(output)
        .split_whitespace()
        .map(ToOwned::to_owned)
        .collect()
}

fn command_output(program: &str, args: &[&str], what: &str) -> Vec<String> {
    let output = Command::new(program)
        .args(args)
        .output()
        .unwrap_or_else(|error| panic!("{what}: failed to spawn: {error}"));

    if !output.status.success() {
        panic!(
            "{what}: exited with {}\n{}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        );
    }

    words(&output.stdout)
}

fn compile_c(
    cc: &OsStr,
    source: &Path,
    output: &Path,
    include_dir: &Path,
    pkg_cflags: &[String],
    extra: &[&str],
    what: &str,
) {
    let mut cmd = Command::new(cc);
    cmd.arg("-fPIC")
        .arg("-c")
        .arg(source)
        .arg("-o")
        .arg(output)
        .arg(format!("-I{}", include_dir.display()));

    for flag in pkg_cflags {
        cmd.arg(flag);
    }

    for flag in extra {
        cmd.arg(flag);
    }

    run(cmd, what);
}

fn main() {
    let manifest_dir =
        PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));

    let protocol_xml = manifest_dir
        .join("../../protocol/onyrion-shell-unstable-v1.xml");
    let bridge_c = manifest_dir.join("src/group_surface_bridge.c");

    let protocol_h =
        out_dir.join("onyrion-shell-unstable-v1-client-protocol.h");
    let protocol_c =
        out_dir.join("onyrion-shell-unstable-v1-protocol.c");
    let bridge_o = out_dir.join("group_surface_bridge.o");
    let protocol_o = out_dir.join("onyrion-shell-protocol.o");
    let archive = out_dir.join("libonyrion_group_surface_bridge.a");

    println!("cargo:rerun-if-changed={}", protocol_xml.display());
    println!("cargo:rerun-if-changed={}", bridge_c.display());
    println!("cargo:rerun-if-changed=build.rs");

    let wayland_scanner =
        env::var_os("WAYLAND_SCANNER").unwrap_or_else(|| "wayland-scanner".into());
    let cc = env::var_os("CC").unwrap_or_else(|| "cc".into());
    let ar = env::var_os("AR").unwrap_or_else(|| "ar".into());

    {
        let mut cmd = Command::new(&wayland_scanner);
        cmd.arg("client-header")
            .arg(&protocol_xml)
            .arg(&protocol_h);
        run(cmd, "generate Onyrion client protocol header");
    }

    {
        let mut cmd = Command::new(&wayland_scanner);
        cmd.arg("private-code")
            .arg(&protocol_xml)
            .arg(&protocol_c);
        run(cmd, "generate Onyrion client protocol code");
    }

    let pkg_cflags = command_output(
        "pkg-config",
        &["--cflags", "gtk4", "wayland-client"],
        "pkg-config cflags",
    );

    compile_c(
        &cc,
        &bridge_c,
        &bridge_o,
        &out_dir,
        &pkg_cflags,
        &[
            "-std=c11",
            "-D_GNU_SOURCE",
            "-Wall",
            "-Wextra",
            "-Werror",
        ],
        "compile Onyrion Group-surface bridge",
    );

    compile_c(
        &cc,
        &protocol_c,
        &protocol_o,
        &out_dir,
        &pkg_cflags,
        &["-std=c11"],
        "compile Onyrion generated protocol code",
    );

    if archive.exists() {
        fs::remove_file(&archive).expect("remove stale bridge archive");
    }

    {
        let mut cmd = Command::new(&ar);
        cmd.arg("crs")
            .arg(&archive)
            .arg(&bridge_o)
            .arg(&protocol_o);
        run(cmd, "archive Onyrion Group-surface bridge");
    }

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=onyrion_group_surface_bridge");
    println!("cargo:rustc-link-lib=dylib=wayland-client");
    println!("cargo:rustc-link-lib=dylib=dl");
}
