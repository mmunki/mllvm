/*
 * mllvm -- a clean-room implementation of the LLVM IR core.
 *
 * The tool parses a self-contained subset of textual LLVM IR, verifies it,
 * optionally constant folds it and executes it in a small interpreter.  It is
 * not a fork of LLVM and it does not link against LLVM; the point is to have
 * a compact, dependency free implementation of the IR itself.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define MEM_SIZE (8u * 1024u * 1024u)
#define MEM_BASE 0x1000u
#define MAX_STEPS 200000000llu
#define MAX_DEPTH 10000

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...)
{
    va_list ap;
    fputs("mllvm: error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ----------------------------------------------------------------- types */

typedef enum {
    TY_VOID = 0,
    TY_I1,
    TY_I8,
    TY_I16,
    TY_I32,
    TY_I64,
    TY_PTR
} Type;

static int type_bits(Type t)
{
    switch (t) {
    case TY_I1:  return 1;
    case TY_I8:  return 8;
    case TY_I16: return 16;
    case TY_I32: return 32;
    case TY_I64: return 64;
    case TY_PTR: return 64;
    default:     return 0;
    }
}

static int type_size(Type t)
{
    int b = type_bits(t);
    return b ? (b + 7) / 8 : 0;
}

static uint64_t type_mask(Type t)
{
    int b = type_bits(t);
    if (b <= 0)
        return 0;
    if (b >= 64)
        return UINT64_MAX;
    return (UINT64_C(1) << b) - 1;
}

static uint64_t value_mask(Type t, uint64_t v)
{
    return v & type_mask(t);
}

static int64_t value_signed(Type t, uint64_t v)
{
    int b = type_bits(t);
    v = value_mask(t, v);
    if (b <= 0 || b >= 64)
        return (int64_t)v;
    if (v & (UINT64_C(1) << (b - 1)))
        return (int64_t)(v | ~type_mask(t));
    return (int64_t)v;
}

static const char *type_name(Type t)
{
    switch (t) {
    case TY_VOID: return "void";
    case TY_I1:   return "i1";
    case TY_I8:   return "i8";
    case TY_I16:  return "i16";
    case TY_I32:  return "i32";
    case TY_I64:  return "i64";
    case TY_PTR:  return "ptr";
    }
    return "?";
}

static int is_type_token(const char *s)
{
    if (!s || !*s)
        return 0;
    if (!strcmp(s, "void") || !strcmp(s, "ptr") || !strcmp(s, "label"))
        return 1;
    if (s[0] != 'i' || !isdigit((unsigned char)s[1]))
        return 0;
    for (const char *p = s + 1; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    return 1;
}

static Type type_from_token(const char *s)
{
    if (!strcmp(s, "void")) return TY_VOID;
    if (!strcmp(s, "ptr"))  return TY_PTR;
    if (!strcmp(s, "i1"))   return TY_I1;
    if (!strcmp(s, "i8"))   return TY_I8;
    if (!strcmp(s, "i16"))  return TY_I16;
    if (!strcmp(s, "i32"))  return TY_I32;
    if (!strcmp(s, "i64"))  return TY_I64;
    if (!strcmp(s, "label")) return TY_PTR;
    die("unsupported type '%s'", s);
    return TY_VOID;
}

/* --------------------------------------------------------------- tokens */

typedef struct {
    char **v;
    int n;
    int cap;
} Toks;

static void toks_push(Toks *t, const char *s, size_t n)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->v = xrealloc(t->v, (size_t)t->cap * sizeof(*t->v));
    }
    t->v[t->n++] = xstrndup(s, n);
}

static int is_punct(char c)
{
    return c == ',' || c == '(' || c == ')' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '=' || c == '*' || c == ':' ||
           c == '<' || c == '>';
}

static Toks tokenize(const char *line)
{
    Toks t = {0};
    const char *p = line;
    while (*p) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        if (is_punct(*p)) {
            toks_push(&t, p, 1);
            p++;
            continue;
        }
        const char *s = p;
        while (*p && !isspace((unsigned char)*p) && !is_punct(*p))
            p++;
        toks_push(&t, s, (size_t)(p - s));
    }
    return t;
}

static const char *tok(const Toks *t, int i)
{
    return (i >= 0 && i < t->n) ? t->v[i] : "";
}

static void expect(const Toks *t, int *i, const char *what)
{
    if (strcmp(tok(t, *i), what))
        die("expected '%s', got '%s'", what, tok(t, *i));
    (*i)++;
}

/* -------------------------------------------------------------- operands */

typedef struct {
    Type ty;
    int is_name;
    char *name;
    uint64_t cval;
} Operand;

static Operand operand_const(Type ty, uint64_t v)
{
    Operand o;
    memset(&o, 0, sizeof(o));
    o.ty = ty;
    o.cval = value_mask(ty, v);
    return o;
}

static Operand operand_name(Type ty, const char *name)
{
    Operand o;
    memset(&o, 0, sizeof(o));
    o.ty = ty;
    o.is_name = 1;
    o.name = xstrdup(name);
    return o;
}

typedef struct {
    Type ty;
    uint64_t u;
} Value;

static Value value_of(Type ty, uint64_t u)
{
    Value v;
    v.ty = ty;
    v.u = value_mask(ty, u);
    return v;
}

/* ----------------------------------------------------------- instructions */

typedef enum {
    OP_RET,
    OP_BR,
    OP_CBR,
    OP_BIN,
    OP_ICMP,
    OP_SELECT,
    OP_CAST,
    OP_PHI,
    OP_CALL,
    OP_ALLOCA,
    OP_LOAD,
    OP_STORE,
    OP_UNREACHABLE
} OpKind;

enum {
    BIN_ADD, BIN_SUB, BIN_MUL, BIN_SDIV, BIN_UDIV, BIN_SREM, BIN_UREM,
    BIN_AND, BIN_OR, BIN_XOR, BIN_SHL, BIN_LSHR, BIN_ASHR
};

enum {
    IC_EQ, IC_NE, IC_UGT, IC_UGE, IC_ULT, IC_ULE,
    IC_SGT, IC_SGE, IC_SLT, IC_SLE
};

enum {
    CAST_ZEXT, CAST_SEXT, CAST_TRUNC, CAST_BITCAST,
    CAST_PTRTOINT, CAST_INTTOPTR
};

typedef struct Instr {
    OpKind kind;
    char *result;
    Type ty;
    int sub;
    Operand a, b, c;
    int nphi;
    Operand *phi_val;
    char **phi_lab;
    char *callee;
    int nargs;
    Operand *args;
    char *target;
    char *target_true;
    char *target_false;
    int alloca_count;
    int folded;
    Value fold;
} Instr;

typedef struct {
    char *label;
    Instr **items;
    int n;
    int cap;
} Block;

typedef struct {
    char *name;
    Type ret;
    int nparams;
    Type *ptypes;
    char **pnames;
    Block *blocks;
    int nblocks;
    int cap;
    int is_declaration;
} Func;

typedef struct {
    char *name;
    Type ty;
    uint64_t init;
    int external;
    uint64_t addr;
} Global;

typedef struct {
    Func **funcs;
    int nfuncs;
    int capfuncs;
    Global *globals;
    int nglobals;
    int capglobals;
} Module;

