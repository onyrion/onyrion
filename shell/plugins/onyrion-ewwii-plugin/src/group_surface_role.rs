use std::{
    ffi::CString,
    slice,
    str,
};

const TABGROUP_WINDOW: &str = "onyrion-tabgroup";
const PROBE_WINDOW: &str = "onyrion-group-surface-probe";

const TABGROUP_NAMESPACE: &str = "onyrion.tabgroup";
const PROBE_NAMESPACE: &str = "onyrion.group-surface-probe";

extern "C" {
    fn onyrion_group_surface_bridge_prepare(
        gtk_window_ptr: usize,
        group_id: *const std::ffi::c_char,
        namespace_name: *const std::ffi::c_char,
        ui_height: i32,
    ) -> i32;
}

fn parse_group_id<'a>(window_name: &str, instance_id: &'a str) -> Option<&'a str> {
    if window_name != TABGROUP_WINDOW && window_name != PROBE_WINDOW {
        return None;
    }

    let prefix = format!("{window_name}:");
    let group_id = instance_id.strip_prefix(&prefix)?;

    if group_id.is_empty()
        || group_id.starts_with('0')
        || !group_id.bytes().all(|byte| byte.is_ascii_digit())
    {
        return None;
    }

    Some(group_id)
}

fn namespace_for(window_name: &str) -> Option<&'static str> {
    match window_name {
        TABGROUP_WINDOW => Some(TABGROUP_NAMESPACE),
        PROBE_WINDOW => Some(PROBE_NAMESPACE),
        _ => None,
    }
}

/// Direct Ewwii pre-realize ABI.
///
/// This callback must stay local-only: no HostProxy/EwwiiAPI calls are legal
/// here because Ewwii is already inside synchronous GtkWindow construction.
///
/// For a named TabGroup/probe window, successful return requires the local
/// bridge to have registered a same-client Onyrion external role. If the
/// patched gtk4-layer-shell seam or Core v14 is unavailable, reject this
/// special window rather than allowing it to escape as an ordinary XDG
/// toplevel.
#[no_mangle]
pub unsafe extern "C" fn ewwii_plugin_prepare_window_v1(
    window_name_ptr: *const u8,
    window_name_len: usize,
    instance_id_ptr: *const u8,
    instance_id_len: usize,
    gtk_window_ptr: usize,
    is_wayland: u8,
) -> i32 {
    if window_name_ptr.is_null()
        || instance_id_ptr.is_null()
        || gtk_window_ptr == 0
    {
        return 90;
    }

    let window_name =
        match str::from_utf8(slice::from_raw_parts(
            window_name_ptr,
            window_name_len,
        )) {
            Ok(value) => value,
            Err(_) => return 91,
        };

    let instance_id =
        match str::from_utf8(slice::from_raw_parts(
            instance_id_ptr,
            instance_id_len,
        )) {
            Ok(value) => value,
            Err(_) => return 92,
        };

    let Some(group_id) =
        parse_group_id(
            window_name,
            instance_id,
        )
    else {
        // Ordinary Ewwii windows are not external-role candidates.
        return 0;
    };

    if is_wayland != 1 {
        eprintln!(
            "[onyrion-group-surface-bridge] reject {}: non-Wayland backend",
            instance_id
        );
        return 93;
    }

    let Some(namespace) =
        namespace_for(window_name)
    else {
        return 94;
    };

    let Ok(group_id) =
        CString::new(group_id)
    else {
        return 95;
    };

    let Ok(namespace) =
        CString::new(namespace)
    else {
        return 96;
    };

    let ui_height =
        if window_name == TABGROUP_WINDOW {
            match crate::tabgroup_renderer::prepare_window(
                gtk_window_ptr,
                group_id
                    .as_c_str()
                    .to_str()
                    .unwrap_or_default(),
            ) {
                Ok(height) => height,
                Err(error) => {
                    eprintln!(
                        "[onyrion-group-surface-bridge] reject {}: TabGroup UI prepare error={}",
                        instance_id,
                        error
                    );
                    return 99;
                }
            }
        } else {
            1
        };

    let rc =
        onyrion_group_surface_bridge_prepare(
            gtk_window_ptr,
            group_id.as_ptr(),
            namespace.as_ptr(),
            ui_height,
        );

    if rc == 1 {
        return 0;
    }

    if window_name == TABGROUP_WINDOW {
        crate::tabgroup_renderer::forget_window(
            gtk_window_ptr
        );
    }

    eprintln!(
        "[onyrion-group-surface-bridge] reject {}: bridge prepare rc={}",
        instance_id,
        rc
    );

    // Do not map a special Group UI window as an ordinary XDG toplevel.
    if rc == 0 {
        97
    } else {
        98
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_tabgroup_and_probe_ids() {
        assert_eq!(
            parse_group_id(
                "onyrion-tabgroup",
                "onyrion-tabgroup:17",
            ),
            Some("17")
        );

        assert_eq!(
            parse_group_id(
                "onyrion-group-surface-probe",
                "onyrion-group-surface-probe:42",
            ),
            Some("42")
        );
    }

    #[test]
    fn rejects_non_group_and_malformed_ids() {
        assert_eq!(
            parse_group_id(
                "onyrion-bar-0",
                "onyrion-bar-0",
            ),
            None
        );

        for instance in [
            "onyrion-tabgroup",
            "onyrion-tabgroup:",
            "onyrion-tabgroup:0",
            "onyrion-tabgroup:01",
            "onyrion-tabgroup:-1",
            "onyrion-tabgroup:1x",
        ] {
            assert_eq!(
                parse_group_id(
                    "onyrion-tabgroup",
                    instance,
                ),
                None,
                "{instance}",
            );
        }
    }

    #[test]
    fn namespaces_are_shell_owned_and_opaque_to_core() {
        assert_eq!(
            namespace_for("onyrion-tabgroup"),
            Some("onyrion.tabgroup")
        );
        assert_eq!(
            namespace_for(
                "onyrion-group-surface-probe"
            ),
            Some("onyrion.group-surface-probe")
        );
        assert_eq!(
            namespace_for("other"),
            None
        );
    }
}
