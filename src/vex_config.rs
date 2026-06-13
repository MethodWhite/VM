#![deny(warnings)]

use alloc::{
    collections::BTreeMap,
    format,
    string::{String, ToString},
    vec::Vec,
};
use core::fmt;

/// Valores por defecto para la configuración Vex
pub const DEFAULT_VEX_VERSION: &str = "1.0.0";
pub const DEFAULT_VEX_DIALECT: &str = "vex";
pub const DEFAULT_VEX_BACKEND: &str = "velb";
pub const DEFAULT_RUNTIME_MODE: &str = "interp";
pub const DEFAULT_JIT_THRESHOLD: u32 = 1000;
pub const DEFAULT_MAX_MEMORY_MB: u32 = 2048;
pub const DEFAULT_GC_GENERATIONS: u8 = 2;
pub const DEFAULT_DISTRIBUTED_PROTOCOL: &str = "vdp";
pub const DEFAULT_AUTHENTICATION: &str = "mtls";
pub const DEFAULT_DEBUG_LEVEL: &str = "info";
pub const DEFAULT_LOG_FILE: &str = "vesta.log";
pub const DEFAULT_LOGGING_LEVEL: &str = "info";
pub const DEFAULT_LOGGING_FORMAT: &str = "pretty";
pub const DEFAULT_LOGGING_OUTPUT: &str = "stdout";
pub const DEFAULT_MAX_LOG_SIZE_MB: u64 = 10;
pub const DEFAULT_MAX_LOG_FILES: u32 = 5;
pub const DEFAULT_LOG_COMPRESS: bool = true;

/// Esquema de configuración Vex
#[derive(Debug, Clone)]
pub struct VexConfig {
    /// Configuración general del ecosistema
    pub general: GeneralConfig,
    /// Configuración del lenguaje Vex
    pub language: LanguageConfig,
    /// Configuración del runtime
    pub runtime: RuntimeConfig,
    /// Configuración distribuida
    pub distributed: DistributedConfig,
    /// Configuración de debugging
    pub debugging: DebuggingConfig,
    /// Configuración de plugins
    pub plugins: PluginsConfig,
    /// Configuración de benchmarking
    pub benchmarking: BenchmarkingConfig,
    /// Configuración de testing
    pub testing: TestingConfig,
    /// Configuración de logging
    pub logging: LoggingConfig,
    /// Configuración de seguridad
    pub security: SecurityConfig,
}

#[derive(Debug, Clone)]
pub struct GeneralConfig {
    pub nombre: String,
    pub versión: String,
    pub autor: String,
    pub descripción: String,
    pub directorio_base: String,
}

#[derive(Debug, Clone)]
pub struct LanguageConfig {
    pub dialecto: String,
    pub backend: String,
    pub optimizaciones: bool,
    pub debug_info: bool,
    pub diagramas: bool,
}

#[derive(Debug, Clone)]
pub struct RuntimeConfig {
    pub modo: String,
    pub jit_threshold: u32,
    pub max_memory_mb: u32,
    pub gc_generation: u8,
    pub stack_size_mb: u32,
    pub max_threads: u32,
    pub scheduler: String,
}

#[derive(Debug, Clone)]
pub struct DistributedConfig {
    pub habilitado: bool,
    pub protocolo: String,
    pub autenticación: String,
    pub discovery_timeout: u32,
    pub max_connections: u32,
    pub heartbeat_interval: u32,
}

#[derive(Debug, Clone)]
pub struct DebuggingConfig {
    pub habilitado: bool,
    pub nivel: String,
    pub archivo: String,
    pub formato: String,
    pub breakpoints: Vec<String>,
    pub profiler_enabled: bool,
}

#[derive(Debug, Clone)]
pub struct PluginsConfig {
    pub habilitados: Vec<String>,
    pub directorio: String,
    pub auto_load: bool,
    pub debug_mode: bool,
}

#[derive(Debug, Clone)]
pub struct BenchmarkingConfig {
    pub habilitado: bool,
    pub directorio: String,
    pub formato: String,
    pub incluir_interp: bool,
    pub incluir_jit: bool,
    pub incluir_aot: bool,
}

#[derive(Debug, Clone)]
pub struct TestingConfig {
    pub habilitado: bool,
    pub rutas: Vec<String>,
    pub patrones: Vec<String>,
    pub timeout_segundos: u32,
    pub verbose: bool,
    pub parallel: bool,
}

#[derive(Debug, Clone)]
pub struct LoggingConfig {
    pub nivel: String,
    pub formato: String,
    pub salida: String,
    pub rotación: LogRotation,
    pub colores: bool,
    pub timestamps: bool,
}