static void module_add_func(Module *m, Func *f)
{
    if (m->nfuncs == m->capfuncs) {
        m->capfuncs = m->capfuncs ? m->capfuncs * 2 : 16;
        m->funcs = xrealloc(m->funcs,
                            (size_t)m->capfuncs * sizeof(*m->funcs));
    }
    m->funcs[m->nfuncs++] = f;
}

static Func *module_find_func(Module *m, const char *name)
{
    for (int i = 0; i < m->nfuncs; i++)
        if (!strcmp(m->funcs[i]->name, name))
            return m->funcs[i];
    return NULL;
}

static void module_add_global(Module *m, Global g)
{
    if (m->nglobals == m->capglobals) {
        m->capglobals = m->capglobals ? m->capglobals * 2 : 16;
        m->globals = xrealloc(m->globals,
                              (size_t)m->capglobals * sizeof(*m->globals));
    }
    m->globals[m->nglobals++] = g;
}

static Global *module_find_global(Module *m, const char *name)
{
    for (int i = 0; i < m->nglobals; i++)
        if (!strcmp(m->globals[i].name, name))
            return &m->globals[i];
    return NULL;
}

static Func *func_defined(Module *m, const char *name);

static Block *func_find_block(Func *f, const char *label)
{
    for (int i = 0; i < f->nblocks; i++)
        if (!strcmp(f->blocks[i].label, label))
            return &f->blocks[i];
    return NULL;
}

static void block_add(Block *b, Instr *in)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->items = xrealloc(b->items, (size_t)b->cap * sizeof(*b->items));
    }
    b->items[b->n++] = in;
}

/* ---------------------------------------------------------------- parser */

static long line_no;

static Type parse_type(const Toks *t, int *i)
{
    const char *s = tok(t, *i);
    if (!is_type_token(s))
        die("line %ld: expected a type, got '%s'", line_no, s);
    Type ty = type_from_token(s);
    (*i)++;
    while (!strcmp(tok(t, *i), "*")) {
        ty = TY_PTR;
        (*i)++;
    }
    return ty;
}

static Operand parse_value(const Toks *t, int *i, Type ty)
{
    const char *s = tok(t, *i);
    if (!*s)
        die("line %ld: expected a value", line_no);
    (*i)++;
    if (s[0] == '%' || s[0] == '@')
        return operand_name(ty, s);
    if (!strcmp(s, "true"))
        return operand_const(ty, 1);
    if (!strcmp(s, "false"))
        return operand_const(ty, 0);
    if (!strcmp(s, "null") || !strcmp(s, "undef") || !strcmp(s, "poison"))
        return operand_const(ty, 0);
    errno = 0;
    char *end = NULL;
    uint64_t v = strtoull(s, &end, 0);
    if (errno || !end || *end)
        die("line %ld: expected a value, got '%s'", line_no, s);
    return operand_const(ty, v);
}

static Operand parse_typed_operand(const Toks *t, int *i)
{
    Type ty = parse_type(t, i);
    return parse_value(t, i, ty);
}

static Operand parse_operand_typed_or_plain(const Toks *t, int *i, Type def)
{
    if (is_type_token(tok(t, *i))) {
        Type ty = parse_type(t, i);
        return parse_value(t, i, ty);
    }
    return parse_value(t, i, def);
}

static char *parse_label_ref(const Toks *t, int *i)
{
    expect(t, i, "label");
    const char *s = tok(t, *i);
    if (!*s)
        die("line %ld: expected a label", line_no);
    (*i)++;
    if (s[0] == '%')
        s++;
    return xstrdup(s);
}

static char *parse_label_name(const Toks *t, int *i)
{
    const char *s = tok(t, *i);
    if (!*s)
        die("line %ld: expected a label", line_no);
    (*i)++;
    if (s[0] == '%')
        s++;
    return xstrdup(s);
}

static Instr *new_instr(OpKind kind)
{
    Instr *in = xmalloc(sizeof(*in));
    memset(in, 0, sizeof(*in));
    in->kind = kind;
    return in;
}

static Instr *parse_bin(const Toks *t, int *i, char *result, int sub)
{
    Instr *in = new_instr(OP_BIN);
    in->result = result;
    in->sub = sub;
    in->a = parse_typed_operand(t, i);
    in->ty = in->a.ty;
    expect(t, i, ",");
    in->b = parse_operand_typed_or_plain(t, i, in->a.ty);
    return in;
}

static Instr *parse_icmp(const Toks *t, int *i, char *result)
{
    static const struct { const char *s; int c; } conds[] = {
        {"eq", IC_EQ}, {"ne", IC_NE},
        {"ugt", IC_UGT}, {"uge", IC_UGE}, {"ult", IC_ULT}, {"ule", IC_ULE},
        {"sgt", IC_SGT}, {"sge", IC_SGE}, {"slt", IC_SLT}, {"sle", IC_SLE}
    };
    const char *cond = tok(t, *i);
    int c = -1;
    for (size_t k = 0; k < sizeof(conds) / sizeof(conds[0]); k++)
        if (!strcmp(cond, conds[k].s))
            c = conds[k].c;
    if (c < 0)
        die("line %ld: unsupported icmp condition '%s'", line_no, cond);
    (*i)++;
    Instr *in = new_instr(OP_ICMP);
    in->result = result;
    in->sub = c;
    in->a = parse_typed_operand(t, i);
    expect(t, i, ",");
    in->b = parse_operand_typed_or_plain(t, i, in->a.ty);
    in->ty = TY_I1;
    return in;
}

static Instr *parse_cast(const Toks *t, int *i, char *result, int sub)
{
    Instr *in = new_instr(OP_CAST);
    in->result = result;
    in->sub = sub;
    in->a = parse_typed_operand(t, i);
    expect(t, i, "to");
    in->ty = parse_type(t, i);
    return in;
}

static Instr *parse_phi(const Toks *t, int *i, char *result)
{
    Instr *in = new_instr(OP_PHI);
    in->result = result;
    in->ty = parse_type(t, i);
    while (!strcmp(tok(t, *i), "[")) {
        (*i)++;
        Operand v = parse_value(t, i, in->ty);
        expect(t, i, ",");
        char *lab = parse_label_name(t, i);
        expect(t, i, "]");
        in->phi_val = xrealloc(in->phi_val,
                               (size_t)(in->nphi + 1) * sizeof(*in->phi_val));
        in->phi_lab = xrealloc(in->phi_lab,
                               (size_t)(in->nphi + 1) * sizeof(*in->phi_lab));
        in->phi_val[in->nphi] = v;
        in->phi_lab[in->nphi] = lab;
        in->nphi++;
        if (!strcmp(tok(t, *i), ","))
            (*i)++;
        else
            break;
    }
    if (in->nphi == 0)
        die("line %ld: phi without incoming values", line_no);
    return in;
}

static Instr *parse_call(const Toks *t, int *i, char *result)
{
    Instr *in = new_instr(OP_CALL);
    in->result = result;
    in->ty = parse_type(t, i);
    const char *callee = tok(t, *i);
    if (callee[0] != '@')
        die("line %ld: only direct calls are supported, got '%s'",
            line_no, callee);
    in->callee = xstrdup(callee);
    (*i)++;
    expect(t, i, "(");
    while (strcmp(tok(t, *i), ")")) {
        if (!strcmp(tok(t, *i), "...")) {
            (*i)++;
            break;
        }
        Operand a = parse_typed_operand(t, i);
        in->args = xrealloc(in->args,
                            (size_t)(in->nargs + 1) * sizeof(*in->args));
        in->args[in->nargs++] = a;
        if (!strcmp(tok(t, *i), ","))
            (*i)++;
        else
            break;
    }
    expect(t, i, ")");
    return in;
}

