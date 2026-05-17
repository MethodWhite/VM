// @vex-snippet: vex_string
// @vex-requires: vex_macros
// @vex-includes: stdint.h, stdlib.h, string.h, stdio.h
// @vex-freestanding-skip: yes

/* =========================================================================
 * Mini runtime VexString (mode=managed).
 *
 * Layout: header 16 bytes + buffer flexible.  Operaciones de strings
 * (concat/equals/length/raw/bytes) usan estos helpers.  El usuario debe
 * llamar @c vex_str_free(s) cuando termine con un VexString* heap-alloc'd
 * (los marcados @c is_owned=1).  Al exit del programa, los unfreed se
 * reportan a @c stderr.
 *
 * Para programacion freestanding/bootloader: este snippet se OMITE.  Si
 * el programa usa strings y se compila con @c --port-freestanding, error.
 * El programador debe implementar sus propios helpers @c vex_str_make /
 * @c vex_str_concat / etc. con su asignador.
 * =========================================================================
 */
typedef struct VexString {
    struct VexString *__prev;
    struct VexString *__next;
    uint32_t byte_len;
    uint32_t code_points;
    uint8_t  is_owned;       /* 1=heap (free at vex_str_free), 0=literal */
    uint8_t  __pad[3];
    char    *data;
} VexString;

/* Self-ref + TLS no compatible en todos los toolchains; lazy-init. */
static VEX_TLS VexString  vex_str_head_;
static VEX_TLS int        vex_str_head_init_ = 0;
static VEX_TLS int        vex_str_live_count_ = 0;
static int                vex_str_atexit_registered_ = 0;

static void vex_str_head_ensure_(void) {
    if (!vex_str_head_init_) {
        vex_str_head_.__prev = &vex_str_head_;
        vex_str_head_.__next = &vex_str_head_;
        vex_str_head_init_ = 1;
    }
}

static void vex_str_track_(VexString *s) {
    vex_str_head_ensure_();
    s->__next = vex_str_head_.__next;
    s->__prev = &vex_str_head_;
    vex_str_head_.__next->__prev = s;
    vex_str_head_.__next = s;
    vex_str_live_count_++;
}

static void vex_str_untrack_(VexString *s) {
    s->__prev->__next = s->__next;
    s->__next->__prev = s->__prev;
    s->__prev = s->__next = 0;
    vex_str_live_count_--;
}

static void vex_str_atexit_warn_(void) {
    if (vex_str_live_count_ > 0) {
        fprintf(stderr,
          "[vex] warning: %d VexString blocks leaked at thread exit\n",
          vex_str_live_count_);
    }
}

static void vex_str_register_atexit_(void) {
    if (!vex_str_atexit_registered_) {
        vex_str_atexit_registered_ = 1;
        atexit(vex_str_atexit_warn_);
    }
}

/* Cuenta code points UTF-8 en un buffer. */
static uint32_t vex_str_count_cp_(const char *buf, uint32_t byte_len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < byte_len; ) {
        unsigned char c = (unsigned char)buf[i];
        uint32_t adv = 1;
        if      ((c & 0x80) == 0x00) adv = 1;
        else if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        i += adv;
        n++;
    }
    return n;
}

static VEX_UNUSED VexString *vex_str_from_lit(const char *lit, uint32_t byte_len) {
    VexString *s = (VexString*)malloc(sizeof(VexString));
    if (!s) return 0;
    s->byte_len = byte_len;
    s->code_points = vex_str_count_cp_(lit, byte_len);
    s->is_owned = 0;
    s->data = (char*)lit;
    vex_str_track_(s);
    vex_str_register_atexit_();
    return s;
}

static VEX_UNUSED VexString *vex_str_make(const char *buf, uint32_t byte_len) {
    VexString *s = (VexString*)malloc(sizeof(VexString));
    if (!s) return 0;
    s->byte_len = byte_len;
    s->code_points = vex_str_count_cp_(buf, byte_len);
    s->is_owned = 1;
    s->data = (char*)malloc(byte_len + 1);
    if (!s->data) { free(s); return 0; }
    memcpy(s->data, buf, byte_len);
    s->data[byte_len] = 0;
    vex_str_track_(s);
    vex_str_register_atexit_();
    return s;
}

static VEX_UNUSED VexString *vex_str_concat(VexString *a, VexString *b) {
    if (!a || !b) return 0;
    uint32_t na = a->byte_len, nb = b->byte_len;
    char *buf = (char*)malloc(na + nb + 1);
    if (!buf) return 0;
    memcpy(buf, a->data, na);
    memcpy(buf + na, b->data, nb);
    buf[na + nb] = 0;
    VexString *r = (VexString*)malloc(sizeof(VexString));
    if (!r) { free(buf); return 0; }
    r->byte_len = na + nb;
    r->code_points = a->code_points + b->code_points;
    r->is_owned = 1;
    r->data = buf;
    vex_str_track_(r);
    return r;
}

static VEX_UNUSED int vex_str_eq(VexString *a, VexString *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->byte_len != b->byte_len) return 0;
    return memcmp(a->data, b->data, a->byte_len) == 0;
}
static VEX_UNUSED uint32_t vex_str_len(VexString *s)      { return s ? s->code_points : 0; }
static VEX_UNUSED uint32_t vex_str_byte_len(VexString *s) { return s ? s->byte_len    : 0; }
static VEX_UNUSED const char *vex_str_raw(VexString *s)   { return s ? s->data : (const char*)""; }

static VEX_UNUSED void vex_str_free(VexString *s) {
    if (!s) return;
    vex_str_untrack_(s);
    if (s->is_owned) free(s->data);
    free(s);
}
