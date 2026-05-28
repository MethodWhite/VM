/**
 * @file sandbox.cpp
 * @brief Implementación de @c Caps, @c CapWhitelists y el parser de strings.
 *
 * Este archivo proporciona la representación en memoria de las
 * capabilities del sandbox y las funciones de serialización/parsing
 * que las convierten desde y hacia el formato textual que usa el
 * usuario (`--vex-caps`, segundo argumento de `loadmodule`, etc.).
 *
 * El diseño del parser es deliberadamente tolerante a tokens
 * desconocidos: si el usuario escribe `--vex-caps "fs:read,foo:bar"`,
 * el `foo:bar` se descarta silenciosamente en lugar de aborter la
 * compilación. La idea es que añadir caps nuevas en el futuro no
 * rompa configuraciones existentes.
 */

#include "loader/sandbox.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace loader {

namespace {

/**
 * @brief Convierte la cadena a minúsculas (ASCII).
 *
 * Solo se usa para el match case-insensitive de nombres de DLL en
 * Windows. POSIX preserva el case original.
 */
inline std::string to_lower_(std::string s) {
  for (auto &c : s) {
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

/**
 * @brief Recorta espacios y saltos de línea al principio y al final.
 *
 * Garantiza que `" fs:read "` y `"fs:read"` se traten igual. Los
 * espacios internos no se tocan: deja intactos paths o nombres con
 * espacios internos.
 */
inline std::string trim_(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'
  || s.front() == '\r' || s.front() == '\n')) {
  s.erase(s.begin());
  }
  while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t'
  || s.back()  == '\r' || s.back()  == '\n')) {
  s.pop_back();
  }
  return s;
}

/**
 * @brief Divide el string al nivel "más exterior" usando coma como separador.
 *
 * El parser de caps usa dos niveles de separadores:
 *
 *  - `,` separa caps distintas: `"fs:read,net,ffi:call"`.
 *  - `;` separa argumentos dentro de una cap: `"ffi:call=a.dll;b.dll"`.
 *
 * Este helper hace el primer split. La función contempla paréntesis y
 * corchetes para no partir dentro de ellos, aunque la gramática actual
 * no los utiliza (queda como reserva para futuras extensiones tipo
 * `cap=(a,b,c)`).
 */
std::vector<std::string> split_top_(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : s) {
  if (c == '[' || c == '(') ++depth;
  else if (c == ']' || c == ')') --depth;
  if (c == ',' && depth == 0) {
  out.push_back(trim_(cur));
  cur.clear();
  } else {
  cur.push_back(c);
  }
  }
  if (!cur.empty()) out.push_back(trim_(cur));
  return out;
}

/**
 * @brief Divide el string usando punto y coma como separador.
 *
 * Solo se invoca sobre el lado derecho de un `cap=...`. Por ejemplo,
 * para `"ffi:call=kernel32.dll;user32.dll"` la entrada a esta función
 * es `"kernel32.dll;user32.dll"` y la salida es
 * `["kernel32.dll", "user32.dll"]`.
 */
std::vector<std::string> split_sub_(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
  if (c == ';') {
  out.push_back(trim_(cur));
  cur.clear();
  } else {
  cur.push_back(c);
  }
  }
  if (!cur.empty()) out.push_back(trim_(cur));
  return out;
}

/**
 * @brief Mapea el nombre textual de una cap al bit correspondiente.
 *
 * Las dos formas combinadas (`fs`, `ffi`) son convenientes para
 * autorizar la familia completa con una sola entrada en el string:
 * `--vex-caps "fs"` equivale a `--vex-caps "fs:read,fs:write"`.
 *
 * @param name Nombre de la cap.
 * @return Bitmask con el o los bits correspondientes; 0 si el nombre
 *  no se reconoce (el caller lo trata como token a ignorar).
 */
uint32_t cap_bit_from_name_(const std::string &name) noexcept {
  if (name == "fs:read")  return Caps::FS_READ;
  if (name == "fs:write") return Caps::FS_WRITE;
  if (name == "fs")  return Caps::FS_READ | Caps::FS_WRITE;
  if (name == "net")  return Caps::NET;
  if (name == "ffi:call") return Caps::FFI_CALL;
  if (name == "ffi:open") return Caps::FFI_OPEN;
  if (name == "ffi")  return Caps::FFI_CALL | Caps::FFI_OPEN;
  if (name == "spawn")  return Caps::SPAWN;
  if (name == "distrib")  return Caps::DISTRIB;
  if (name == "classreg") return Caps::CLASSREG;
  if (name == "mem:host") return Caps::MEM_HOST;
  if (name == "loadmod")  return Caps::LOADMOD;
  return 0;
}