static Instr *parse_alloca(const Toks *t, int *i, char *result)
{
    Instr *in = new_instr(OP_ALLOCA);
    in->result = result;
    in->ty = parse_type(t, i);
    in->alloca_count = 1;
    if (!strcmp(tok(t, *i), ",")) {
        (*i)++;
        if (!strcmp(tok(t, *i), "align")) {
            while (*i < t->n)
                (*i)++;
        } else if (is_type_token(tok(t, *i))) {
            Operand n = parse_typed_operand(t, i);
            if (n.is_name)
                die("line %ld: dynamic alloca is not supported", line_no);
            if (n.cval > 1000000)
                die("line %ld: alloca is too large", line_no);
            in->alloca_count = (int)n.cval;
        } else {
            while (*i < t->n)
                (*i)++;
        }
    }
    return in;
}

static Instr *parse_load(const Toks *t, int *i, char *result)
{
    Instr *in = new_instr(OP_LOAD);
    in->result = result;
    in->ty = parse_type(t, i);
    expect(t, i, ",");
    in->b = parse_typed_operand(t, i);
    return in;
}

static Instr *parse_store(const Toks *t, int *i)
{
    Instr *in = new_instr(OP_STORE);
    in->a = parse_typed_operand(t, i);
    expect(t, i, ",");
    in->b = parse_typed_operand(t, i);
    return in;
}

static Instr *parse_instr(const Toks *t)
{
    int i = 0;
    char *result = NULL;
    if (t->n >= 2 && t->v[0][0] == '%' && !strcmp(t->v[1], "=")) {
        result = xstrdup(t->v[0]);
        i = 2;
    }
    if (i >= t->n)
        die("line %ld: empty instruction", line_no);
    const char *op = tok(t, i);
    i++;

    if (!strcmp(op, "ret")) {
        Instr *in = new_instr(OP_RET);
        in->result = result;
        if (!strcmp(tok(t, i), "void")) {
            i++;
            in->a.ty = TY_VOID;
        } else {
            in->a = parse_typed_operand(t, &i);
        }
        return in;
    }
    if (!strcmp(op, "br")) {
        Instr *in = new_instr(OP_BR);
        in->result = result;
        if (!strcmp(tok(t, i), "label")) {
            in->target = parse_label_ref(t, &i);
        } else {
            in->kind = OP_CBR;
            in->a = parse_typed_operand(t, &i);
            expect(t, &i, ",");
            in->target_true = parse_label_ref(t, &i);
            expect(t, &i, ",");
            in->target_false = parse_label_ref(t, &i);
        }
        return in;
    }
    if (!strcmp(op, "add"))  return parse_bin(t, &i, result, BIN_ADD);
    if (!strcmp(op, "sub"))  return parse_bin(t, &i, result, BIN_SUB);
    if (!strcmp(op, "mul"))  return parse_bin(t, &i, result, BIN_MUL);
    if (!strcmp(op, "sdiv")) return parse_bin(t, &i, result, BIN_SDIV);
    if (!strcmp(op, "udiv")) return parse_bin(t, &i, result, BIN_UDIV);
    if (!strcmp(op, "srem")) return parse_bin(t, &i, result, BIN_SREM);
    if (!strcmp(op, "urem")) return parse_bin(t, &i, result, BIN_UREM);
    if (!strcmp(op, "and"))  return parse_bin(t, &i, result, BIN_AND);
    if (!strcmp(op, "or"))   return parse_bin(t, &i, result, BIN_OR);
    if (!strcmp(op, "xor"))  return parse_bin(t, &i, result, BIN_XOR);
    if (!strcmp(op, "shl"))  return parse_bin(t, &i, result, BIN_SHL);
    if (!strcmp(op, "lshr")) return parse_bin(t, &i, result, BIN_LSHR);
    if (!strcmp(op, "ashr")) return parse_bin(t, &i, result, BIN_ASHR);
    if (!strcmp(op, "icmp")) return parse_icmp(t, &i, result);
    if (!strcmp(op, "select")) {
        Instr *in = new_instr(OP_SELECT);
        in->result = result;
        in->a = parse_typed_operand(t, &i);
        expect(t, &i, ",");
        in->b = parse_typed_operand(t, &i);
        expect(t, &i, ",");
        in->c = parse_typed_operand(t, &i);
        in->ty = in->b.ty;
        return in;
    }
    if (!strcmp(op, "zext"))    return parse_cast(t, &i, result, CAST_ZEXT);
    if (!strcmp(op, "sext"))    return parse_cast(t, &i, result, CAST_SEXT);
    if (!strcmp(op, "trunc"))   return parse_cast(t, &i, result, CAST_TRUNC);
    if (!strcmp(op, "bitcast")) return parse_cast(t, &i, result, CAST_BITCAST);
    if (!strcmp(op, "ptrtoint")) return parse_cast(t, &i, result, CAST_PTRTOINT);
    if (!strcmp(op, "inttoptr")) return parse_cast(t, &i, result, CAST_INTTOPTR);
    if (!strcmp(op, "phi"))     return parse_phi(t, &i, result);
    if (!strcmp(op, "call") || !strcmp(op, "tail") ||
        !strcmp(op, "musttail") || !strcmp(op, "notail")) {
        if (strcmp(op, "call"))
            op = tok(t, i), i++;
        return parse_call(t, &i, result);
    }
    if (!strcmp(op, "alloca"))  return parse_alloca(t, &i, result);
    if (!strcmp(op, "load"))    return parse_load(t, &i, result);
    if (!strcmp(op, "store"))   return parse_store(t, &i);
    if (!strcmp(op, "unreachable")) {
        Instr *in = new_instr(OP_UNREACHABLE);
        in->result = result;
        return in;
    }
    die("line %ld: unsupported instruction '%s'", line_no, op);
    return NULL;
}

static Type type_before_at(const Toks *t, int at)
{
    int j = at - 1;
    if (j >= 0 && !strcmp(t->v[j], "*"))
        return TY_PTR;
    if (j < 0)
        die("line %ld: function without a return type", line_no);
    return type_from_token(t->v[j]);
}