#[derive(Debug, Clone)]
pub struct LogRotation {
    pub max_size_mb: u64,
    pub max_files: u32,
    pub compress: bool,
    pub archive_dir: String,
}

#[derive(Debug, Clone)]
pub struct SecurityConfig {
    pub pqc_habilitado: bool,
    pub encriptacion_habilitada: bool,
    pub integridad_habilitada: bool,
    pub integridad_check_interval_secs: u32,
    pub max_fallos_auths: u32,
    pub session_timeout_secs: u64,
}

impl Default for VexConfig {
    fn default() -> Self {
        Self {
            general: GeneralConfig::default(),
            language: LanguageConfig::default(),
            runtime: RuntimeConfig::default(),
            distributed: DistributedConfig::default(),
            debugging: DebuggingConfig::default(),
            plugins: PluginsConfig::default(),
            benchmarking: BenchmarkingConfig::default(),
            testing: TestingConfig::default(),
            logging: LoggingConfig::default(),
            security: SecurityConfig::default(),
        }
    }
}

impl Default for GeneralConfig {
    fn default() -> Self {
        Self {
            nombre: "VestaVM".to_string(),
            versión: DEFAULT_VEX_VERSION.to_string(),
            autor: "Equipo VestaVM".to_string(),
            descripción: "Máquina virtual distribuida y lenguaje moderno".to_string(),
            directorio_base: ".".to_string(),
        }
    }
}

impl Default for LanguageConfig {
    fn default() -> Self {
        Self {
            dialecto: DEFAULT_VEX_DIALECT.to_string(),
            backend: DEFAULT_VEX_BACKEND.to_string(),
            optimizaciones: true,
            debug_info: false,
            diagramas: false,
        }
    }
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            modo: DEFAULT_RUNTIME_MODE.to_string(),
            jit_threshold: DEFAULT_JIT_THRESHOLD,
            max_memory_mb: DEFAULT_MAX_MEMORY_MB,
            gc_generation: DEFAULT_GC_GENERATIONS,
            stack_size_mb: 64,
            max_threads: 8,
            scheduler: "round_robin".to_string(),
        }
    }
}

impl Default for DistributedConfig {
    fn default() -> Self {
        Self {
            habilitado: true,
            protocolo: DEFAULT_DISTRIBUTED_PROTOCOL.to_string(),
            autenticación: DEFAULT_AUTHENTICATION.to_string(),
            discovery_timeout: 30,
            max_connections: 100,
            heartbeat_interval: 60,
        }
    }
}

impl Default for DebuggingConfig {
    fn default() -> Self {
        Self {
            habilitado: true,
            nivel: DEFAULT_DEBUG_LEVEL.to_string(),
            archivo: DEFAULT_LOG_FILE.to_string(),
            formato: "pretty".to_string(),
            breakpoints: vec![],
            profiler_enabled: false,
        }
    }
}

impl Default for PluginsConfig {
    fn default() -> Self {
        Self {
            habilitados: vec![
                "vesta_io".to_string(),
                "vesta_math".to_string(),
                "vesta_collections".to_string(),
                "vesta_runtime".to_string(),
                "vesta_photonic".to_string(),
                "vesta_materia".to_string(),
                "vesta_quantum".to_string(),
            ],
            directorio: "stdlib/native".to_string(),
            auto_load: true,
            debug_mode: false,
        }
    }
}

impl Default for BenchmarkingConfig {
    fn default() -> Self {
        Self {
            habilitado: true,
            directorio: "bench_results".to_string(),
            formato: "json".to_string(),
            incluir_interp: true,
            incluir_jit: true,
            incluir_aot: false,
        }
    }
}

impl Default for TestingConfig {
    fn default() -> Self {
        Self {
            habilitado: true,
            rutas: vec!["tests/vex/".to_string(), "examples_codes_vex/".to_string()],
            patrones: vec!["*.vex".to_string(), "*.vsh".to_string()],
            timeout_segundos: 300,
            verbose: false,
            parallel: true,
        }
    }
}

impl Default for LoggingConfig {
    fn default() -> Self {
        Self {
            nivel: DEFAULT_LOGGING_LEVEL.to_string(),
            formato: DEFAULT_LOGGING_FORMAT.to_string(),
            salida: DEFAULT_LOGGING_OUTPUT.to_string(),
            rotación: LogRotation::default(),
            colores: true,
            timestamps: true,
        }
    }
}

