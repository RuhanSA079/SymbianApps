/*
 * rs_eval.cpp: values, C#'s numeric rules, conversions, equality and
 * ordering, scopes, patterns, statements and expressions.
 *
 * The interpreter is dynamically typed, but values keep their C# type
 * (int, uint, long, double, ...) and follow C#'s rules: binary numeric
 * promotion, unchecked wrap-around (OverflowException inside checked),
 * integer division, shifts masked to the operand width. Variables keep the
 * type they were declared with, so `double d = 1; d / 2` is 0.5.
 */
#include "rs_int.h"
#include <math.h>
#include <stdio.h>

namespace rs {

bool gChecked;
static bool gUnchecked;     // inside unchecked(...): constants may overflow
static bool gConstEval;     // evaluating a constant expression (checked at compile time)

void ResetEvalFlags()
{
    gChecked = gUnchecked = gConstEval = false;
}

// ---------------------------------------------------------------------------
// values

int Bits(int t)
{
    switch (t) {
    case T_SBYTE: case T_BYTE: return 8;
    case T_CHAR: case T_SHORT: case T_USHORT: return 16;
    case T_INT: case T_UINT: case T_FLOAT: return 32;
    default: return 64;
    }
}

Value VNum(int aTy, i64 aBits)
{
    Value v;
    v.t = (uint8_t)aTy;
    switch (aTy) {
    case T_SBYTE: v.i = (int8_t)aBits; break;
    case T_BYTE: v.u = (uint8_t)aBits; break;
    case T_SHORT: v.i = (int16_t)aBits; break;
    case T_USHORT: case T_CHAR: v.u = (uint16_t)aBits; break;
    case T_INT: v.i = (int32_t)aBits; break;
    case T_UINT: v.u = (uint32_t)aBits; break;
    default: v.i = aBits; break;
    }
    return v;
}

Value VReal(int aTy, double x)
{
    Value v;
    v.t = (uint8_t)aTy;
    v.d = aTy == T_FLOAT ? (double)(float)x : x;
    return v;
}

Value VStr(Str *s)
{
    Value v;
    if (!s) return VNull();
    v.t = T_STRING;
    v.s = s;
    return v;
}

Value VTy(int aType)
{
    Value v;
    v.t = T_TYPE;
    v.i = aType;
    return v;
}

const char *TyName(int t)
{
    switch (t) {
    case T_NULL: return "null";
    case T_BOOL: return "bool";
    case T_CHAR: return "char";
    case T_SBYTE: return "sbyte";
    case T_BYTE: return "byte";
    case T_SHORT: return "short";
    case T_USHORT: return "ushort";
    case T_INT: return "int";
    case T_UINT: return "uint";
    case T_LONG: return "long";
    case T_ULONG: return "ulong";
    case T_FLOAT: return "float";
    case T_DOUBLE: return "double";
    case T_STRING: return "string";
    case T_ARRAY: return "array";
    case T_LIST: return "List";
    case T_SEQ: return "IEnumerable";
    case T_FUNC: return "Func";
    case T_OBJ: return "object";
    case T_GROUP: return "IGrouping";
    case T_DICT: return "Dictionary";
    case T_SET: return "HashSet";
    case T_SB: return "StringBuilder";
    case T_ENUM: return "enum";
    case T_TYPE: return "Type";
    case T_NS: return "namespace";
    default: return "object";
    }
}

const char *TooBigFor(int t)
{
    switch (t) {
    case T_SBYTE: return "a signed byte";
    case T_BYTE: return "an unsigned byte";
    case T_SHORT: return "an Int16";
    case T_USHORT: return "a UInt16";
    case T_CHAR: return "a character";
    case T_UINT: return "a UInt32";
    case T_LONG: return "an Int64";
    case T_ULONG: return "a UInt64";
    default: return "an Int32";
    }
}

const char *ValTyName(const Value &v)
{
    if (v.t == T_OBJ) {
        switch (v.o->kind) {
        case OK_ANON: return "anonymous type";
        case OK_TUPLE: return "tuple";
        case OK_KVP: return "KeyValuePair";
        default: return "Exception";
        }
    }
    return TyName(v.t);
}

// ---------------------------------------------------------------------------
// strings

static Str gEmpty;
const Str *EmptyStr() { return &gEmpty; }

Str *NewStr(int aLen)
{
    Str *s = (Str *)Alloc(sizeof(Str) + sizeof(rs_char) * (aLen > 0 ? aLen : 0));
    s->n = aLen;
    return s;
}

Str *StrFromAscii(const char *a)
{
    int n = strlen(a);
    Str *s = NewStr(n);
    for (int i = 0; i < n; i++) s->c[i] = (unsigned char)a[i];
    return s;
}

Str *StrFromUtf8(const char *a, int aLen)
{
    Str *s = NewStr(aLen);
    int n = 0;
    for (int i = 0; i < aLen; ) {
        unsigned char c = a[i];
        unsigned cp;
        if (c < 0x80) { cp = c; i++; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < aLen) { cp = ((c & 0x1F) << 6) | (a[i + 1] & 0x3F); i += 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < aLen) { cp = ((c & 0x0F) << 12) | ((a[i + 1] & 0x3F) << 6) | (a[i + 2] & 0x3F); i += 3; }
        else { cp = '?'; i++; }
        s->c[n++] = (rs_char)cp;
    }
    s->n = n;
    return s;
}

Str *StrSub(const Str *s, int aFrom, int aLen)
{
    Str *r = NewStr(aLen);
    memcpy(r->c, s->c + aFrom, aLen * sizeof(rs_char));
    return r;
}

Str *StrCat(const Str *a, const Str *b)
{
    Str *r = NewStr(a->n + b->n);
    memcpy(r->c, a->c, a->n * sizeof(rs_char));
    memcpy(r->c + a->n, b->c, b->n * sizeof(rs_char));
    return r;
}

bool StrEq(const Str *a, const Str *b)
{
    return a->n == b->n && !memcmp(a->c, b->c, a->n * sizeof(rs_char));
}

int StrCmpOrdinal(const Str *a, const Str *b)
{
    int n = a->n < b->n ? a->n : b->n;
    for (int i = 0; i < n; i++)
        if (a->c[i] != b->c[i])
            return a->c[i] < b->c[i] ? -1 : 1;
    return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
}

// Close to .NET's culture-aware (ICU) order: punctuation and symbols, then
// digits, then letters alphabetically; case only breaks ties, lower first.
static int Primary(rs_char c)
{
    if (c >= 'a' && c <= 'z') return 2000 + (c - 'a') * 4;
    if (c >= 'A' && c <= 'Z') return 2000 + (c - 'A') * 4;
    if (c >= '0' && c <= '9') return 1000 + (c - '0');
    if (c >= 0x80) return 3000 + c;
    return c;
}

int StrCmpCulture(const Str *a, const Str *b)
{
    int n = a->n < b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        int pa = Primary(a->c[i]), pb = Primary(b->c[i]);
        if (pa != pb)
            return pa < pb ? -1 : 1;
    }
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = 0; i < n; i++) {
        rs_char ca = a->c[i], cb = b->c[i];
        if (ca != cb) {
            bool la = ca >= 'a' && ca <= 'z', lb = cb >= 'a' && cb <= 'z';
            if (la != lb) return la ? -1 : 1;
            return ca < cb ? -1 : 1;
        }
    }
    return 0;
}

char *StrUtf8(const Str *s)
{
    char *out = (char *)Alloc(s->n * 3 + 1);
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        unsigned c = s->c[i];
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = 0;
    return out;
}

// ---------------------------------------------------------------------------
// arrays, maps, objects

Arr *NewArr(int aCap, int aElemTy)
{
    Arr *a = New<Arr>();
    a->cap = aCap > 0 ? aCap : 4;
    a->v = NewArr<Value>(a->cap);
    a->et = (uint8_t)aElemTy;
    return a;
}

void ArrPush(Arr *a, Value v)
{
    if (a->n == a->cap) {
        int cap = a->cap * 2;
        Value *nv = NewArr<Value>(cap);
        memcpy(nv, a->v, a->n * sizeof(Value));
        a->v = nv;
        a->cap = cap;
    }
    a->v[a->n++] = v;
}

Value *ArrAt(Arr *a, i64 aIndex)
{
    if (aIndex < 0 || aIndex >= a->n)
        Throw("IndexOutOfRangeException", "Index was outside the bounds of the array (index %lld, length %d).",
              (long long)aIndex, a->n);
    return &a->v[aIndex];
}

Map *NewMap(bool aDict)
{
    Map *m = New<Map>();
    m->keys = NewArr(8, T_OBJECT);
    m->vals = aDict ? NewArr(8, T_OBJECT) : NULL;
    m->kt = m->vt = T_OBJECT;
    return m;
}

static void MapRehash(Map *m, int aCap)
{
    m->cap = aCap;
    m->slots = NewArr<int>(aCap);
    for (int i = 0; i < m->keys->n; i++) {
        if (m->keys->v[i].t == T_COUNT) continue;           // removed
        unsigned h = ValHash(m->keys->v[i]) & (aCap - 1);
        while (m->slots[h]) h = (h + 1) & (aCap - 1);
        m->slots[h] = i + 1;
    }
}

int MapFind(Map *m, Value k)
{
    if (!m->cap) return -1;
    unsigned h = ValHash(k) & (m->cap - 1);
    while (m->slots[h]) {
        int at = m->slots[h] - 1;
        if (at >= 0 && m->keys->v[at].t != T_COUNT && ValEquals(m->keys->v[at], k))
            return at;
        h = (h + 1) & (m->cap - 1);
    }
    return -1;
}