static Func *parse_signature(const Toks *t)
{
    Func *f = xmalloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    int at = -1;
    for (int i = 0; i < t->n; i++)
        if (t->v[i][0] == '@') {
            at = i;
            break;
        }
    if (at < 0)
        die("line %ld: expected a function name", line_no);
    f->name = xstrdup(t->v[at]);
    f->ret = type_before_at(t, at);

    int open = -1;
    for (int i = at + 1; i < t->n; i++)
        if (!strcmp(t->v[i], "(")) {
            open = i;
            break;
        }
    if (open < 0) {
        f->is_declaration = 1;
        return f;
    }

    int depth = 0, close = -1;
    for (int i = open; i < t->n; i++) {
        if (!strcmp(t->v[i], "("))
            depth++;
        else if (!strcmp(t->v[i], ")")) {
            depth--;
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }
    if (close < 0)
        die("line %ld: unterminated parameter list", line_no);

    int i = open + 1;
    while (i < close) {
        if (!strcmp(t->v[i], ",")) {
            i++;
            continue;
        }
        if (!strcmp(t->v[i], "...")) {
            i++;
            continue;
        }
        Type ty = parse_type(t, &i);
        char *pname = NULL;
        while (i < close && strcmp(t->v[i], ",")) {
            if (t->v[i][0] == '%' && !pname)
                pname = xstrdup(t->v[i]);
            i++;
        }
        if (!pname) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%%arg%d", f->nparams);
            pname = xstrdup(buf);
        }
        f->ptypes = xrealloc(f->ptypes,
                             (size_t)(f->nparams + 1) * sizeof(*f->ptypes));
        f->pnames = xrealloc(f->pnames,
                             (size_t)(f->nparams + 1) * sizeof(*f->pnames));
        f->ptypes[f->nparams] = ty;
        f->pnames[f->nparams] = pname;
        f->nparams++;
        if (!strcmp(tok(t, i), ","))
            i++;
    }
    return f;
}

static void strip_comment(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == ';') {
            *p = '\0';
            return;
        }
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static void parse_function_body(FILE *fp, Func *f, char **line, size_t *cap)
{
    Block *cur = NULL;
    ssize_t len;
    while ((len = getline(line, cap, fp)) != -1) {
        line_no++;
        strip_comment(*line);
        char *s = trim(*line);
        if (!*s)
            continue;
        if (!strcmp(s, "}"))
            return;

        Toks t = tokenize(s);
        if (t.n == 0)
            continue;

        if (t.n == 2 && !strcmp(t.v[1], ":")) {
            if (f->nblocks == f->cap) {
                f->cap = f->cap ? f->cap * 2 : 8;
                f->blocks = xrealloc(f->blocks,
                                     (size_t)f->cap * sizeof(*f->blocks));
            }
            cur = &f->blocks[f->nblocks++];
            memset(cur, 0, sizeof(*cur));
            cur->label = xstrdup(t.v[0]);
            continue;
        }
        if (s[strlen(s) - 1] == ':' && s[0] != '%') {
            if (f->nblocks == f->cap) {
                f->cap = f->cap ? f->cap * 2 : 8;
                f->blocks = xrealloc(f->blocks,
                                     (size_t)f->cap * sizeof(*f->blocks));
            }
            cur = &f->blocks[f->nblocks++];
            memset(cur, 0, sizeof(*cur));
            s[strlen(s) - 1] = '\0';
            cur->label = xstrdup(trim(s));
            continue;
        }
        if (!cur) {
            f->blocks = xrealloc(f->blocks,
                                 (size_t)(f->nblocks + 1) * sizeof(*f->blocks));
            f->cap = f->nblocks + 1;
            cur = &f->blocks[f->nblocks++];
            memset(cur, 0, sizeof(*cur));
            cur->label = xstrdup("entry");
        }
        block_add(cur, parse_instr(&t));
    }
    die("line %ld: unterminated function %s", line_no, f->name);
}

static Module parse_module(FILE *fp)
{
    Module m;
    memset(&m, 0, sizeof(m));
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    while ((len = getline(&line, &cap, fp)) != -1) {
        line_no++;
        strip_comment(line);
        char *s = trim(line);
        if (!*s)
            continue;
        Toks t = tokenize(s);
        if (t.n == 0)
            continue;

        if (!strcmp(t.v[0], "define")) {
            Func *f = parse_signature(&t);
            if (func_defined(&m, f->name))
                die("line %ld: duplicate function %s", line_no, f->name);
            parse_function_body(fp, f, &line, &cap);
            module_add_func(&m, f);
            continue;
        }
        if (!strcmp(t.v[0], "declare")) {
            Func *f = parse_signature(&t);
            f->is_declaration = 1;
            if (!module_find_func(&m, f->name))
                module_add_func(&m, f);
            continue;
        }
        if (t.v[0][0] == '@' && t.n >= 3 && !strcmp(t.v[1], "=")) {
            Global g;
            memset(&g, 0, sizeof(g));
            g.name = xstrdup(t.v[0]);
            int i = 2;
            if (!strcmp(tok(&t, i), "external")) {
                g.external = 1;
                module_add_global(&m, g);
                continue;
            }
            while (i < t.n && !is_type_token(tok(&t, i)))
                i++;
            if (i >= t.n)
                die("line %ld: global %s has no type", line_no, g.name);
            g.ty = parse_type(&t, &i);
            if (i < t.n) {
                Operand init = parse_value(&t, &i, g.ty);
                if (init.is_name)
                    die("line %ld: global initialiser must be a constant",
                        line_no);
                g.init = init.cval;
            }
            module_add_global(&m, g);
            continue;
        }
        /* target, attributes, module asm and metadata are ignored. */
    }
    free(line);
    return m;
}

/* -------------------------------------------------------------- verifier */

typedef struct {
    char **v;
    int n;
    int cap;
} Names;

static void names_add(Names *n, const char *s)
{
    for (int i = 0; i < n->n; i++)
        if (!strcmp(n->v[i], s))
            die("duplicate definition of %s", s);
    if (n->n == n->cap) {
        n->cap = n->cap ? n->cap * 2 : 32;
        n->v = xrealloc(n->v, (size_t)n->cap * sizeof(*n->v));
    }
    n->v[n->n++] = xstrdup(s);
}

static int names_has(const Names *n, const char *s)
{
    for (int i = 0; i < n->n; i++)
        if (!strcmp(n->v[i], s))
            return 1;
    return 0;
}

static void check_operand(Func *f, const Names *defined, const Operand *o)
{
    if (o->is_name && o->name[0] == '%' && !names_has(defined, o->name))
        die("function %s uses undefined value %s", f->name, o->name);
}

static void check_label(Func *f, const char *label)
{
    if (!func_find_block(f, label))
        die("function %s branches to unknown block %s", f->name, label);
}

static void verify_function(Func *f)
{
    if (f->is_declaration)
        return;
    if (f->nblocks == 0)
        die("function %s has no body", f->name);

    Names defined = {0};
    for (int p = 0; p < f->nparams; p++)
        names_add(&defined, f->pnames[p]);

    for (int b = 0; b < f->nblocks; b++) {
        Block *blk = &f->blocks[b];
        for (int k = 0; k < blk->n; k++) {
            Instr *in = blk->items[k];
            if (in->result)
                names_add(&defined, in->result);
        }
    }

    for (int b = 0; b < f->nblocks; b++) {
        Block *blk = &f->blocks[b];
        if (blk->n == 0)
            die("function %s has an empty block %s", f->name, blk->label);
        for (int k = 0; k < blk->n; k++) {
            Instr *in = blk->items[k];
            switch (in->kind) {
            case OP_BIN:
            case OP_ICMP:
            case OP_CAST:
            case OP_SELECT:
            case OP_LOAD:
            case OP_ALLOCA:
            case OP_CALL:
                /* checked below */
                break;
            default:
                break;
            }
            if (in->kind == OP_BIN || in->kind == OP_ICMP ||
                in->kind == OP_CAST || in->kind == OP_SELECT)
                check_operand(f, &defined, &in->a);
            if (in->kind == OP_BIN || in->kind == OP_ICMP ||
                in->kind == OP_SELECT)
                check_operand(f, &defined, &in->b);
            if (in->kind == OP_SELECT)
                check_operand(f, &defined, &in->c);
            if (in->kind == OP_RET)
                check_operand(f, &defined, &in->a);
            if (in->kind == OP_CBR)
                check_operand(f, &defined, &in->a);
            if (in->kind == OP_LOAD || in->kind == OP_STORE)
                check_operand(f, &defined, &in->b);
            if (in->kind == OP_STORE)
                check_operand(f, &defined, &in->a);
            if (in->kind == OP_PHI) {
                for (int j = 0; j < in->nphi; j++) {
                    check_operand(f, &defined, &in->phi_val[j]);
                    check_label(f, in->phi_lab[j]);
                }
            }
            if (in->kind == OP_CALL)
                for (int j = 0; j < in->nargs; j++)
                    check_operand(f, &defined, &in->args[j]);
        }
        Instr *last = blk->items[blk->n - 1];
        if (last->kind != OP_RET && last->kind != OP_BR &&
            last->kind != OP_CBR && last->kind != OP_UNREACHABLE)
            die("function %s block %s does not end with a terminator",
                f->name, blk->label);
        if (last->kind == OP_BR)
            check_label(f, last->target);
        if (last->kind == OP_CBR) {
            check_label(f, last->target_true);
            check_label(f, last->target_false);
        }
        if (last->kind == OP_RET && f->ret == TY_VOID &&
            last->a.ty != TY_VOID)
            die("function %s returns a value but is declared void", f->name);
    }
}

