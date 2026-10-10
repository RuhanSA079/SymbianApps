/*
 * rs_parse.cpp: tokens -> syntax tree (recursive descent, C# precedence).
 *
 * A program is a list of statements, as in a LINQPad "C# Statements"
 * query. When the last line is an expression without ';', its value is the
 * result, as in a LINQPad "C# Expression" query. `using` lines are skipped.
 */
#include "rs_int.h"

namespace rs {

struct Parser
    {
    const rs_char *src;
    Tok *t;
    int n, i;
    int loopDepth;
    };

static Parser *P;

static Tok &Cur() { return P->t[P->i]; }
static Tok &At(int aOff) { int j = P->i + aOff; return P->t[j < P->n ? j : P->n - 1]; }
static bool Is(int k) { return Cur().k == k; }
static bool IsAt(int aOff, int k) { return At(aOff).k == k; }
static bool IsName(int aId) { return Cur().k == TK_NAME && !Cur().verbatim && Cur().name->id == aId; }
static bool IsNameAt(int aOff, int aId) { Tok &t = At(aOff); return t.k == TK_NAME && !t.verbatim && t.name->id == aId; }
static int Pos() { return Cur().pos; }
static void Next() { if (P->i < P->n - 1) P->i++; }
static bool Accept(int k) { if (Is(k)) { Next(); return true; } return false; }
static bool AcceptName(int aId) { if (IsName(aId)) { Next(); return true; } return false; }

RS_NORETURN static void Expected(const char *aWhat)
{
    Tok &t = Cur();
    if (t.k == TK_EOF && P->i > 0)
        CompileError(P->t[P->i - 1].end, "%s expected, found 'end of input'", aWhat);
    if (t.k == TK_NAME)
        CompileError(t.pos, "%s expected, found '%s'", aWhat, t.name->s);
    CompileError(t.pos, "%s expected, found '%s'", aWhat, TokText(t.k));
}

static void Expect(int k)
{
    if (!Accept(k)) {
        char what[16];
        what[0] = '\'';
        strcpy(what + 1, TokText(k));
        strcat(what, "'");
        Expected(what);
    }
}

// Reserved words that can't be names (contextual ones like var, from, select can).
static bool Reserved(const Tok &t)
{
    if (t.k != TK_NAME || t.verbatim)
        return false;
    switch (t.name->id) {
    case K_int: case K_uint: case K_long: case K_ulong: case K_short: case K_ushort:
    case K_byte: case K_sbyte: case K_char: case K_bool: case K_float: case K_double:
    case K_decimal: case K_string: case K_object: case K_void: case K_true: case K_false:
    case K_null: case K_new: case K_if: case K_else: case K_while: case K_do: case K_for:
    case K_foreach: case K_in: case K_break: case K_continue: case K_return: case K_is:
    case K_as: case K_default: case K_checked: case K_unchecked: case K_typeof:
    case K_out: case K_ref: case K_using: case K_switch: case K_case: case K_throw:
    case K_static: case K_const: case K_params: case K_this: case K_base:
    case K_namespace: case K_class: case K_struct: case K_try: case K_catch:
    case K_finally: case K_goto: case K_lock:
        return true;
    default:
        return false;
    }
}

static Name *ExpectIdent()
{
    Tok &t = Cur();
    if (t.k != TK_NAME || Reserved(t))
        Expected("Identifier");
    Next();
    return t.name;
}

static Node *Mk(int k, int aPos)
{
    Node *e = New<Node>();
    e->k = (uint8_t)k;
    e->pos = aPos;
    return e;
}

// growable node lists
struct List { Node **v; int n, cap; };
static void Add(List &l, Node *e)
{
    if (l.n == l.cap) {
        int cap = l.cap ? l.cap * 2 : 8;
        Node **v = NewArr<Node *>(cap);
        if (l.n) memcpy(v, l.v, l.n * sizeof(Node *));
        l.v = v;
        l.cap = cap;
    }
    l.v[l.n++] = e;
}
static void SetList(Node *e, List &l) { e->xs = l.v; e->n = l.n; }

struct NameList { Name **v; TypeRef **types; int n, cap; };
static void AddName(NameList &l, Name *aName, TypeRef *aType)
{
    if (l.n == l.cap) {
        int cap = l.cap ? l.cap * 2 : 4;
        Name **v = NewArr<Name *>(cap);
        TypeRef **t = NewArr<TypeRef *>(cap);
        if (l.n) {
            memcpy(v, l.v, l.n * sizeof(Name *));
            memcpy(t, l.types, l.n * sizeof(TypeRef *));
        }
        l.v = v;
        l.types = t;
        l.cap = cap;
    }
    l.v[l.n] = aName;
    l.types[l.n] = aType;
    l.n++;
}

static Node *ParseExpr();
static Node *ParseUnary();
static Node *ParseStatement(bool aTop);
static Node *ParseBlock();
static Node *ParsePattern();
static Node *ParseShift();

// ---------------------------------------------------------------------------
// types

static int PrimOf(int aId)
{
    switch (aId) {
    case K_int: return T_INT;
    case K_uint: return T_UINT;
    case K_long: return T_LONG;
    case K_ulong: return T_ULONG;
    case K_short: return T_SHORT;
    case K_ushort: return T_USHORT;
    case K_byte: return T_BYTE;
    case K_sbyte: return T_SBYTE;
    case K_char: return T_CHAR;
    case K_bool: return T_BOOL;
    case K_float: return T_FLOAT;
    case K_double: return T_DOUBLE;
    case K_string: return T_STRING;
    case K_object: case K_dynamic: case K_var: return T_OBJECT;
    case K_void: return T_NULL;
    default: return -1;
    }
}

// The type at the cursor, or NULL (cursor unchanged) when there is none.
// aStrict: only accept what can only be a type (keyword types, generics,
// arrays), for casts.
static TypeRef *TryType(bool aAllowNullable)
{
    int save = P->i;
    Tok &t = Cur();
    TypeRef *r = NULL;
    if (t.k == TK_NAME && !t.verbatim && t.name->id == K_decimal) {
        CompileError(t.pos, "decimal is not supported in rSharp; use double");
    } else if (t.k == TK_NAME && !t.verbatim && PrimOf(t.name->id) >= 0) {
        r = New<TypeRef>();
        r->id = PrimOf(t.name->id);
        r->name = t.name;
        r->isVar = t.name->id == K_var;
        Next();
    } else if (t.k == TK_LPAR) {
        // tuple type: (int, string) or (int a, int b)
        Next();
        TypeRef *items[8];
        int count = 0;
        for (;;) {
            TypeRef *it = TryType(true);
            if (!it || count == 8) { P->i = save; return NULL; }
            items[count++] = it;
            if (Is(TK_NAME) && !Reserved(Cur())) Next();
            if (Accept(TK_COMMA)) continue;
            if (Accept(TK_RPAR)) break;
            P->i = save;
            return NULL;
        }
        if (count < 2) { P->i = save; return NULL; }
        r = New<TypeRef>();
        r->id = TY_TUPLE;
        r->nargs = count;
        r->args = NewArr<TypeRef *>(count);
        memcpy(r->args, items, count * sizeof(TypeRef *));
    } else if (t.k == TK_NAME && !Reserved(t)) {
        Name *nm = t.name;
        Next();
        // dotted: System.Collections.Generic.List
        while (Is(TK_DOT) && IsAt(1, TK_NAME) && !Reserved(At(1))) {
            Next();
            nm = Cur().name;
            Next();
        }
        r = New<TypeRef>();
        r->name = nm;
        r->id = TypeIdOf(nm);
        if (Is(TK_LT)) {
            int lt = P->i;
            Next();
            TypeRef *args[8];
            int count = 0;
            for (;;) {
                TypeRef *a = TryType(true);
                if (!a || count == 8) { P->i = lt; break; }
                args[count++] = a;
                if (Accept(TK_COMMA)) continue;
                if (Accept(TK_GT)) {
                    r->nargs = count;
                    r->args = NewArr<TypeRef *>(count);
                    memcpy(r->args, args, count * sizeof(TypeRef *));
                } else {
                    P->i = lt;
                }
                break;
            }
            if (!r->nargs) { P->i = save; return NULL; }
        }
    } else {
        return NULL;
    }
    for (;;) {
        if (aAllowNullable && Is(TK_QUESTION)) {
            // `int? x` but not `a ? b : c`: a nullable type is followed by a
            // name, ')', ',', '>', '[' or '='.
            int k1 = At(1).k;
            if (k1 == TK_NAME || k1 == TK_RPAR || k1 == TK_COMMA || k1 == TK_GT || k1 == TK_LBRACK) {
                Next();
                r->nullable = true;
                continue;
            }
        }
        if (Is(TK_LBRACK) && IsAt(1, TK_RBRACK)) {
            Next();
            Next();
            TypeRef *arr = New<TypeRef>();
            *arr = *r;
            arr->rank = r->rank + 1;
            r = arr;
            continue;
        }
        break;
    }
    return r;
}

static TypeRef *ExpectType()
{
    TypeRef *t = TryType(true);
    if (!t)
        Expected("Type");
    return t;
}

static void CheckKnownType(TypeRef *t, int aPos)
{
    if (!t || t->isVar || t->rank)
        return;
    if (t->id < 0)
        CompileError(aPos, "The type '%s' is not known to rSharp", t->name ? t->name->s : "?");
}

// Generic arguments after a name in an expression (`Cast<int>(`), if the
// tokens after the closing '>' say they are (C# spec 6.2.5).
static bool TryTypeArgs(TypeRef ***aArgs, int *aCount)
{
    if (!Is(TK_LT))
        return false;
    int save = P->i;
    Next();
    TypeRef *args[8];
    int count = 0;
    for (;;) {
        TypeRef *a = TryType(true);
        if (!a || count == 8) { P->i = save; return false; }
        args[count++] = a;
        if (Accept(TK_COMMA)) continue;
        if (!Accept(TK_GT)) { P->i = save; return false; }
        break;
    }
    switch (Cur().k) {
    case TK_LPAR: case TK_RPAR: case TK_RBRACK: case TK_RBRACE: case TK_COLON:
    case TK_SEMI: case TK_COMMA: case TK_DOT: case TK_QUESTION: case TK_EQEQ:
    case TK_NE: case TK_BAR: case TK_CARET: case TK_ANDAND: case TK_OROR:
    case TK_AMP: case TK_LBRACK: case TK_EOF:
        break;
    default:
        P->i = save;
        return false;
    }
    *aArgs = NewArr<TypeRef *>(count);
    memcpy(*aArgs, args, count * sizeof(TypeRef *));
    *aCount = count;
    return true;
}

// ---------------------------------------------------------------------------
// expressions

static bool IsConst(Node *e) { return e && (e->flags & F_CONST); }

static Node *Lit(Value v, int aPos)
{
    Node *e = Mk(N_LIT, aPos);
    e->lit = v;
    e->flags = F_CONST;
    return e;
}

static void ParseArgs(Node *aCall, int aClose)
{
    List args = { NULL, 0, 0 };
    if (!Is(aClose)) {
        for (;;) {
            // named arguments (name: value) are taken in order
            if (Is(TK_NAME) && IsAt(1, TK_COLON) && !IsAt(2, TK_COLON))
                { Next(); Next(); }
            if (IsName(K_out) || IsName(K_ref)) {
                int at = Pos();
                Next();
                Node *o = Mk(N_OUTARG, at);
                // out var x / out int x / out x
                int save = P->i;
                TypeRef *ty = NULL;
                if (Is(TK_NAME) && IsAt(1, TK_NAME))
                    ty = TryType(true);
                if (ty && Is(TK_NAME)) {
                    o->flags |= F_OUT_DECL;
                    o->type = ty;
                    o->name = ExpectIdent();
                } else {
                    P->i = save;
                    o->a = ParseUnary();
                }
                Add(args, o);
            } else {
                Add(args, ParseExpr());
            }
            if (!Accept(TK_COMMA))
                break;
        }
    }
    Expect(aClose);
    SetList(aCall, args);
}

// { a, b, { k, v }, [k] = v } after `new T` or in `int[] a = { ... }`
static Node *ParseInitializer()
{
    Node *init = Mk(N_COLLECTION, Pos());
    Expect(TK_LBRACE);
    List items = { NULL, 0, 0 };
    while (!Is(TK_RBRACE)) {
        if (Is(TK_LBRACE)) {
            // { k, v } -> Add(k, v)
            Node *grp = Mk(N_TUPLE, Pos());
            Next();
            List parts = { NULL, 0, 0 };
            while (!Is(TK_RBRACE)) {
                Add(parts, ParseExpr());
                if (!Accept(TK_COMMA)) break;
            }
            Expect(TK_RBRACE);
            SetList(grp, parts);
            Add(items, grp);
        } else if (Is(TK_LBRACK)) {
            // [k] = v
            Node *ix = Mk(N_INDEX, Pos());
            Next();
            List k = { NULL, 0, 0 };
            Add(k, ParseExpr());
            Expect(TK_RBRACK);
            SetList(ix, k);
            Node *as = Mk(N_ASSIGN, Pos());
            Expect(TK_EQ);
            as->a = ix;
            as->b = ParseExpr();
            Add(items, as);
        } else {
            Add(items, ParseExpr());
        }
        if (!Accept(TK_COMMA))
            break;
    }
    Expect(TK_RBRACE);
    SetList(init, items);
    return init;
}

static Node *ParseNew(int aPos)
{
    // new { a = 1, b }   anonymous type
    if (Is(TK_LBRACE)) {
        Node *e = Mk(N_NEWANON, aPos);
        Next();
        List vals = { NULL, 0, 0 };
        NameList names = { NULL, NULL, 0, 0 };
        while (!Is(TK_RBRACE)) {
            int at = Pos();
            Name *nm = NULL;
            if (Is(TK_NAME) && IsAt(1, TK_EQ)) {
                nm = ExpectIdent();
                Next();
            }
            Node *v = ParseExpr();
            if (!nm) {
                // projection initializer: new { x, p.Name }
                if (v->k == N_NAME || v->k == N_MEMBER) nm = v->name;
                else CompileError(at, "Invalid anonymous type member declarator");
            }
            for (int k = 0; k < names.n; k++)
                if (names.v[k] == nm)
                    CompileError(at, "An anonymous type cannot have multiple properties with the same name");
            AddName(names, nm, NULL);
            Add(vals, v);
            if (!Accept(TK_COMMA)) break;
        }
        Expect(TK_RBRACE);
        SetList(e, vals);
        e->names = names.v;
        return e;
    }
    // new[] { ... }
    if (Is(TK_LBRACK) && IsAt(1, TK_RBRACK)) {
        Next();
        Next();
        Node *e = Mk(N_NEWARR, aPos);
        e->b = ParseInitializer();
        return e;
    }
    // new() target-typed
    if (Is(TK_LPAR)) {
        Node *e = Mk(N_NEW, aPos);
        Next();
        ParseArgs(e, TK_RPAR);
        if (Is(TK_LBRACE))
            e->b = ParseInitializer();
        return e;
    }
    // new T(...) / new T[n] / new T[] { ... }
    TypeRef *t = TryType(false);
    if (!t)
        Expected("Type");
    if (t->rank == 0 && Is(TK_LBRACK)) {
        // new int[n]
        Node *e = Mk(N_NEWARR, aPos);
        Next();
        e->a = ParseExpr();
        if (Is(TK_COMMA))
            CompileError(Pos(), "Multi-dimensional arrays are not supported; use jagged arrays (int[][])");
        Expect(TK_RBRACK);
        TypeRef *et = t;
        while (Is(TK_LBRACK) && IsAt(1, TK_RBRACK)) {        // new int[3][]
            Next();
            Next();
            TypeRef *arr = New<TypeRef>();
            *arr = *et;
            arr->rank = et->rank + 1;
            et = arr;
        }
        e->type = et;
        CheckKnownType(et, aPos);
        if (Is(TK_LBRACE))
            e->b = ParseInitializer();
        return e;
    }
    if (t->rank > 0) {
        Node *e = Mk(N_NEWARR, aPos);
        TypeRef *et = New<TypeRef>();
        *et = *t;
        et->rank = t->rank - 1;
        e->type = et;
        CheckKnownType(et, aPos);
        e->b = ParseInitializer();
        return e;
    }
    CheckKnownType(t, aPos);
    Node *e = Mk(N_NEW, aPos);
    e->type = t;
    if (Is(TK_LPAR)) {
        Next();
        ParseArgs(e, TK_RPAR);
    }
    if (Is(TK_LBRACE))
        e->b = ParseInitializer();
    else if (!e->xs && e->n == 0 && At(-1).k != TK_RPAR)
        Expected("'(' or '{'");
    return e;
}

// $"..." -> N_INTERP of N_LIT and N_HOLE parts
static Node *ParseInterp(Tok &t)
{
    Node *e = Mk(N_INTERP, t.pos);
    const rs_char *s = P->src;
    int i = (int)t.lit.i, end = t.end - 1;
    bool vb = t.verbatim;
    List parts = { NULL, 0, 0 };
    SB lit = { NULL, 0, 0 };
    while (i < end) {
        rs_char c = s[i];
        if (c == '{' && i + 1 < end && s[i + 1] == '{') { SbAppendChars(&lit, &c, 1); i += 2; continue; }
        if (c == '}' && i + 1 < end && s[i + 1] == '}') { SbAppendChars(&lit, &c, 1); i += 2; continue; }
        if (c == '"' && vb) { SbAppendChars(&lit, &c, 1); i += 2; continue; }
        if (c == '\\' && !vb) {
            // the lexer has validated escapes; decode the simple ones
            rs_char e2 = s[i + 1], out = e2;
            int len = 2;
            switch (e2) {
            case 'n': out = '\n'; break;
            case 't': out = '\t'; break;
            case 'r': out = '\r'; break;
            case '0': out = 0; break;
            case 'a': out = 7; break;
            case 'b': out = 8; break;
            case 'f': out = 12; break;
            case 'v': out = 11; break;
            case 'e': out = 27; break;
            case 'u': case 'x': {
                unsigned v = 0;
                int k = i + 2, digits = 0;
                while (digits < 4 && k < end) {
                    rs_char h = s[k];
                    int hv = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                             (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (hv < 0) break;
                    v = v * 16 + hv;
                    k++;
                    digits++;
                }
                out = (rs_char)v;
                len = 2 + digits;
                break;
            }
            default: break;
            }
            SbAppendChars(&lit, &out, 1);
            i += len;
            continue;
        }
        if (c == '{') {
            if (lit.n) {
                Value v;
                v.t = T_STRING;
                v.s = SbStr(&lit);
                Add(parts, Lit(v, i));
                lit.n = 0;
                lit.c = NULL;
                lit.cap = 0;
            }
            int from = ++i, depth = 0, comma = -1, colon = -1;
            while (i < end) {
                rs_char ch = s[i];
                if (ch == '"' || ch == '\'') {
                    rs_char q = ch;
                    bool v2 = ch == '"' && i > 0 && s[i - 1] == '@';
                    i++;
                    while (i < end && s[i] != q) {
                        if (s[i] == '\\' && !v2) i++;
                        i++;
                    }
                    i++;
                    continue;
                }
                if (ch == '(' || ch == '[' || ch == '{') depth++;
                else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) depth--;
                else if (ch == '}' && depth == 0) break;
                else if (ch == ',' && depth == 0 && comma < 0 && colon < 0) comma = i;
                else if (ch == ':' && depth == 0 && colon < 0) {
                    // `a ? b : c` needs parentheses in a hole, as in C#; but
                    // `::` and `?:`-free text means a format
                    colon = i;
                    break;
                }
                i++;
            }
            int exprEnd = comma >= 0 ? comma : colon >= 0 ? colon : i;
            Node *h = Mk(N_HOLE, from);
            h->a = ParseExprRange(s, from, exprEnd);
            if (comma >= 0)
                h->b = ParseExprRange(s, comma + 1, colon >= 0 ? colon : i);
            if (colon >= 0) {
                int f = colon + 1;
                while (i < end && s[i] != '}') i++;
                h->fmt = NewStr(i - f);
                memcpy(h->fmt->c, s + f, (i - f) * sizeof(rs_char));
            }
            if (i >= end)
                CompileError(t.pos, "Unterminated hole in interpolated string ('}' expected)");
            i++;                                    // '}'
            Add(parts, h);
            continue;
        }
        SbAppendChars(&lit, &c, 1);
        i++;
    }
    if (lit.n || !parts.n) {
        Value v;
        v.t = T_STRING;
        v.s = lit.n ? SbStr(&lit) : (Str *)EmptyStr();
        Add(parts, Lit(v, end));
    }
    SetList(e, parts);
    return e;
}

static bool LambdaAhead()
{
    // x => ...
    if (Is(TK_NAME) && !Reserved(Cur()) && IsAt(1, TK_ARROW))
        return true;
    // (...) => ...
    if (!Is(TK_LPAR))
        return false;
    int depth = 0;
    for (int j = P->i; j < P->n; j++) {
        int k = P->t[j].k;
        if (k == TK_LPAR) depth++;
        else if (k == TK_RPAR && --depth == 0)
            return j + 1 < P->n && P->t[j + 1].k == TK_ARROW;
        else if (k == TK_EOF || k == TK_SEMI || k == TK_LBRACE)
            return false;
    }
    return false;
}

static Node *ParseLambda()
{
    Node *e = Mk(N_LAMBDA, Pos());
    NameList ps = { NULL, NULL, 0, 0 };
    if (Is(TK_NAME)) {
        AddName(ps, ExpectIdent(), NULL);
    } else {
        Expect(TK_LPAR);
        while (!Is(TK_RPAR)) {
            TypeRef *ty = NULL;
            if (Is(TK_NAME) && !IsAt(1, TK_COMMA) && !IsAt(1, TK_RPAR)) {
                ty = ExpectType();
                CheckKnownType(ty, Pos());
            }
            AddName(ps, ExpectIdent(), ty);
            if (!Accept(TK_COMMA)) break;
        }
        Expect(TK_RPAR);
    }
    Expect(TK_ARROW);
    e->names = ps.v;
    e->targs = ps.types;
    e->ntargs = ps.n;
    e->n = ps.n;
    if (Is(TK_LBRACE)) {
        e->flags |= F_BLOCKBODY;
        int saveLoop = P->loopDepth;
        P->loopDepth = 0;
        e->a = ParseBlock();
        P->loopDepth = saveLoop;
    } else {
        e->a = ParseExpr();
    }
    return e;
}

// ---- query expressions ----

static bool QueryAhead()
{
    if (!IsName(K_from) || !IsAt(1, TK_NAME))
        return false;
    // from x in ... / from int x in ...
    return IsNameAt(2, K_in) || IsNameAt(3, K_in) || (IsAt(2, TK_LT));
}

static Node *ParseFrom(int aPos)
{
    Node *c = Mk(Q_FROM, aPos);
    // optional type: from int x in xs
    if (Is(TK_NAME) && !IsNameAt(1, K_in)) {
        c->type = ExpectType();
    }
    c->name = ExpectIdent();
    if (!AcceptName(K_in)) Expected("'in'");
    c->a = ParseExpr();
    return c;
}

static Node *ParseQuery()
{
    Node *q = Mk(N_QUERY, Pos());
    List cl = { NULL, 0, 0 };
    Next();                                         // from
    Add(cl, ParseFrom(q->pos));
    for (;;) {
        int at = Pos();
        if (AcceptName(K_from)) {
            Add(cl, ParseFrom(at));
        } else if (AcceptName(K_let)) {
            Node *c = Mk(Q_LET, at);
            c->name = ExpectIdent();
            Expect(TK_EQ);
            c->a = ParseExpr();
            Add(cl, c);
        } else if (AcceptName(K_where)) {
            Node *c = Mk(Q_WHERE, at);
            c->a = ParseExpr();
            Add(cl, c);
        } else if (AcceptName(K_join)) {
            Node *c = Mk(Q_JOIN, at);
            if (Is(TK_NAME) && !IsNameAt(1, K_in))
                c->type = ExpectType();
            c->name = ExpectIdent();
            if (!AcceptName(K_in)) Expected("'in'");
            c->a = ParseExpr();
            if (!AcceptName(K_on)) Expected("'on'");
            c->b = ParseExpr();
            if (!AcceptName(K_equals)) Expected("'equals'");
            c->c = ParseExpr();
            if (AcceptName(K_into)) {
                c->names = NewArr<Name *>(1);
                c->names[0] = ExpectIdent();
            }
            Add(cl, c);
        } else if (AcceptName(K_orderby)) {
            Node *c = Mk(Q_ORDERBY, at);
            List keys = { NULL, 0, 0 };
            for (;;) {
                Node *k = ParseExpr();
                if (AcceptName(K_descending)) k->flags |= F_DESC;
                else AcceptName(K_ascending);
                // keys carry their own F_DESC; wrap so the flag can't clash
                Node *w = Mk(Q_ORDERBY, k->pos);
                w->a = k;
                w->flags = k->flags & F_DESC;
                k->flags &= ~F_DESC;
                Add(keys, w);
                if (!Accept(TK_COMMA)) break;
            }
            SetList(c, keys);
            Add(cl, c);
        } else if (AcceptName(K_select)) {
            Node *c = Mk(Q_SELECT, at);
            c->a = ParseExpr();
            Add(cl, c);
            if (IsName(K_into)) { Next(); Node *in = Mk(Q_INTO, Pos()); in->name = ExpectIdent(); Add(cl, in); continue; }
            break;
        } else if (AcceptName(K_group)) {
            Node *c = Mk(Q_GROUP, at);
            c->a = ParseExpr();
            if (!AcceptName(K_by)) Expected("'by'");
            c->b = ParseExpr();
            Add(cl, c);
            if (IsName(K_into)) { Next(); Node *in = Mk(Q_INTO, Pos()); in->name = ExpectIdent(); Add(cl, in); continue; }
            break;
        } else {
            Expected("'select' or 'group'");
        }
    }
    SetList(q, cl);
    return q;
}

// ---- primary ----

static Node *ParsePrimaryBase()
{
    Tok &t = Cur();
    int at = t.pos;
    switch (t.k) {
    case TK_NUM:
    case TK_CHARLIT:
    case TK_STR: {
        Next();
        return Lit(t.lit, at);
    }
    case TK_INTERP:
        Next();
        return ParseInterp(t);
    case TK_LBRACK: {
        // collection expression [1, 2, ..xs]
        Node *e = Mk(N_COLLECTION, at);
        Next();
        List items = { NULL, 0, 0 };
        while (!Is(TK_RBRACK)) {
            if (Is(TK_RANGE)) {
                Node *sp = Mk(N_RANGEX, Pos());
                Next();
                sp->a = ParseExpr();
                Add(items, sp);
            } else {
                Add(items, ParseExpr());
            }
            if (!Accept(TK_COMMA)) break;
        }
        Expect(TK_RBRACK);
        SetList(e, items);
        e->op = 1;                                  // [ ] form
        return e;
    }
    case TK_LPAR: {
        // cast: (int)x, (double)(a + b), (int[])o
        if (IsAt(1, TK_NAME) && !At(1).verbatim && PrimOf(At(1).name->id) >= 0 &&
            At(1).name->id != K_var && At(1).name->id != K_dynamic) {
            int save = P->i;
            Next();
            TypeRef *ty = TryType(true);
            if (ty && Accept(TK_RPAR)) {
                Node *e = Mk(N_CAST, at);
                e->type = ty;
                e->a = ParseUnary();
                if (IsConst(e->a) && ty->rank == 0 && !ty->nullable) e->flags |= F_CONST;
                return e;
            }
            P->i = save;
        }
        Next();
        // tuple or parenthesised expression
        Name *nm = NULL;
        if (Is(TK_NAME) && IsAt(1, TK_COLON)) {
            nm = ExpectIdent();
            Next();
        }
        Node *first = ParseExpr();
        if (Is(TK_COMMA)) {
            Node *e = Mk(N_TUPLE, at);
            List items = { NULL, 0, 0 };
            NameList names = { NULL, NULL, 0, 0 };
            Add(items, first);
            AddName(names, nm, NULL);
            while (Accept(TK_COMMA)) {
                Name *n2 = NULL;
                if (Is(TK_NAME) && IsAt(1, TK_COLON)) {
                    n2 = ExpectIdent();
                    Next();
                }
                Add(items, ParseExpr());
                AddName(names, n2, NULL);
            }
            Expect(TK_RPAR);
            if (items.n > 7)
                CompileError(at, "Tuples of more than 7 elements are not supported");
            SetList(e, items);
            e->names = names.v;
            // implicit names: (p.X, y) -> X, y
            for (int k = 0; k < items.n; k++)
                if (!e->names[k] && (items.v[k]->k == N_NAME || items.v[k]->k == N_MEMBER))
                    e->names[k] = items.v[k]->name;
            return e;
        }
        if (nm)
            CompileError(at, "A tuple must have at least two elements");
        Expect(TK_RPAR);
        return first;
    }
    case TK_NAME:
        break;
    default:
        Expected("Expression");
    }

    // names and keywords
    int id = t.verbatim ? K_NONE : t.name->id;
    switch (id) {
    case K_true: Next(); return Lit(VBool(true), at);
    case K_false: Next(); return Lit(VBool(false), at);
    case K_null: Next(); return Lit(VNull(), at);
    case K_new:
        Next();
        return ParseNew(at);
    case K_default: {
        Next();
        Node *e = Mk(N_DEFAULT, at);
        if (Accept(TK_LPAR)) {
            e->type = ExpectType();
            CheckKnownType(e->type, at);
            Expect(TK_RPAR);
            e->flags |= F_CONST;
        }
        return e;
    }
    case K_typeof: {
        Next();
        Node *e = Mk(N_TYPEOF, at);
        Expect(TK_LPAR);
        e->type = ExpectType();
        Expect(TK_RPAR);
        return e;
    }
    case K_nameof: {
        if (!IsAt(1, TK_LPAR)) break;
        Next();
        Next();
        Node *e = Mk(N_NAMEOF, at);
        Node *x = ParseExpr();
        Expect(TK_RPAR);
        if (x->k != N_NAME && x->k != N_MEMBER)
            CompileError(at, "This expression does not have a name");
        Value v;
        v.t = T_STRING;
        v.s = StrFromUtf8(x->name->s, x->name->len);
        e->lit = v;
        e->k = N_LIT;
        e->flags = F_CONST;
        return e;
    }
    case K_checked:
    case K_unchecked: {
        Next();
        Node *e = Mk(N_CHECKED, at);
        e->op = id == K_checked;
        Expect(TK_LPAR);
        e->a = ParseExpr();
        Expect(TK_RPAR);
        if (IsConst(e->a)) e->flags |= F_CONST;
        return e;
    }
    case K_throw: {
        Next();
        Node *e = Mk(N_THROWX, at);
        e->a = ParseExpr();
        return e;
    }
    case K_this: case K_base:
        CompileError(at, "'%s' is not available here", t.name->s);
    default:
        break;
    }
    if (id != K_NONE && PrimOf(id) >= 0 && id != K_var && id != K_dynamic) {
        // int.MaxValue, string.Join(...): the type as a value
        Next();
        Node *e = Mk(N_NAME, at);
        e->name = t.name;
        return e;
    }
    if (Reserved(t))
        CompileError(at, "Unexpected keyword '%s'", t.name->s);
    Next();
    Node *e = Mk(N_NAME, at);
    e->name = t.name;
    TryTypeArgs(&e->targs, &e->ntargs);
    return e;
}

static Node *ParsePostfix(Node *e)
{
    for (;;) {
        int at = Pos();
        if (Is(TK_DOT) || Is(TK_QDOT)) {
            bool cond = Is(TK_QDOT);
            Next();
            Node *m = Mk(N_MEMBER, at);
            m->a = e;
            Tok &nt = Cur();
            if (nt.k != TK_NAME)
                Expected("Identifier");
            Next();
            m->name = nt.name;
            if (cond) m->flags |= F_NULLCOND;
            TryTypeArgs(&m->targs, &m->ntargs);
            if (IsConst(e) && e->k == N_NAME) {
                // int.MaxValue etc. are constants
                int nid = nt.name->id;
                if (nid == K_MaxValue || nid == K_MinValue || nid == K_PI || nid == K_E ||
                    nid == K_NaN || nid == K_PositiveInfinity || nid == K_NegativeInfinity || nid == K_Epsilon)
                    m->flags |= F_CONST;
            } else if (e->k == N_NAME && e->name->id != K_NONE && PrimOf(e->name->id) >= 0) {
                int nid = nt.name->id;
                if (nid == K_MaxValue || nid == K_MinValue || nid == K_NaN || nid == K_Epsilon ||
                    nid == K_PositiveInfinity || nid == K_NegativeInfinity)
                    m->flags |= F_CONST;
            } else if (e->k == N_NAME && e->name->id == K_Math && (nt.name->id == K_PI || nt.name->id == K_E || nt.name->id == K_Tau)) {
                m->flags |= F_CONST;
            }
            e = m;
        } else if (Is(TK_LPAR)) {
            Next();
            Node *c = Mk(N_CALL, at);
            c->a = e;
            ParseArgs(c, TK_RPAR);
            e = c;
        } else if (Is(TK_LBRACK) || Is(TK_QLBRACK)) {
            bool cond = Is(TK_QLBRACK);
            Next();
            Node *ix = Mk(N_INDEX, at);
            ix->a = e;
            if (cond) ix->flags |= F_NULLCOND;
            List args = { NULL, 0, 0 };
            for (;;) {
                if (Is(TK_TILDE) || (Is(TK_CARET))) {
                    // ^1: index from the end
                    int hp = Pos();
                    Next();
                    Node *h = Mk(N_UNARY, hp);
                    h->op = TK_CARET;
                    h->a = ParseShift();
                    Add(args, h);
                } else if (Is(TK_RANGE) || IsAt(1, TK_RANGE)) {
                    // a..b ranges
                    Node *r = Mk(N_RANGEX, Pos());
                    if (!Is(TK_RANGE)) r->a = ParseShift();
                    Expect(TK_RANGE);
                    if (!Is(TK_RBRACK)) {
                        if (Is(TK_CARET)) {
                            int hp = Pos();
                            Next();
                            Node *h = Mk(N_UNARY, hp);
                            h->op = TK_CARET;
                            h->a = ParseShift();
                            r->b = h;
                        } else {
                            r->b = ParseShift();
                        }
                    }
                    Add(args, r);
                } else {
                    Add(args, ParseExpr());
                }
                if (!Accept(TK_COMMA)) break;
            }
            Expect(TK_RBRACK);
            SetList(ix, args);
            e = ix;
        } else if (Is(TK_INC) || Is(TK_DEC)) {
            Node *u = Mk(Is(TK_INC) ? N_POSTINC : N_POSTDEC, at);
            Next();
            u->a = e;
            e = u;
        } else if (Is(TK_NOT) && (IsAt(1, TK_DOT) || IsAt(1, TK_RPAR) || IsAt(1, TK_SEMI) || IsAt(1, TK_COMMA) || IsAt(1, TK_LBRACK))) {
            Next();                                 // null-forgiving x!
        } else {
            return e;
        }
    }
}

static Node *ParsePrimary()
{
    return ParsePostfix(ParsePrimaryBase());
}

static Node *ParseUnary()
{
    int at = Pos();
    int k = Cur().k;
    if (k == TK_PLUS || k == TK_MINUS || k == TK_NOT || k == TK_TILDE) {
        Next();
        Node *a = ParseUnary();
        if (k == TK_MINUS && a->k == N_LIT && (a->flags & F_CONST)) {
            // -2147483648 and -9223372036854775808 are int and long
            if (a->lit.t == T_UINT && a->lit.u == 0x80000000u) {
                a->lit = VInt(-2147483647 - 1);
                a->pos = at;
                return a;
            }
            if (a->lit.t == T_ULONG && a->lit.u == 0x8000000000000000ull) {
                a->lit.t = T_LONG;
                a->pos = at;
                return a;
            }
        }
        Node *e = Mk(N_UNARY, at);
        e->op = (uint8_t)k;
        e->a = a;
        if (IsConst(a)) e->flags |= F_CONST;
        return e;
    }
    if (k == TK_INC || k == TK_DEC) {
        Next();
        Node *e = Mk(k == TK_INC ? N_PREINC : N_PREDEC, at);
        e->a = ParseUnary();
        return e;
    }
    if (k == TK_CARET) {
        Next();
        Node *e = Mk(N_UNARY, at);
        e->op = TK_CARET;
        e->a = ParseUnary();
        return e;
    }
    Node *e = ParsePrimary();
    // x switch { ... }
    while (IsName(K_switch)) {
        int sp = Pos();
        Next();
        Node *sw = Mk(N_SWITCHX, sp);
        sw->a = e;
        Expect(TK_LBRACE);
        List arms = { NULL, 0, 0 };
        while (!Is(TK_RBRACE)) {
            Node *arm = Mk(N_COND, Pos());
            arm->a = ParsePattern();
            if (AcceptName(K_when))
                arm->b = ParseExpr();
            Expect(TK_ARROW);
            arm->c = ParseExpr();
            Add(arms, arm);
            if (!Accept(TK_COMMA)) break;
        }
        Expect(TK_RBRACE);
        SetList(sw, arms);
        e = sw;
    }
    return e;
}

// '>' '>' as a shift, when the two tokens touch
static int ShiftOp(int *aLen)
{
    if (Is(TK_SHL)) { *aLen = 1; return TK_SHL; }
    if (Is(TK_GT) && IsAt(1, TK_GT) && At(1).pos == Cur().end) {
        if (IsAt(2, TK_GT) && At(2).pos == At(1).end) { *aLen = 3; return TK_USHR; }
        *aLen = 2;
        return TK_SHR;
    }
    return 0;
}

static Node *Bin(int aOp, Node *a, Node *b, int aPos)
{
    Node *e = Mk(N_BINARY, aPos);
    e->op = (uint8_t)aOp;
    e->a = a;
    e->b = b;
    if (IsConst(a) && IsConst(b)) e->flags |= F_CONST;
    return e;
}

static Node *ParseMul()
{
    Node *e = ParseUnary();
    for (;;) {
        int k = Cur().k;
        if (k != TK_STAR && k != TK_SLASH && k != TK_PERCENT) return e;
        int at = Pos();
        Next();
        e = Bin(k, e, ParseUnary(), at);
    }
}

static Node *ParseAdd()
{
    Node *e = ParseMul();
    for (;;) {
        int k = Cur().k;
        if (k != TK_PLUS && k != TK_MINUS) return e;
        int at = Pos();
        Next();
        e = Bin(k, e, ParseMul(), at);
    }
}

static Node *ParseShift()
{
    Node *e = ParseAdd();
    for (;;) {
        int len;
        int op = ShiftOp(&len);
        if (!op) return e;
        // but not >>= (an assignment)
        if (op != TK_SHL && IsAt(len, TK_EQ) && At(len).pos == At(len - 1).end) return e;
        if (op != TK_SHL && IsAt(len - 1, TK_GE)) return e;
        int at = Pos();
        for (int k = 0; k < len; k++) Next();
        e = Bin(op, e, ParseAdd(), at);
    }
}

// ---- patterns: x is int n, switch arms ----

static Node *ParsePrimaryPattern()
{
    int at = Pos();
    if (Accept(TK_LPAR)) {
        Node *p = ParsePattern();
        Expect(TK_RPAR);
        return p;
    }
    if (Is(TK_NAME) && !Cur().verbatim && Cur().name->len == 1 && Cur().name->s[0] == '_' &&
        (IsAt(1, TK_ARROW) || IsAt(1, TK_RPAR) || IsAt(1, TK_COMMA) || IsNameAt(1, K_when) || IsAt(1, TK_COLON))) {
        Next();
        return Mk(P_DISCARD, at);
    }
    if (IsName(K_var)) {
        Next();
        Node *p = Mk(P_VAR, at);
        p->name = ExpectIdent();
        return p;
    }
    int k = Cur().k;
    if (k == TK_LT || k == TK_GT || k == TK_LE || k == TK_GE) {
        Next();
        Node *p = Mk(P_REL, at);
        p->op = (uint8_t)k;
        p->a = ParseShift();
        return p;
    }
    if (IsName(K_null)) {
        Next();
        Node *p = Mk(P_CONST, at);
        p->a = Lit(VNull(), at);
        return p;
    }
    // type pattern: keyword types, known library types
    if (Is(TK_NAME) && !Cur().verbatim &&
        ((PrimOf(Cur().name->id) >= 0 && Cur().name->id != K_var) || TypeIdOf(Cur().name) >= 100)) {
        int save = P->i;
        TypeRef *ty = TryType(false);
        if (ty && !Is(TK_DOT) && !Is(TK_LPAR)) {
            Node *p = Mk(P_TYPE, at);
            p->type = ty;
            if (Is(TK_NAME) && !Reserved(Cur()) && !IsName(K_and) && !IsName(K_or) && !IsName(K_when))
                p->name = ExpectIdent();
            return p;
        }
        P->i = save;
    }
    Node *p = Mk(P_CONST, at);
    p->a = ParseShift();
    return p;
}

static Node *ParseNotPattern()
{
    int at = Pos();
    if (AcceptName(K_not)) {
        Node *p = Mk(P_NOT, at);
        p->a = ParseNotPattern();
        return p;
    }
    return ParsePrimaryPattern();
}

static Node *ParseAndPattern()
{
    Node *p = ParseNotPattern();
    while (IsName(K_and)) {
        int at = Pos();
        Next();
        Node *q = Mk(P_AND, at);
        q->a = p;
        q->b = ParseNotPattern();
        p = q;
    }
    return p;
}

static Node *ParsePattern()
{
    Node *p = ParseAndPattern();
    while (IsName(K_or)) {
        int at = Pos();
        Next();
        Node *q = Mk(P_OR, at);
        q->a = p;
        q->b = ParseAndPattern();
        p = q;
    }
    return p;
}

static Node *ParseRelational()
{
    Node *e = ParseShift();
    for (;;) {
        int k = Cur().k;
        int at = Pos();
        if (k == TK_LT || k == TK_GT || k == TK_LE || k == TK_GE) {
            // `y >>= 2` and `y >>>= 2` are assignments, not comparisons
            if (k == TK_GT && (IsAt(1, TK_GE) || IsAt(1, TK_GT)) && At(1).pos == Cur().end)
                return e;
            Next();
            e = Bin(k, e, ParseShift(), at);
        } else if (IsName(K_is)) {
            Next();
            Node *x = Mk(N_IS, at);
            x->a = e;
            x->b = ParsePattern();
            e = x;
        } else if (IsName(K_as)) {
            Next();
            Node *x = Mk(N_AS, at);
            x->a = e;
            x->type = ExpectType();
            e = x;
        } else {
            return e;
        }
    }
}

static Node *ParseEquality()
{
    Node *e = ParseRelational();
    for (;;) {
        int k = Cur().k;
        if (k != TK_EQEQ && k != TK_NE) return e;
        int at = Pos();
        Next();
        e = Bin(k, e, ParseRelational(), at);
    }
}

static Node *ParseBitAnd()
{
    Node *e = ParseEquality();
    while (Is(TK_AMP)) {
        int at = Pos();
        Next();
        e = Bin(TK_AMP, e, ParseEquality(), at);
    }
    return e;
}

static Node *ParseBitXor()
{
    Node *e = ParseBitAnd();
    while (Is(TK_CARET)) {
        int at = Pos();
        Next();
        e = Bin(TK_CARET, e, ParseBitAnd(), at);
    }
    return e;
}

static Node *ParseBitOr()
{
    Node *e = ParseBitXor();
    while (Is(TK_BAR)) {
        int at = Pos();
        Next();
        e = Bin(TK_BAR, e, ParseBitXor(), at);
    }
    return e;
}

static Node *ParseAndAnd()
{
    Node *e = ParseBitOr();
    while (Is(TK_ANDAND)) {
        int at = Pos();
        Next();
        Node *x = Mk(N_ANDAND, at);
        x->a = e;
        x->b = ParseBitOr();
        if (IsConst(x->a) && IsConst(x->b)) x->flags |= F_CONST;
        e = x;
    }
    return e;
}

static Node *ParseOrOr()
{
    Node *e = ParseAndAnd();
    while (Is(TK_OROR)) {
        int at = Pos();
        Next();
        Node *x = Mk(N_OROR, at);
        x->a = e;
        x->b = ParseAndAnd();
        if (IsConst(x->a) && IsConst(x->b)) x->flags |= F_CONST;
        e = x;
    }
    return e;
}

static Node *ParseCoalesce()
{
    Node *e = ParseOrOr();
    if (Is(TK_QQ)) {
        int at = Pos();
        Next();
        Node *x = Mk(N_COALESCE, at);
        x->a = e;
        x->b = ParseCoalesce();                     // right-associative
        return x;
    }
    return e;
}

static Node *ParseConditional()
{
    Node *e = ParseCoalesce();
    if (Is(TK_QUESTION)) {
        int at = Pos();
        Next();
        Node *x = Mk(N_COND, at);
        x->a = e;
        x->b = ParseExpr();
        Expect(TK_COLON);
        x->c = ParseExpr();
        if (IsConst(x->a) && IsConst(x->b) && IsConst(x->c)) x->flags |= F_CONST;
        return x;
    }
    return e;
}

static int AssignOp(int *aLen)
{
    *aLen = 1;
    switch (Cur().k) {
    case TK_EQ: return TK_EQ;
    case TK_PLUSEQ: return TK_PLUS;
    case TK_MINUSEQ: return TK_MINUS;
    case TK_STAREQ: return TK_STAR;
    case TK_SLASHEQ: return TK_SLASH;
    case TK_PERCENTEQ: return TK_PERCENT;
    case TK_AMPEQ: return TK_AMP;
    case TK_BAREQ: return TK_BAR;
    case TK_CARETEQ: return TK_CARET;
    case TK_SHLEQ: return TK_SHL;
    case TK_QQEQ: return TK_QQ;
    case TK_GT:
        // >>= and >>>=
        if (IsAt(1, TK_GE) && At(1).pos == Cur().end) { *aLen = 2; return TK_SHR; }
        if (IsAt(1, TK_GT) && IsAt(2, TK_GE) && At(1).pos == Cur().end && At(2).pos == At(1).end) { *aLen = 3; return TK_USHR; }
        return 0;
    default:
        return 0;
    }
}

static Node *ParseExpr()
{
    StackCheck();
    if (LambdaAhead())
        return ParseLambda();
    if ((IsName(K_static)) && (IsAt(1, TK_LPAR) || IsAt(1, TK_NAME))) {
        Next();
        return ParseLambda();
    }
    if (QueryAhead())
        return ParseQuery();
    Node *e = ParseConditional();
    int len;
    int op = AssignOp(&len);
    if (op) {
        int at = Pos();
        if (e->k != N_NAME && e->k != N_MEMBER && e->k != N_INDEX && e->k != N_TUPLE)
            CompileError(at, "The left-hand side of an assignment must be a variable, property or indexer");
        for (int k = 0; k < len; k++) Next();
        Node *x = Mk(N_ASSIGN, at);
        x->op = (uint8_t)(op == TK_EQ ? 0 : op);
        x->a = e;
        x->b = ParseExpr();                         // right-associative
        return x;
    }
    return e;
}

// ---------------------------------------------------------------------------
// statements

static Node *ParseEmbedded()
{
    Node *s = ParseStatement(false);
    if (s->k == S_VAR || s->k == S_FUNC)
        CompileError(s->pos, "Embedded statement cannot be a declaration or labeled statement");
    return s;
}

static Node *ParseBlock()
{
    Node *b = Mk(S_BLOCK, Pos());
    Expect(TK_LBRACE);
    List st = { NULL, 0, 0 };
    while (!Is(TK_RBRACE)) {
        if (Is(TK_EOF))
            Expected("'}'");
        Add(st, ParseStatement(false));
    }
    Next();
    SetList(b, st);
    return b;
}

// After the type and name: = init, more declarators, ';'
static Node *ParseVarRest(TypeRef *aType, Name *aFirst, int aPos)
{
    Node *d = Mk(S_VAR, aPos);
    d->type = aType;
    List decls = { NULL, 0, 0 };
    Name *nm = aFirst;
    for (;;) {
        Node *one = Mk(S_VAR, Pos());
        one->name = nm;
        if (Accept(TK_EQ)) {
            if (Is(TK_LBRACE) && aType->rank > 0)
                one->a = ParseInitializer();        // int[] a = { 1, 2 }
            else
                one->a = ParseExpr();
        } else if (aType->isVar) {
            CompileError(one->pos, "Implicitly-typed variables must be initialized");
        }
        Add(decls, one);
        if (!Accept(TK_COMMA)) break;
        if (aType->isVar)
            CompileError(Pos(), "Implicitly-typed variables cannot have multiple declarators");
        nm = ExpectIdent();
    }
    SetList(d, decls);
    return d;
}

static Node *ParseLocalFunc(TypeRef *aRet, Name *aName, int aPos)
{
    Node *f = Mk(S_FUNC, aPos);
    f->name = aName;
    f->type = aRet;
    Expect(TK_LPAR);
    NameList ps = { NULL, NULL, 0, 0 };
    while (!Is(TK_RPAR)) {
        AcceptName(K_params);
        TypeRef *ty = ExpectType();
        CheckKnownType(ty, Pos());
        Name *pn = ExpectIdent();
        if (Is(TK_EQ))
            CompileError(Pos(), "Optional parameters are not supported");
        AddName(ps, pn, ty);
        if (!Accept(TK_COMMA)) break;
    }
    Expect(TK_RPAR);
    f->names = ps.v;
    f->targs = ps.types;
    f->ntargs = ps.n;
    f->n = ps.n;
    int saveLoop = P->loopDepth;
    P->loopDepth = 0;
    if (Is(TK_LBRACE)) {
        f->flags |= F_BLOCKBODY;
        f->a = ParseBlock();
    } else {
        Expect(TK_ARROW);
        f->a = ParseExpr();
        Expect(TK_SEMI);
    }
    P->loopDepth = saveLoop;
    return f;
}

// A declaration (local variable or function) at the cursor, or NULL.
static Node *TryDeclaration()
{
    int save = P->i;
    int at = Pos();
    bool isConst = AcceptName(K_const);
    bool isStatic = AcceptName(K_static);
    (void)isStatic;
    if (!Is(TK_NAME) && !Is(TK_LPAR)) { P->i = save; return NULL; }
    TypeRef *ty = TryType(true);
    if (!ty || !Is(TK_NAME) || Reserved(Cur())) { P->i = save; return NULL; }
    int k1 = At(1).k;
    if (k1 != TK_EQ && k1 != TK_SEMI && k1 != TK_COMMA && k1 != TK_LPAR && !(isConst)) {
        P->i = save;
        return NULL;
    }
    // `x y;` with an unknown type x is still a declaration (error below)
    Name *nm = ExpectIdent();
    if (Is(TK_LPAR)) {
        CheckKnownType(ty, at);
        return ParseLocalFunc(ty, nm, at);
    }
    if (ty->id == T_NULL && !ty->rank)
        CompileError(at, "A variable cannot be of type void");
    CheckKnownType(ty, at);
    Node *d = ParseVarRest(ty, nm, at);
    Expect(TK_SEMI);
    return d;
}

static Node *ParseSwitchStatement(int at)
{
    Node *sw = Mk(S_SWITCH, at);
    Expect(TK_LPAR);
    sw->a = ParseExpr();
    Expect(TK_RPAR);
    Expect(TK_LBRACE);
    List sections = { NULL, 0, 0 };
    P->loopDepth++;
    while (!Is(TK_RBRACE)) {
        Node *sec = Mk(S_BLOCK, Pos());
        Node *labels = Mk(N_TUPLE, Pos());
        List ls = { NULL, 0, 0 };
        while (IsName(K_case) || IsName(K_default)) {
            Node *lab = Mk(N_COND, Pos());
            if (AcceptName(K_default)) {
                lab->a = NULL;
            } else {
                Next();
                lab->a = ParsePattern();
                if (AcceptName(K_when))
                    lab->b = ParseExpr();
            }
            Expect(TK_COLON);
            Add(ls, lab);
        }
        if (!ls.n)
            Expected("'case' or 'default'");
        SetList(labels, ls);
        sec->b = labels;
        List st = { NULL, 0, 0 };
        while (!IsName(K_case) && !IsName(K_default) && !Is(TK_RBRACE)) {
            if (Is(TK_EOF)) Expected("'}'");
            Add(st, ParseStatement(false));
        }
        SetList(sec, st);
        Add(sections, sec);
    }
    P->loopDepth--;
    Next();
    SetList(sw, sections);
    return sw;
}

static Node *ParseStatement(bool aTop)
{
    StackCheck();
    int at = Pos();
    Tok &t = Cur();
    if (t.k == TK_LBRACE)
        return ParseBlock();
    if (t.k == TK_SEMI) {
        Next();
        return Mk(S_EMPTY, at);
    }
    int id = (t.k == TK_NAME && !t.verbatim) ? t.name->id : K_NONE;
    switch (id) {
    case K_if: {
        Next();
        Node *s = Mk(S_IF, at);
        Expect(TK_LPAR);
        s->a = ParseExpr();
        Expect(TK_RPAR);
        s->b = ParseEmbedded();
        if (AcceptName(K_else))
            s->c = ParseEmbedded();
        return s;
    }
    case K_while: {
        Next();
        Node *s = Mk(S_WHILE, at);
        Expect(TK_LPAR);
        s->a = ParseExpr();
        Expect(TK_RPAR);
        P->loopDepth++;
        s->c = ParseEmbedded();
        P->loopDepth--;
        return s;
    }
    case K_do: {
        Next();
        Node *s = Mk(S_DO, at);
        P->loopDepth++;
        s->c = ParseEmbedded();
        P->loopDepth--;
        if (!AcceptName(K_while)) Expected("'while'");
        Expect(TK_LPAR);
        s->a = ParseExpr();
        Expect(TK_RPAR);
        Expect(TK_SEMI);
        return s;
    }
    case K_for: {
        Next();
        Node *s = Mk(S_FOR, at);
        Expect(TK_LPAR);
        if (!Is(TK_SEMI)) {
            int save = P->i;
            TypeRef *ty = TryType(true);
            if (ty && Is(TK_NAME) && !Reserved(Cur()) && (IsAt(1, TK_EQ) || IsAt(1, TK_COMMA) || IsAt(1, TK_SEMI))) {
                Name *nm = ExpectIdent();
                CheckKnownType(ty, at);
                s->d = ParseVarRest(ty, nm, at);
            } else {
                P->i = save;
                Node *list = Mk(N_TUPLE, Pos());
                List xs = { NULL, 0, 0 };
                for (;;) {
                    Add(xs, ParseExpr());
                    if (!Accept(TK_COMMA)) break;
                }
                SetList(list, xs);
                s->d = list;
            }
        }
        Expect(TK_SEMI);
        if (!Is(TK_SEMI)) s->a = ParseExpr();
        Expect(TK_SEMI);
        if (!Is(TK_RPAR)) {
            Node *list = Mk(N_TUPLE, Pos());
            List xs = { NULL, 0, 0 };
            for (;;) {
                Add(xs, ParseExpr());
                if (!Accept(TK_COMMA)) break;
            }
            SetList(list, xs);
            s->b = list;
        }
        Expect(TK_RPAR);
        P->loopDepth++;
        s->c = ParseEmbedded();
        P->loopDepth--;
        return s;
    }
    case K_foreach: {
        Next();
        Node *s = Mk(S_FOREACH, at);
        Expect(TK_LPAR);
        if (Is(TK_LPAR))
            CompileError(Pos(), "Deconstruction in foreach is not supported; use item.Item1");
        s->type = ExpectType();
        CheckKnownType(s->type, at);
        s->name = ExpectIdent();
        if (!AcceptName(K_in)) Expected("'in'");
        s->a = ParseExpr();
        Expect(TK_RPAR);
        P->loopDepth++;
        s->c = ParseEmbedded();
        P->loopDepth--;
        return s;
    }
    case K_break:
    case K_continue: {
        Next();
        if (!P->loopDepth)
            CompileError(at, "No enclosing loop out of which to break or continue");
        Expect(TK_SEMI);
        return Mk(id == K_break ? S_BREAK : S_CONTINUE, at);
    }
    case K_return: {
        Next();
        Node *s = Mk(S_RETURN, at);
        if (!Is(TK_SEMI)) s->a = ParseExpr();
        Expect(TK_SEMI);
        return s;
    }
    case K_throw: {
        Next();
        Node *s = Mk(S_THROW, at);
        if (!Is(TK_SEMI)) s->a = ParseExpr();
        Expect(TK_SEMI);
        return s;
    }
    case K_switch:
        if (IsAt(1, TK_LPAR)) {
            Next();
            return ParseSwitchStatement(at);
        }
        break;
    case K_checked:
    case K_unchecked:
        if (IsAt(1, TK_LBRACE)) {
            Next();
            Node *b = ParseBlock();
            b->op = id == K_checked ? 1 : 2;
            return b;
        }
        break;
    case K_try: case K_class: case K_struct: case K_namespace: case K_goto:
    case K_lock: case K_yield:
        CompileError(at, "'%s' is not supported in rSharp", t.name->s);
    case K_using:
        CompileError(at, "'using' must come before the code");
    default:
        break;
    }
    if (Node *d = TryDeclaration())
        return d;
    // var (a, b) = ...
    if (IsName(K_var) && IsAt(1, TK_LPAR))
        CompileError(at, "Deconstruction is not supported; use t.Item1, t.Item2");
    Node *s = Mk(S_EXPR, at);
    s->a = ParseExpr();
    if (aTop && Is(TK_EOF)) {
        s->flags |= F_RESULT;
        return s;
    }
    Expect(TK_SEMI);
    return s;
}

// ---------------------------------------------------------------------------

Program Parse(const rs_char *aSrc, int aLen)
{
    Lexed lx = Lex(aSrc, 0, aLen);
    Parser p;
    p.src = aSrc;
    p.t = lx.t;
    p.n = lx.n;
    p.i = 0;
    p.loopDepth = 0;
    Parser *saved = P;
    P = &p;
    // using System; using System.Linq; using static System.Math; ...
    while (IsName(K_using) && !IsAt(1, TK_LPAR)) {
        while (!Is(TK_SEMI) && !Is(TK_EOF)) Next();
        Expect(TK_SEMI);
    }
    Node *body = Mk(S_BLOCK, Pos());
    List st = { NULL, 0, 0 };
    while (!Is(TK_EOF))
        Add(st, ParseStatement(true));
    SetList(body, st);
    P = saved;
    Program prog;
    prog.body = body;
    return prog;
}

Node *ParseExprRange(const rs_char *aSrc, int aFrom, int aTo)
{
    Lexed lx = Lex(aSrc, aFrom, aTo);
    Parser p;
    p.src = aSrc;
    p.t = lx.t;
    p.n = lx.n;
    p.i = 0;
    p.loopDepth = 0;
    Parser *saved = P;
    P = &p;
    if (Is(TK_EOF))
        CompileError(aFrom, "Empty expression in interpolated string");
    Node *e = ParseExpr();
    if (!Is(TK_EOF))
        Expected("'}'");
    P = saved;
    return e;
}

} // namespace rs