impl Default for LogRotation {
    fn default() -> Self {
        Self {
            max_size_mb: DEFAULT_MAX_LOG_SIZE_MB,
            max_files: DEFAULT_MAX_LOG_FILES,
            compress: DEFAULT_LOG_COMPRESS,
            archive_dir: "logs".to_string(),
        }
    }
}

impl Default for SecurityConfig {
    fn default() -> Self {
        Self {
            pqc_habilitado: true,
            encriptacion_habilitada: true,
            integridad_habilitada: true,
            integridad_check_interval_secs: 300,
            max_fallos_auths: 5,
            session_timeout_secs: 86400,
        }
    }
}

impl VexConfig {
    /// Cargar configuración desde un archivo
    pub fn load(path: &str) -> Result<Self, ConfigError> {
        let content = read_file(path)?;
        Self::parse(&content)
    }

    /// Cargar desde ubicaciones por defecto
    pub fn load_default() -> Result<Self, ConfigError> {
        // Intentar XDG_CONFIG_HOME primero
        if let Ok(xdg) = std::env::var("XDG_CONFIG_HOME") {
            let path = format!("{}/vex/config.toml", xdg);
            if file_exists(&path) {
                return Self::load(&path);
            }
        }

        // Intentar ~/.config/vex/config.toml
        if let Ok(home) = std::env::var("HOME") {
            let path = format!("{}/.config/vex/config.toml", home);
            if file_exists(&path) {
                return Self::load(&path);
            }
        }

        // Intentar ~/.vex/config.toml
        if let Ok(home) = std::env::var("HOME") {
            let path = format!("{}/.vex/config.toml", home);
            if file_exists(&path) {
                return Self::load(&path);
            }
        }

        // Retornar configuración por defecto
        Ok(Self::default())
    }

    /// Parsear contenido TOML
    pub fn parse(content: &str) -> Result<Self, ConfigError> {
        let mut config = Self::default();
        let mut current_section = String::new();

        for line in content.lines() {
            let line = line.trim();

            // Saltar líneas vacías y comentarios
            if line.is_empty() || line.starts_with('#') {
                continue;
            }

            // Encabezado de sección
            if line.starts_with('[') && line.ends_with(']') {
                current_section = line[1..line.len() - 1].to_string();
                continue;
            }

            // Par de clave-valor
            if let Some(eq_pos) = line.find('=') {
                let key = line[..eq_pos].trim();
                let value = line[eq_pos + 1..].trim();

                match current_section.as_str() {
                    "" | "general" => Self::parse_general(&mut config.general, key, value),
                    "language" => Self::parse_language(&mut config.language, key, value),
                    "runtime" => Self::parse_runtime(&mut config.runtime, key, value),
                    "distributed" => Self::parse_distributed(&mut config.distributed, key, value),
                    "debugging" => Self::parse_debugging(&mut config.debugging, key, value),
                    "plugins" => Self::parse_plugins(&mut config.plugins, key, value),
                    "benchmarking" => Self::parse_benchmarking(&mut config.benchmarking, key, value),
                    "testing" => Self::parse_testing(&mut config.testing, key, value),
                    "logging" => Self::parse_logging(&mut config.logging, key, value),
                    "security" => Self::parse_security(&mut config.security, key, value),
                    _ => {} // Sección desconocida, saltar
                }
            }
        }

        Ok(config)
    }

    fn parse_general(cfg: &mut GeneralConfig, key: &str, value: &str) {
        match key {
            "nombre" => cfg.nombre = value.to_string(),
            "versión" => cfg.versión = value.to_string(),
            "autor" => cfg.autor = value.to_string(),
            "descripción" => cfg.descripción = value.to_string(),
            "directorio_base" => cfg.directorio_base = value.to_string(),
            _ => {}
        }
    }

    fn parse_language(cfg: &mut LanguageConfig, key: &str, value: &str) {
        match key {
            "dialecto" => cfg.dialecto = value.to_string(),
            "backend" => cfg.backend = value.to_string(),
            "optimizaciones" => cfg.optimizaciones = value.parse().unwrap_or(true),
            "debug_info" => cfg.debug_info = value.parse().unwrap_or(false),
            "diagramas" => cfg.diagramas = value.parse().unwrap_or(false),
            _ => {}
        }
    }

    fn parse_runtime(cfg: &mut RuntimeConfig, key: &str, value: &str) {
        match key {
            "modo" => cfg.modo = value.to_string(),
            "jit_threshold" => cfg.jit_threshold = value.parse().unwrap_or(DEFAULT_JIT_THRESHOLD),
            "max_memory_mb" => cfg.max_memory_mb = value.parse().unwrap_or(DEFAULT_MAX_MEMORY_MB),
            "gc_generation" => cfg.gc_generation = value.parse().unwrap_or(DEFAULT_GC_GENERATIONS),
            "stack_size_mb" => cfg.stack_size_mb = value.parse().unwrap_or(64),
            "max_threads" => cfg.max_threads = value.parse().unwrap_or(8),
            "scheduler" => cfg.scheduler = value.to_string(),
            _ => {}
        }
    }