/**
 * @brief Parsea un rango hexadecimal `"0xAAAA-0xBBBB"`.
 *
 * Se utiliza para las whitelists de @c mem:host . La base se
 * autodetecta vía `std::stoull` con `base = 0`, lo que permite
 * también valores decimales aunque el caso típico sea hex.
 *
 * @param s  Texto del rango.
 * @param start [out] Inicio del rango (inclusive).
 * @param end  [out] Fin del rango (exclusive).
 * @return @c true si el parseo fue correcto Y @p end > @p start;
 *  @c false en caso de error o rango degenerado.
 */
bool parse_range_(const std::string &s, uint64_t &start, uint64_t &end) noexcept {
  size_t dash = s.find('-');
  if (dash == std::string::npos) return false;
  std::string a = trim_(s.substr(0, dash));
  std::string b = trim_(s.substr(dash + 1));
  try {
  start = std::stoull(a, nullptr, 0);  // base = 0: autodetecta 0x prefix
  end  = std::stoull(b, nullptr, 0);
  } catch (...) {
  // Cualquier error de conversión (formato inválido, overflow) se
  // trata como rango no parseable. El caller lo ignora.
  return false;
  }
  return end > start;
}

}  // namespace anónimo

// ---------------------------------------------------------------------
//  parse_caps : texto de configuración -> Caps en memoria
// ---------------------------------------------------------------------

Caps parse_caps(const std::string &raw) noexcept {
  Caps caps;
  std::string s = trim_(raw);

  // Atajos: el string vacío y `"all"` son equivalentes y producen el
  // modo "sin sandbox" (todas las caps concedidas, whitelists vacías).
  // Este es el comportamiento por defecto, compatible con código que
  // no usa el sandbox.
  if (s.empty() || s == "all") {
  caps.bits = Caps::ALL;
  return caps;
  }

  // `"none"` produce el sandbox total: ningún permiso concedido.
  // Útil para correr código completamente aislado salvo computación
  // pura e IPC local (mailbox intra-VM).
  if (s == "none") {
  caps.bits = Caps::NONE;
  return caps;
  }

  // Modo restringido: empezamos SIN nada y añadimos los permisos
  // explícitamente listados. Esto evita sorpresas: si el usuario
  // escribe `"fs:read"` no obtiene también `net`, `ffi`, etc.
  caps.bits = Caps::NONE;

  for (const auto &tok_raw : split_top_(s)) {
  std::string tok = trim_(tok_raw);
  if (tok.empty()) continue;

  // Cada token puede ser `cap` o `cap=arg1;arg2;...`. El `=`
  // separa el nombre de los argumentos de whitelist.
  size_t eq = tok.find('=');
  std::string cap_name_s = (eq == std::string::npos) ? tok : tok.substr(0, eq);
  std::string args_s  = (eq == std::string::npos) ? std::string() : tok.substr(eq + 1);
  cap_name_s = trim_(cap_name_s);

  const uint32_t bit = cap_bit_from_name_(cap_name_s);
  if (bit == 0) {
  // Token desconocido: se descarta en silencio. Esto permite
  // que el formato evolucione sin romper configuraciones
  // existentes (futuras caps no implementadas todavía).
  continue;
  }

  // Conceder la cap. Si hay varios bits (caso de los alias `fs` o
  // `ffi`), se conceden todos a la vez.
  caps.bits |= bit;
  if (args_s.empty()) continue;

  // Procesar los argumentos según el tipo de cap. Cada cap usa
  // una whitelist distinta, por lo que enrutamos los args al
  // contenedor adecuado.
  const auto subs = split_sub_(args_s);

  if (bit & Caps::FFI_CALL) {
  for (const auto &sub : subs) {
  if (!sub.empty()) caps.wl.dll_whitelist.push_back(sub);
  }
  }
  if (bit & (Caps::FS_READ | Caps::FS_WRITE)) {
  for (const auto &sub : subs) {
  if (!sub.empty()) caps.wl.path_whitelist.push_back(sub);
  }
  }
  if (bit & Caps::NET) {
  for (const auto &sub : subs) {
  if (!sub.empty()) caps.wl.host_whitelist.push_back(sub);
  }
  }
  if (bit & Caps::MEM_HOST) {
  // Forma especial para MEM_HOST: si trae argumentos, no se
  // concede como bool sino que se restringe a los rangos
  // listados. La intuición es:
  //  "mem:host"  -> todo permitido
  //  "mem:host=0x10000000-0x20000000" -> solo ese rango
  // Para el segundo caso quitamos el bit y registramos los
  // rangos como whitelist.
  if (!subs.empty()) {
  caps.bits &= ~Caps::MEM_HOST;
  for (const auto &sub : subs) {
  uint64_t a = 0, b = 0;
  if (parse_range_(sub, a, b)) {
  caps.wl.mem_ranges.push_back({a, b});
  }
  }
  }
  }
  }
  return caps;
}

