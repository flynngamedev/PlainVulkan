//! `pvproject.json` schema -- the small manifest `pv new` creates and
//! `pv build` / `pv run` read back.

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WindowConfig {
    #[serde(default = "default_width")]
    pub width: u32,
    #[serde(default = "default_height")]
    pub height: u32,
    #[serde(default = "default_title")]
    pub title: String,
}

fn default_width() -> u32 {
    1280
}
fn default_height() -> u32 {
    720
}
fn default_title() -> String {
    "PlainVulkan Game".to_string()
}

impl Default for WindowConfig {
    fn default() -> Self {
        WindowConfig { width: default_width(), height: default_height(), title: default_title() }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct BuildConfig {
    #[serde(default)]
    pub vulkan_validation: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PvProject {
    pub name: String,
    #[serde(default = "default_version")]
    pub version: String,
    #[serde(default = "default_entry")]
    pub entry: String,
    #[serde(default)]
    pub window: WindowConfig,
    #[serde(default)]
    pub build: BuildConfig,
}

fn default_version() -> String {
    "0.1.0".to_string()
}
fn default_entry() -> String {
    "main.pv".to_string()
}

impl PvProject {
    pub fn new(name: &str) -> Self {
        PvProject {
            name: name.to_string(),
            version: default_version(),
            entry: default_entry(),
            window: WindowConfig { title: name.to_string(), ..WindowConfig::default() },
            build: BuildConfig::default(),
        }
    }

    pub fn load(path: &Path) -> Result<Self, String> {
        let text = fs::read_to_string(path)
            .map_err(|e| format!("could not read '{}': {}", path.display(), e))?;
        serde_json::from_str(&text).map_err(|e| format!("invalid pvproject.json: {}", e))
    }

    pub fn save(&self, path: &Path) -> Result<(), String> {
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        fs::write(path, text + "\n").map_err(|e| format!("could not write '{}': {}", path.display(), e))
    }
}