    fn parse_distributed(cfg: &mut DistributedConfig, key: &str, value: &str) {
        match key {
            "habilitado" => cfg.habilitado = value.parse().unwrap_or(true),
            "protocolo" => cfg.protocolo = value.to_string(),
            "autenticación" => cfg.autenticación = value.to_string(),
            "discovery_timeout" => cfg.discovery_timeout = value.parse().unwrap_or(30),
            "max_connections" => cfg.max_connections = value.parse().unwrap_or(100),
            "heartbeat_interval" => cfg.heartbeat_interval = value.parse().unwrap_or(60),
            _ => {}
        }
    }

    fn parse_debugging(cfg: &mut DebuggingConfig, key: &str, value: &str) {
        match key {
            "habilitado" => cfg.habilitado = value.parse().unwrap_or(true),
            "nivel" => cfg.nivel = value.to_string(),
            "archivo" => cfg.archivo = value.to_string(),
            "formato" => cfg.formato = value.to_string(),
            "profiler_enabled" => cfg.profiler_enabled = value.parse().unwrap_or(false),
            _ => {}
        }
    }

    fn parse_plugins(cfg: &mut PluginsConfig, key: &str, value: &str) {
        match key {
            "habilitados" => {
                cfg.habilitados = value
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect();
            }
            "directorio" => cfg.directorio = value.to_string(),
            "auto_load" => cfg.auto_load = value.parse().unwrap_or(true),
            "debug_mode" => cfg.debug_mode = value.parse().unwrap_or(false),
            _ => {}
        }
    }

    fn parse_benchmarking(cfg: &mut BenchmarkingConfig, key: &str, value: &str) {
        match key {
            "habilitado" => cfg.habilitado = value.parse().unwrap_or(true),
            "directorio" => cfg.directorio = value.to_string(),
            "formato" => cfg.formato = value.to_string(),
            "incluir_interp" => cfg.incluir_interp = value.parse().unwrap_or(true),
            "incluir_jit" => cfg.incluir_jit = value.parse().unwrap_or(true),
            "incluir_aot" => cfg.incluir_aot = value.parse().unwrap_or(false),
            _ => {}
        }
    }

    fn parse_testing(cfg: &mut TestingConfig, key: &str, value: &str) {
        match key {
            "habilitado" => cfg.habilitado = value.parse().unwrap_or(true),
            "rutas" => {
                cfg.rutas = value
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect();
            }
            "patrones" => {
                cfg.patrones = value
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect();
            }
            "timeout_segundos" => cfg.timeout_segundos = value.parse().unwrap_or(300),
            "verbose" => cfg.verbose = value.parse().unwrap_or(false),
            "parallel" => cfg.parallel = value.parse().unwrap_or(true),
            _ => {}
        }
    }

    fn parse_logging(cfg: &mut LoggingConfig, key: &str, value: &str) {
        match key {
            "nivel" => cfg.nivel = value.to_string(),
            "formato" => cfg.formato = value.to_string(),
            "salida" => cfg.salida = value.to_string(),
            "colores" => cfg.colores = value.parse().unwrap_or(true),
            "timestamps" => cfg.timestamps = value.parse().unwrap_or(true),
            _ => {}
        }
    }

    fn parse_security(cfg: &mut SecurityConfig, key: &str, value: &str) {
        match key {
            "pqc_habilitado" => cfg.pqc_habilitado = value.parse().unwrap_or(true),
            "encriptacion_habilitada" => cfg.encriptacion_habilitada = value.parse().unwrap_or(true),
            "integridad_habilitada" => cfg.integridad_habilitada = value.parse().unwrap_or(true),
            "integridad_check_interval_secs" => {
                cfg.integridad_check_interval_secs = value.parse().unwrap_or(300)
            }
            "max_fallos_auths" => cfg.max_fallos_auths = value.parse().unwrap_or(5),
            "session_timeout_secs" => cfg.session_timeout_secs = value.parse().unwrap_or(86400),
            _ => {}
        }
    }

    /// Guardar configuración a un archivo
    pub fn save(&self, path: &str) -> Result<(), ConfigError> {
        let content = self.to_toml();
        write_file(path, &content)
    }