// ---------------------------------------------------------------------
//  caps_to_string : Caps en memoria -> texto canónico
// ---------------------------------------------------------------------

std::string caps_to_string(const Caps &caps) noexcept {
  // Casos especiales primero, para que el formato sea estable y
  // los diagnósticos lean "all" / "none" en lugar de listar las 10 caps.
  if (caps.bits == Caps::ALL && caps.wl.empty()) return "all";
  if (caps.bits == Caps::NONE && caps.wl.empty()) return "none";

  std::string out;

  // Helper local que añade una cap al string si está concedida. Si
  // tiene whitelist asociada, se añade tras un `=` con los elementos
  // separados por `;`.
  auto emit_bit = [&](const char *name,
  uint32_t bit,
  const std::vector<std::string> *wl_list) {
  if (caps.bits & bit) {
  if (!out.empty()) out += ",";
  out += name;
  if (wl_list && !wl_list->empty()) {
  out += "=";
  for (size_t i = 0; i < wl_list->size(); ++i) {
  if (i > 0) out += ";";
  out += (*wl_list)[i];
  }
  }
  }
  };

  emit_bit("fs:read",  Caps::FS_READ,  &caps.wl.path_whitelist);
  // path_whitelist se comparte entre fs:read y fs:write; la
  // serializamos solo una vez (junto a fs:read) para no duplicarla.
  emit_bit("fs:write", Caps::FS_WRITE, nullptr);
  emit_bit("net",  Caps::NET,  &caps.wl.host_whitelist);
  emit_bit("ffi:call", Caps::FFI_CALL, &caps.wl.dll_whitelist);
  emit_bit("ffi:open", Caps::FFI_OPEN, nullptr);
  emit_bit("spawn",  Caps::SPAWN,  nullptr);
  emit_bit("distrib",  Caps::DISTRIB,  nullptr);
  emit_bit("classreg", Caps::CLASSREG, nullptr);
  emit_bit("mem:host", Caps::MEM_HOST, nullptr);
  emit_bit("loadmod",  Caps::LOADMOD,  nullptr);

  // Caso especial: MEM_HOST denegado pero con rangos whitelisted.
  // Lo emitimos como `mem:host=0xAAAA-0xBBBB` para que un round-trip
  // parse_caps(caps_to_string(...)) produzca un Caps equivalente.
  if (!caps.wl.mem_ranges.empty() && !(caps.bits & Caps::MEM_HOST)) {
  if (!out.empty()) out += ",";
  out += "mem:host=";
  for (size_t i = 0; i < caps.wl.mem_ranges.size(); ++i) {
  if (i > 0) out += ";";
  char buf[64];
  std::snprintf(buf, sizeof buf, "0x%llX-0x%llX",
  static_cast<unsigned long long>(caps.wl.mem_ranges[i].start),
  static_cast<unsigned long long>(caps.wl.mem_ranges[i].end));
  out += buf;
  }
  }
  return out;
}

// ---------------------------------------------------------------------
//  cap_name : bit individual -> nombre canónico
// ---------------------------------------------------------------------

