use std::sync::{Arc, Mutex};

use ewwii_plugin_api::{EwwiiAPI, NativeFn, NativeFnExt, NbclType, PluginValue};
use serde_json::Value;

#[derive(Debug, Default)]
struct CachedJson {
    raw: Option<String>,
    value: Option<Value>,
    parses: u64,
}

impl CachedJson {
    fn ensure(&mut self, raw: &str) {
        if self.raw.as_deref() == Some(raw) {
            return;
        }

        self.raw = Some(raw.to_owned());
        self.value = serde_json::from_str(raw).ok();
        self.parses += 1;
    }

    fn project(&mut self, raw: &str, field: &str) -> String {
        self.ensure(raw);
        self.value
            .as_ref()
            .and_then(|value| value_at(value, field))
            .map(value_to_string)
            .unwrap_or_default()
    }
}

#[derive(Debug, Default)]
struct ScalarProjectionCache {
    status: CachedJson,
    audio: CachedJson,
    control: CachedJson,
}

impl ScalarProjectionCache {
    fn project(&mut self, scope: &str, field: &str, raw: &str) -> String {
        match scope {
            "status" => self.status.project(raw, field),
            "audio" => self.audio.project(raw, field),
            "control" => self.control.project(raw, field),
            _ => String::new(),
        }
    }
}

fn value_at<'a>(value: &'a Value, field: &str) -> Option<&'a Value> {
    field
        .split('.')
        .try_fold(value, |current, part| current.get(part))
}

fn value_to_string(value: &Value) -> String {
    match value {
        Value::Null => String::new(),
        Value::String(value) => value.clone(),
        Value::Bool(value) => value.to_string(),
        Value::Number(value) => value.to_string(),
        Value::Array(_) | Value::Object(_) => value.to_string(),
    }
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    let cache = Arc::new(Mutex::new(ScalarProjectionCache::default()));

    host.register_function(
        "onyrion_scalar_project",
        vec![NbclType::String, NbclType::String, NbclType::String],
        NbclType::String,
        NativeFn::new(move |args| {
            let PluginValue::String(ref scope) = args[0] else {
                unreachable!("onyrion_scalar_project scope requires String")
            };
            let PluginValue::String(ref field) = args[1] else {
                unreachable!("onyrion_scalar_project field requires String")
            };
            let PluginValue::String(ref raw) = args[2] else {
                unreachable!("onyrion_scalar_project raw requires String")
            };

            let projected = cache
                .lock()
                .expect("scalar projection cache mutex poisoned")
                .project(scope, field, raw);

            Ok(PluginValue::String(projected))
        }),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_payload_is_parsed_once_for_multiple_fields() {
        let mut cache = ScalarProjectionCache::default();
        let raw = r#"{"network":"Wi-Fi","battery":"88%","clock":"19:20"}"#;

        assert_eq!(cache.project("status", "network", raw), "Wi-Fi");
        assert_eq!(cache.project("status", "battery", raw), "88%");
        assert_eq!(cache.project("status", "clock", raw), "19:20");
        assert_eq!(cache.status.parses, 1);
    }

    #[test]
    fn audio_nested_fields_share_one_parse() {
        let mut cache = ScalarProjectionCache::default();
        let raw =
            r#"{"sink":{"display":"Speakers"},"source":{"display":"Mic"}}"#;

        assert_eq!(cache.project("audio", "sink.display", raw), "Speakers");
        assert_eq!(cache.project("audio", "source.display", raw), "Mic");
        assert_eq!(cache.audio.parses, 1);
    }

    #[test]
    fn control_payload_reparses_only_when_raw_changes() {
        let mut cache = ScalarProjectionCache::default();
        let raw_a = r#"{
            "wifi":{"display":"Wi-Fi on"},
            "bluetooth":{"display":"Bluetooth off"},
            "battery":{"display":"88%"},
            "power_profile":{"display":"balanced"}
        }"#;
        let raw_b = r#"{
            "wifi":{"display":"Wi-Fi off"},
            "bluetooth":{"display":"Bluetooth off"},
            "battery":{"display":"88%"},
            "power_profile":{"display":"balanced"}
        }"#;

        assert_eq!(
            cache.project("control", "wifi.display", raw_a),
            "Wi-Fi on"
        );
        assert_eq!(
            cache.project("control", "power_profile.display", raw_a),
            "balanced"
        );
        assert_eq!(cache.control.parses, 1);

        assert_eq!(
            cache.project("control", "wifi.display", raw_b),
            "Wi-Fi off"
        );
        assert_eq!(cache.control.parses, 2);
    }

    #[test]
    fn invalid_scope_field_or_json_degrades_to_empty_string() {
        let mut cache = ScalarProjectionCache::default();

        assert_eq!(cache.project("unknown", "x", "{}"), "");
        assert_eq!(cache.project("status", "missing", r#"{"network":"x"}"#), "");
        assert_eq!(cache.project("status", "network", "not-json"), "");
    }
}