    /// Convertir a string TOML
    pub fn to_toml(&self) -> String {
        let mut s = String::new();

        s.push_str("# Configuración Vex Ecosystem\n\n");

        s.push_str("[general]\n");
        s.push_str(&format!("nombre = \"{}\"\n", self.general.nombre));
        s.push_str(&format!("versión = \"{}\"\n", self.general.versión));
        s.push_str(&format!("autor = \"{}\"\n", self.general.autor));
        s.push_str(&format!("descripción = \"{}\"\n", self.general.descripción));
        s.push_str(&format!("directorio_base = \"{}\"\n\n", self.general.directorio_base));

        s.push_str("[language]\n");
        s.push_str(&format!("dialecto = \"{}\"\n", self.language.dialecto));
        s.push_str(&format!("backend = \"{}\"\n", self.language.backend));
        s.push_str(&format!("optimizaciones = {}\n", self.language.optimizaciones));
        s.push_str(&format!("debug_info = {}\n", self.language.debug_info));
        s.push_str(&format!("diagramas = {}\n\n", self.language.diagramas));

        s.push_str("[runtime]\n");
        s.push_str(&format!("modo = \"{}\"\n", self.runtime.modo));
        s.push_str(&format!("jit_threshold = {}\n", self.runtime.jit_threshold));
        s.push_str(&format!("max_memory_mb = {}\n", self.runtime.max_memory_mb));
        s.push_str(&format!("gc_generation = {}\n", self.runtime.gc_generation));
        s.push_str(&format!("stack_size_mb = {}\n", self.runtime.stack_size_mb));
        s.push_str(&format!("max_threads = {}\n", self.runtime.max_threads));
        s.push_str(&format!("scheduler = \"{}\"\n\n", self.runtime.scheduler));

        s.push_str("[distributed]\n");
        s.push_str(&format!("habilitado = {}\n", self.distributed.habilitado));
        s.push_str(&format!("protocolo = \"{}\"\n", self.distributed.protocolo));
        s.push_str(&format!("autenticación = \"{}\"\n", self.distributed.autenticación));
        s.push_str(&format!("discovery_timeout = {}\n", self.distributed.discovery_timeout));
        s.push_str(&format!("max_connections = {}\n", self.distributed.max_connections));
        s.push_str(&format!("heartbeat_interval = {}\n\n", self.distributed.heartbeat_interval));

        s.push_str("[debugging]\n");
        s.push_str(&format!("habilitado = {}\n", self.debugging.habilitado));
        s.push_str(&format!("nivel = \"{}\"\n", self.debugging.nivel));
        s.push_str(&format!("archivo = \"{}\"\n", self.debugging.archivo));
        s.push_str(&format!("formato = \"{}\"\n", self.debugging.formato));
        s.push_str(&format!("profiler_enabled = {}\n\n", self.debugging.profiler_enabled));

        s.push_str("[plugins]\n");
        if !self.plugins.habilitados.is_empty() {
            s.push_str(&format!("habilitados = \"{}\"\n", self.plugins.habilitados.join(", ")));
        }
        s.push_str(&format!("directorio = \"{}\"\n", self.plugins.directorio));
        s.push_str(&format!("auto_load = {}\n", self.plugins.auto_load));
        s.push_str(&format!("debug_mode = {}\n\n", self.plugins.debug_mode));

        s.push_str("[benchmarking]\n");
        s.push_str(&format!("habilitado = {}\n", self.benchmarking.habilitado));
        s.push_str(&format!("directorio = \"{}\"\n", self.benchmarking.directorio));
        s.push_str(&format!("formato = \"{}\"\n", self.benchmarking.formato));
        s.push_str(&format!("incluir_interp = {}\n", self.benchmarking.incluir_interp));
        s.push_str(&format!("incluir_jit = {}\n", self.benchmarking.incluir_jit));
        s.push_str(&format!("incluir_aot = {}\n\n", self.benchmarking.incluir_aot));

        s.push_str("[testing]\n");
        s.push_str(&format!("habilitado = {}\n", self.testing.habilitado));
        if !self.testing.rutas.is_empty() {
            s.push_str(&format!("rutas = \"{}\"\n", self.testing.rutas.join(", ")));
        }
        if !self.testing.patrones.is_empty() {
            s.push_str(&format!("patrones = \"{}\"\n", self.testing.patrones.join(", ")));
        }
        s.push_str(&format!("timeout_segundos = {}\n", self.testing.timeout_segundos));
        s.push_str(&format!("verbose = {}\n", self.testing.verbose));
        s.push_str(&format!("parallel = {}\n\n", self.testing.parallel));

        s.push_str("[logging]\n");
        s.push_str(&format!("nivel = \"{}\"\n", self.logging.nivel));
        s.push_str(&format!("formato = \"{}\"\n", self.logging.formato));
        s.push_str(&format!("salida = \"{}\"\n", self.logging.salida));
        s.push_str(&format!("colores = {}\n", self.logging.colores));
        s.push_str(&format!("timestamps = {}\n\n", self.logging.timestamps));

        s.push_str("[security]\n");
        s.push_str(&format!("pqc_habilitado = {}\n", self.security.pqc_habilitado));
        s.push_str(&format!("encriptacion_habilitada = {}\n", self.security.encriptacion_habilitada));
        s.push_str(&format!("integridad_habilitada = {}\n", self.security.integridad_habilitada));
        s.push_str(&format!("integridad_check_interval_secs = {}\n", self.security.integridad_check_interval_secs));
        s.push_str(&format!("max_fallos_auths = {}\n", self.security.max_fallos_auths));
        s.push_str(&format!("session_timeout_secs = {}\n", self.security.session_timeout_secs));

        s
    }