const char *cap_name(uint32_t single_cap) noexcept {
  // El switch es exhaustivo sobre los bits definidos. Combinaciones
  // de bits (varios encendidos) no se cubren: el caller que necesite
  // formatear un bitmask completo debe usar @c caps_to_string.
  switch (single_cap) {
  case Caps::FS_READ:  return "fs:read";
  case Caps::FS_WRITE: return "fs:write";
  case Caps::NET:  return "net";
  case Caps::FFI_CALL: return "ffi:call";
  case Caps::FFI_OPEN: return "ffi:open";
  case Caps::SPAWN:  return "spawn";
  case Caps::DISTRIB:  return "distrib";
  case Caps::CLASSREG: return "classreg";
  case Caps::MEM_HOST: return "mem:host";
  case Caps::LOADMOD:  return "loadmod";
  default:  return "unknown";
  }
}

// ---------------------------------------------------------------------
//  Métodos de chequeo de whitelist
// ---------------------------------------------------------------------

bool Caps::dll_allowed(const std::string &dll_name) const noexcept {
  // Whitelist vacía implica "sin restricción": cualquier DLL pasa.
  if (wl.dll_whitelist.empty()) return true;

#if defined(_WIN32)
  // En Windows los nombres de DLL son case-insensitive: `KERNEL32.DLL`
  // y `kernel32.dll` apuntan al mismo archivo. Normalizamos a minúscula
  // para que el match no dependa del case que el usuario escribió.
  const std::string needle = to_lower_(dll_name);
  for (const auto &allowed : wl.dll_whitelist) {
  if (to_lower_(allowed) == needle) return true;
  }
#else
  // En POSIX los nombres son case-sensitive (`libfoo.so` != `LIBFOO.SO`).
  // Comparamos sin normalizar.
  for (const auto &allowed : wl.dll_whitelist) {
  if (allowed == dll_name) return true;
  }
#endif
  return false;
}

bool Caps::path_allowed(const std::string &path) const noexcept {
  if (wl.path_whitelist.empty()) return true;

  // Normalización: convertimos backslash a forward slash en ambos
  // operandos para que `C:\foo\bar` matchee con whitelist `"C:/foo"`.
  // Sin esta normalización el comportamiento sería incongruente entre
  // Windows y POSIX para el mismo archivo lógico.
  std::string norm = path;
  for (auto &c : norm) {
  if (c == '\\') c = '/';
  }

  for (const auto &prefix : wl.path_whitelist) {
  std::string np = prefix;
  for (auto &c : np) {
  if (c == '\\') c = '/';
  }
  // Match por prefijo. El caller decide si añade trailing slash
  // a la entry: `"/tmp"` matchea `/tmp/foo.txt` y también
  // `/tmpfoo.txt`. Para evitar el segundo caso, conviene escribir
  // `"/tmp/"`.
  if (norm.size() >= np.size()
  && norm.compare(0, np.size(), np) == 0) {
  return true;
  }
  }
  return false;
}

bool Caps::host_allowed(const std::string &host_port) const noexcept {
  if (wl.host_whitelist.empty()) return true;

  for (const auto &allowed : wl.host_whitelist) {
  // Match exacto: cubre el caso `"host:port" == "host:port"`.
  if (allowed == host_port) return true;

  // Match relajado: si la entry whitelisted es solo `"host"` (sin
  // puerto explícito) y el destino solicitado es `"host:port"`,
  // aceptamos el match. Permite escribir `--vex-caps "net=api.foo"`
  // y que cualquier puerto de api.foo sea válido.
  const size_t colon = host_port.find(':');
  if (colon != std::string::npos
  && allowed == host_port.substr(0, colon)) {
  return true;
  }
  }
  return false;
}

bool Caps::mem_addr_allowed(uint64_t ptr) const noexcept {
  // Sin rangos definidos no hay restricción adicional. Importante:
  // este método solo se invoca cuando la cap MEM_HOST está denegada,
  // así que si llegamos aquí con la lista vacía es porque el caller
  // ya decidió que MEM_HOST está denegada Y no hay rangos
  // alternativos -> rechazo total. El caller que detecta esto debe
  // tratar el "true" como "permitido salvo que el host opte por
  // bloquear todo MEM_HOST sin rangos".
  if (wl.mem_ranges.empty()) return true;

  // Búsqueda lineal. El número de rangos típicamente es pequeño
  // (<10), así que no merece la pena indexar. Si surge un caso con
  // muchos rangos, se podría ordenar y usar binary search.
  for (const auto &r : wl.mem_ranges) {
  if (ptr >= r.start && ptr < r.end) return true;
  }
  return false;
}

}  // namespace loader
