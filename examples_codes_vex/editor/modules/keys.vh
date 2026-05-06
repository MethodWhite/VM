// =============================================================================
// modules/keys.vh - constantes de teclas (codigos sinteticos)
// =============================================================================
// Codigos positivos < 256 son ASCII directos; negativos son sinteticos
// para teclas especiales (flechas, Fn, navegacion).  El backend de input
// se encarga de traducir scancodes BIOS / escapes ANSI a estos codigos.

const i32 KEY_NONE        = -1;
const i32 KEY_CTRL_Q      = 17;
const i32 KEY_CTRL_S      = 19;
const i32 KEY_CTRL_E      = 5;
const i32 KEY_ENTER       = 13;
const i32 KEY_BS          = 8;
const i32 KEY_TAB         = 9;
const i32 KEY_ESC         = 27;
const i32 KEY_UP          = -1000;
const i32 KEY_DOWN        = -1001;
const i32 KEY_LEFT        = -1002;
const i32 KEY_RIGHT       = -1003;
const i32 KEY_HOME        = -1004;
const i32 KEY_END         = -1005;
const i32 KEY_PGUP        = -1006;
const i32 KEY_PGDN        = -1007;
const i32 KEY_DEL         = -1008;
const i32 KEY_F1          = -1100;
const i32 KEY_F2          = -1101;
const i32 KEY_F3          = -1102;
const i32 KEY_F4          = -1103;
const i32 KEY_F5          = -1104;

// Codigos virtuales Win32 (subset).  Util con GetAsyncKeyState para
// detectar teclas modificadoras Ctrl, Shift, Alt fisicamente pulsadas.
const i32 VK_SHIFT  = 16;
const i32 VK_CTRL   = 17;
const i32 VK_ALT    = 18;
const i32 VK_ESCAPE = 27;