    /// Obtener valor por clave (ejemplo: "server.port")
    pub fn get(&self, key: &str) -> Option<String> {
        let partes: Vec<&str> = key.split('.').collect();

        match partes.as_slice() {
            ["general", k] => self.general.get(k),
            ["language", k] => self.language.get(k),
            ["runtime", k] => self.runtime.get(k),
            ["distributed", k] => self.distributed.get(k),
            ["debugging", k] => self.debugging.get(k),
            ["plugins", k] => self.plugins.get(k),
            ["benchmarking", k] => self.benchmarking.get(k),
            ["testing", k] => self.testing.get(k),
            ["logging", k] => self.logging.get(k),
            ["security", k] => self.security.get(k),
            _ => None,
        }
    }

    /// Establecer valor por clave
    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        let partes: Vec<&str> = key.split('.').collect();

        match partes.as_slice() {
            ["general", k] => self.general.set(k, value),
            ["language", k] => self.language.set(k, value),
            ["runtime", k] => self.runtime.set(k, value),
            ["distributed", k] => self.distributed.set(k, value),
            ["debugging", k] => self.debugging.set(k, value),
            ["plugins", k] => self.plugins.set(k, value),
            ["benchmarking", k] => self.benchmarking.set(k, value),
            ["testing", k] => self.testing.set(k, value),
            ["logging", k] => self.logging.set(k, value),
            ["security", k] => self.security.set(k, value),
            _ => Err(ConfigError::InvalidKey(key.to_string())),
        }
    }

    /// Inicializar archivo de configuración por defecto
    pub fn init(path: &str) -> Result<(), ConfigError> {
        let config = Self::default();
        config.save(path)
    }
}

impl GeneralConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "nombre" => Some(self.nombre.clone()),
            "versión" => Some(self.versión.clone()),
            "autor" => Some(self.autor.clone()),
            "descripción" => Some(self.descripción.clone()),
            "directorio_base" => Some(self.directorio_base.clone()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "nombre" => {
                self.nombre = value.to_string();
                Ok(())
            }
            "versión" => {
                self.versión = value.to_string();
                Ok(())
            }
            "autor" => {
                self.autor = value.to_string();
                Ok(())
            }
            "descripción" => {
                self.descripción = value.to_string();
                Ok(())
            }
            "directorio_base" => {
                self.directorio_base = value.to_string();
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("general.{}", key))),
        }
    }
}

impl LanguageConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "dialecto" => Some(self.dialecto.clone()),
            "backend" => Some(self.backend.clone()),
            "optimizaciones" => Some(self.optimizaciones.to_string()),
            "debug_info" => Some(self.debug_info.to_string()),
            "diagramas" => Some(self.diagramas.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "dialecto" => {
                self.dialecto = value.to_string();
                Ok(())
            }
            "backend" => {
                self.backend = value.to_string();
                Ok(())
            }
            "optimizaciones" => {
                self.optimizaciones = value.parse().unwrap_or(true);
                Ok(())
            }
            "debug_info" => {
                self.debug_info = value.parse().unwrap_or(false);
                Ok(())
            }
            "diagramas" => {
                self.diagramas = value.parse().unwrap_or(false);
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("language.{}", key))),
        }
    }
}