static void verify_module(Module *m)
{
    for (int i = 0; i < m->nfuncs; i++)
        verify_function(m->funcs[i]);
    if (!module_find_func(m, "@main"))
        die("no @main function");
}

static Func *func_defined(Module *m, const char *name)
{
    for (int i = 0; i < m->nfuncs; i++)
        if (!strcmp(m->funcs[i]->name, name) && !m->funcs[i]->is_declaration)
            return m->funcs[i];
    return NULL;
}

/* ---------------------------------------------------------------- runtime */

typedef struct {
    char *name;
    Value v;
} Binding;

typedef struct {
    Binding *b;
    int n;
    int cap;
} Env;

static void env_set(Env *e, const char *name, Value v)
{
    for (int i = 0; i < e->n; i++)
        if (!strcmp(e->b[i].name, name)) {
            e->b[i].v = v;
            return;
        }
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 16;
        e->b = xrealloc(e->b, (size_t)e->cap * sizeof(*e->b));
    }
    e->b[e->n].name = xstrdup(name);
    e->b[e->n].v = v;
    e->n++;
}

static int env_get(Env *e, const char *name, Value *out)
{
    for (int i = 0; i < e->n; i++)
        if (!strcmp(e->b[i].name, name)) {
            *out = e->b[i].v;
            return 1;
        }
    return 0;
}

typedef struct {
    Module *m;
    uint8_t *mem;
    uint64_t mem_size;
    uint64_t mem_top;
    uint64_t steps;
    int exiting;
    int exit_code;
    int trace;
} State;

static Value eval_operand(State *st, Env *env, const Operand *o)
{
    if (!o->is_name)
        return value_of(o->ty, o->cval);
    if (o->name[0] == '@') {
        Global *g = module_find_global(st->m, o->name);
        if (!g)
            die("undefined global %s", o->name);
        return value_of(TY_PTR, g->addr);
    }
    Value v;
    if (!env_get(env, o->name, &v))
        die("undefined value %s", o->name);
    return v;
}

static void check_mem(State *st, uint64_t addr, int size)
{
    if (size <= 0)
        return;
    if (addr < MEM_BASE || addr + (uint64_t)size > st->mem_top ||
        addr + (uint64_t)size > st->mem_size)
        die("memory access out of bounds at 0x%" PRIx64 " (%d bytes)",
            addr, size);
}

static uint64_t mem_read(State *st, uint64_t addr, Type ty)
{
    int size = type_size(ty);
    check_mem(st, addr, size);
    uint64_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint64_t)st->mem[addr + (uint64_t)i] << (8 * i);
    return value_mask(ty, v);
}

static void mem_write(State *st, uint64_t addr, Type ty, uint64_t v)
{
    int size = type_size(ty);
    check_mem(st, addr, size);
    v = value_mask(ty, v);
    for (int i = 0; i < size; i++)
        st->mem[addr + (uint64_t)i] = (uint8_t)(v >> (8 * i));
}

static uint64_t alloc_mem(State *st, uint64_t n, uint64_t align)
{
    if (align < 1)
        align = 1;
    uint64_t a = (st->mem_top + align - 1) & ~(align - 1);
    if (a < st->mem_top || a + n > st->mem_size)
        die("out of interpreter memory");
    st->mem_top = a + n;
    return a;
}

static Value exec_function(State *st, Func *f, const Value *args,
                           int nargs, int depth);

static Value builtin_call(State *st, const char *name, Value *argv, int argc)
{
    (void)argc;
    if (!strcmp(name, "@putchar") || !strcmp(name, "@putc")) {
        int c = (int)(argv[0].u & 0xff);
        fputc(c, stdout);
        fflush(stdout);
        return value_of(TY_I32, (uint64_t)(unsigned char)c);
    }
    if (!strcmp(name, "@print_i64")) {
        printf("%" PRId64 "\n", value_signed(TY_I64, argv[0].u));
        fflush(stdout);
        return value_of(TY_VOID, 0);
    }
    if (!strcmp(name, "@print_i32")) {
        printf("%" PRId64 "\n", value_signed(TY_I32, argv[0].u));
        fflush(stdout);
        return value_of(TY_VOID, 0);
    }
    if (!strcmp(name, "@print_u64")) {
        printf("%" PRIu64 "\n", value_mask(TY_I64, argv[0].u));
        fflush(stdout);
        return value_of(TY_VOID, 0);
    }
    if (!strcmp(name, "@exit")) {
        st->exiting = 1;
        st->exit_code = (int)(argv[0].u & 0xff);
        return value_of(TY_VOID, 0);
    }
    if (!strcmp(name, "@abort") || !strcmp(name, "@llvm.trap")) {
        st->exiting = 1;
        st->exit_code = 134;
        return value_of(TY_VOID, 0);
    }
    die("call to undefined function %s", name);
    return value_of(TY_VOID, 0);
}