int MapAdd(Map *m, Value k, Value v, bool aReplace)
{
    if (k.t == T_NULL)
        Throw("ArgumentNullException", "Value cannot be null. (Parameter 'key')");
    int at = MapFind(m, k);
    if (at >= 0) {
        if (!aReplace) return -1;
        if (m->vals) m->vals->v[at] = v;
        return at;
    }
    if ((m->keys->n + 1) * 2 > m->cap)
        MapRehash(m, m->cap ? m->cap * 2 : 16);
    ArrPush(m->keys, k);
    if (m->vals) ArrPush(m->vals, v);
    at = m->keys->n - 1;
    unsigned h = ValHash(k) & (m->cap - 1);
    while (m->slots[h]) h = (h + 1) & (m->cap - 1);
    m->slots[h] = at + 1;
    return at;
}

bool MapRemove(Map *m, Value k)
{
    int at = MapFind(m, k);
    if (at < 0) return false;
    // compact (keeps insertion order), then rebuild the index
    int n = m->keys->n;
    memmove(&m->keys->v[at], &m->keys->v[at + 1], (n - at - 1) * sizeof(Value));
    m->keys->n--;
    if (m->vals) {
        memmove(&m->vals->v[at], &m->vals->v[at + 1], (n - at - 1) * sizeof(Value));
        m->vals->n--;
    }
    MapRehash(m, m->cap);
    return true;
}

Obj *NewObj(int aKind, int aCount)
{
    Obj *o = New<Obj>();
    o->kind = (uint8_t)aKind;
    o->n = aCount;
    o->names = NewArr<Name *>(aCount);
    o->f = NewArr<Value>(aCount);
    return o;
}

Value VTuple2(Value a, Value b)
{
    Obj *o = NewObj(OK_TUPLE, 2);
    o->f[0] = a;
    o->f[1] = b;
    Value v;
    v.t = T_OBJ;
    v.o = o;
    return v;
}

Value VKvp(Value k, Value v)
{
    Obj *o = NewObj(OK_KVP, 2);
    o->names[0] = Intern("Key", 3);
    o->names[1] = Intern("Value", 5);
    o->f[0] = k;
    o->f[1] = v;
    Value r;
    r.t = T_OBJ;
    r.o = o;
    return r;
}

// ---------------------------------------------------------------------------
// numbers

i64 ToI64(Value v, int aPos)
{
    if (!IsIntegral(v.t))
        ThrowAt(aPos, "InvalidCastException", "Expected an integer, not %s", ValTyName(v));
    return v.i;
}

double ToDouble(Value v)
{
    switch (v.t) {
    case T_FLOAT: case T_DOUBLE: return v.d;
    case T_ULONG: return (double)v.u;
    default: return IsIntegral(v.t) ? (double)v.i : 0;
    }
}

int ToIndex(Value v, int aPos)
{
    if (v.t == T_INT || v.t == T_SHORT || v.t == T_USHORT || v.t == T_BYTE || v.t == T_SBYTE || v.t == T_CHAR)
        return (int)v.i;
    if (v.t == T_UINT || v.t == T_LONG || v.t == T_ULONG) {
        if (v.t == T_ULONG ? v.u > 0x7FFFFFFF : (v.i < -0x7FFFFFFFLL || v.i > 0x7FFFFFFF))
            ThrowAt(aPos, "OverflowException", "Arithmetic operation resulted in an overflow.");
        return (int)v.i;
    }
    ThrowAt(aPos, "InvalidCastException", "Cannot implicitly convert type '%s' to 'int'", ValTyName(v));
}

static int UnaryPromote(int t)
{
    switch (t) {
    case T_SBYTE: case T_BYTE: case T_SHORT: case T_USHORT: case T_CHAR: return T_INT;
    default: return t;
    }
}

static int Promote(int a, int b)
{
    if (a == T_DOUBLE || b == T_DOUBLE) return T_DOUBLE;
    if (a == T_FLOAT || b == T_FLOAT) return T_FLOAT;
    if (a == T_ULONG || b == T_ULONG) return T_ULONG;
    if (a == T_LONG || b == T_LONG) return T_LONG;
    if (a == T_UINT || b == T_UINT) {
        int o = a == T_UINT ? b : a;
        return (o == T_SBYTE || o == T_SHORT || o == T_INT) ? T_LONG : T_UINT;
    }
    return T_INT;
}

RS_NORETURN static void Overflow(int aPos)
{
    if (gConstEval)
        CompileError(aPos, "The operation overflows at compile time in checked mode (use unchecked(...))");
    ThrowAt(aPos, "OverflowException", "Arithmetic operation resulted in an overflow.");
}

static bool FitsSigned(int t, i64 x)
{
    switch (t) {
    case T_SBYTE: return x >= -128 && x <= 127;
    case T_BYTE: return x >= 0 && x <= 255;
    case T_SHORT: return x >= -32768 && x <= 32767;
    case T_USHORT: case T_CHAR: return x >= 0 && x <= 65535;
    case T_INT: return x >= -2147483647LL - 1 && x <= 2147483647LL;
    case T_UINT: return x >= 0 && x <= 0xFFFFFFFFLL;
    case T_LONG: return true;
    case T_ULONG: return x >= 0;
    default: return true;
    }
}

static bool FitsUnsigned(int t, u64 x)
{
    switch (t) {
    case T_LONG: return x <= 0x7FFFFFFFFFFFFFFFull;
    case T_ULONG: return true;
    default: return x <= 0xFFFFFFFFull && FitsSigned(t, (i64)x);
    }
}

// value (integral) converted to integral type t, checked if aChecked
static Value IntToInt(Value v, int t, bool aChecked, int aPos)
{
    if (aChecked) {
        bool ok = IsUnsigned(v.t) ? FitsUnsigned(t, v.u) : FitsSigned(t, v.i);
        if (!ok) Overflow(aPos);
    }
    return VNum(t, v.i);
}

static Value RealToInt(double d, int t, bool aChecked, int aPos)
{
    double lo, hi;
    switch (t) {
    case T_SBYTE: lo = -128; hi = 127; break;
    case T_BYTE: lo = 0; hi = 255; break;
    case T_SHORT: lo = -32768; hi = 32767; break;
    case T_USHORT: case T_CHAR: lo = 0; hi = 65535; break;
    case T_INT: lo = -2147483648.0; hi = 2147483647.0; break;
    case T_UINT: lo = 0; hi = 4294967295.0; break;
    case T_LONG: lo = -9223372036854775808.0; hi = 9223372036854775807.0; break;
    default: lo = 0; hi = 18446744073709551615.0; break;
    }
    double tr = trunc(d);
    if (d != d || tr < lo || tr > hi || (t == T_LONG && tr >= 9223372036854775808.0) ||
        (t == T_ULONG && tr >= 18446744073709551616.0)) {
        if (aChecked) Overflow(aPos);
        // .NET 9 and later: saturating; NaN is 0
        if (d != d) return VNum(t, 0);
        if (tr < lo) {
            if (t == T_ULONG) return VNum(t, 0);
            return t == T_LONG ? VLong((i64)0x8000000000000000ull) : VNum(t, (i64)lo);
        }
        if (t == T_ULONG) { Value v; v.t = T_ULONG; v.u = ~0ull; return v; }
        if (t == T_LONG) return VLong(0x7FFFFFFFFFFFFFFFLL);
        return VNum(t, (i64)hi);
    }
    if (t == T_ULONG) {
        Value v;
        v.t = T_ULONG;
        v.u = (u64)tr;
        return v;
    }
    return VNum(t, (i64)tr);
}

static bool ImplicitNum(int f, int t)
{
    if (f == t) return true;
    switch (f) {
    case T_SBYTE: return t == T_SHORT || t == T_INT || t == T_LONG || t == T_FLOAT || t == T_DOUBLE;
    case T_BYTE: return t == T_SHORT || t == T_USHORT || t == T_INT || t == T_UINT || t == T_LONG || t == T_ULONG || t == T_FLOAT || t == T_DOUBLE;
    case T_SHORT: return t == T_INT || t == T_LONG || t == T_FLOAT || t == T_DOUBLE;
    case T_USHORT: return t == T_INT || t == T_UINT || t == T_LONG || t == T_ULONG || t == T_FLOAT || t == T_DOUBLE;
    case T_CHAR: return t == T_USHORT || t == T_INT || t == T_UINT || t == T_LONG || t == T_ULONG || t == T_FLOAT || t == T_DOUBLE;
    case T_INT: return t == T_LONG || t == T_FLOAT || t == T_DOUBLE;
    case T_UINT: return t == T_LONG || t == T_ULONG || t == T_FLOAT || t == T_DOUBLE;
    case T_LONG: case T_ULONG: return t == T_FLOAT || t == T_DOUBLE;
    case T_FLOAT: return t == T_DOUBLE;
    default: return false;
    }
}

static Value NumConvert(Value v, int t, bool aChecked, int aPos)
{
    if (v.t == t) return v;
    if (IsReal(t)) return VReal(t, ToDouble(v));
    if (IsReal(v.t)) return RealToInt(v.d, t, aChecked, aPos);
    return IntToInt(v, t, aChecked, aPos);
}

int PromoteTypes(int a, int b) { return Promote(UnaryPromote(a), UnaryPromote(b)); }
Value NumTo(Value v, int aTy, bool aChecked, int aPos) { return NumConvert(v, aTy, aChecked, aPos); }

Value DefaultOf(int aTy)
{
    if (aTy == T_BOOL) return VBool(false);
    if (IsIntegral(aTy)) return VNum(aTy, 0);
    if (IsReal(aTy)) return VReal(aTy, 0);
    return VNull();
}