impl RuntimeConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "modo" => Some(self.modo.clone()),
            "jit_threshold" => Some(self.jit_threshold.to_string()),
            "max_memory_mb" => Some(self.max_memory_mb.to_string()),
            "gc_generation" => Some(self.gc_generation.to_string()),
            "stack_size_mb" => Some(self.stack_size_mb.to_string()),
            "max_threads" => Some(self.max_threads.to_string()),
            "scheduler" => Some(self.scheduler.clone()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "modo" => {
                self.modo = value.to_string();
                Ok(())
            }
            "jit_threshold" => {
                self.jit_threshold = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "max_memory_mb" => {
                self.max_memory_mb = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "gc_generation" => {
                self.gc_generation = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "stack_size_mb" => {
                self.stack_size_mb = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "max_threads" => {
                self.max_threads = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "scheduler" => {
                self.scheduler = value.to_string();
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("runtime.{}", key))),
        }
    }
}

impl DistributedConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "habilitado" => Some(self.habilitado.to_string()),
            "protocolo" => Some(self.protocolo.clone()),
            "autenticación" => Some(self.autenticación.clone()),
            "discovery_timeout" => Some(self.discovery_timeout.to_string()),
            "max_connections" => Some(self.max_connections.to_string()),
            "heartbeat_interval" => Some(self.heartbeat_interval.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "habilitado" => {
                self.habilitado = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "protocolo" => {
                self.protocolo = value.to_string();
                Ok(())
            }
            "autenticación" => {
                self.autenticación = value.to_string();
                Ok(())
            }
            "discovery_timeout" => {
                self.discovery_timeout = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "max_connections" => {
                self.max_connections = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "heartbeat_interval" => {
                self.heartbeat_interval = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("distributed.{}", key))),
        }
    }
}

impl DebuggingConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "habilitado" => Some(self.habilitado.to_string()),
            "nivel" => Some(self.nivel.clone()),
            "archivo" => Some(self.archivo.clone()),
            "formato" => Some(self.formato.clone()),
            "profiler_enabled" => Some(self.profiler_enabled.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "habilitado" => {
                self.habilitado = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "nivel" => {
                self.nivel = value.to_string();
                Ok(())
            }
            "archivo" => {
                self.archivo = value.to_string();
                Ok(())
            }
            "formato" => {
                self.formato = value.to_string();
                Ok(())
            }
            "profiler_enabled" => {
                self.profiler_enabled = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("debugging.{}", key))),
        }
    }
}

impl PluginsConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "habilitados" => Some(self.habilitados.join(", ")),
            "directorio" => Some(self.directorio.clone()),
            "auto_load" => Some(self.auto_load.to_string()),
            "debug_mode" => Some(self.debug_mode.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "habilitados" => {
                self.habilitados = value
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect();
                Ok(())
            }
            "directorio" => {
                self.directorio = value.to_string();
                Ok(())
            }
            "auto_load" => {
                self.auto_load = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "debug_mode" => {
                self.debug_mode = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("plugins.{}", key))),
        }
    }
}

impl BenchmarkingConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "habilitado" => Some(self.habilitado.to_string()),
            "directorio" => Some(self.directorio.clone()),
            "formato" => Some(self.formato.clone()),
            "incluir_interp" => Some(self.incluir_interp.to_string()),
            "incluir_jit" => Some(self.incluir_jit.to_string()),
            "incluir_aot" => Some(self.incluir_aot.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "habilitado" => {
                self.habilitado = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "directorio" => {
                self.directorio = value.to_string();
                Ok(())
            }
            "formato" => {
                self.formato = value.to_string();
                Ok(())
            }
            "incluir_interp" => {
                self.incluir_interp = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "incluir_jit" => {
                self.incluir_jit = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "incluir_aot" => {
                self.incluir_aot = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("benchmarking.{}", key))),
        }
    }
}

impl TestingConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "habilitado" => Some(self.habilitado.to_string()),
            "rutas" => Some(self.rutas.join(", ")),
            "patrones" => Some(self.patrones.join(", ")),
            "timeout_segundos" => Some(self.timeout_segundos.to_string()),
            "verbose" => Some(self.verbose.to_string()),
            "parallel" => Some(self.parallel.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "habilitado" => {
                self.habilitado = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "rutas" => {
                self.rutas = value
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect();
                Ok(())
            }
            "patrones" => {
                self.patrones = value
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect();
                Ok(())
            }
            "timeout_segundos" => {
                self.timeout_segundos = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "verbose" => {
                self.verbose = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "parallel" => {
                self.parallel = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("testing.{}", key))),
        }
    }
}