static Value exec_function(State *st, Func *f, const Value *args,
                           int nargs, int depth)
{
    if (depth > MAX_DEPTH)
        die("call depth exceeded in %s", f->name);
    if (nargs != f->nparams)
        die("function %s expects %d arguments, got %d",
            f->name, f->nparams, nargs);

    Env env = {0};
    for (int i = 0; i < nargs; i++)
        env_set(&env, f->pnames[i], value_of(f->ptypes[i], args[i].u));

    Block *blk = &f->blocks[0];
    const char *prev = NULL;

    for (;;) {
        int jumped = 0;
        for (int k = 0; k < blk->n; k++) {
            Instr *in = blk->items[k];
            if (++st->steps > MAX_STEPS)
                die("execution step limit exceeded");
            if (st->trace)
                fprintf(stderr, "[%s.%s] op %d\n", f->name, blk->label,
                        (int)in->kind);

            switch (in->kind) {
            case OP_RET: {
                if (in->a.ty == TY_VOID)
                    return value_of(TY_VOID, 0);
                return eval_operand(st, &env, &in->a);
            }
            case OP_BR:
                prev = blk->label;
                blk = func_find_block(f, in->target);
                jumped = 1;
                break;
            case OP_CBR: {
                Value c = eval_operand(st, &env, &in->a);
                prev = blk->label;
                blk = func_find_block(f,
                                      c.u ? in->target_true : in->target_false);
                jumped = 1;
                break;
            }
            case OP_UNREACHABLE:
                die("reached unreachable in %s", f->name);
                break;
            case OP_BIN: {
                if (in->folded) {
                    if (in->result)
                        env_set(&env, in->result, in->fold);
                    break;
                }
                Value a = eval_operand(st, &env, &in->a);
                Value b = eval_operand(st, &env, &in->b);
                Type ty = in->ty;
                int bits = type_bits(ty);
                uint64_t x = value_mask(ty, a.u);
                uint64_t y = value_mask(ty, b.u);
                uint64_t r = 0;
                switch (in->sub) {
                case BIN_ADD: r = x + y; break;
                case BIN_SUB: r = x - y; break;
                case BIN_MUL: r = x * y; break;
                case BIN_UDIV:
                    if (y == 0) die("division by zero");
                    r = x / y;
                    break;
                case BIN_UREM:
                    if (y == 0) die("division by zero");
                    r = x % y;
                    break;
                case BIN_SDIV: {
                    if (y == 0) die("division by zero");
                    int64_t sx = value_signed(ty, x);
                    int64_t sy = value_signed(ty, y);
                    r = (uint64_t)(sx / sy);
                    break;
                }
                case BIN_SREM: {
                    if (y == 0) die("division by zero");
                    int64_t sx = value_signed(ty, x);
                    int64_t sy = value_signed(ty, y);
                    r = (uint64_t)(sx % sy);
                    break;
                }
                case BIN_AND: r = x & y; break;
                case BIN_OR:  r = x | y; break;
                case BIN_XOR: r = x ^ y; break;
                case BIN_SHL:
                    r = x << (y & (uint64_t)(bits - 1));
                    break;
                case BIN_LSHR:
                    r = x >> (y & (uint64_t)(bits - 1));
                    break;
                case BIN_ASHR:
                    r = (uint64_t)(value_signed(ty, x) >>
                                   (y & (uint64_t)(bits - 1)));
                    break;
                default:
                    die("bad binary op");
                }
                if (in->result)
                    env_set(&env, in->result, value_of(ty, r));
                break;
            }
            case OP_ICMP: {
                if (in->folded) {
                    if (in->result)
                        env_set(&env, in->result, in->fold);
                    break;
                }
                Value a = eval_operand(st, &env, &in->a);
                Value b = eval_operand(st, &env, &in->b);
                Type ty = a.ty;
                uint64_t x = value_mask(ty, a.u);
                uint64_t y = value_mask(ty, b.u);
                int64_t sx = value_signed(ty, x);
                int64_t sy = value_signed(ty, y);
                int r = 0;
                switch (in->sub) {
                case IC_EQ: r = x == y; break;
                case IC_NE: r = x != y; break;
                case IC_UGT: r = x > y; break;
                case IC_UGE: r = x >= y; break;
                case IC_ULT: r = x < y; break;
                case IC_ULE: r = x <= y; break;
                case IC_SGT: r = sx > sy; break;
                case IC_SGE: r = sx >= sy; break;
                case IC_SLT: r = sx < sy; break;
                case IC_SLE: r = sx <= sy; break;
                default: die("bad icmp condition");
                }
                if (in->result)
                    env_set(&env, in->result, value_of(TY_I1, (uint64_t)r));
                break;
            }
            case OP_SELECT: {
                Value c = eval_operand(st, &env, &in->a);
                Value v = eval_operand(st, &env, c.u ? &in->b : &in->c);
                if (in->result)
                    env_set(&env, in->result, v);
                break;
            }
            case OP_CAST: {
                Value a = eval_operand(st, &env, &in->a);
                uint64_t r;
                switch (in->sub) {
                case CAST_ZEXT:
                    r = value_mask(in->a.ty, a.u);
                    break;
                case CAST_SEXT:
                    r = (uint64_t)value_signed(in->a.ty, a.u);
                    break;
                case CAST_TRUNC:
                case CAST_BITCAST:
                case CAST_PTRTOINT:
                case CAST_INTTOPTR:
                    r = a.u;
                    break;
                default:
                    die("bad cast");
                    r = 0;
                }
                if (in->result)
                    env_set(&env, in->result, value_of(in->ty, r));
                break;
            }
            case OP_PHI: {
                Value v = value_of(in->ty, 0);
                int found = 0;
                for (int j = 0; j < in->nphi; j++) {
                    if (prev && !strcmp(prev, in->phi_lab[j])) {
                        v = eval_operand(st, &env, &in->phi_val[j]);
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    die("phi in block %s has no value for predecessor %s",
                        blk->label, prev ? prev : "(entry)");
                if (in->result)
                    env_set(&env, in->result, v);
                break;
            }
            case OP_CALL: {
                Value argv[64];
                if (in->nargs > 64)
                    die("too many call arguments");
                for (int j = 0; j < in->nargs; j++)
                    argv[j] = eval_operand(st, &env, &in->args[j]);
                Func *callee = func_defined(st->m, in->callee);
                Value r;
                if (callee) {
                    r = exec_function(st, callee, argv, in->nargs, depth + 1);
                } else if (module_find_func(st->m, in->callee)) {
                    r = builtin_call(st, in->callee, argv, in->nargs);
                } else if (!strncmp(in->callee, "@llvm.", 6)) {
                    r = builtin_call(st, in->callee, argv, in->nargs);
                } else {
                    r = builtin_call(st, in->callee, argv, in->nargs);
                }
                if (st->exiting)
                    return value_of(TY_VOID, 0);
                if (in->result)
                    env_set(&env, in->result, r);
                break;
            }
            case OP_ALLOCA: {
                int size = type_size(in->ty);
                uint64_t n = (uint64_t)size * (uint64_t)in->alloca_count;
                uint64_t addr = alloc_mem(st, n, 16);
                if (in->result)
                    env_set(&env, in->result, value_of(TY_PTR, addr));
                break;
            }
            case OP_LOAD: {
                Value p = eval_operand(st, &env, &in->b);
                uint64_t v = mem_read(st, p.u, in->ty);
                if (in->result)
                    env_set(&env, in->result, value_of(in->ty, v));
                break;
            }
            case OP_STORE: {
                Value v = eval_operand(st, &env, &in->a);
                Value p = eval_operand(st, &env, &in->b);
                mem_write(st, p.u, in->a.ty, v.u);
                break;
            }
            }
            if (jumped)
                break;
        }
        if (!jumped)
            die("function %s fell off block %s", f->name, blk->label);
    }
}


/* --------------------------------------------------------------- driver */

static void init_globals(State *st)
{
    uint64_t top = MEM_BASE;
    for (int i = 0; i < st->m->nglobals; i++) {
        Global *g = &st->m->globals[i];
        if (g->external) {
            g->addr = 0;
            continue;
        }
        int size = type_size(g->ty);
        if (size <= 0)
            size = 8;
        top = (top + 7) & ~(uint64_t)7;
        g->addr = top;
        top += (uint64_t)size;
        if (top > st->mem_size)
            die("globals do not fit in interpreter memory");
        st->mem_top = top;
        mem_write(st, g->addr, g->ty, g->init);
    }
    st->mem_top = top;
}


static int run_module(Module *m, int argc, char **argv, int trace)
{
    (void)argc;
    (void)argv;
    State st;
    memset(&st, 0, sizeof(st));
    st.m = m;
    st.mem = xmalloc(MEM_SIZE);
    memset(st.mem, 0, MEM_SIZE);
    st.mem_size = MEM_SIZE;
    st.mem_top = MEM_BASE;
    st.trace = trace;
    init_globals(&st);

    Func *mainf = module_find_func(m, "@main");
    if (!mainf || mainf->is_declaration)
        die("no definition of @main");

    Value args[3];
    int nargs = mainf->nparams;
    if (nargs > 3)
        die("@main takes too many parameters");
    for (int i = 0; i < nargs; i++)
        args[i] = value_of(mainf->ptypes[i], i == 0 ? 1 : 0);

    Value r = exec_function(&st, mainf, args, nargs, 0);
    (void)r;
    int code = st.exiting ? st.exit_code : (int)(r.u & 0xff);
    free(st.mem);
    return code;
}

static void usage(FILE *out)
{
    fprintf(out,
        "mllvm -- a small, dependency free implementation of the LLVM IR core\n"
        "\n"
        "usage:\n"
        "  mllvm run [--trace] [--opt] FILE.ll   parse, verify and execute\n"
        "  mllvm check FILE.ll                   parse and verify only\n"
        "  mllvm opt FILE.ll                     constant fold and print the IR\n"
        "  mllvm version                         print the version\n"
        "\n"
        "The interpreter supports i1/i8/i16/i32/i64/pointer values, integer\n"
        "arithmetic, icmp, select, casts, phi, direct calls, alloca, load,\n"
        "store, br, cbr and ret.  Builtins: putchar, print_i32, print_i64,\n"
        "print_u64, exit, abort.\n");
}

/* The optimizer and printer live in the second half of the file. */
static void optimize_module(Module *m);
static void print_module(Module *m, FILE *out);

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "version") || !strcmp(argv[1], "--version")) {
        puts("mllvm 1.0.0");
        return 0;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }

    const char *cmd = argv[1];
    int trace = 0, opt = 0, argi = 2;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--trace"))
            trace = 1;
        else if (!strcmp(argv[argi], "--opt"))
            opt = 1;
        else if (!strcmp(argv[argi], "-h") || !strcmp(argv[argi], "--help")) {
            usage(stdout);
            return 0;
        } else {
            die("unknown option %s", argv[argi]);
        }
        argi++;
    }
    if (argi >= argc) {
        usage(stderr);
        return 2;
    }
    const char *path = argv[argi];

    FILE *fp;
    if (!strcmp(path, "-")) {
        fp = stdin;
    } else {
        fp = fopen(path, "r");
        if (!fp)
            die("cannot open %s: %s", path, strerror(errno));
    }
    Module m = parse_module(fp);
    if (fp != stdin)
        fclose(fp);

    if (!strcmp(cmd, "check")) {
        verify_module(&m);
        printf("%s: ok (%d functions, %d globals)\n",
               path, m.nfuncs, m.nglobals);
        return 0;
    }
    if (!strcmp(cmd, "opt")) {
        verify_module(&m);
        optimize_module(&m);
        print_module(&m, stdout);
        return 0;
    }
    if (!strcmp(cmd, "run")) {
        verify_module(&m);
        if (opt)
            optimize_module(&m);
        int code = run_module(&m, argc - argi, argv + argi, trace);
        return code;
    }
    usage(stderr);
    return 2;
}