Value ConvertImplicit(Value v, int aTo, bool aConst, int aPos)
{
    if (aTo == T_OBJECT || aTo < 0 || aTo >= T_OBJECT || v.t == aTo)
        return v;
    if (v.t == T_NULL) {
        if (aTo == T_STRING || aTo >= T_STRING) return v;
        ThrowAt(aPos, "InvalidCastException", "Cannot convert null to '%s' because it is a non-nullable value type", TyName(aTo));
    }
    if (IsNumeric(v.t) && IsNumeric(aTo)) {
        if (ImplicitNum(v.t, aTo))
            return NumConvert(v, aTo, false, aPos);
        // constant int expressions convert to smaller types when they fit
        if (aConst && aTo != T_CHAR && ((v.t == T_INT && FitsSigned(aTo, v.i)) ||
                                        (v.t == T_LONG && aTo == T_ULONG && v.i >= 0)))
            return VNum(aTo, v.i);
        ThrowAt(aPos, "InvalidCastException",
                "Cannot implicitly convert type '%s' to '%s'. An explicit conversion exists (are you missing a cast?)",
                TyName(v.t), TyName(aTo));
    }
    if (aTo == T_STRING || aTo == T_BOOL || IsNumeric(aTo))
        ThrowAt(aPos, "InvalidCastException", "Cannot implicitly convert type '%s' to '%s'", ValTyName(v), TyName(aTo));
    return v;
}

Value ConvertExplicit(Value v, int aTo, int aPos)
{
    if (aTo == T_OBJECT || aTo < 0 || v.t == aTo)
        return v;
    if (v.t == T_NULL) {
        if (aTo >= T_STRING) return v;
        ThrowAt(aPos, "NullReferenceException", "Object reference not set to an instance of an object.");
    }
    if (IsNumeric(v.t) && IsNumeric(aTo))
        return NumConvert(v, aTo, gChecked, aPos);
    if (aTo >= T_STRING && aTo != T_STRING)
        return v;
    ThrowAt(aPos, "InvalidCastException", "Unable to cast object of type '%s' to type '%s'.", ValTyName(v), TyName(aTo));
}

bool Truthy(Value v, int aPos)
{
    if (v.t != T_BOOL)
        ThrowAt(aPos, "InvalidCastException", "Cannot implicitly convert type '%s' to 'bool'", ValTyName(v));
    return v.i != 0;
}

// ---------------------------------------------------------------------------
// operators

static Value IntArith(int aOp, int t, Value a, Value b, int aPos)
{
    bool chk = gChecked;
    if (t == T_INT || t == T_UINT) {
        i64 x = a.i, y = b.i;               // already converted to t
        if (t == T_UINT) { x = (i64)(uint32_t)a.u; y = (i64)(uint32_t)b.u; }
        i64 r;
        switch (aOp) {
        case TK_PLUS: r = x + y; break;
        case TK_MINUS: r = x - y; break;
        case TK_STAR: r = x * y; break;
        case TK_SLASH:
        case TK_PERCENT:
            if (y == 0) ThrowAt(aPos, "DivideByZeroException", "Attempted to divide by zero.");
            if (t == T_INT && x == -2147483648LL && y == -1) Overflow(aPos);
            r = aOp == TK_SLASH ? x / y : x % y;
            break;
        default: r = 0;
        }
        if (chk && !FitsSigned(t, r)) Overflow(aPos);
        return VNum(t, r);
    }
    if (t == T_LONG) {
        i64 x = a.i, y = b.i, r = 0;
        bool ov = false;
        switch (aOp) {
        case TK_PLUS: ov = __builtin_add_overflow(x, y, &r); if (ov) r = (i64)((u64)x + (u64)y); break;
        case TK_MINUS: ov = __builtin_sub_overflow(x, y, &r); if (ov) r = (i64)((u64)x - (u64)y); break;
        case TK_STAR: ov = __builtin_mul_overflow(x, y, &r); if (ov) r = (i64)((u64)x * (u64)y); break;
        case TK_SLASH:
        case TK_PERCENT:
            if (y == 0) ThrowAt(aPos, "DivideByZeroException", "Attempted to divide by zero.");
            if (x == (i64)0x8000000000000000ull && y == -1) Overflow(aPos);
            r = aOp == TK_SLASH ? x / y : x % y;
            break;
        }
        if (ov && chk) Overflow(aPos);
        return VLong(r);
    }
    // ulong
    u64 x = a.u, y = b.u, r = 0;
    bool ov = false;
    switch (aOp) {
    case TK_PLUS: ov = __builtin_add_overflow(x, y, &r); break;
    case TK_MINUS: ov = __builtin_sub_overflow(x, y, &r); break;
    case TK_STAR: ov = __builtin_mul_overflow(x, y, &r); break;
    case TK_SLASH:
    case TK_PERCENT:
        if (y == 0) ThrowAt(aPos, "DivideByZeroException", "Attempted to divide by zero.");
        r = aOp == TK_SLASH ? x / y : x % y;
        break;
    }
    if (ov && chk) Overflow(aPos);
    Value v;
    v.t = T_ULONG;
    v.u = r;
    return v;
}

static int CmpNum(Value a, Value b)
{
    int t = Promote(UnaryPromote(a.t), UnaryPromote(b.t));
    if (IsReal(t)) {
        double x = ToDouble(a), y = ToDouble(b);
        return x < y ? -1 : x > y ? 1 : x == y ? 0 : 2;       // 2: unordered (NaN)
    }
    if (t == T_ULONG) {
        // a negative signed value is below any ulong
        if (!IsUnsigned(a.t) && a.i < 0) return -1;
        if (!IsUnsigned(b.t) && b.i < 0) return 1;
        return a.u < b.u ? -1 : a.u > b.u ? 1 : 0;
    }
    return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
}

static Value Shift(int aOp, Value a, Value b, int aPos)
{
    int t = UnaryPromote(a.t);
    if (!IsIntegral(a.t) || !IsIntegral(b.t) || UnaryPromote(b.t) != T_INT)
        ThrowAt(aPos, "InvalidOperationException", "Operator '%s' cannot be applied to operands of type '%s' and '%s'",
                TokText(aOp), ValTyName(a), ValTyName(b));
    int c = (int)b.i & (Bits(t) == 64 ? 63 : 31);
    Value r;
    r.t = (uint8_t)t;
    switch (t) {
    case T_INT: {
        uint32_t x = (uint32_t)a.i;
        int32_t sx = (int32_t)a.i;
        r.i = aOp == TK_SHL ? (int32_t)(x << c) : aOp == TK_SHR ? (sx >> c) : (int32_t)(x >> c);
        break;
    }
    case T_UINT: {
        uint32_t x = (uint32_t)a.u;
        r.u = aOp == TK_SHL ? (uint32_t)(x << c) : (x >> c);
        break;
    }
    case T_LONG: {
        u64 x = a.u;
        r.i = aOp == TK_SHL ? (i64)(x << c) : aOp == TK_SHR ? (a.i >> c) : (i64)(x >> c);
        break;
    }
    default:
        r.u = aOp == TK_SHL ? (a.u << c) : (a.u >> c);
        break;
    }
    return r;
}

RS_NORETURN static void BadOp(int aOp, Value a, Value b, int aPos)
{
    ThrowAt(aPos, "InvalidOperationException", "Operator '%s' cannot be applied to operands of type '%s' and '%s'",
            TokText(aOp), ValTyName(a), ValTyName(b));
}

Value Binary(int aOp, Value a, Value b, int aPos)
{
    switch (aOp) {
    case TK_EQEQ: return VBool(OpEquals(a, b, aPos));
    case TK_NE: return VBool(!OpEquals(a, b, aPos));
    case TK_SHL: case TK_SHR: case TK_USHR:
        if (a.t == T_NULL || b.t == T_NULL) return VNull();
        return Shift(aOp, a, b, aPos);
    default: break;
    }
    if (aOp == TK_PLUS && (a.t == T_STRING || b.t == T_STRING)) {
        const Str *x = a.t == T_NULL ? EmptyStr() : ToStr(a);
        const Str *y = b.t == T_NULL ? EmptyStr() : ToStr(b);
        return VStr(StrCat(x, y));
    }
    if (a.t == T_ENUM && b.t == T_ENUM && (a.i >> 8) == (b.i >> 8) &&
        (aOp == TK_BAR || aOp == TK_AMP || aOp == TK_CARET)) {
        // flags: StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
        int x = (int)(a.i & 0xFF), y = (int)(b.i & 0xFF);
        int r = aOp == TK_BAR ? x | y : aOp == TK_AMP ? x & y : x ^ y;
        return VEnum((int)(a.i >> 8), r);
    }
    if (a.t == T_BOOL && b.t == T_BOOL) {
        switch (aOp) {
        case TK_AMP: return VBool(a.i & b.i);
        case TK_BAR: return VBool(a.i | b.i);
        case TK_CARET: return VBool(a.i ^ b.i);
        default: BadOp(aOp, a, b, aPos);
        }
    }
    if ((a.t == T_NULL && (IsNumeric(b.t) || b.t == T_BOOL || b.t == T_NULL)) ||
        (b.t == T_NULL && (IsNumeric(a.t) || a.t == T_BOOL))) {
        // lifted operators on int? and friends
        if (aOp == TK_LT || aOp == TK_GT || aOp == TK_LE || aOp == TK_GE) return VBool(false);
        return VNull();
    }
    if (!IsNumeric(a.t) || !IsNumeric(b.t))
        BadOp(aOp, a, b, aPos);
    if (aOp == TK_LT || aOp == TK_GT || aOp == TK_LE || aOp == TK_GE) {
        int c = CmpNum(a, b);
        if (c == 2) return VBool(false);
        switch (aOp) {
        case TK_LT: return VBool(c < 0);
        case TK_GT: return VBool(c > 0);
        case TK_LE: return VBool(c <= 0);
        default: return VBool(c >= 0);
        }
    }
    int t = Promote(UnaryPromote(a.t), UnaryPromote(b.t));
    if (IsReal(t)) {
        double x = ToDouble(a), y = ToDouble(b), r;
        switch (aOp) {
        case TK_PLUS: r = x + y; break;
        case TK_MINUS: r = x - y; break;
        case TK_STAR: r = x * y; break;
        case TK_SLASH: r = x / y; break;
        case TK_PERCENT: r = fmod(x, y); break;
        default: BadOp(aOp, a, b, aPos);
        }
        return VReal(t, r);
    }
    Value x = NumConvert(a, t, false, aPos), y = NumConvert(b, t, false, aPos);
    switch (aOp) {
    case TK_AMP: { Value r = x; r.u = x.u & y.u; return r; }
    case TK_BAR: { Value r = x; r.u = x.u | y.u; return r; }
    case TK_CARET: { Value r = x; r.u = x.u ^ y.u; return VNum(t, (i64)r.u); }
    case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT:
        return IntArith(aOp, t, x, y, aPos);
    default:
        BadOp(aOp, a, b, aPos);
    }
}