impl LoggingConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "nivel" => Some(self.nivel.clone()),
            "formato" => Some(self.formato.clone()),
            "salida" => Some(self.salida.clone()),
            "colores" => Some(self.colores.to_string()),
            "timestamps" => Some(self.timestamps.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "nivel" => {
                self.nivel = value.to_string();
                Ok(())
            }
            "formato" => {
                self.formato = value.to_string();
                Ok(())
            }
            "salida" => {
                self.salida = value.to_string();
                Ok(())
            }
            "colores" => {
                self.colores = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "timestamps" => {
                self.timestamps = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("logging.{}", key))),
        }
    }
}

impl SecurityConfig {
    pub fn get(&self, key: &str) -> Option<String> {
        match key {
            "pqc_habilitado" => Some(self.pqc_habilitado.to_string()),
            "encriptacion_habilitada" => Some(self.encriptacion_habilitada.to_string()),
            "integridad_habilitada" => Some(self.integridad_habilitada.to_string()),
            "integridad_check_interval_secs" => Some(self.integridad_check_interval_secs.to_string()),
            "max_fallos_auths" => Some(self.max_fallos_auths.to_string()),
            "session_timeout_secs" => Some(self.session_timeout_secs.to_string()),
            _ => None,
        }
    }

    pub fn set(&mut self, key: &str, value: &str) -> Result<(), ConfigError> {
        match key {
            "pqc_habilitado" => {
                self.pqc_habilitado = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "encriptacion_habilitada" => {
                self.encriptacion_habilitada = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "integridad_habilitada" => {
                self.integridad_habilitada = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "integridad_check_interval_secs" => {
                self.integridad_check_interval_secs = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "max_fallos_auths" => {
                self.max_fallos_auths = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            "session_timeout_secs" => {
                self.session_timeout_secs = value
                    .parse()
                    .map_err(|_| ConfigError::InvalidValue(key.to_string()))?;
                Ok(())
            }
            _ => Err(ConfigError::InvalidKey(format!("security.{}", key))),
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// UTILIDADES DE ARCHIVO
// ═══════════════════════════════════════════════════════════════════════════

fn default_directorio_base() -> String {
    if let Ok(home) = std::env::var("HOME") {
        format!("{}/.vex", home)
    } else {
        ".vex".to_string()
    }
}

fn file_exists(path: &str) -> bool {
    std::fs::metadata(path).is_ok()
}

fn read_file(path: &str) -> Result<String, ConfigError> {
    std::fs::read_to_string(path).map_err(|_| ConfigError::IoError(path.to_string()))
}

fn write_file(path: &str, content: &str) -> Result<(), ConfigError> {
    // Asegurar que el directorio existe
    if let Some(parent) = std::path::Path::new(path).parent() {
        std::fs::create_dir_all(parent).map_err(|_| ConfigError::IoError(path.to_string()))?;
    }

    std::fs::write(path, content).map_err(|_| ConfigError::IoError(path.to_string()))
}

// ═══════════════════════════════════════════════════════════════════════════
// ERRORES
// ═══════════════════════════════════════════════════════════════════════════

#[derive(Debug, Clone)]
pub enum ConfigError {
    IoError(String),
    ParseError(String),
    InvalidKey(String),
    InvalidValue(String),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IoError(path) => write!(f, "Error de E/S accediendo a: {}", path),
            Self::ParseError(msg) => write!(f, "Error de parseo: {}", msg),
            Self::InvalidKey(key) => write!(f, "Clave de configuración inválida: {}", key),
            Self::InvalidValue(key) => write!(f, "Valor inválido para la clave: {}", key),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = VexConfig::default();
        assert_eq!(config.runtime.jit_threshold, DEFAULT_JIT_THRESHOLD);
        assert_eq!(config.runtime.max_memory_mb, DEFAULT_MAX_MEMORY_MB);
    }

    #[test]
    fn test_toml_roundtrip() {
        let config = VexConfig::default();
        let toml = config.to_toml();
        let parsed = VexConfig::parse(&toml).unwrap();
        assert_eq!(parsed.runtime.jit_threshold, config.runtime.jit_threshold);
    }

    #[test]
    fn test_get_set() {
        let mut config = VexConfig::default();
        config.set("runtime.jit_threshold", "2000").unwrap();
        assert_eq!(config.get("runtime.jit_threshold"), Some("2000".to_string()));
    }

    #[test]
    fn test_parse_toml() {
        let toml = r#"
[general]
nombre = "/custom/path"

[runtime]
modo = "jit"
max_memory_mb = 4096
"#;
        let config = VexConfig::parse(toml).unwrap();
        assert_eq!(config.general.nombre, "/custom/path");
        assert_eq!(config.runtime.modo, "jit");
        assert_eq!(config.runtime.max_memory_mb, 4096);
    }
}
