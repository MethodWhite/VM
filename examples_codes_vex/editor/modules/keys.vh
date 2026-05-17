// =============================================================================
// modules/keys.vh - constantes de teclas (codigos sinteticos)
// =============================================================================
// Codigos positivos < 256 son ASCII directos; negativos son sinteticos
// para teclas especiales (flechas, Fn, navegacion).  El backend de input
// se encarga de traducir scancodes BIOS / escapes ANSI a estos codigos.

const i32 KEY_NONE        = -1;
const i32 KEY_CTRL_A      = 1;
const i32 KEY_CTRL_B      = 2;
const i32 KEY_CTRL_C      = 3;
const i32 KEY_CTRL_E      = 5;
const i32 KEY_CTRL_F      = 6;
const i32 KEY_CTRL_G      = 7;
const i32 KEY_CTRL_H      = 8;
const i32 KEY_CTRL_K      = 11;
const i32 KEY_CTRL_L      = 12;
const i32 KEY_CTRL_N      = 14;
const i32 KEY_CTRL_O      = 15;
const i32 KEY_CTRL_P      = 16;
const i32 KEY_BACKSPACE   = 8;
const i32 KEY_CTRL_Q      = 17;
const i32 KEY_CTRL_R      = 18;
const i32 KEY_CTRL_S      = 19;
const i32 KEY_CTRL_U      = 21;
const i32 KEY_CTRL_V      = 22;
const i32 KEY_CTRL_W      = 23;
const i32 KEY_CTRL_X      = 24;
const i32 KEY_CTRL_Y      = 25;
const i32 KEY_CTRL_Z      = 26;
const i32 KEY_ENTER       = 13;
const i32 KEY_BS          = 8;     // alias: tambien Ctrl+H en algunos terms
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
const i32 KEY_F6          = -1105;
const i32 KEY_F7          = -1106;
const i32 KEY_F8          = -1107;
const i32 KEY_F9          = -1108;
const i32 KEY_F10         = -1109;
const i32 KEY_F11         = -1110;
const i32 KEY_F12         = -1111;

// Shift + flechas / nav: codigos sinteticos negativos que el InputBackend
// emite cuando GetAsyncKeyState reporta SHIFT pulsado durante el escape.
// El editor los usa para extender la seleccion en lugar de mover el
// cursor sin seleccion.
const i32 KEY_SHIFT_UP    = -1200;
const i32 KEY_SHIFT_DOWN  = -1201;
const i32 KEY_SHIFT_LEFT  = -1202;
const i32 KEY_SHIFT_RIGHT = -1203;
const i32 KEY_SHIFT_HOME  = -1204;
const i32 KEY_SHIFT_END   = -1205;

// Codigos virtuales Win32 (subset).  Util con GetAsyncKeyState para
// detectar teclas modificadoras Ctrl, Shift, Alt fisicamente pulsadas.
const i32 VK_SHIFT  = 16;
const i32 VK_CTRL   = 17;
const i32 VK_ALT    = 18;
const i32 VK_ESCAPE = 27;