Value Unary(int aOp, Value a, int aPos)
{
    if (a.t == T_NULL && aOp != TK_NOT) return VNull();
    switch (aOp) {
    case TK_NOT:
        if (a.t == T_NULL) return VNull();
        return VBool(!Truthy(a, aPos));
    case TK_PLUS:
        if (!IsNumeric(a.t)) break;
        return NumConvert(a, UnaryPromote(a.t), false, aPos);
    case TK_MINUS: {
        if (!IsNumeric(a.t)) break;
        if (IsReal(a.t)) return VReal(a.t, -a.d);
        int t = UnaryPromote(a.t);
        if (t == T_ULONG)
            ThrowAt(aPos, "InvalidOperationException", "Operator '-' cannot be applied to operand of type 'ulong'");
        if (t == T_UINT) return VLong(-(i64)a.u);
        Value zero = VNum(t, 0);
        return IntArith(TK_MINUS, t, zero, NumConvert(a, t, false, aPos), aPos);
    }
    case TK_TILDE: {
        if (!IsIntegral(a.t)) break;
        int t = UnaryPromote(a.t);
        return VNum(t, ~NumConvert(a, t, false, aPos).i);
    }
    case TK_CARET:
        ThrowAt(aPos, "InvalidOperationException", "'^' (index from end) is only supported inside [ ]");
    default:
        break;
    }
    ThrowAt(aPos, "InvalidOperationException", "Operator '%s' cannot be applied to operand of type '%s'",
            TokText(aOp), ValTyName(a));
}

// ---------------------------------------------------------------------------
// equality, hashing, ordering

bool OpEquals(Value a, Value b, int aPos)
{
    if (a.t == T_NULL || b.t == T_NULL)
        return a.t == b.t;
    if (IsNumeric(a.t) && IsNumeric(b.t))
        return CmpNum(a, b) == 0;
    if (a.t == T_BOOL && b.t == T_BOOL) return a.i == b.i;
    if (a.t == T_STRING && b.t == T_STRING) return StrEq(a.s, b.s);
    if (a.t == T_OBJ && b.t == T_OBJ && a.o->kind == OK_TUPLE && b.o->kind == OK_TUPLE) {
        if (a.o->n != b.o->n)
            ThrowAt(aPos, "InvalidOperationException", "Tuple types of different sizes cannot be compared");
        for (int i = 0; i < a.o->n; i++)
            if (!OpEquals(a.o->f[i], b.o->f[i], aPos)) return false;
        return true;
    }
    if (a.t == T_TYPE && b.t == T_TYPE) return a.i == b.i;
    if (a.t != b.t) {
        if ((IsNumeric(a.t) || a.t == T_BOOL || a.t == T_STRING) || (IsNumeric(b.t) || b.t == T_BOOL || b.t == T_STRING))
            BadOp(TK_EQEQ, a, b, aPos);
        return false;
    }
    return a.i == b.i;                      // the same object
}

bool ValEquals(Value a, Value b)
{
    if (a.t != b.t) return false;
    switch (a.t) {
    case T_NULL: return true;
    case T_FLOAT: case T_DOUBLE: return a.d == b.d || (a.d != a.d && b.d != b.d);
    case T_STRING: return StrEq(a.s, b.s);
    case T_OBJ:
        if (a.o == b.o) return true;
        if (a.o->kind != b.o->kind || a.o->n != b.o->n || a.o->kind == OK_EXC) return false;
        for (int i = 0; i < a.o->n; i++) {
            if (a.o->kind == OK_ANON && a.o->names[i] != b.o->names[i]) return false;
            if (!ValEquals(a.o->f[i], b.o->f[i])) return false;
        }
        return true;
    default:
        return a.i == b.i;
    }
}

uint32_t ValHash(Value v)
{
    uint32_t h = 0x811C9DC5u ^ v.t;
    switch (v.t) {
    case T_NULL: return 0;
    case T_FLOAT: case T_DOUBLE: {
        double d = v.d == 0 ? 0 : v.d;
        if (d != d) return 0x7FF8;
        u64 bits;
        memcpy(&bits, &d, 8);
        return (uint32_t)(bits ^ (bits >> 32)) * 2654435761u;
    }
    case T_STRING:
        for (int i = 0; i < v.s->n; i++)
            h = (h ^ v.s->c[i]) * 16777619u;
        return h;
    case T_OBJ:
        if (v.o->kind == OK_EXC) break;
        for (int i = 0; i < v.o->n; i++)
            h = (h ^ ValHash(v.o->f[i])) * 16777619u;
        return h;
    default:
        break;
    }
    u64 x = v.u;
    return (uint32_t)((x ^ (x >> 32)) * 2654435761u) ^ h;
}

int Compare(Value a, Value b, int aPos)
{
    if (a.t == T_NULL || b.t == T_NULL)
        return a.t == b.t ? 0 : a.t == T_NULL ? -1 : 1;
    if (IsNumeric(a.t) && IsNumeric(b.t)) {
        int c = CmpNum(a, b);
        if (c == 2) {
            // CompareTo: NaN is below everything, equal to itself
            bool an = IsReal(a.t) && a.d != a.d, bn = IsReal(b.t) && b.d != b.d;
            return an && bn ? 0 : an ? -1 : 1;
        }
        return c;
    }
    if (a.t == T_BOOL && b.t == T_BOOL) return (int)a.i - (int)b.i;
    if (a.t == T_STRING && b.t == T_STRING) return StrCmpCulture(a.s, b.s);
    if (a.t == T_OBJ && b.t == T_OBJ && a.o->kind == OK_TUPLE && b.o->kind == OK_TUPLE && a.o->n == b.o->n) {
        for (int i = 0; i < a.o->n; i++) {
            int c = Compare(a.o->f[i], b.o->f[i], aPos);
            if (c) return c;
        }
        return 0;
    }
    ThrowAt(aPos, "InvalidOperationException",
            "Failed to compare two elements in the array: %s does not implement IComparable.", ValTyName(a.t == T_OBJ ? a : b));
}

// ---------------------------------------------------------------------------
// scopes

Scope *NewScope(Scope *aUp)
{
    Scope *s = g.freeScopes;
    if (s) {
        g.freeScopes = s->up;
        memset(s, 0, sizeof *s);
    } else {
        s = New<Scope>();
    }
    s->up = aUp;
    s->vars = s->inl;
    s->cap = 4;
    return s;
}

void ReleaseScope(Scope *s)
{
    if (!s || s->captured) return;
    s->up = g.freeScopes;
    g.freeScopes = s;
}

void Capture(Scope *s)
{
    for (; s && !s->captured; s = s->up)
        s->captured = true;
}

Var *Lookup(Scope *s, Name *aName)
{
    for (; s; s = s->up)
        for (int i = 0; i < s->n; i++)
            if (s->vars[i].name == aName)
                return &s->vars[i];
    return NULL;
}

Var *Declare(Scope *s, Name *aName, int aTy, int aPos)
{
    for (int i = 0; i < s->n; i++)
        if (s->vars[i].name == aName)
            CompileError(aPos, "A local variable named '%s' is already defined in this scope", aName->s);
    if (s->n == s->cap) {
        int cap = s->cap * 2;
        Var *v = NewArr<Var>(cap);
        memcpy(v, s->vars, s->n * sizeof(Var));
        s->vars = v;
        s->cap = cap;
    }
    Var *v = &s->vars[s->n++];
    v->name = aName;
    v->ty = (uint8_t)aTy;
    v->v = VNull();
    return v;
}

static Var *DeclareOrReuse(Scope *s, Name *aName, int aTy, int aPos)
{
    for (int i = 0; i < s->n; i++)
        if (s->vars[i].name == aName) {
            s->vars[i].ty = (uint8_t)aTy;
            return &s->vars[i];
        }
    return Declare(s, aName, aTy, aPos);
}

// ---------------------------------------------------------------------------
// types

int TypeTy(TypeRef *t)
{
    if (!t || t->rank || t->nullable || t->isVar) return T_OBJECT;
    if (t->id >= 0 && t->id < T_OBJECT) return t->id;
    return T_OBJECT;
}

static bool TypeMatches(Value v, TypeRef *t)
{
    if (v.t == T_NULL) return false;
    if (t->rank) return v.t == T_ARRAY;
    switch (t->id) {
    case T_OBJECT: return true;
    case TY_LIST: return v.t == T_LIST;
    case TY_DICT: return v.t == T_DICT;
    case TY_SET: return v.t == T_SET;
    case TY_SB: return v.t == T_SB;
    case TY_TUPLE: return v.t == T_OBJ && v.o->kind == OK_TUPLE && v.o->n == t->nargs;
    case TY_KVP: return v.t == T_OBJ && v.o->kind == OK_KVP;
    case TY_ENUMERABLE_T: return IsEnumerable(v);
    case TY_FUNC: return v.t == T_FUNC;
    case TY_GROUPING: return v.t == T_GROUP;
    case TY_EXCEPTION: return v.t == T_OBJ && v.o->kind == OK_EXC;
    default: return t->id == v.t;
    }
}