/* --------------------------------------------------------------- optimizer */

static int fold_bin(int sub, Type ty, uint64_t x, uint64_t y, uint64_t *out)
{
    int bits = type_bits(ty);
    x = value_mask(ty, x);
    y = value_mask(ty, y);
    uint64_t r = 0;
    switch (sub) {
    case BIN_ADD: r = x + y; break;
    case BIN_SUB: r = x - y; break;
    case BIN_MUL: r = x * y; break;
    case BIN_UDIV: if (!y) return 0; r = x / y; break;
    case BIN_UREM: if (!y) return 0; r = x % y; break;
    case BIN_SDIV:
        if (!y) return 0;
        r = (uint64_t)(value_signed(ty, x) / value_signed(ty, y));
        break;
    case BIN_SREM:
        if (!y) return 0;
        r = (uint64_t)(value_signed(ty, x) % value_signed(ty, y));
        break;
    case BIN_AND: r = x & y; break;
    case BIN_OR:  r = x | y; break;
    case BIN_XOR: r = x ^ y; break;
    case BIN_SHL: r = x << (y & (uint64_t)(bits - 1)); break;
    case BIN_LSHR: r = x >> (y & (uint64_t)(bits - 1)); break;
    case BIN_ASHR:
        r = (uint64_t)(value_signed(ty, x) >> (y & (uint64_t)(bits - 1)));
        break;
    default: return 0;
    }
    *out = value_mask(ty, r);
    return 1;
}

static int fold_icmp(int sub, Type ty, uint64_t x, uint64_t y, uint64_t *out)
{
    x = value_mask(ty, x);
    y = value_mask(ty, y);
    int64_t sx = value_signed(ty, x), sy = value_signed(ty, y);
    int r;
    switch (sub) {
    case IC_EQ: r = x == y; break;
    case IC_NE: r = x != y; break;
    case IC_UGT: r = x > y; break;
    case IC_UGE: r = x >= y; break;
    case IC_ULT: r = x < y; break;
    case IC_ULE: r = x <= y; break;
    case IC_SGT: r = sx > sy; break;
    case IC_SGE: r = sx >= sy; break;
    case IC_SLT: r = sx < sy; break;
    case IC_SLE: r = sx <= sy; break;
    default: return 0;
    }
    *out = (uint64_t)r;
    return 1;
}

static int fold_cast(int sub, Type from, uint64_t v, Type to, uint64_t *out)
{
    switch (sub) {
    case CAST_ZEXT: *out = value_mask(to, value_mask(from, v)); return 1;
    case CAST_SEXT: *out = value_mask(to, (uint64_t)value_signed(from, v));
        return 1;
    case CAST_TRUNC:
    case CAST_BITCAST:
    case CAST_PTRTOINT:
    case CAST_INTTOPTR:
        *out = value_mask(to, v);
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    char *name;
    uint64_t v;
    Type ty;
} Constant;

static int const_get(Constant *c, int n, const Operand *o, uint64_t *v)
{
    if (!o->is_name) {
        *v = o->cval;
        return 1;
    }
    for (int i = n - 1; i >= 0; i--)
        if (!strcmp(c[i].name, o->name)) {
            *v = c[i].v;
            return 1;
        }
    return 0;
}

static void optimize_function(Func *f)
{
    if (f->is_declaration)
        return;
    Constant *c = NULL;
    int n = 0, cap = 0;
    for (int b = 0; b < f->nblocks; b++) {
        Block *blk = &f->blocks[b];
        for (int k = 0; k < blk->n; k++) {
            Instr *in = blk->items[k];
            uint64_t x = 0, y = 0, r = 0;
            int ok = 0;
            if (in->kind == OP_BIN &&
                const_get(c, n, &in->a, &x) &&
                const_get(c, n, &in->b, &y)) {
                ok = fold_bin(in->sub, in->ty, x, y, &r);
            } else if (in->kind == OP_ICMP &&
                       const_get(c, n, &in->a, &x) &&
                       const_get(c, n, &in->b, &y)) {
                ok = fold_icmp(in->sub, in->a.ty, x, y, &r);
            } else if (in->kind == OP_CAST && const_get(c, n, &in->a, &x)) {
                ok = fold_cast(in->sub, in->a.ty, x, in->ty, &r);
            } else if (in->kind == OP_SELECT &&
                       const_get(c, n, &in->a, &x)) {
                const Operand *pick = x ? &in->b : &in->c;
                ok = const_get(c, n, pick, &r);
            }
            if (ok && in->result) {
                in->folded = 1;
                in->fold = value_of(in->ty, r);
                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    c = xrealloc(c, (size_t)cap * sizeof(*c));
                }
                c[n].name = in->result;
                c[n].v = value_mask(in->ty, r);
                c[n].ty = in->ty;
                n++;
            }
        }
    }
    free(c);
}