// ---------------------------------------------------------------------------
// patterns

static bool Match(Value v, Node *p, Scope *s)
{
    switch (p->k) {
    case P_DISCARD:
        return true;
    case P_VAR:
        DeclareOrReuse(s, p->name, T_OBJECT, p->pos)->v = v;
        return true;
    case P_CONST: {
        Value c = Eval(p->a, s);
        if (c.t == T_NULL || v.t == T_NULL) return c.t == v.t;
        if (IsNumeric(c.t) && IsNumeric(v.t)) return CmpNum(v, c) == 0;
        if (c.t == T_STRING && v.t == T_STRING) return StrEq(c.s, v.s);
        if (c.t == T_BOOL && v.t == T_BOOL) return c.i == v.i;
        return ValEquals(v, c);
    }
    case P_TYPE:
        if (!TypeMatches(v, p->type)) return false;
        if (p->name)
            DeclareOrReuse(s, p->name, TypeTy(p->type), p->pos)->v = v;
        return true;
    case P_REL: {
        Value c = Eval(p->a, s);
        if (!IsNumeric(v.t) || !IsNumeric(c.t)) return false;
        return Truthy(Binary(p->op, v, c, p->pos), p->pos);
    }
    case P_NOT:
        return !Match(v, p->a, s);
    case P_AND:
        return Match(v, p->a, s) && Match(v, p->b, s);
    case P_OR:
        return Match(v, p->a, s) || Match(v, p->b, s);
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// calls

int FuncArity(const Value &f)
{
    if (f.t != T_FUNC || !f.fn->node) return -1;
    return f.fn->node->n;
}

enum TFlow { FL_NORMAL, FL_BREAK, FL_CONTINUE, FL_RETURN };
static int Exec(Node *st, Scope *s);
static void Hoist(Node *aBlock, Scope *s);

Value CallFunc(Func *f, Value *aArgs, int aCount, int aPos)
{
    Tick();
    if (!f->node) {
        Name *nm = Intern(NameText(f->nameId), strlen(NameText(f->nameId)));
        return StaticCall(f->typeId, nm, aArgs, aCount, NULL, NULL);
    }
    Node *fn = f->node;
    if (fn->n != aCount)
        ThrowAt(aPos, "ArgumentException", "The function takes %d argument%s, not %d",
                fn->n, fn->n == 1 ? "" : "s", aCount);
    Scope *s = NewScope(f->env);
    TypeRef **ptypes = f->ptypes ? f->ptypes : fn->targs;
    for (int i = 0; i < aCount; i++) {
        int ty = ptypes && ptypes[i] ? TypeTy(ptypes[i]) : T_OBJECT;
        Var *v = Declare(s, fn->names[i], ty, fn->pos);
        v->v = ConvertImplicit(aArgs[i], ty, false, aPos);
    }
    Value r;
    if (fn->flags & F_BLOCKBODY) {
        Hoist(fn->a, s);
        int flow = FL_NORMAL;
        for (int i = 0; i < fn->a->n && flow == FL_NORMAL; i++)
            flow = Exec(fn->a->xs[i], s);
        if (flow == FL_RETURN) {
            r = g.result;
        } else {
            r = VVoid();
        }
    } else {
        r = Eval(fn->a, s);
    }
    TypeRef *ret = f->ret ? f->ret : (fn->k == S_FUNC ? fn->type : NULL);
    if (ret && !IsVoid(r)) {
        if (ret->id == T_NULL && !ret->rank)
            r = VVoid();
        else
            r = ConvertImplicit(r, TypeTy(ret), fn->k == S_FUNC && !(fn->flags & F_BLOCKBODY) && (fn->a->flags & F_CONST), aPos);
    }
    ReleaseScope(s);
    return r;
}

Value Call1(const Value &f, Value a, int aPos)
{
    if (f.t != T_FUNC) ThrowAt(aPos, "InvalidOperationException", "A function was expected");
    return CallFunc(f.fn, &a, 1, aPos);
}

Value Call2(const Value &f, Value a, Value b, int aPos)
{
    if (f.t != T_FUNC) ThrowAt(aPos, "InvalidOperationException", "A function was expected");
    Value args[2] = { a, b };
    return CallFunc(f.fn, args, 2, aPos);
}

// ---------------------------------------------------------------------------
// assignment targets

struct LRef
    {
    int kind;               // 0 variable, 1 index, 2 member
    Var *var;
    Value obj;
    Value idx[4];
    int nidx;
    Name *name;
    int pos;
    };

static int IndexFromEnd(Node *aArg, Value aObj, Scope *s, bool *aHat);

static int Length(Value v, int aPos)
{
    switch (v.t) {
    case T_STRING: return v.s->n;
    case T_ARRAY: case T_LIST: return v.a->n;
    default:
        ThrowAt(aPos, "InvalidOperationException", "'^' and ranges need a string, array or List");
    }
}

static void EvalLRef(Node *e, Scope *s, LRef &r)
{
    r.pos = e->pos;
    switch (e->k) {
    case N_NAME:
        r.kind = 0;
        r.var = Lookup(s, e->name);
        if (!r.var)
            CompileError(e->pos, "The name '%s' does not exist in the current context", e->name->s);
        break;
    case N_INDEX:
        r.kind = 1;
        r.obj = Eval(e->a, s);
        if (r.obj.t == T_NULL)
            ThrowAt(e->pos, "NullReferenceException", "Object reference not set to an instance of an object.");
        r.nidx = e->n > 4 ? 4 : e->n;
        for (int i = 0; i < r.nidx; i++) {
            Node *ax = e->xs[i];
            if (ax->k == N_UNARY && ax->op == TK_CARET) {
                bool hat;
                r.idx[i] = VInt(IndexFromEnd(ax, r.obj, s, &hat));
            } else {
                r.idx[i] = Eval(ax, s);
            }
        }
        break;
    case N_MEMBER:
        r.kind = 2;
        r.obj = Eval(e->a, s);
        r.name = e->name;
        break;
    default:
        CompileError(e->pos, "The left-hand side of an assignment must be a variable, property or indexer");
    }
}

static Value ReadLRef(LRef &r)
{
    switch (r.kind) {
    case 0: return r.var->v;
    case 1: return IndexGet(r.obj, r.idx, r.nidx, r.pos);
    default:
        if (r.obj.t == T_TYPE) return StaticMember((int)r.obj.i, r.name, r.pos);
        return InstanceMember(r.obj, r.name, r.pos);
    }
}

// The declared type of the target (for conversions), T_OBJECT if any.
static int LRefTy(LRef &r)
{
    if (r.kind == 0) return r.var->ty;
    if (r.kind == 1 && (r.obj.t == T_ARRAY || r.obj.t == T_LIST)) return r.obj.a->et;
    if (r.kind == 1 && r.obj.t == T_DICT) return r.obj.m->vt;
    return T_OBJECT;
}

static void WriteLRef(LRef &r, Value v)
{
    switch (r.kind) {
    case 0: r.var->v = v; break;
    case 1: IndexSet(r.obj, r.idx, r.nidx, v, r.pos); break;
    default: MemberSet(r.obj, r.name, v, r.pos); break;
    }
}

void SetOut(Node *aCall, int aIndex, Value v, Scope *s)
{
    if (!aCall || aIndex >= aCall->n || aCall->xs[aIndex]->k != N_OUTARG)
        return;
    Node *o = aCall->xs[aIndex];
    if (o->flags & F_OUT_DECL) {
        int ty = TypeTy(o->type);
        int vty = o->type->isVar ? ((v.t <= T_STRING && v.t != T_NULL) ? v.t : T_OBJECT) : ty;
        DeclareOrReuse(s, o->name, vty, o->pos)->v = ConvertImplicit(v, ty, false, o->pos);
        return;
    }
    LRef r;
    EvalLRef(o->a, s, r);
    WriteLRef(r, ConvertImplicit(v, LRefTy(r), false, o->pos));
}

// ---------------------------------------------------------------------------
// expressions

static Value EvalTarget(Node *e, TypeRef *t, Scope *s);

static int IndexFromEnd(Node *aArg, Value aObj, Scope *s, bool *aHat)
{
    *aHat = true;
    Value v = Eval(aArg->a, s);
    return Length(aObj, aArg->pos) - ToIndex(v, aArg->pos);
}

static Value Slice(Value obj, Node *r, Scope *s)
{
    int len = Length(obj, r->pos);
    int from = 0, to = len;
    bool hat;
    if (r->a) from = (r->a->k == N_UNARY && r->a->op == TK_CARET) ? IndexFromEnd(r->a, obj, s, &hat) : ToIndex(Eval(r->a, s), r->pos);
    if (r->b) to = (r->b->k == N_UNARY && r->b->op == TK_CARET) ? IndexFromEnd(r->b, obj, s, &hat) : ToIndex(Eval(r->b, s), r->pos);
    if (from < 0 || to > len || from > to)
        ThrowAt(r->pos, "ArgumentOutOfRangeException", "The range %d..%d is outside 0..%d", from, to, len);
    if (obj.t == T_STRING) return VStr(StrSub(obj.s, from, to - from));
    Arr *a = NewArr(to - from, obj.a->et);
    for (int i = from; i < to; i++) ArrPush(a, obj.a->v[i]);
    Value v;
    v.t = obj.t;
    v.a = a;
    return v;
}

__attribute__((noinline)) static Value EvalCall(Node *e, Scope *s)
{
    Node *tg = e->a;
    // arguments
    int n = e->n;
    Value argsBuf[8];
    Value *args = n <= 8 ? argsBuf : NewArr<Value>(n);
    for (int i = 0; i < n; i++)
        args[i] = e->xs[i]->k == N_OUTARG ? VNull() : Eval(e->xs[i], s);
    if (tg->k == N_MEMBER) {
        Value recv;
        Node *ra = tg->a;
        if (ra->k == N_NAME && !Lookup(s, ra->name)) {
            int ty = TypeIdOf(ra->name);
            if (ty < 0)
                CompileError(ra->pos, "The name '%s' does not exist in the current context", ra->name->s);
            recv = VTy(ty);
        } else {
            recv = Eval(ra, s);
        }
        if (recv.t == T_NS) {
            int ty = TypeIdOf(tg->name);
            CompileError(tg->pos, ty < 0 ? "The type or namespace name '%s' does not exist" : "'%s' is a type, not a method", tg->name->s);
        }
        if (recv.t == T_TYPE)
            return StaticCall((int)recv.i, tg->name, args, n, e, s);
        if (recv.t == T_NULL) {
            if (tg->flags & F_NULLCOND) return VNull();
            ThrowAt(e->pos, "NullReferenceException", "Object reference not set to an instance of an object (calling %s).", tg->name->s);
        }
        // a property holding a function: o.F(x)
        if (recv.t == T_OBJ) {
            for (int i = 0; i < recv.o->n; i++)
                if (recv.o->names[i] == tg->name && recv.o->f[i].t == T_FUNC)
                    return CallFunc(recv.o->f[i].fn, args, n, e->pos);
        }
        return InstanceCall(recv, tg->name, args, n, e, s);
    }
    if (tg->k == N_NAME) {
        Var *v = Lookup(s, tg->name);
        if (!v)
            CompileError(tg->pos, "The name '%s' does not exist in the current context", tg->name->s);
        if (v->v.t != T_FUNC)
            ThrowAt(e->pos, "InvalidOperationException", "'%s' is a %s, not a function", tg->name->s, ValTyName(v->v));
        return CallFunc(v->v.fn, args, n, e->pos);
    }
    Value f = Eval(tg, s);
    if (f.t == T_NULL && (tg->flags & F_NULLCOND)) return VNull();
    if (f.t != T_FUNC)
        ThrowAt(e->pos, "InvalidOperationException", "This %s is not a function", ValTyName(f));
    return CallFunc(f.fn, args, n, e->pos);
}

// best common element type for new[] { ... } and [ ... ]
static int CommonType(Value *v, int n)
{
    if (!n) return T_OBJECT;
    int t = -1;
    for (int i = 0; i < n; i++) {
        int vt = v[i].t;
        if (vt == T_NULL) continue;
        if (t < 0) { t = vt; continue; }
        if (t == vt) continue;
        if (IsNumeric(t) && IsNumeric(vt)) {
            if (ImplicitNum(vt, t)) continue;
            if (ImplicitNum(t, vt)) { t = vt; continue; }
            t = Promote(UnaryPromote(t), UnaryPromote(vt));
            continue;
        }
        return T_OBJECT;
    }
    if (t < 0 || t >= T_STRING) return t == T_STRING ? T_STRING : T_OBJECT;
    return t;
}

// aInfer: the element type comes from the elements (new[] { ... }, var x = [ ... ])
static Value MakeCollection(Node *e, Scope *s, int aElemTy, int aKind, bool aInfer)
{
    Arr *a = NewArr(e->n, aElemTy);
    for (int i = 0; i < e->n; i++) {
        Node *x = e->xs[i];
        if (x->k == N_RANGEX) {
            Iter *it = OpenSeq(Eval(x->a, s), x->pos);
            Value v;
            while (it->Next(v)) ArrPush(a, v);
        } else {
            ArrPush(a, Eval(x, s));
        }
    }
    if (aInfer)
        a->et = (uint8_t)CommonType(a->v, a->n);
    bool allConst = true;
    for (int i = 0; i < e->n; i++)
        if (!(e->xs[i]->flags & F_CONST)) allConst = false;
    for (int i = 0; i < a->n; i++)
        a->v[i] = ConvertImplicit(a->v[i], a->et, allConst, e->pos);
    Value r;
    r.t = aKind == T_LIST ? T_LIST : T_ARRAY;
    r.a = a;
    return r;
}

static void ApplyInitializer(Value obj, Node *init, Scope *s)
{
    for (int i = 0; i < init->n; i++) {
        Node *x = init->xs[i];
        if (x->k == N_ASSIGN && x->a->k == N_INDEX && !x->a->a) {
            // [k] = v
            Value k = Eval(x->a->xs[0], s);
            IndexSet(obj, &k, 1, Eval(x->b, s), x->pos);
        } else if (x->k == N_TUPLE) {
            // { k, v } -> Add(k, v)
            Value args[4];
            int n = x->n > 4 ? 4 : x->n;
            for (int j = 0; j < n; j++) args[j] = Eval(x->xs[j], s);
            InstanceCall(obj, Intern("Add", 3), args, n, NULL, s);
        } else {
            Value v = Eval(x, s);
            InstanceCall(obj, Intern("Add", 3), &v, 1, NULL, s);
        }
    }
}

static Value EvalInterp(Node *e, Scope *s)
{
    SB b = { NULL, 0, 0 };
    for (int i = 0; i < e->n; i++) {
        Node *p = e->xs[i];
        if (p->k == N_LIT) {
            SbAppend(&b, p->lit.s);
            continue;
        }
        Value v = Eval(p->a, s);
        Str *str = v.t == T_NULL ? (Str *)EmptyStr() : p->fmt ? FormatValue(v, p->fmt, p->pos) : ToStr(v);
        if (p->b) str = Align(str, ToIndex(Eval(p->b, s), p->pos));
        SbAppend(&b, str);
    }
    return VStr(SbStr(&b));
}

static Value MakeFunc(Node *lam, Scope *s)
{
    Func *f = New<Func>();
    f->node = lam;
    f->env = s;
    Capture(s);
    Value v;
    v.t = T_FUNC;
    v.fn = f;
    return v;
}

RS_NORETURN static Value ThrowValue(Value v, int aPos)
{
    if (v.t == T_OBJ && v.o->kind == OK_EXC) {
        char *type = (char *)v.o->names[0]->s;
        char *msg = v.o->f[0].t == T_STRING ? StrUtf8(v.o->f[0].s) : (char *)"";
        ThrowAt(aPos, type, "%s", msg);
    }
    if (v.t == T_NULL)
        ThrowAt(aPos, "NullReferenceException", "Object reference not set to an instance of an object.");
    ThrowAt(aPos, "Exception", "%s", StrUtf8(ToStr(v)));
}

// The hot cases live in small functions, so that the recursion through
// Eval -> EvalCall -> CallFunc -> Eval uses little C stack per level.
__attribute__((noinline)) static Value EvalBinary(Node *e, Scope *s)
{
    bool cnst = (e->flags & F_CONST) && !gUnchecked && !gConstEval;
    bool saved = gChecked;
    if (cnst) gChecked = gConstEval = true;     // constants are checked at compile time
    Value a = Eval(e->a, s), b = Eval(e->b, s);
    // u - 1 with uint u is uint: a non-negative int constant converts to uint
    if (e->op != TK_SHL && e->op != TK_SHR && e->op != TK_USHR) {
        if (a.t == T_UINT && b.t == T_INT && (e->b->flags & F_CONST) && b.i >= 0) b = VNum(T_UINT, b.i);
        else if (b.t == T_UINT && a.t == T_INT && (e->a->flags & F_CONST) && a.i >= 0) a = VNum(T_UINT, a.i);
    }
    Value v = Binary(e->op, a, b, e->pos);
    if (cnst) {
        gChecked = saved;
        gConstEval = false;
    }
    return v;
}

__attribute__((noinline)) static Value EvalCond(Node *e, Scope *s)
{
    bool c = Truthy(Eval(e->a, s), e->pos);
    Node *pick = c ? e->b : e->c, *other = c ? e->c : e->b;
    Value v = Eval(pick, s);
    // `c ? 1 : 2.5` is a double either way (when the other side is a literal)
    if (IsNumeric(v.t) && other->k == N_LIT && IsNumeric(other->lit.t) && ImplicitNum(v.t, other->lit.t))
        v = NumConvert(v, other->lit.t, false, e->pos);
    return v;
}

__attribute__((noinline)) static Value EvalSlow(Node *e, Scope *s)
{
    switch (e->k) {
    case N_LIT:
        return e->lit;

    case N_NAME: {
        Var *v = Lookup(s, e->name);
        if (v) return v->v;
        if (e->name->id == K_System) {
            Value ns;
            ns.t = T_NS;
            ns.i = 0;
            return ns;
        }
        int ty = TypeIdOf(e->name);
        if (ty >= 0) return VTy(ty);
        if (e->name->len == 1 && e->name->s[0] == '_')
            CompileError(e->pos, "'_' (discard) can't be read");
        CompileError(e->pos, "The name '%s' does not exist in the current context", e->name->s);
    }

    case N_MEMBER: {
        Value o;
        if (e->a->k == N_NAME && !Lookup(s, e->a->name) && TypeIdOf(e->a->name) >= 0)
            o = VTy(TypeIdOf(e->a->name));
        else
            o = Eval(e->a, s);
        if (o.t == T_NS) {
            int ty = TypeIdOf(e->name);
            if (ty >= 0) return VTy(ty);
            return o;                           // System.Linq, System.Collections, ...
        }
        if (o.t == T_TYPE)
            return StaticMember((int)o.i, e->name, e->pos);
        if (o.t == T_NULL) {
            if (e->flags & F_NULLCOND) return VNull();
            ThrowAt(e->pos, "NullReferenceException", "Object reference not set to an instance of an object (reading %s).", e->name->s);
        }
        return InstanceMember(o, e->name, e->pos);
    }

    case N_CALL:
        return EvalCall(e, s);

    case N_INDEX: {
        Value o = Eval(e->a, s);
        if (o.t == T_NULL) {
            if (e->flags & F_NULLCOND) return VNull();
            ThrowAt(e->pos, "NullReferenceException", "Object reference not set to an instance of an object.");
        }
        if (e->n == 1 && e->xs[0]->k == N_RANGEX)
            return Slice(o, e->xs[0], s);
        Value idx[4];
        int n = e->n > 4 ? 4 : e->n;
        for (int i = 0; i < n; i++) {
            Node *ax = e->xs[i];
            if (ax->k == N_UNARY && ax->op == TK_CARET) {
                bool hat;
                idx[i] = VInt(IndexFromEnd(ax, o, s, &hat));
            } else {
                idx[i] = Eval(ax, s);
            }
        }
        return IndexGet(o, idx, n, e->pos);
    }

    case N_UNARY:
        return Unary(e->op, Eval(e->a, s), e->pos);

    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: {
        LRef r;
        EvalLRef(e->a, s, r);
        Value old = ReadLRef(r);
        if (!IsNumeric(old.t))
            ThrowAt(e->pos, "InvalidOperationException", "Operator '%s' cannot be applied to operand of type '%s'",
                    (e->k == N_PREINC || e->k == N_POSTINC) ? "++" : "--", ValTyName(old));
        Value one = VInt(1);
        Value nv = Binary((e->k == N_PREINC || e->k == N_POSTINC) ? TK_PLUS : TK_MINUS, old, one, e->pos);
        nv = NumConvert(nv, old.t, gChecked, e->pos);
        WriteLRef(r, nv);
        return (e->k == N_PREINC || e->k == N_PREDEC) ? nv : old;
    }

    case N_BINARY:
        return EvalBinary(e, s);

    case N_ANDAND:
        return VBool(Truthy(Eval(e->a, s), e->pos) && Truthy(Eval(e->b, s), e->pos));
    case N_OROR:
        return VBool(Truthy(Eval(e->a, s), e->pos) || Truthy(Eval(e->b, s), e->pos));

    case N_COALESCE: {
        Value a = Eval(e->a, s);
        return a.t != T_NULL ? a : Eval(e->b, s);
    }

    case N_COND:
        return EvalCond(e, s);

    case N_ASSIGN: {
        if (e->a->k == N_TUPLE) {
            // (a, b) = (b, a)
            Value r = Eval(e->b, s);
            if (r.t != T_OBJ || r.o->kind != OK_TUPLE || r.o->n != e->a->n)
                ThrowAt(e->pos, "InvalidOperationException", "Cannot deconstruct this value into %d variables", e->a->n);
            LRef refs[8];
            for (int i = 0; i < e->a->n && i < 8; i++) {
                if (e->a->xs[i]->k == N_NAME && e->a->xs[i]->name->len == 1 && e->a->xs[i]->name->s[0] == '_' && !Lookup(s, e->a->xs[i]->name))
                    { refs[i].kind = -1; continue; }
                EvalLRef(e->a->xs[i], s, refs[i]);
            }
            for (int i = 0; i < e->a->n && i < 8; i++)
                if (refs[i].kind >= 0)
                    WriteLRef(refs[i], ConvertImplicit(r.o->f[i], LRefTy(refs[i]), false, e->pos));
            return r;
        }
        LRef r;
        EvalLRef(e->a, s, r);
        int ty = LRefTy(r);
        Value v;
        if (!e->op) {
            v = ConvertImplicit(Eval(e->b, s), ty, e->b->flags & F_CONST, e->pos);
        } else if (e->op == TK_QQ) {
            Value cur = ReadLRef(r);
            if (cur.t != T_NULL) return cur;
            v = ConvertImplicit(Eval(e->b, s), ty, e->b->flags & F_CONST, e->pos);
        } else {
            Value cur = ReadLRef(r);
            Value rhs = Eval(e->b, s);
            if (cur.t == T_FUNC || rhs.t == T_FUNC)
                ThrowAt(e->pos, "InvalidOperationException", "Combining delegates with += is not supported");
            v = Binary(e->op, cur, rhs, e->pos);
            // x op= y is x = (T)(x op y) for numeric types
            if (IsNumeric(cur.t) && IsNumeric(v.t) && v.t != cur.t) {
                if (!ImplicitNum(rhs.t, cur.t) && !((e->b->flags & F_CONST) && rhs.t == T_INT) &&
                    e->op != TK_SHL && e->op != TK_SHR && e->op != TK_USHR)
                    ThrowAt(e->pos, "InvalidCastException",
                            "Cannot implicitly convert type '%s' to '%s'. An explicit conversion exists (are you missing a cast?)",
                            TyName(v.t), TyName(cur.t));
                v = NumConvert(v, cur.t, gChecked, e->pos);
            }
            v = ConvertImplicit(v, ty, false, e->pos);
        }
        WriteLRef(r, v);
        return v;
    }

    case N_LAMBDA:
        return MakeFunc(e, s);

    case N_NEW: {
        if (!e->type)
            CompileError(e->pos, "Target-typed new() needs a declared type: write new T()");
        Value args[8];
        int n = e->n > 8 ? 8 : e->n;
        for (int i = 0; i < n; i++) args[i] = Eval(e->xs[i], s);
        Value o = NewObject(e->type, args, n, e->pos);
        if (e->b) ApplyInitializer(o, e->b, s);
        return o;
    }

    case N_NEWARR: {
        int et = e->type ? (e->type->rank ? T_OBJECT : TypeTy(e->type)) : T_OBJECT;
        if (e->a) {
            i64 n = ToI64(Eval(e->a, s), e->pos);
            if (n < 0)
                ThrowAt(e->pos, "OverflowException", "Arithmetic operation resulted in an overflow (negative array size).");
            if (n > 50000000)
                ThrowAt(e->pos, "OutOfMemoryException", "Array of %lld elements is too big for rSharp", (long long)n);
            Arr *a = NewArr((int)n, et);
            Value d = DefaultOf(et);
            for (int i = 0; i < n; i++) a->v[i] = d;
            a->n = (int)n;
            Value v;
            v.t = T_ARRAY;
            v.a = a;
            if (e->b) {
                Value init = MakeCollection(e->b, s, et, T_ARRAY, false);
                if (init.a->n != n)
                    CompileError(e->pos, "An array initializer of length %d is expected", (int)n);
                return init;
            }
            return v;
        }
        return MakeCollection(e->b, s, et, T_ARRAY, !e->type);
    }

    case N_NEWANON: {
        Obj *o = NewObj(OK_ANON, e->n);
        for (int i = 0; i < e->n; i++) {
            o->names[i] = e->names[i];
            o->f[i] = Eval(e->xs[i], s);
        }
        Value v;
        v.t = T_OBJ;
        v.o = o;
        return v;
    }

    case N_COLLECTION:
        return MakeCollection(e, s, T_OBJECT, T_ARRAY, true);

    case N_CAST: {
        if ((e->flags & F_CONST) && !gUnchecked && !gConstEval && IsNumeric(e->type->id)) {
            bool saved = gChecked;
            gChecked = gConstEval = true;
            Value v = ConvertExplicit(Eval(e->a, s), e->type->id, e->pos);
            gChecked = saved;
            gConstEval = false;
            return v;
        }
        Value v = Eval(e->a, s);
        if (e->type->rank || e->type->nullable) {
            if (v.t == T_NULL) return v;
            if (e->type->nullable && !e->type->rank)
                return ConvertExplicit(v, e->type->id, e->pos);
            return v;
        }
        return ConvertExplicit(v, e->type->id, e->pos);
    }

    case N_IS:
        return VBool(Match(Eval(e->a, s), e->b, s));

    case N_AS: {
        Value v = Eval(e->a, s);
        return TypeMatches(v, e->type) ? v : VNull();
    }

    case N_DEFAULT:
        return e->type ? (e->type->nullable || e->type->rank ? VNull() : DefaultOf(e->type->id)) : VNull();

    case N_CHECKED: {
        bool saved = gChecked, savedU = gUnchecked;
        gChecked = e->op == 1;
        gUnchecked = e->op == 0;
        Value v = Eval(e->a, s);
        gChecked = saved;
        gUnchecked = savedU;
        return v;
    }

    case N_TUPLE: {
        Obj *o = NewObj(OK_TUPLE, e->n);
        for (int i = 0; i < e->n; i++) {
            o->names[i] = e->names ? e->names[i] : NULL;
            o->f[i] = Eval(e->xs[i], s);
        }
        Value v;
        v.t = T_OBJ;
        v.o = o;
        return v;
    }

    case N_INTERP:
        return EvalInterp(e, s);

    case N_QUERY:
        return EvalQuery(e, s);

    case N_SWITCHX: {
        Value v = Eval(e->a, s);
        for (int i = 0; i < e->n; i++) {
            Node *arm = e->xs[i];
            Scope *as = NewScope(s);
            if (Match(v, arm->a, as) && (!arm->b || Truthy(Eval(arm->b, as), arm->pos))) {
                Value r = Eval(arm->c, as);
                ReleaseScope(as);
                return r;
            }
            ReleaseScope(as);
        }
        ThrowAt(e->pos, "Runtime.CompilerServices.SwitchExpressionException",
                "Non-exhaustive switch expression failed to match its input (%s).", StrUtf8(ToStr(v)));
    }

    case N_TYPEOF:
        return VTy(e->type->rank ? TY_ARRAYT : e->type->id);

    case N_THROWX:
        return ThrowValue(Eval(e->a, s), e->pos);

    case N_OUTARG:
        return VNull();

    case N_RANGEX:
        ThrowAt(e->pos, "InvalidOperationException", "Ranges (a..b) are only supported inside [ ]");

    default:
        CompileError(e->pos, "This expression is not supported");
    }
}

Value Eval(Node *e, Scope *s)
{
    StackCheck();
    switch (e->k) {
    case N_LIT:
        return e->lit;
    case N_NAME: {
        Var *v = Lookup(s, e->name);
        if (v) return v->v;
        break;
    }
    case N_CALL:
        return EvalCall(e, s);
    case N_BINARY:
        return EvalBinary(e, s);
    case N_COND:
        return EvalCond(e, s);
    case N_UNARY:
        return Unary(e->op, Eval(e->a, s), e->pos);
    case N_ANDAND:
        return VBool(Truthy(Eval(e->a, s), e->pos) && Truthy(Eval(e->b, s), e->pos));
    case N_OROR:
        return VBool(Truthy(Eval(e->a, s), e->pos) || Truthy(Eval(e->b, s), e->pos));
    default:
        break;
    }
    return EvalSlow(e, s);
}

// An initializer evaluated against a declared type: int[] a = { ... },
// List<int> l = [ ... ], Func<double, double> f = x => ..., new(), default.
static Value EvalTarget(Node *e, TypeRef *t, Scope *s)
{
    if (!t || t->isVar)
        return Eval(e, s);
    if (e->k == N_NEW && !e->type) {
        e->type = t;
        return Eval(e, s);
    }
    if (e->k == N_DEFAULT && !e->type)
        return t->nullable || t->rank ? VNull() : DefaultOf(t->id);
    if (e->k == N_COLLECTION) {
        if (t->rank)
            return MakeCollection(e, s, t->rank == 1 && t->id >= 0 && t->id < T_OBJECT && !t->nullable ? t->id : T_OBJECT,
                                  T_ARRAY, false);
        if (t->id == TY_LIST || t->id == TY_ENUMERABLE_T) {
            int et = t->nargs ? TypeTy(t->args[0]) : T_OBJECT;
            return MakeCollection(e, s, et, t->id == TY_LIST ? T_LIST : T_ARRAY, false);
        }
        if (t->id == TY_SET) {
            Value o = NewObject(t, NULL, 0, e->pos);
            for (int i = 0; i < e->n; i++) {
                Value v = Eval(e->xs[i], s);
                InstanceCall(o, Intern("Add", 3), &v, 1, NULL, s);
            }
            return o;
        }
    }
    if (e->k == N_LAMBDA && t->id == TY_FUNC && t->nargs) {
        Value f = MakeFunc(e, s);
        if (t->name && t->name->id == K_Func) {
            f.fn->ptypes = t->args;
            f.fn->ret = t->args[t->nargs - 1];
        } else {
            f.fn->ptypes = t->args;                 // Action<...>, Predicate<T>
            if (t->name && t->name->id == K_Predicate) {
                static TypeRef boolRef;
                boolRef.id = T_BOOL;
                f.fn->ret = &boolRef;
            }
        }
        return f;
    }
    return Eval(e, s);
}

// ---------------------------------------------------------------------------
// statements

static void Hoist(Node *aBlock, Scope *s)
{
    for (int i = 0; i < aBlock->n; i++) {
        Node *st = aBlock->xs[i];
        if (st->k == S_FUNC) {
            Var *v = Declare(s, st->name, T_OBJECT, st->pos);
            v->v = MakeFunc(st, s);
        }
    }
}

static int ExecBlock(Node *b, Scope *up)
{
    Scope *s = NewScope(up);
    bool saved = gChecked;
    if (b->op) gChecked = b->op == 1;
    Hoist(b, s);
    int flow = FL_NORMAL;
    for (int i = 0; i < b->n && flow == FL_NORMAL; i++)
        flow = Exec(b->xs[i], s);
    gChecked = saved;
    ReleaseScope(s);
    return flow;
}

static void ExecVar(Node *st, Scope *s)
{
    TypeRef *t = st->type;
    int ty = TypeTy(t);
    for (int i = 0; i < st->n; i++) {
        Node *d = st->xs[i];
        Value v = d->a ? EvalTarget(d->a, t, s) : DefaultOf(t->nullable ? T_OBJECT : ty);
        if (!d->a && (t->nullable || t->rank || ty == T_OBJECT || ty == T_STRING))
            v = VNull();
        int vty = ty;
        if (t->isVar) {
            if (v.t == T_NULL && d->a && d->a->k == N_LIT)
                CompileError(d->pos, "Cannot assign <null> to an implicitly-typed variable");
            if (IsVoid(v))
                CompileError(d->pos, "Cannot assign void to an implicitly-typed variable");
            vty = (v.t <= T_STRING && v.t != T_NULL) ? v.t : T_OBJECT;
        } else if (d->a) {
            v = ConvertImplicit(v, ty, d->a->flags & F_CONST, d->pos);
        }
        Var *var = Declare(s, d->name, vty, d->pos);
        var->v = v;
    }
}

static int Exec(Node *st, Scope *s)
{
    Tick();
    switch (st->k) {
    case S_BLOCK:
        return ExecBlock(st, s);
    case S_EMPTY:
    case S_FUNC:
        return FL_NORMAL;
    case S_VAR:
        ExecVar(st, s);
        return FL_NORMAL;
    case S_EXPR: {
        Value v = Eval(st->a, s);
        if (st->flags & F_RESULT)
            Dump(v, NULL, true);
        return FL_NORMAL;
    }
    case S_IF:
        if (Truthy(Eval(st->a, s), st->pos))
            return Exec(st->b, s);
        return st->c ? Exec(st->c, s) : FL_NORMAL;
    case S_WHILE:
        while (Truthy(Eval(st->a, s), st->pos)) {
            int f = Exec(st->c, s);
            if (f == FL_BREAK) break;
            if (f == FL_RETURN) return f;
        }
        return FL_NORMAL;
    case S_DO:
        do {
            int f = Exec(st->c, s);
            if (f == FL_BREAK) break;
            if (f == FL_RETURN) return f;
        } while (Truthy(Eval(st->a, s), st->pos));
        return FL_NORMAL;
    case S_FOR: {
        Scope *fs = NewScope(s);
        if (st->d) {
            if (st->d->k == S_VAR) ExecVar(st->d, fs);
            else for (int i = 0; i < st->d->n; i++) Eval(st->d->xs[i], fs);
        }
        int flow = FL_NORMAL;
        for (;;) {
            if (st->a && !Truthy(Eval(st->a, fs), st->pos)) break;
            int f = Exec(st->c, fs);
            if (f == FL_BREAK) break;
            if (f == FL_RETURN) { flow = f; break; }
            if (st->b) for (int i = 0; i < st->b->n; i++) Eval(st->b->xs[i], fs);
        }
        ReleaseScope(fs);
        return flow;
    }
    case S_FOREACH: {
        Value coll = Eval(st->a, s);
        if (coll.t == T_NULL)
            ThrowAt(st->pos, "NullReferenceException", "Object reference not set to an instance of an object.");
        Iter *it = OpenSeq(coll, st->pos);
        int ty = TypeTy(st->type);
        Value v;
        int flow = FL_NORMAL;
        while (it->Next(v)) {
            Scope *ls = NewScope(s);
            Var *var = Declare(ls, st->name, st->type->isVar ? T_OBJECT : ty, st->pos);
            var->v = st->type->isVar ? v : ConvertExplicit(v, ty, st->pos);
            int f = Exec(st->c, ls);
            ReleaseScope(ls);
            if (f == FL_BREAK) break;
            if (f == FL_RETURN) { flow = f; break; }
        }
        return flow;
    }
    case S_BREAK:
        return FL_BREAK;
    case S_CONTINUE:
        return FL_CONTINUE;
    case S_RETURN:
        g.result = st->a ? Eval(st->a, s) : VVoid();
        return FL_RETURN;
    case S_THROW:
        if (!st->a)
            ThrowAt(st->pos, "Exception", "throw; is only allowed in a catch block");
        ThrowValue(Eval(st->a, s), st->pos);
    case S_SWITCH: {
        Value v = Eval(st->a, s);
        Scope *ss = NewScope(s);
        Node *pick = NULL, *deflt = NULL;
        for (int i = 0; i < st->n && !pick; i++) {
            Node *sec = st->xs[i];
            for (int j = 0; j < sec->b->n; j++) {
                Node *lab = sec->b->xs[j];
                if (!lab->a) { deflt = sec; continue; }
                if (Match(v, lab->a, ss) && (!lab->b || Truthy(Eval(lab->b, ss), lab->pos))) {
                    pick = sec;
                    break;
                }
            }
        }
        if (!pick) pick = deflt;
        int flow = FL_NORMAL;
        if (pick) {
            Hoist(pick, ss);
            for (int i = 0; i < pick->n && flow == FL_NORMAL; i++)
                flow = Exec(pick->xs[i], ss);
        }
        ReleaseScope(ss);
        return flow == FL_BREAK ? FL_NORMAL : flow;
    }
    default:
        CompileError(st->pos, "This statement is not supported");
    }
}

// The program: runs the top-level block.
void RunProgram(Program &p)
{
    Scope *s = NewScope(NULL);
    Capture(s);
    Hoist(p.body, s);
    for (int i = 0; i < p.body->n; i++) {
        int flow = Exec(p.body->xs[i], s);
        if (flow == FL_RETURN) {
            if (!IsVoid(g.result)) Dump(g.result, NULL, true);
            break;
        }
        if (flow != FL_NORMAL) break;
    }
}

} // namespace rs