static void optimize_module(Module *m)
{
    for (int i = 0; i < m->nfuncs; i++)
        optimize_function(m->funcs[i]);
}

/* ----------------------------------------------------------------- printer */

static void print_operand(FILE *out, const Operand *o, Type ty)
{
    if (o->is_name) {
        fputs(o->name, out);
    } else if (ty == TY_I1) {
        fputs(o->cval ? "true" : "false", out);
    } else {
        fprintf(out, "%" PRIu64, o->cval);
    }
}

static void print_instr(FILE *out, Instr *in)
{
    fputs("  ", out);
    if (in->result)
        fprintf(out, "%s = ", in->result);

    if (in->folded && in->result) {
        fprintf(out, "add %s ", type_name(in->ty));
        print_operand(out, &(Operand){.ty = in->ty, .cval = in->fold.u},
                      in->ty);
        fputs(", 0\n", out);
        return;
    }

    switch (in->kind) {
    case OP_RET:
        if (in->a.ty == TY_VOID)
            fputs("ret void\n", out);
        else {
            fprintf(out, "ret %s ", type_name(in->a.ty));
            print_operand(out, &in->a, in->a.ty);
            fputc('\n', out);
        }
        break;
    case OP_BR:
        fprintf(out, "br label %%%s\n", in->target);
        break;
    case OP_CBR:
        fputs("br i1 ", out);
        print_operand(out, &in->a, TY_I1);
        fprintf(out, ", label %%%s, label %%%s\n",
                in->target_true, in->target_false);
        break;
    case OP_BIN: {
        static const char *names[] = {
            "add", "sub", "mul", "sdiv", "udiv", "srem", "urem",
            "and", "or", "xor", "shl", "lshr", "ashr"
        };
        fprintf(out, "%s %s ", names[in->sub], type_name(in->ty));
        print_operand(out, &in->a, in->ty);
        fputs(", ", out);
        print_operand(out, &in->b, in->ty);
        fputc('\n', out);
        break;
    }
    case OP_ICMP: {
        static const char *names[] = {
            "eq", "ne", "ugt", "uge", "ult", "ule",
            "sgt", "sge", "slt", "sle"
        };
        fprintf(out, "icmp %s %s ", names[in->sub], type_name(in->a.ty));
        print_operand(out, &in->a, in->a.ty);
        fputs(", ", out);
        print_operand(out, &in->b, in->a.ty);
        fputc('\n', out);
        break;
    }
    case OP_SELECT:
        fputs("select i1 ", out);
        print_operand(out, &in->a, TY_I1);
        fputs(", ", out);
        fprintf(out, "%s ", type_name(in->b.ty));
        print_operand(out, &in->b, in->b.ty);
        fputs(", ", out);
        fprintf(out, "%s ", type_name(in->c.ty));
        print_operand(out, &in->c, in->c.ty);
        fputc('\n', out);
        break;
    case OP_CAST: {
        static const char *names[] = {
            "zext", "sext", "trunc", "bitcast", "ptrtoint", "inttoptr"
        };
        fprintf(out, "%s %s ", names[in->sub], type_name(in->a.ty));
        print_operand(out, &in->a, in->a.ty);
        fprintf(out, " to %s\n", type_name(in->ty));
        break;
    }
    case OP_PHI:
        fprintf(out, "phi %s ", type_name(in->ty));
        for (int j = 0; j < in->nphi; j++) {
            if (j) fputs(", ", out);
            fputc('[', out);
            print_operand(out, &in->phi_val[j], in->ty);
            fprintf(out, ", %%%s]", in->phi_lab[j]);
        }
        fputc('\n', out);
        break;
    case OP_CALL:
        fprintf(out, "call %s %s(", type_name(in->ty), in->callee);
        for (int j = 0; j < in->nargs; j++) {
            if (j) fputs(", ", out);
            fprintf(out, "%s ", type_name(in->args[j].ty));
            print_operand(out, &in->args[j], in->args[j].ty);
        }
        fputs(")\n", out);
        break;
    case OP_ALLOCA:
        fprintf(out, "alloca %s", type_name(in->ty));
        if (in->alloca_count != 1)
            fprintf(out, ", i64 %d", in->alloca_count);
        fputc('\n', out);
        break;
    case OP_LOAD:
        fprintf(out, "load %s, ptr ", type_name(in->ty));
        print_operand(out, &in->b, TY_PTR);
        fputc('\n', out);
        break;
    case OP_STORE:
        fprintf(out, "store %s ", type_name(in->a.ty));
        print_operand(out, &in->a, in->a.ty);
        fputs(", ptr ", out);
        print_operand(out, &in->b, TY_PTR);
        fputc('\n', out);
        break;
    case OP_UNREACHABLE:
        fputs("unreachable\n", out);
        break;
    }
}

static void print_module(Module *m, FILE *out)
{
    for (int i = 0; i < m->nglobals; i++) {
        Global *g = &m->globals[i];
        if (g->external) {
            fprintf(out, "%s = external global %s\n", g->name,
                    type_name(g->ty));
            continue;
        }
        fprintf(out, "%s = global %s %" PRIu64 "\n", g->name,
                type_name(g->ty), g->init);
    }
    if (m->nglobals)
        fputc('\n', out);

    for (int i = 0; i < m->nfuncs; i++) {
        Func *f = m->funcs[i];
        if (f->is_declaration) {
            fprintf(out, "declare %s %s(", type_name(f->ret), f->name);
            for (int p = 0; p < f->nparams; p++)
                fprintf(out, "%s%s %s", p ? ", " : "",
                        type_name(f->ptypes[p]), f->pnames[p]);
            fputs(")\n", out);
            continue;
        }
        fprintf(out, "define %s %s(", type_name(f->ret), f->name);
        for (int p = 0; p < f->nparams; p++)
            fprintf(out, "%s%s %s", p ? ", " : "",
                    type_name(f->ptypes[p]), f->pnames[p]);
        fputs(") {\n", out);
        for (int b = 0; b < f->nblocks; b++) {
            Block *blk = &f->blocks[b];
            fprintf(out, "%s:\n", blk->label);
            for (int k = 0; k < blk->n; k++)
                print_instr(out, blk->items[k]);
        }
        fputs("}\n\n", out);
    }
}
