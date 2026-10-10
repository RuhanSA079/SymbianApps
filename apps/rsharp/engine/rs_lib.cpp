/*
 * rs_lib.cpp: the library. Static members and methods of Math, Convert,
 * BitOperations, Console, string, char, the number types and Array;
 * instance members of strings, arrays, List, Dictionary, HashSet,
 * StringBuilder, tuples and anonymous objects; indexers; `new`.
 * LINQ methods are in rs_linq.cpp.
 */
#include "rs_int.h"
#include <math.h>
#include <stdio.h>

namespace rs {

static int PosOf(Node *aCall) { return aCall ? aCall->pos : -1; }

// C# reports these when it compiles; rSharp finds them when the code runs,
// but reports them the same way (nothing else of the run is shown).
RS_NORETURN static void NoOverload(Name *aName, int aCount, int aPos)
{
    CompileError(aPos, "No overload for method '%s' takes %d argument%s",
                 aName->s, aCount, aCount == 1 ? "" : "s");
}

RS_NORETURN static void NoMember(const Value &v, Name *aName, int aPos)
{
    CompileError(aPos, "'%s' does not contain a definition for '%s'", ValTyName(v), aName->s);
}

RS_NORETURN static void NoStatic(int aType, Name *aName, int aPos)
{
    const char *tn = aType < T_COUNT ? TyName(aType) :
        aType == TY_MATH ? "Math" : aType == TY_CONSOLE ? "Console" : aType == TY_ENUMERABLE ? "Enumerable" :
        aType == TY_CONVERT ? "Convert" : aType == TY_BITOPS ? "BitOperations" : aType == TY_ARRAYT ? "Array" :
        aType == TY_LIST ? "List" : aType == TY_DICT ? "Dictionary" : aType == TY_SET ? "HashSet" :
        aType == TY_SB ? "StringBuilder" : aType == TY_TUPLE ? "Tuple" : "this type";
    CompileError(aPos, "'%s' does not contain a definition for '%s' (or rSharp doesn't support it yet)",
                 tn, aName->s);
}

static const Str *ArgStr(const Value &v, const char *aParam, int aPos)
{
    if (v.t == T_STRING) return v.s;
    if (v.t == T_NULL)
        ThrowAt(aPos, "ArgumentNullException", "Value cannot be null. (Parameter '%s')", aParam);
    ThrowAt(aPos, "InvalidCastException", "Argument '%s': cannot convert from '%s' to 'string'", aParam, ValTyName(v));
}

static Value VArr(Arr *a)
{
    Value v;
    v.t = T_ARRAY;
    v.a = a;
    return v;
}

static Value VList(Arr *a)
{
    Value v;
    v.t = T_LIST;
    v.a = a;
    return v;
}

static Value VUnsigned(int t, u64 x)
{
    return VNum(t, (i64)x);
}

// ---------------------------------------------------------------------------
// characters (ASCII and Latin-1; enough for a phone)

static bool CIsUpper(unsigned c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) || (c >= 0x410 && c <= 0x42F); }
static bool CIsLower(unsigned c) { return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9) || (c >= 0x430 && c <= 0x44F); }
static bool CIsDigit(unsigned c) { return c >= '0' && c <= '9'; }
static bool CIsLetter(unsigned c) { return CIsUpper(c) || CIsLower(c) || c == 0xAA || c == 0xBA || (c >= 0x100 && c <= 0x24F) || (c >= 0x370 && c <= 0x52F) || (c >= 0x4E00 && c <= 0x9FFF); }
static bool CIsWhite(unsigned c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000; }
static bool CIsPunct(unsigned c) { return c < 0x80 && strchr("!\"#%&'()*,-./:;?@[\\]_{}", (int)c) && c; }
static bool CIsSymbol(unsigned c) { return c < 0x80 && strchr("$+<=>^`|~", (int)c) && c; }
static unsigned CUpper(unsigned c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    return c;
}
static unsigned CLower(unsigned c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    return c;
}

// ---------------------------------------------------------------------------
// types by name

int TypeIdOf(Name *aName)
{
    switch (aName->id) {
    case K_int: case K_Int32: return T_INT;
    case K_uint: case K_UInt32: return T_UINT;
    case K_long: case K_Int64: return T_LONG;
    case K_ulong: case K_UInt64: return T_ULONG;
    case K_short: case K_Int16: return T_SHORT;
    case K_ushort: case K_UInt16: return T_USHORT;
    case K_byte: case K_Byte: return T_BYTE;
    case K_sbyte: case K_SByte: return T_SBYTE;
    case K_char: case K_Char: return T_CHAR;
    case K_bool: case K_Boolean: return T_BOOL;
    case K_float: case K_Single: return T_FLOAT;
    case K_double: case K_Double: return T_DOUBLE;
    case K_string: case K_String: return T_STRING;
    case K_object: case K_Object: case K_dynamic: return T_OBJECT;
    case K_Math: return TY_MATH;
    case K_Console: return TY_CONSOLE;
    case K_Enumerable: return TY_ENUMERABLE;
    case K_Convert: return TY_CONVERT;
    case K_BitOperations: return TY_BITOPS;
    case K_Array: return TY_ARRAYT;
    case K_List: return TY_LIST;
    case K_Dictionary: return TY_DICT;
    case K_HashSet: return TY_SET;
    case K_StringBuilder: return TY_SB;
    case K_IEnumerable: case K_IList: case K_ICollection: case K_IReadOnlyList:
    case K_IReadOnlyCollection: case K_IOrderedEnumerable: return TY_ENUMERABLE_T;
    case K_IGrouping: return TY_GROUPING;
    case K_Func: case K_Action: case K_Predicate: return TY_FUNC;
    case K_KeyValuePair: return TY_KVP;
    case K_ValueTuple: case K_Tuple: return TY_TUPLE;
    case K_StringSplitOptions: return TY_SPLITOPT;
    case K_MidpointRounding: return TY_MIDPOINT;
    case K_Environment: return TY_ENVIRONMENT;
    case K_Exception: return TY_EXCEPTION;
    default:
        break;
    }
    if (aName->len > 9 && !strcmp(aName->s + aName->len - 9, "Exception"))
        return TY_EXCEPTION;
    return -1;
}

// ---------------------------------------------------------------------------
// output

void Out(const Str *s)
{
    if (g.outFull) return;
    size_t room = g.outLimit > (size_t)g.out.n ? g.outLimit - g.out.n : 0;
    if ((size_t)s->n > room) {
        SbAppendChars(&g.out, s->c, (int)room);
        SbAppendAscii(&g.out, "\n... (output truncated)\n");
        g.outFull = true;
        return;
    }
    SbAppend(&g.out, s);
}

void OutAscii(const char *s)
{
    Out(StrFromAscii(s));
}

// ---------------------------------------------------------------------------
// static members

static Value MaxOf(int t, bool aMax)
{
    switch (t) {
    case T_SBYTE: return VNum(t, aMax ? 127 : -128);
    case T_BYTE: return VNum(t, aMax ? 255 : 0);
    case T_SHORT: return VNum(t, aMax ? 32767 : -32768);
    case T_USHORT: case T_CHAR: return VNum(t, aMax ? 65535 : 0);
    case T_INT: return VNum(t, aMax ? 2147483647LL : -2147483648LL);
    case T_UINT: return VNum(t, aMax ? 0xFFFFFFFFLL : 0);
    case T_LONG: return VLong(aMax ? 0x7FFFFFFFFFFFFFFFLL : (i64)0x8000000000000000ull);
    case T_ULONG: return VUnsigned(t, aMax ? ~0ull : 0);
    case T_FLOAT: return VReal(t, aMax ? 3.4028234663852886e38 : -3.4028234663852886e38);
    default: return VDouble(aMax ? 1.7976931348623157e308 : -1.7976931348623157e308);
    }
}

Value StaticMember(int aType, Name *aName, int aPos)
{
    int id = aName->id;
    if (IsNumeric(aType)) {
        if (id == K_MaxValue) return MaxOf(aType, true);
        if (id == K_MinValue) return MaxOf(aType, false);
        if (IsReal(aType)) {
            if (id == K_NaN) return VReal(aType, NAN);
            if (id == K_PositiveInfinity) return VReal(aType, INFINITY);
            if (id == K_NegativeInfinity) return VReal(aType, -INFINITY);
            if (id == K_Epsilon) return VReal(aType, aType == T_FLOAT ? 1.401298464324817e-45 : 4.9406564584124654e-324);
            if (id == K_PI) return VReal(aType, M_PI);
            if (id == K_E) return VReal(aType, M_E);
            if (id == K_Tau) return VReal(aType, 2 * M_PI);
        }
    }
    switch (aType) {
    case T_STRING:
        if (id == K_Empty) return VStr((Str *)EmptyStr());
        break;
    case T_BOOL:
        break;
    case TY_MATH:
        if (id == K_PI) return VDouble(M_PI);
        if (id == K_E) return VDouble(M_E);
        if (id == K_Tau) return VDouble(2 * M_PI);
        break;
    case TY_SPLITOPT:
        if (id == K_None) return VEnum(TY_SPLITOPT, 0);
        if (id == K_RemoveEmptyEntries) return VEnum(TY_SPLITOPT, 1);
        if (id == K_TrimEntries) return VEnum(TY_SPLITOPT, 2);
        break;
    case TY_MIDPOINT:
        if (id == K_ToEven) return VEnum(TY_MIDPOINT, 0);
        if (id == K_AwayFromZero) return VEnum(TY_MIDPOINT, 1);
        if (id == K_ToZero) return VEnum(TY_MIDPOINT, 2);
        if (id == K_ToNegativeInfinity) return VEnum(TY_MIDPOINT, 3);
        if (id == K_ToPositiveInfinity) return VEnum(TY_MIDPOINT, 4);
        break;
    case TY_ENVIRONMENT:
        if (id == K_NewLine) return VStr(StrFromAscii("\n"));
        break;
    default:
        break;
    }
    if (id != K_NONE) {
        // a method group: Math.Sqrt, int.Parse, ...
        Func *f = New<Func>();
        f->typeId = aType;
        f->nameId = id;
        Value v;
        v.t = T_FUNC;
        v.fn = f;
        return v;
    }
    NoStatic(aType, aName, aPos);
}

// ---------------------------------------------------------------------------
// Math

static double RoundEven(double x)
{
    double f = floor(x), d = x - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return fmod(f, 2) == 0 ? f : f + 1;
}

static double RoundMode(double x, int aMode)
{
    switch (aMode) {
    case 1: return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);       // AwayFromZero
    case 2: return trunc(x);
    case 3: return floor(x);
    case 4: return ceil(x);
    default: return RoundEven(x);
    }
}

static double RoundDigits(double x, int aDigits, int aMode, int aPos)
{
    if (aDigits < 0 || aDigits > 15)
        ThrowAt(aPos, "ArgumentOutOfRangeException", "Rounding digits must be between 0 and 15, inclusive. (Parameter 'digits')");
    if (x != x || x == INFINITY || x == -INFINITY) return x;
    double p = pow(10.0, aDigits);
    double y = x * p;
    if (fabs(y) >= 1e16) return x;
    return RoundMode(y, aMode) / p;
}

static int NumArg(const Value &v, Name *aName, int aPos)
{
    if (!IsNumeric(v.t))
        ThrowAt(aPos, "InvalidCastException", "%s: cannot convert from '%s' to a number", aName->s, ValTyName(v));
    return v.t;
}

static Value MathCall(Name *nm, Value *a, int n, int pos)
{
    int id = nm->id;
    if (n >= 1) NumArg(a[0], nm, pos);
    if (n >= 2 && id != K_Round) NumArg(a[1], nm, pos);
    switch (id) {
    case K_Abs: {
        if (n != 1) break;
        Value v = a[0];
        if (IsReal(v.t)) return VReal(v.t, fabs(v.d));
        if (IsUnsigned(v.t)) return v;
        int t = v.t == T_SBYTE || v.t == T_SHORT || v.t == T_INT || v.t == T_LONG ? v.t : T_INT;
        if (v.i == MaxOf(t, false).i && t != T_CHAR)
            ThrowAt(pos, "OverflowException", "Negating the minimum value of a twos complement number is invalid.");
        return VNum(t, v.i < 0 ? -v.i : v.i);
    }
    case K_Max:
    case K_Min: {
        if (n != 2) break;
        int t = a[0].t == a[1].t ? a[0].t : PromoteTypes(a[0].t, a[1].t);
        Value x = NumTo(a[0], t, false, pos), y = NumTo(a[1], t, false, pos);
        if (IsReal(t) && (x.d != x.d || y.d != y.d)) return VReal(t, NAN);
        int c = Compare(x, y, pos);
        return (id == K_Max) == (c >= 0) ? x : y;
    }
    case K_Clamp: {
        if (n != 3) break;
        NumArg(a[2], nm, pos);
        int t = PromoteTypes(PromoteTypes(a[0].t, a[1].t), a[2].t);
        if (a[0].t == a[1].t && a[1].t == a[2].t) t = a[0].t;
        Value v = NumTo(a[0], t, false, pos), lo = NumTo(a[1], t, false, pos), hi = NumTo(a[2], t, false, pos);
        if (Compare(lo, hi, pos) > 0)
            ThrowAt(pos, "ArgumentException", "'%s' cannot be greater than %s.", StrUtf8(ToStr(lo)), StrUtf8(ToStr(hi)));
        if (Compare(v, lo, pos) < 0) return lo;
        if (Compare(v, hi, pos) > 0) return hi;
        return v;
    }
    case K_Sign: {
        if (n != 1) break;
        if (IsReal(a[0].t)) {
            if (a[0].d != a[0].d)
                ThrowAt(pos, "ArithmeticException", "Function does not accept floating point Not-a-Number values.");
            return VInt(a[0].d > 0 ? 1 : a[0].d < 0 ? -1 : 0);
        }
        if (IsUnsigned(a[0].t)) return VInt(a[0].u ? 1 : 0);
        return VInt(a[0].i > 0 ? 1 : a[0].i < 0 ? -1 : 0);
    }
    case K_Round: {
        if (n < 1 || n > 3) break;
        double x = ToDouble(a[0]);
        int digits = 0, mode = 0;
        if (n >= 2) {
            if (a[1].t == T_ENUM) mode = EnumVal(a[1], TY_MIDPOINT);
            else digits = ToIndex(a[1], pos);
        }
        if (n == 3) mode = EnumVal(a[2], TY_MIDPOINT);
        if (mode < 0) ThrowAt(pos, "ArgumentException", "Math.Round: the rounding mode must be a MidpointRounding value");
        Value r = VDouble(n == 1 || (n == 2 && a[1].t == T_ENUM) ? RoundMode(x, mode) : RoundDigits(x, digits, mode, pos));
        return a[0].t == T_FLOAT ? VReal(T_FLOAT, r.d) : r;
    }
    case K_DivRem: {
        if (n != 2) break;
        int t = PromoteTypes(a[0].t, a[1].t);
        Value q = Binary(TK_SLASH, a[0], a[1], pos), r = Binary(TK_PERCENT, a[0], a[1], pos);
        (void)t;
        Value tv = VTuple2(q, r);
        tv.o->names[0] = Intern("Quotient", 8);
        tv.o->names[1] = Intern("Remainder", 9);
        return tv;
    }
    case K_BigMul:
        if (n != 2) break;
        return Binary(TK_STAR, NumTo(a[0], T_LONG, false, pos), NumTo(a[1], T_LONG, false, pos), pos);
    default:
        break;
    }
    // double functions
    double x = n >= 1 ? ToDouble(a[0]) : 0, y = n >= 2 ? ToDouble(a[1]) : 0;
    double r;
    bool fl = n >= 1 && a[0].t == T_FLOAT && (n < 2 || a[1].t == T_FLOAT);
    switch (id) {
    case K_Floor: if (n != 1) NoOverload(nm, n, pos); r = floor(x); break;
    case K_Ceiling: if (n != 1) NoOverload(nm, n, pos); r = ceil(x); break;
    case K_Truncate: if (n != 1) NoOverload(nm, n, pos); r = trunc(x); break;
    case K_Sqrt: if (n != 1) NoOverload(nm, n, pos); r = sqrt(x); fl = false; break;
    case K_Cbrt: if (n != 1) NoOverload(nm, n, pos); r = x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3); { double c = round(r); if (c * c * c == x) r = c; } fl = false; break;
    case K_Pow: if (n != 2) NoOverload(nm, n, pos); r = pow(x, y); fl = false; break;
    case K_Exp: if (n != 1) NoOverload(nm, n, pos); r = exp(x); fl = false; break;
    case K_Log:
        if (n == 1) r = log(x);
        else if (n == 2) r = log(x) / log(y);
        else NoOverload(nm, n, pos);
        fl = false;
        break;
    case K_Log10: if (n != 1) NoOverload(nm, n, pos); r = log10(x); fl = false; break;
    case K_Log2: if (n != 1) NoOverload(nm, n, pos); r = log(x) / log(2.0); { double c = round(r); if (c >= 0 && c < 1024 && pow(2.0, c) == x) r = c; } fl = false; break;
    case K_Sin: r = sin(x); fl = false; break;
    case K_Cos: r = cos(x); fl = false; break;
    case K_Tan: r = tan(x); fl = false; break;
    case K_Asin: r = asin(x); fl = false; break;
    case K_Acos: r = acos(x); fl = false; break;
    case K_Atan: r = atan(x); fl = false; break;
    case K_Atan2: if (n != 2) NoOverload(nm, n, pos); r = atan2(x, y); fl = false; break;
    case K_Sinh: r = sinh(x); fl = false; break;
    case K_Cosh: r = cosh(x); fl = false; break;
    case K_Tanh: r = tanh(x); fl = false; break;
    case K_CopySign: if (n != 2) NoOverload(nm, n, pos); r = copysign(x, y); fl = false; break;
    case K_ScaleB: if (n != 2) NoOverload(nm, n, pos); r = ldexp(x, (int)ToI64(a[1], pos)); fl = false; break;
    case K_Hypot: if (n != 2) NoOverload(nm, n, pos); r = sqrt(x * x + y * y); fl = false; break;
    default:
        NoStatic(TY_MATH, nm, pos);
    }
    if (n < 1) NoOverload(nm, n, pos);
    return VReal(fl ? T_FLOAT : T_DOUBLE, r);
}

// ---------------------------------------------------------------------------
// Convert

static const char KDigits[] = "0123456789abcdef";

static Str *ToBase(Value v, int aBase, int aPos)
{
    if (aBase == 10) return ToStr(v);
    if (aBase != 2 && aBase != 8 && aBase != 16)
        ThrowAt(aPos, "ArgumentException", "Invalid Base.");
    if (!IsIntegral(v.t) || v.t == T_CHAR)
        ThrowAt(aPos, "ArgumentException", "Convert.ToString(value, base) needs a byte, short, int or long");
    int bits = Bits(v.t);
    u64 x = v.u;
    if (bits < 64) x &= (1ull << bits) - 1;
    char buf[70];
    int n = 0;
    do {
        buf[n++] = KDigits[x % aBase];
        x /= aBase;
    } while (x);
    Str *s = NewStr(n);
    for (int i = 0; i < n; i++) s->c[i] = buf[n - 1 - i];
    return s;
}

// Convert.ToInt32("ff", 16): the digits as an unsigned number of the type's
// width, then reinterpreted (so "FFFFFFFF" is -1).
static Value FromBase(const Str *s, int aBase, int aTy, int aPos)
{
    if (aBase == 10) {
        Value v;
        if (!ParseNumber(s, aTy, v))
            ThrowAt(aPos, "FormatException", "The input string '%s' was not in a correct format.", StrUtf8(s));
        return v;
    }
    if (aBase != 2 && aBase != 8 && aBase != 16)
        ThrowAt(aPos, "ArgumentException", "Invalid Base.");
    int i = 0, n = s->n;
    while (i < n && CIsWhite(s->c[i])) i++;
    while (n > i && CIsWhite(s->c[n - 1])) n--;
    if (aBase == 16 && n - i > 2 && s->c[i] == '0' && (s->c[i + 1] == 'x' || s->c[i + 1] == 'X')) i += 2;
    if (i >= n)
        ThrowAt(aPos, "FormatException", "Could not find any recognizable digits.");
    int bits = Bits(aTy);
    u64 x = 0;
    for (; i < n; i++) {
        unsigned c = s->c[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        if (d >= aBase)
            ThrowAt(aPos, "FormatException", "Additional non-parsable characters are at the end of the string.");
        u64 nx = x * aBase + d;
        if (nx / aBase != x || (bits < 64 && nx >> bits))
            ThrowAt(aPos, "OverflowException", "Value was either too large or too small for %s.", TooBigFor(aTy));
        x = nx;
    }
    return VNum(aTy, (i64)x);
}

static Value ConvertTo(int aTy, Value *a, int n, Name *nm, int pos)
{
    if (n == 2) {
        if (a[0].t != T_STRING) NoOverload(nm, n, pos);
        return FromBase(a[0].s, ToIndex(a[1], pos), aTy, pos);
    }
    if (n != 1) NoOverload(nm, n, pos);
    Value v = a[0];
    if (v.t == T_NULL) return DefaultOf(aTy);
    if (aTy == T_STRING) return VStr(ToStr(v));
    if (v.t == T_STRING) {
        if (aTy == T_BOOL) {
            char *u = StrUtf8(v.s);
            for (char *p = u; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            if (!strcmp(u, "true")) return VBool(true);
            if (!strcmp(u, "false")) return VBool(false);
            ThrowAt(pos, "FormatException", "String '%s' was not recognized as a valid Boolean.", StrUtf8(v.s));
        }
        if (aTy == T_CHAR) {
            if (v.s->n != 1) ThrowAt(pos, "FormatException", "String must be exactly one character long.");
            return VChar(v.s->c[0]);
        }
        return FromBase(v.s, 10, aTy, pos);
    }
    if (aTy == T_BOOL) {
        if (v.t == T_BOOL) return v;
        if (IsNumeric(v.t) && v.t != T_CHAR) return VBool(IsReal(v.t) ? v.d != 0 : v.u != 0);
        ThrowAt(pos, "InvalidCastException", "Invalid cast from '%s' to 'Boolean'.", ValTyName(v));
    }
    if (v.t == T_BOOL) {
        if (aTy == T_CHAR) ThrowAt(pos, "InvalidCastException", "Invalid cast from 'Boolean' to 'Char'.");
        return NumTo(VInt(v.i), aTy, true, pos);
    }
    if (!IsNumeric(v.t))
        ThrowAt(pos, "InvalidCastException", "Unable to cast object of type '%s' to type '%s'.", ValTyName(v), TyName(aTy));
    if (IsReal(v.t) && IsIntegral(aTy)) {
        if (aTy == T_CHAR) ThrowAt(pos, "InvalidCastException", "Invalid cast from '%s' to 'Char'.", ValTyName(v));
        v = VDouble(RoundEven(v.d));                // Convert rounds half to even
    }
    if ((v.t == T_CHAR && IsReal(aTy)) || (aTy == T_CHAR && IsReal(v.t)))
        ThrowAt(pos, "InvalidCastException", "Invalid cast from '%s' to '%s'.", ValTyName(v), TyName(aTy));
    if (IsReal(v.t) && IsIntegral(aTy) && (v.d != v.d))
        ThrowAt(pos, "OverflowException", "Value was either too large or too small for %s.", TooBigFor(aTy));
    bool saved = gChecked;
    gChecked = true;
    Value r = NumTo(v, aTy, true, pos);
    gChecked = saved;
    return r;
}

// ---------------------------------------------------------------------------
// BitOperations, and int.PopCount(x) etc. (generic math, .NET 7)

static Value BitOp(int id, Value *a, int n, Name *nm, int pos, int aKeepType)
{
    if (n < 1 || !IsIntegral(a[0].t)) NoOverload(nm, n, pos);
    int bits = Bits(a[0].t) == 64 ? 64 : (aKeepType ? Bits(a[0].t) : 32);
    if (aKeepType && Bits(a[0].t) < 32 && id != K_PopCount) bits = Bits(a[0].t);
    u64 x = a[0].u;
    if (bits < 64) x &= (1ull << bits) - 1;
    int rt = aKeepType ? a[0].t : (bits == 64 ? T_ULONG : T_UINT);
    switch (id) {
    case K_PopCount: {
        int c = 0;
        for (u64 y = x; y; y &= y - 1) c++;
        return aKeepType ? VNum(rt, c) : VInt(c);
    }
    case K_LeadingZeroCount: {
        int c = 0;
        for (int b = bits - 1; b >= 0 && !((x >> b) & 1); b--) c++;
        return aKeepType ? VNum(rt, c) : VInt(c);
    }
    case K_TrailingZeroCount: {
        int c = 0;
        if (!x) c = bits;
        else while (!((x >> c) & 1)) c++;
        return aKeepType ? VNum(rt, c) : VInt(c);
    }
    case K_Log2: {
        if (aKeepType && !IsUnsigned(a[0].t) && a[0].i < 0)
            ThrowAt(pos, "ArgumentOutOfRangeException", "value must be non-negative");
        int c = 0;
        for (u64 y = x; y > 1; y >>= 1) c++;
        return aKeepType ? VNum(rt, c) : VInt(c);
    }
    case K_IsPow2:
        if (aKeepType && !IsUnsigned(a[0].t) && a[0].i <= 0) return VBool(false);
        return VBool(x && !(x & (x - 1)));
    case K_RotateLeft:
    case K_RotateRight: {
        if (n != 2) NoOverload(nm, n, pos);
        int k = (int)ToI64(a[1], pos) & (bits - 1);
        if (id == K_RotateRight) k = (bits - k) & (bits - 1);
        u64 r = k ? ((x << k) | (x >> (bits - k))) : x;
        if (bits < 64) r &= (1ull << bits) - 1;
        return VNum(rt, (i64)r);
    }
    default:
        NoStatic(TY_BITOPS, nm, pos);
    }
}

// ---------------------------------------------------------------------------
// strings

static Value StrJoin(Value sep, Value *vals, int n, int pos)
{
    const Str *sp = sep.t == T_CHAR ? NULL : sep.t == T_NULL ? EmptyStr() : ArgStr(sep, "separator", pos);
    rs_char sc = sep.t == T_CHAR ? (rs_char)sep.u : 0;
    SB b = { NULL, 0, 0 };
    bool first = true;
    if (n == 1 && vals[0].t != T_STRING && IsEnumerable(vals[0])) {
        Iter *it = OpenSeq(vals[0], pos);
        Value v;
        while (it->Next(v)) {
            if (!first) { if (sp) SbAppend(&b, sp); else SbAppendChars(&b, &sc, 1); }
            first = false;
            SbAppendValue(&b, v);
            Tick();
        }
    } else {
        for (int i = 0; i < n; i++) {
            if (i) { if (sp) SbAppend(&b, sp); else SbAppendChars(&b, &sc, 1); }
            if (vals[i].t != T_NULL) SbAppend(&b, ToStr(vals[i]));
        }
    }
    return VStr(SbStr(&b));
}

static bool StrIsWhiteOrEmpty(const Str *s)
{
    for (int i = 0; i < s->n; i++)
        if (!CIsWhite(s->c[i])) return false;
    return true;
}

static Value StringStatic(Name *nm, Value *a, int n, int pos)
{
    switch (nm->id) {
    case K_Join:
        if (n < 2) NoOverload(nm, n, pos);
        return StrJoin(a[0], a + 1, n - 1, pos);
    case K_Concat: {
        Value sep = VStr((Str *)EmptyStr());
        return StrJoin(sep, a, n, pos);
    }
    case K_Format:
        if (n < 1) NoOverload(nm, n, pos);
        if (n == 2 && a[1].t == T_ARRAY && a[1].a->et == T_OBJECT)
            return VStr(FormatString(ArgStr(a[0], "format", pos), a[1].a->v, a[1].a->n, pos));
        return VStr(FormatString(ArgStr(a[0], "format", pos), a + 1, n - 1, pos));
    case K_IsNullOrEmpty:
        if (n != 1) NoOverload(nm, n, pos);
        return VBool(a[0].t == T_NULL || (a[0].t == T_STRING && a[0].s->n == 0));
    case K_IsNullOrWhiteSpace:
        if (n != 1) NoOverload(nm, n, pos);
        return VBool(a[0].t == T_NULL || (a[0].t == T_STRING && StrIsWhiteOrEmpty(a[0].s)));
    case K_Compare: {
        if (n < 2 || n > 3) NoOverload(nm, n, pos);
        if (a[0].t == T_NULL || a[1].t == T_NULL)
            return VInt(a[0].t == a[1].t ? 0 : a[0].t == T_NULL ? -1 : 1);
        const Str *x = ArgStr(a[0], "strA", pos), *y = ArgStr(a[1], "strB", pos);
        if (n == 3 && Truthy(a[2], pos)) {
            Str *lx = NewStr(x->n), *ly = NewStr(y->n);
            for (int i = 0; i < x->n; i++) lx->c[i] = (rs_char)CLower(x->c[i]);
            for (int i = 0; i < y->n; i++) ly->c[i] = (rs_char)CLower(y->c[i]);
            return VInt(StrCmpCulture(lx, ly));
        }
        return VInt(StrCmpCulture(x, y));
    }
    case K_CompareOrdinal: {
        if (n != 2) NoOverload(nm, n, pos);
        const Str *x = ArgStr(a[0], "strA", pos), *y = ArgStr(a[1], "strB", pos);
        int m = x->n < y->n ? x->n : y->n;
        for (int i = 0; i < m; i++)
            if (x->c[i] != y->c[i]) return VInt((int)x->c[i] - (int)y->c[i]);
        return VInt(x->n - y->n);
    }
    case K_Equals:
        if (n != 2) NoOverload(nm, n, pos);
        return VBool(OpEquals(a[0], a[1], pos));
    default:
        NoStatic(T_STRING, nm, pos);
    }
}

static Value CharStatic(Name *nm, Value *a, int n, int pos)
{
    if (n == 2 && a[0].t == T_STRING) {
        // char.IsDigit(s, i)
        int i = ToIndex(a[1], pos);
        if (i < 0 || i >= a[0].s->n)
            ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. (Parameter 'index')");
        a[0] = VChar(a[0].s->c[i]);
        n = 1;
    }
    if (n != 1) NoOverload(nm, n, pos);
    if (nm->id == K_Parse) {
        const Str *s = ArgStr(a[0], "s", pos);
        if (s->n != 1) ThrowAt(pos, "FormatException", "String must be exactly one character long.");
        return VChar(s->c[0]);
    }
    if (a[0].t != T_CHAR) {
        if (IsIntegral(a[0].t) && a[0].i >= 0 && a[0].i <= 0xFFFF && (nm->id == K_IsDigit || nm->id == K_IsLetter))
            a[0] = VChar((unsigned)a[0].i);
        else
            ThrowAt(pos, "InvalidCastException", "char.%s: cannot convert from '%s' to 'char'", nm->s, ValTyName(a[0]));
    }
    unsigned c = (unsigned)a[0].u;
    switch (nm->id) {
    case K_IsDigit: return VBool(CIsDigit(c));
    case K_IsLetter: return VBool(CIsLetter(c));
    case K_IsLetterOrDigit: return VBool(CIsLetter(c) || CIsDigit(c));
    case K_IsWhiteSpace: return VBool(CIsWhite(c));
    case K_IsUpper: return VBool(CIsUpper(c));
    case K_IsLower: return VBool(CIsLower(c));
    case K_IsPunctuation: return VBool(CIsPunct(c));
    case K_IsSymbol: return VBool(CIsSymbol(c));
    case K_IsControl: return VBool(c < 0x20 || (c >= 0x7F && c <= 0x9F));
    case K_IsSeparator: return VBool(c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000);
    case K_IsAscii: return VBool(c < 0x80);
    case K_ToUpper: case K_ToUpperInvariant: return VChar(CUpper(c));
    case K_ToLower: case K_ToLowerInvariant: return VChar(CLower(c));
    case K_GetNumericValue: return VDouble(CIsDigit(c) ? c - '0' : -1);
    default: NoStatic(T_CHAR, nm, pos);
    }
}

static Value NumberStatic(int aTy, Name *nm, Value *a, int n, Node *call, Scope *s, int pos)
{
    int id = nm->id;
    switch (id) {
    case K_Parse: {
        if (n != 1) NoOverload(nm, n, pos);
        const Str *str = ArgStr(a[0], "s", pos);
        Value v;
        if (!ParseNumber(str, aTy, v)) {
            Value probe;
            if (IsIntegral(aTy) && ParseNumber(str, T_DOUBLE, probe) && probe.d == trunc(probe.d))
                ThrowAt(pos, "OverflowException", "Value was either too large or too small for %s.", TooBigFor(aTy));
            ThrowAt(pos, "FormatException", "The input string '%s' was not in a correct format.", StrUtf8(str));
        }
        return v;
    }
    case K_TryParse: {
        if (n != 2) NoOverload(nm, n, pos);
        Value v;
        bool ok = a[0].t == T_STRING && ParseNumber(a[0].s, aTy, v);
        SetOut(call, 1, ok ? v : DefaultOf(aTy), s);
        return VBool(ok);
    }
    case K_IsNaN: if (n != 1) break; return VBool(IsReal(a[0].t) && a[0].d != a[0].d);
    case K_IsInfinity: if (n != 1) break; return VBool(IsReal(a[0].t) && (a[0].d == INFINITY || a[0].d == -INFINITY));
    case K_IsPositiveInfinity: if (n != 1) break; return VBool(IsReal(a[0].t) && a[0].d == INFINITY);
    case K_IsNegativeInfinity: if (n != 1) break; return VBool(IsReal(a[0].t) && a[0].d == -INFINITY);
    case K_IsFinite: if (n != 1) break; return VBool(!IsReal(a[0].t) || (a[0].d == a[0].d && a[0].d != INFINITY && a[0].d != -INFINITY));
    case K_IsEvenInteger: case K_IsOddInteger: {
        if (n != 1 || !IsNumeric(a[0].t)) break;
        if (IsReal(a[0].t)) {
            double d = a[0].d;
            if (d != trunc(d) || d != d) return VBool(false);
            return VBool((fmod(fabs(d), 2) == 0) == (id == K_IsEvenInteger));
        }
        return VBool(((a[0].u & 1) == 0) == (id == K_IsEvenInteger));
    }
    case K_PopCount: case K_LeadingZeroCount: case K_TrailingZeroCount: case K_RotateLeft:
    case K_RotateRight: case K_Log2: case K_IsPow2:
        if (IsIntegral(aTy)) {
            if (n >= 1) a[0] = ConvertImplicit(a[0], aTy, true, pos);
            return BitOp(id, a, n, nm, pos, 1);
        }
        if (id == K_Log2) return MathCall(nm, a, n, pos);
        break;
    case K_Abs: case K_Max: case K_Min: case K_Clamp: case K_Sign: case K_Sqrt: case K_Pow:
    case K_Floor: case K_Ceiling: case K_Round: case K_Truncate: case K_Cbrt: case K_Hypot:
    case K_Exp: case K_Log: case K_Log10: case K_Sin: case K_Cos: case K_Tan: case K_Atan2: {
        for (int i = 0; i < n; i++)
            if (IsNumeric(a[i].t) && !(id == K_Round && i > 0)) a[i] = NumTo(a[i], aTy, false, pos);
        Value r = MathCall(nm, a, n, pos);
        if (IsNumeric(r.t) && id != K_Sign && r.t != aTy) r = NumTo(r, aTy, false, pos);
        return r;
    }
    default:
        break;
    }
    NoStatic(aTy, nm, pos);
}

// ---------------------------------------------------------------------------
// Array.*

static Arr *ArgArr(const Value &v, int pos)
{
    if (v.t != T_ARRAY && v.t != T_LIST)
        ThrowAt(pos, "InvalidCastException", "An array was expected, not %s", ValTyName(v));
    return v.a;
}

static Value ListLike(Arr *src, int aKind, int pos, Value pred, int aOp);

static Value ArrayStatic(Name *nm, Value *a, int n, int pos)
{
    switch (nm->id) {
    case K_Empty:
        return VArr(NewArr(0, T_OBJECT));
    case K_Sort:
        if (n < 1 || n > 2) NoOverload(nm, n, pos);
        SortValues(ArgArr(a[0], pos)->v, a[0].a->n, n == 2 ? a[1] : VNull(), pos);
        return VVoid();
    case K_Reverse: {
        if (n != 1) NoOverload(nm, n, pos);
        Arr *x = ArgArr(a[0], pos);
        for (int i = 0, j = x->n - 1; i < j; i++, j--) {
            Value t = x->v[i];
            x->v[i] = x->v[j];
            x->v[j] = t;
        }
        return VVoid();
    }
    case K_IndexOf: case K_LastIndexOf: {
        if (n != 2) NoOverload(nm, n, pos);
        Arr *x = ArgArr(a[0], pos);
        Value v = ConvertImplicit(a[1], x->et, true, pos);
        if (nm->id == K_IndexOf) {
            for (int i = 0; i < x->n; i++) if (ValEquals(x->v[i], v)) return VInt(i);
        } else {
            for (int i = x->n - 1; i >= 0; i--) if (ValEquals(x->v[i], v)) return VInt(i);
        }
        return VInt(-1);
    }
    case K_Fill: {
        if (n != 2) NoOverload(nm, n, pos);
        Arr *x = ArgArr(a[0], pos);
        Value v = ConvertImplicit(a[1], x->et, true, pos);
        for (int i = 0; i < x->n; i++) x->v[i] = v;
        return VVoid();
    }
    case K_Exists: case K_Find: case K_FindAll: case K_FindIndex: case K_ConvertAll: case K_TrueForAll:
        if (n != 2) NoOverload(nm, n, pos);
        return ListLike(ArgArr(a[0], pos), T_ARRAY, pos, a[1], nm->id);
    default:
        NoStatic(TY_ARRAYT, nm, pos);
    }
}

// ---------------------------------------------------------------------------
// static calls

Value StaticCall(int aType, Name *nm, Value *a, int n, Node *call, Scope *s)
{
    int pos = PosOf(call);
    int id = nm->id;
    switch (aType) {
    case TY_MATH:
        return MathCall(nm, a, n, pos);
    case TY_CONSOLE:
        if (id == K_WriteLine || id == K_Write) {
            if (n == 1) {
                if (a[0].t == T_ARRAY && a[0].a->et == T_CHAR) {
                    Str *str = NewStr(a[0].a->n);
                    for (int i = 0; i < a[0].a->n; i++) str->c[i] = (rs_char)a[0].a->v[i].u;
                    Out(str);
                } else if (a[0].t != T_NULL) {
                    Out(ToStr(a[0]));
                }
            } else if (n > 1) {
                Out(FormatString(ArgStr(a[0], "format", pos), a + 1, n - 1, pos));
            }
            if (id == K_WriteLine) OutAscii("\n");
            return VVoid();
        }
        ThrowAt(pos, "InvalidOperationException", "Console.%s is not supported (rSharp has no keyboard input)", nm->s);
    case TY_ENUMERABLE: {
        if (id == K_Range) {
            if (n != 2) NoOverload(nm, n, pos);
            return RangeSeq(ToI64(a[0], pos), ToI64(a[1], pos), pos);
        }
        if (id == K_Repeat) {
            if (n != 2) NoOverload(nm, n, pos);
            return RepeatSeq(a[0], ToI64(a[1], pos), pos);
        }
        if (id == K_Empty) {
            if (n) NoOverload(nm, n, pos);
            return VArr(NewArr(0, T_OBJECT));
        }
        if (n < 1) NoOverload(nm, n, pos);
        bool found;
        Value r = Linq(a[0], id, a + 1, n - 1, call, pos, &found);
        if (found) return r;
        NoStatic(TY_ENUMERABLE, nm, pos);
    }
    case TY_CONVERT:
        switch (id) {
        case K_ToString:
            if (n == 2) return VStr(ToBase(a[0], ToIndex(a[1], pos), pos));
            if (n != 1) NoOverload(nm, n, pos);
            return a[0].t == T_NULL ? VStr((Str *)EmptyStr()) : VStr(ToStr(a[0]));
        case K_ToInt32: return ConvertTo(T_INT, a, n, nm, pos);
        case K_ToInt64: return ConvertTo(T_LONG, a, n, nm, pos);
        case K_ToUInt32: return ConvertTo(T_UINT, a, n, nm, pos);
        case K_ToUInt64: return ConvertTo(T_ULONG, a, n, nm, pos);
        case K_ToInt16: return ConvertTo(T_SHORT, a, n, nm, pos);
        case K_ToUInt16: return ConvertTo(T_USHORT, a, n, nm, pos);
        case K_ToByte: return ConvertTo(T_BYTE, a, n, nm, pos);
        case K_ToSByte: return ConvertTo(T_SBYTE, a, n, nm, pos);
        case K_ToDouble: return ConvertTo(T_DOUBLE, a, n, nm, pos);
        case K_ToSingle: return ConvertTo(T_FLOAT, a, n, nm, pos);
        case K_ToChar: return ConvertTo(T_CHAR, a, n, nm, pos);
        case K_ToBoolean: return ConvertTo(T_BOOL, a, n, nm, pos);
        default: NoStatic(TY_CONVERT, nm, pos);
        }
    case TY_BITOPS:
        if (n >= 1 && IsIntegral(a[0].t) && !IsUnsigned(a[0].t) && a[0].t != T_LONG) a[0] = NumTo(a[0], T_UINT, false, pos);
        if (n >= 1 && a[0].t == T_LONG) a[0] = NumTo(a[0], T_ULONG, false, pos);
        return BitOp(id, a, n, nm, pos, 0);
    case TY_ARRAYT:
        return ArrayStatic(nm, a, n, pos);
    case TY_TUPLE: case TY_KVP:
        if (id == K_Create) {
            if (aType == TY_KVP) {
                if (n != 2) NoOverload(nm, n, pos);
                return VKvp(a[0], a[1]);
            }
            if (n < 1 || n > 7) NoOverload(nm, n, pos);
            Obj *o = NewObj(OK_TUPLE, n);
            for (int i = 0; i < n; i++) o->f[i] = a[i];
            Value v;
            v.t = T_OBJ;
            v.o = o;
            return v;
        }
        NoStatic(aType, nm, pos);
    case T_STRING:
        return StringStatic(nm, a, n, pos);
    case T_CHAR:
        return CharStatic(nm, a, n, pos);
    default:
        if (IsNumeric(aType))
            return NumberStatic(aType, nm, a, n, call, s, pos);
        if (aType == T_BOOL && id == K_Parse && n == 1) {
            Value v = ConvertTo(T_BOOL, a, 1, nm, pos);
            return v;
        }
        if (aType == T_BOOL && id == K_TryParse && n == 2) {
            const Str *str = a[0].t == T_STRING ? a[0].s : EmptyStr();
            char *u = StrUtf8(str);
            for (char *p = u; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            bool t = !strcmp(u, "true"), f = !strcmp(u, "false");
            SetOut(call, 1, VBool(t), s);
            return VBool(t || f);
        }
        NoStatic(aType, nm, pos);
    }
}

// ---------------------------------------------------------------------------
// instance members

Value InstanceMember(Value o, Name *nm, int pos)
{
    int id = nm->id;
    switch (o.t) {
    case T_STRING:
        if (id == K_Length) return VInt(o.s->n);
        break;
    case T_ARRAY:
        if (id == K_Length) return VInt(o.a->n);
        if (id == K_LongLength) return VLong(o.a->n);
        break;
    case T_LIST:
        if (id == K_Count) return VInt(o.a->n);
        if (id == K_Capacity) return VInt(o.a->cap);
        break;
    case T_DICT:
    case T_SET:
        if (id == K_Count) return VInt(o.m->keys->n);
        if (o.t == T_DICT && (id == K_Keys || id == K_Values)) {
            Arr *src = id == K_Keys ? o.m->keys : o.m->vals;
            Arr *r = NewArr(src->n, id == K_Keys ? o.m->kt : o.m->vt);
            for (int i = 0; i < src->n; i++) ArrPush(r, src->v[i]);
            return VArr(r);
        }
        break;
    case T_GROUP:
        if (id == K_Key) return o.g->key;
        break;
    case T_SB:
        if (id == K_Length) return VInt(o.sb->n);
        break;
    case T_OBJ: {
        Obj *ob = o.o;
        for (int i = 0; i < ob->n; i++) {
            if (ob->kind == OK_EXC) {
                if (id == K_Message) return ob->f[0];
                break;
            }
            if (ob->names[i] == nm) return ob->f[i];
            if (ob->kind == OK_TUPLE && id >= K_Item1 && id <= K_Item7 && id - K_Item1 == i) return ob->f[i];
        }
        break;
    }
    case T_TYPE:
        if (id == K_Name) {
            if (o.i < T_COUNT) return VStr(StrFromAscii(TyName((int)o.i)));
        }
        break;
    default:
        break;
    }
    NoMember(o, nm, pos);
}

void MemberSet(Value o, Name *nm, Value v, int pos)
{
    if (o.t == T_OBJ && o.o->kind == OK_TUPLE) {
        for (int i = 0; i < o.o->n; i++)
            if (o.o->names[i] == nm || (nm->id >= K_Item1 && nm->id <= K_Item7 && nm->id - K_Item1 == i)) {
                o.o->f[i] = v;
                return;
            }
    }
    if (o.t == T_OBJ && o.o->kind == OK_ANON)
        ThrowAt(pos, "InvalidOperationException", "Property or indexer '%s' of an anonymous type cannot be assigned to -- it is read only", nm->s);
    if (o.t == T_LIST && nm->id == K_Capacity) return;
    if (o.t == T_SB && nm->id == K_Length) {
        int len = ToIndex(v, pos);
        if (len < 0) ThrowAt(pos, "ArgumentOutOfRangeException", "Length cannot be less than zero.");
        while (o.sb->n < len) SbAppendAscii(o.sb, " ");
        o.sb->n = len;
        return;
    }
    ThrowAt(pos, "InvalidOperationException", "'%s.%s' cannot be assigned to", ValTyName(o), nm->s);
}

// ---------------------------------------------------------------------------
// indexers

static int CheckIndex(int i, int n, int pos)
{
    if (i < 0 || i >= n)
        ThrowAt(pos, "IndexOutOfRangeException", "Index was outside the bounds of the array (index %d, length %d).", i, n);
    return i;
}

Value IndexGet(Value o, Value *idx, int n, int pos)
{
    if (n != 1)
        ThrowAt(pos, "InvalidOperationException", "Wrong number of indices inside []; expected 1");
    switch (o.t) {
    case T_STRING:
        return VChar(o.s->c[CheckIndex(ToIndex(idx[0], pos), o.s->n, pos)]);
    case T_ARRAY:
        return o.a->v[CheckIndex(ToIndex(idx[0], pos), o.a->n, pos)];
    case T_LIST: {
        int i = ToIndex(idx[0], pos);
        if (i < 0 || i >= o.a->n)
            ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. Must be non-negative and less than the size of the collection. (Parameter 'index')");
        return o.a->v[i];
    }
    case T_DICT: {
        int at = MapFind(o.m, idx[0]);
        if (at < 0)
            ThrowAt(pos, "Collections.Generic.KeyNotFoundException", "The given key '%s' was not present in the dictionary.", StrUtf8(ToStr(idx[0])));
        return o.m->vals->v[at];
    }
    case T_SB:
        return VChar(o.sb->c[CheckIndex(ToIndex(idx[0], pos), o.sb->n, pos)]);
    case T_OBJ:
        if (o.o->kind == OK_TUPLE) break;
        break;
    default:
        break;
    }
    ThrowAt(pos, "InvalidOperationException", "Cannot apply indexing with [] to a value of type '%s'%s", ValTyName(o),
            o.t == T_SEQ || o.t == T_GROUP ? " (use ElementAt(i) or ToList())" : "");
}

void IndexSet(Value o, Value *idx, int n, Value v, int pos)
{
    if (n != 1)
        ThrowAt(pos, "InvalidOperationException", "Wrong number of indices inside []; expected 1");
    switch (o.t) {
    case T_ARRAY:
        o.a->v[CheckIndex(ToIndex(idx[0], pos), o.a->n, pos)] = ConvertImplicit(v, o.a->et, false, pos);
        return;
    case T_LIST: {
        int i = ToIndex(idx[0], pos);
        if (i < 0 || i >= o.a->n)
            ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. Must be non-negative and less than the size of the collection. (Parameter 'index')");
        o.a->v[i] = ConvertImplicit(v, o.a->et, false, pos);
        return;
    }
    case T_DICT:
        MapAdd(o.m, ConvertImplicit(idx[0], o.m->kt, true, pos), ConvertImplicit(v, o.m->vt, true, pos), true);
        return;
    case T_SB: {
        int i = CheckIndex(ToIndex(idx[0], pos), o.sb->n, pos);
        if (v.t != T_CHAR) ThrowAt(pos, "InvalidCastException", "A char was expected");
        o.sb->c[i] = (rs_char)v.u;
        return;
    }
    case T_STRING:
        ThrowAt(pos, "InvalidOperationException", "Property or indexer 'string.this[int]' cannot be assigned to -- it is read only");
    default:
        ThrowAt(pos, "InvalidOperationException", "Cannot apply indexing with [] to a value of type '%s'", ValTyName(o));
    }
}

// ---------------------------------------------------------------------------
// new T(...)

static int ArgTy(TypeRef *t, int i)
{
    return t->nargs > i ? TypeTy(t->args[i]) : T_OBJECT;
}

Value NewObject(TypeRef *t, Value *a, int n, int pos)
{
    Value r;
    switch (t->id) {
    case TY_LIST: {
        int et = ArgTy(t, 0);
        Arr *l = NewArr(n == 1 && IsIntegral(a[0].t) ? (int)(a[0].i > 0 && a[0].i < 1000000 ? a[0].i : 4) : 4, et);
        if (n == 1 && !IsIntegral(a[0].t)) {
            Iter *it = OpenSeq(a[0], pos);
            Value v;
            while (it->Next(v)) ArrPush(l, ConvertImplicit(v, et, false, pos));
        } else if (n > 1) {
            NoOverload(Intern("List", 4), n, pos);
        }
        return VList(l);
    }
    case TY_DICT:
    case TY_SET: {
        Map *m = NewMap(t->id == TY_DICT);
        m->kt = (uint8_t)ArgTy(t, 0);
        m->vt = (uint8_t)ArgTy(t, 1);
        r.t = t->id == TY_DICT ? T_DICT : T_SET;
        r.m = m;
        if (n == 1 && !IsIntegral(a[0].t)) {
            Iter *it = OpenSeq(a[0], pos);
            Value v;
            while (it->Next(v)) {
                if (r.t == T_DICT) {
                    if (v.t != T_OBJ || v.o->kind != OK_KVP)
                        ThrowAt(pos, "InvalidCastException", "A Dictionary can only be copied from key/value pairs");
                    MapAdd(m, v.o->f[0], v.o->f[1], false);
                } else {
                    MapAdd(m, ConvertImplicit(v, m->kt, false, pos), VNull(), false);
                }
            }
        }
        return r;
    }
    case TY_SB: {
        SB *b = New<SB>();
        if (n == 1 && a[0].t == T_STRING) SbAppend(b, a[0].s);
        r.t = T_SB;
        r.sb = b;
        return r;
    }
    case T_STRING: {
        if (n == 2 && a[0].t == T_CHAR) {
            int cnt = ToIndex(a[1], pos);
            if (cnt < 0) ThrowAt(pos, "ArgumentOutOfRangeException", "Count cannot be less than zero.");
            Str *s = NewStr(cnt);
            for (int i = 0; i < cnt; i++) s->c[i] = (rs_char)a[0].u;
            return VStr(s);
        }
        if (n >= 1 && (a[0].t == T_ARRAY || a[0].t == T_LIST || a[0].t == T_SEQ)) {
            Arr *x = ToArr(a[0], pos);
            int from = n >= 2 ? ToIndex(a[1], pos) : 0;
            int cnt = n >= 3 ? ToIndex(a[2], pos) : x->n - from;
            if (from < 0 || cnt < 0 || from + cnt > x->n)
                ThrowAt(pos, "ArgumentOutOfRangeException", "Index and count must refer to a location within the array.");
            Str *s = NewStr(cnt);
            for (int i = 0; i < cnt; i++) {
                Value c = x->v[from + i];
                if (c.t != T_CHAR) ThrowAt(pos, "InvalidCastException", "new string(...) needs chars");
                s->c[i] = (rs_char)c.u;
            }
            return VStr(s);
        }
        NoOverload(Intern("string", 6), n, pos);
    }
    case TY_EXCEPTION: {
        Obj *o = NewObj(OK_EXC, 1);
        o->names[0] = t->name;
        o->f[0] = n >= 1 ? a[0] : VStr(StrFromAscii("Exception of type 'System.Exception' was thrown."));
        r.t = T_OBJ;
        r.o = o;
        return r;
    }
    case TY_TUPLE: {
        Obj *o = NewObj(OK_TUPLE, n);
        for (int i = 0; i < n; i++) o->f[i] = ConvertImplicit(a[i], ArgTy(t, i), true, pos);
        r.t = T_OBJ;
        r.o = o;
        return r;
    }
    case TY_KVP:
        if (n != 2) NoOverload(Intern("KeyValuePair", 12), n, pos);
        return VKvp(a[0], a[1]);
    default:
        if (t->id >= 0 && t->id < T_STRING && n == 0)
            return DefaultOf(t->id);
        if (t->id == T_OBJECT && n == 0) {
            Obj *o = NewObj(OK_ANON, 0);
            r.t = T_OBJ;
            r.o = o;
            return r;
        }
        ThrowAt(pos, "InvalidOperationException", "Cannot create an instance of '%s'", t->name ? t->name->s : "?");
    }
}

// ---------------------------------------------------------------------------
// List<T> (and the Array.* helpers): methods taking a predicate or function

static Value ListLike(Arr *src, int aKind, int pos, Value f, int aOp)
{
    switch (aOp) {
    case K_Exists:
        for (int i = 0; i < src->n; i++) if (Truthy(Call1(f, src->v[i], pos), pos)) return VBool(true);
        return VBool(false);
    case K_TrueForAll:
        for (int i = 0; i < src->n; i++) if (!Truthy(Call1(f, src->v[i], pos), pos)) return VBool(false);
        return VBool(true);
    case K_Find:
        for (int i = 0; i < src->n; i++) if (Truthy(Call1(f, src->v[i], pos), pos)) return src->v[i];
        return DefaultOf(src->et);
    case K_FindIndex:
        for (int i = 0; i < src->n; i++) if (Truthy(Call1(f, src->v[i], pos), pos)) return VInt(i);
        return VInt(-1);
    case K_FindAll: {
        Arr *r = NewArr(src->n, src->et);
        for (int i = 0; i < src->n; i++) if (Truthy(Call1(f, src->v[i], pos), pos)) ArrPush(r, src->v[i]);
        return aKind == T_LIST ? VList(r) : VArr(r);
    }
    case K_ConvertAll: {
        Arr *r = NewArr(src->n, T_OBJECT);
        for (int i = 0; i < src->n; i++) ArrPush(r, Call1(f, src->v[i], pos));
        if (r->n) {
            int t = r->v[0].t;
            for (int i = 1; i < r->n; i++) if (r->v[i].t != t) t = T_OBJECT;
            r->et = (uint8_t)(t <= T_STRING ? t : T_OBJECT);
        }
        return aKind == T_LIST ? VList(r) : VArr(r);
    }
    default:
        return VNull();
    }
}

// ---------------------------------------------------------------------------
// string instance methods

static bool HasAt(const Str *s, int i, const Str *what)
{
    return i >= 0 && i + what->n <= s->n && !memcmp(s->c + i, what->c, what->n * sizeof(rs_char));
}

static Str *OneChar(const Value &v)
{
    Str *s = NewStr(1);
    s->c[0] = (rs_char)v.u;
    return s;
}

static const Str *StrOrChar(const Value &v, const char *aParam, int pos)
{
    if (v.t == T_CHAR) return OneChar(v);
    return ArgStr(v, aParam, pos);
}

static Str *Trim(const Str *s, Value *chars, int nchars, int aWhich)
{
    int from = 0, to = s->n;
    struct L {
        static bool In(rs_char c, Value *cs, int n) {
            if (!n) return CIsWhite(c);
            for (int i = 0; i < n; i++) {
                if (cs[i].t == T_CHAR && cs[i].u == c) return true;
                if (cs[i].t == T_ARRAY)
                    for (int k = 0; k < cs[i].a->n; k++)
                        if (cs[i].a->v[k].u == c) return true;
            }
            return false;
        }
    };
    if (aWhich != 2) while (from < to && L::In(s->c[from], chars, nchars)) from++;
    if (aWhich != 1) while (to > from && L::In(s->c[to - 1], chars, nchars)) to--;
    return StrSub(s, from, to - from);
}

static Value Split(const Str *s, Value *a, int n, int pos)
{
    int opts = 0;
    if (n && a[n - 1].t == T_ENUM) {
        opts = EnumVal(a[n - 1], TY_SPLITOPT);
        n--;
    }
    int maxCount = 0x7FFFFFFF;
    if (n == 2 && IsIntegral(a[1].t) && a[1].t != T_CHAR) {
        maxCount = ToIndex(a[1], pos);
        n--;
    }
    // separators: chars or strings
    Str *seps[32];
    int nseps = 0;
    for (int i = 0; i < n && nseps < 32; i++) {
        if (a[i].t == T_CHAR) seps[nseps++] = OneChar(a[i]);
        else if (a[i].t == T_STRING) { if (a[i].s->n) seps[nseps++] = a[i].s; }
        else if (a[i].t == T_ARRAY) {
            for (int k = 0; k < a[i].a->n && nseps < 32; k++) {
                Value e = a[i].a->v[k];
                if (e.t == T_CHAR) seps[nseps++] = OneChar(e);
                else if (e.t == T_STRING && e.s->n) seps[nseps++] = e.s;
            }
        } else if (a[i].t != T_NULL) {
            ThrowAt(pos, "InvalidCastException", "Split: separators must be chars or strings");
        }
    }
    Arr *r = NewArr(8, T_STRING);
    int start = 0, i = 0;
    while (i <= s->n) {
        int sepLen = -1;
        if (i < s->n && r->n < maxCount - 1) {
            if (!nseps) {
                if (CIsWhite(s->c[i])) sepLen = 1;
            } else {
                for (int k = 0; k < nseps; k++)
                    if (HasAt(s, i, seps[k])) { sepLen = seps[k]->n; break; }
            }
        }
        if (sepLen >= 0 || i == s->n) {
            Str *part = StrSub(s, start, i - start);
            if (opts & 2) part = Trim(part, NULL, 0, 0);
            if (!((opts & 1) && part->n == 0)) ArrPush(r, VStr(part));
            if (i == s->n) break;
            i += sepLen;
            start = i;
            continue;
        }
        i++;
    }
    return VArr(r);
}

static Value StringCall(Value o, Name *nm, Value *a, int n, int pos)
{
    const Str *s = o.s;
    int id = nm->id;
    switch (id) {
    case K_ToUpper: case K_ToUpperInvariant: case K_ToLower: case K_ToLowerInvariant: {
        if (n) NoOverload(nm, n, pos);
        Str *r = NewStr(s->n);
        bool up = id == K_ToUpper || id == K_ToUpperInvariant;
        for (int i = 0; i < s->n; i++) r->c[i] = (rs_char)(up ? CUpper(s->c[i]) : CLower(s->c[i]));
        return VStr(r);
    }
    case K_Substring: {
        if (n < 1 || n > 2) NoOverload(nm, n, pos);
        int from = ToIndex(a[0], pos);
        if (from < 0 || from > s->n)
            ThrowAt(pos, "ArgumentOutOfRangeException", "startIndex cannot be larger than length of string. (Parameter 'startIndex')");
        int len = n == 2 ? ToIndex(a[1], pos) : s->n - from;
        if (len < 0 || from + len > s->n)
            ThrowAt(pos, "ArgumentOutOfRangeException", "Index and length must refer to a location within the string. (Parameter 'length')");
        return VStr(StrSub(s, from, len));
    }
    case K_IndexOf: case K_LastIndexOf: {
        if (n < 1 || n > 2) NoOverload(nm, n, pos);
        const Str *w = StrOrChar(a[0], "value", pos);
        if (id == K_IndexOf) {
            int from = n == 2 ? ToIndex(a[1], pos) : 0;
            if (from < 0 || from > s->n) ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. (Parameter 'startIndex')");
            for (int i = from; i + w->n <= s->n; i++) if (HasAt(s, i, w)) return VInt(i);
        } else {
            int from = n == 2 ? ToIndex(a[1], pos) : s->n - 1;
            for (int i = from - w->n + 1 < s->n - w->n ? from - w->n + 1 : s->n - w->n; i >= 0; i--) if (HasAt(s, i, w)) return VInt(i);
        }
        return VInt(-1);
    }
    case K_Contains: case K_StartsWith: case K_EndsWith: {
        if (n != 1) NoOverload(nm, n, pos);
        const Str *w = StrOrChar(a[0], "value", pos);
        if (id == K_StartsWith) return VBool(HasAt(s, 0, w));
        if (id == K_EndsWith) return VBool(HasAt(s, s->n - w->n, w));
        for (int i = 0; i + w->n <= s->n; i++) if (HasAt(s, i, w)) return VBool(true);
        return VBool(false);
    }
    case K_Replace: {
        if (n != 2) NoOverload(nm, n, pos);
        const Str *from = StrOrChar(a[0], "oldValue", pos);
        const Str *to = a[1].t == T_NULL ? EmptyStr() : StrOrChar(a[1], "newValue", pos);
        if (!from->n) ThrowAt(pos, "ArgumentException", "The value cannot be an empty string. (Parameter 'oldValue')");
        SB b = { NULL, 0, 0 };
        for (int i = 0; i < s->n; ) {
            if (HasAt(s, i, from)) { SbAppend(&b, to); i += from->n; }
            else { SbAppendChars(&b, s->c + i, 1); i++; }
        }
        return VStr(SbStr(&b));
    }
    case K_Trim: return VStr(Trim(s, a, n, 0));
    case K_TrimStart: return VStr(Trim(s, a, n, 1));
    case K_TrimEnd: return VStr(Trim(s, a, n, 2));
    case K_Split: return Split(s, a, n, pos);
    case K_PadLeft: case K_PadRight: {
        if (n < 1 || n > 2) NoOverload(nm, n, pos);
        int w = ToIndex(a[0], pos);
        if (w < 0) ThrowAt(pos, "ArgumentOutOfRangeException", "Non-negative number required. (Parameter 'totalWidth')");
        rs_char pad = n == 2 ? (rs_char)a[1].u : ' ';
        if (w <= s->n) return o;
        Str *r = NewStr(w);
        int extra = w - s->n;
        for (int i = 0; i < w; i++) {
            if (id == K_PadLeft) r->c[i] = i < extra ? pad : s->c[i - extra];
            else r->c[i] = i < s->n ? s->c[i] : pad;
        }
        return VStr(r);
    }
    case K_Insert: {
        if (n != 2) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos);
        if (at < 0 || at > s->n) ThrowAt(pos, "ArgumentOutOfRangeException", "Index must be within the bounds of the string. (Parameter 'startIndex')");
        const Str *w = ArgStr(a[1], "value", pos);
        return VStr(StrCat(StrCat(StrSub(s, 0, at), w), StrSub(s, at, s->n - at)));
    }
    case K_Remove: {
        if (n < 1 || n > 2) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos);
        int cnt = n == 2 ? ToIndex(a[1], pos) : s->n - at;
        if (at < 0 || cnt < 0 || at + cnt > s->n)
            ThrowAt(pos, "ArgumentOutOfRangeException", "Index and count must refer to a location within the string.");
        return VStr(StrCat(StrSub(s, 0, at), StrSub(s, at + cnt, s->n - at - cnt)));
    }
    case K_ToCharArray: {
        Arr *r = NewArr(s->n, T_CHAR);
        for (int i = 0; i < s->n; i++) ArrPush(r, VChar(s->c[i]));
        return VArr(r);
    }
    case K_CompareTo:
        if (n != 1) NoOverload(nm, n, pos);
        if (a[0].t == T_NULL) return VInt(1);
        return VInt(StrCmpCulture(s, ArgStr(a[0], "strB", pos)));
    default:
        break;
    }
    bool found;
    Value r = Linq(o, id, a, n, NULL, pos, &found);
    if (found) return r;
    NoMember(o, nm, pos);
}

// ---------------------------------------------------------------------------
// instance calls

static Value SbCall(Value o, Name *nm, Value *a, int n, int pos)
{
    SB *b = o.sb;
    switch (nm->id) {
    case K_Append:
        if (n == 1) SbAppendValue(b, a[0]);
        else if (n == 2 && a[0].t == T_CHAR) { int c = ToIndex(a[1], pos); for (int i = 0; i < c; i++) SbAppend(b, OneChar(a[0])); }
        else NoOverload(nm, n, pos);
        return o;
    case K_AppendLine:
        if (n == 1) SbAppendValue(b, a[0]);
        else if (n > 1) NoOverload(nm, n, pos);
        SbAppendAscii(b, "\n");
        return o;
    case K_Insert: {
        if (n != 2) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos);
        if (at < 0 || at > b->n) ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. (Parameter 'index')");
        Str *tail = NewStr(b->n - at);
        memcpy(tail->c, b->c + at, tail->n * sizeof(rs_char));
        b->n = at;
        SbAppend(b, ToStr(a[1]));
        SbAppend(b, tail);
        return o;
    }
    case K_Remove: {
        if (n != 2) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos), cnt = ToIndex(a[1], pos);
        if (at < 0 || cnt < 0 || at + cnt > b->n) ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range.");
        memmove(b->c + at, b->c + at + cnt, (b->n - at - cnt) * sizeof(rs_char));
        b->n -= cnt;
        return o;
    }
    case K_Replace: {
        if (n != 2) NoOverload(nm, n, pos);
        Str *cur = SbStr(b);
        Value args[2] = { a[0], a[1] };
        Value r = StringCall(VStr(cur), nm, args, 2, pos);
        b->n = 0;
        SbAppend(b, r.s);
        return o;
    }
    case K_Clear:
        b->n = 0;
        return o;
    case K_ToString:
        return VStr(SbStr(b));
    default:
        NoMember(o, nm, pos);
    }
}

static Value ListCall(Value o, Name *nm, Value *a, int n, int pos)
{
    Arr *l = o.a;
    int id = nm->id;
    switch (id) {
    case K_Add:
        if (n != 1) NoOverload(nm, n, pos);
        ArrPush(l, ConvertImplicit(a[0], l->et, true, pos));
        return VVoid();
    case K_AddRange: {
        if (n != 1) NoOverload(nm, n, pos);
        Arr *src = ToArr(a[0], pos);
        for (int i = 0; i < src->n; i++) ArrPush(l, ConvertImplicit(src->v[i], l->et, false, pos));
        return VVoid();
    }
    case K_Insert: {
        if (n != 2) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos);
        if (at < 0 || at > l->n) ThrowAt(pos, "ArgumentOutOfRangeException", "Index must be within the bounds of the List. (Parameter 'index')");
        ArrPush(l, VNull());
        memmove(&l->v[at + 1], &l->v[at], (l->n - 1 - at) * sizeof(Value));
        l->v[at] = ConvertImplicit(a[1], l->et, true, pos);
        return VVoid();
    }
    case K_RemoveAt: {
        if (n != 1) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos);
        if (at < 0 || at >= l->n) ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. Must be non-negative and less than the size of the collection. (Parameter 'index')");
        memmove(&l->v[at], &l->v[at + 1], (l->n - 1 - at) * sizeof(Value));
        l->n--;
        return VVoid();
    }
    case K_Remove: {
        if (n != 1) NoOverload(nm, n, pos);
        Value v = ConvertImplicit(a[0], l->et, true, pos);
        for (int i = 0; i < l->n; i++)
            if (ValEquals(l->v[i], v)) {
                memmove(&l->v[i], &l->v[i + 1], (l->n - 1 - i) * sizeof(Value));
                l->n--;
                return VBool(true);
            }
        return VBool(false);
    }
    case K_RemoveAll: {
        if (n != 1) NoOverload(nm, n, pos);
        int w = 0, removed = 0;
        for (int i = 0; i < l->n; i++) {
            if (Truthy(Call1(a[0], l->v[i], pos), pos)) removed++;
            else l->v[w++] = l->v[i];
        }
        l->n = w;
        return VInt(removed);
    }
    case K_Clear:
        l->n = 0;
        return VVoid();
    case K_Contains: case K_IndexOf: case K_LastIndexOf: {
        if (n != 1) NoOverload(nm, n, pos);
        Value v = ConvertImplicit(a[0], l->et, true, pos);
        if (id == K_LastIndexOf) {
            for (int i = l->n - 1; i >= 0; i--) if (ValEquals(l->v[i], v)) return VInt(i);
            return VInt(-1);
        }
        for (int i = 0; i < l->n; i++)
            if (ValEquals(l->v[i], v)) return id == K_Contains ? VBool(true) : VInt(i);
        return id == K_Contains ? VBool(false) : VInt(-1);
    }
    case K_Sort:
        if (n > 1) NoOverload(nm, n, pos);
        SortValues(l->v, l->n, n ? a[0] : VNull(), pos);
        return VVoid();
    case K_Reverse:
        if (n) break;
        for (int i = 0, j = l->n - 1; i < j; i++, j--) {
            Value t = l->v[i];
            l->v[i] = l->v[j];
            l->v[j] = t;
        }
        return VVoid();
    case K_ForEach:
        if (n != 1) NoOverload(nm, n, pos);
        for (int i = 0; i < l->n; i++) Call1(a[0], l->v[i], pos);
        return VVoid();
    case K_Exists: case K_Find: case K_FindAll: case K_FindIndex: case K_ConvertAll: case K_TrueForAll:
        if (n != 1) NoOverload(nm, n, pos);
        return ListLike(l, T_LIST, pos, a[0], id);
    case K_GetRange: {
        if (n != 2) NoOverload(nm, n, pos);
        int at = ToIndex(a[0], pos), cnt = ToIndex(a[1], pos);
        if (at < 0 || cnt < 0 || at + cnt > l->n) ThrowAt(pos, "ArgumentException", "Offset and length were out of bounds for the array or count is greater than the number of elements from index to the end of the source collection.");
        Arr *r = NewArr(cnt, l->et);
        for (int i = 0; i < cnt; i++) ArrPush(r, l->v[at + i]);
        return VList(r);
    }
    default:
        break;
    }
    bool found;
    Value r = Linq(o, id, a, n, NULL, pos, &found);
    if (found) return r;
    NoMember(o, nm, pos);
}

static Value MapCall(Value o, Name *nm, Value *a, int n, Node *call, Scope *s, int pos)
{
    Map *m = o.m;
    bool dict = o.t == T_DICT;
    int id = nm->id;
    switch (id) {
    case K_Add:
        if (dict) {
            if (n != 2) NoOverload(nm, n, pos);
            Value k = ConvertImplicit(a[0], m->kt, true, pos);
            if (MapAdd(m, k, ConvertImplicit(a[1], m->vt, true, pos), false) < 0)
                ThrowAt(pos, "ArgumentException", "An item with the same key has already been added. Key: %s", StrUtf8(ToStr(k)));
            return VVoid();
        }
        if (n != 1) NoOverload(nm, n, pos);
        return VBool(MapAdd(m, ConvertImplicit(a[0], m->kt, true, pos), VNull(), false) >= 0);
    case K_TryAdd:
        if (!dict || n != 2) break;
        return VBool(MapAdd(m, ConvertImplicit(a[0], m->kt, true, pos), ConvertImplicit(a[1], m->vt, true, pos), false) >= 0);
    case K_ContainsKey: case K_Contains:
        if (n != 1 || (dict && id == K_Contains)) break;
        return VBool(MapFind(m, ConvertImplicit(a[0], m->kt, true, pos)) >= 0);
    case K_ContainsValue:
        if (!dict || n != 1) break;
        for (int i = 0; i < m->vals->n; i++) if (ValEquals(m->vals->v[i], a[0])) return VBool(true);
        return VBool(false);
    case K_Remove:
        if (n != 1) NoOverload(nm, n, pos);
        return VBool(MapRemove(m, ConvertImplicit(a[0], m->kt, true, pos)));
    case K_Clear:
        m->keys->n = 0;
        if (m->vals) m->vals->n = 0;
        if (m->cap) memset(m->slots, 0, m->cap * sizeof(int));
        return VVoid();
    case K_TryGetValue: {
        if (!dict || n != 2) break;
        int at = MapFind(m, ConvertImplicit(a[0], m->kt, true, pos));
        SetOut(call, 1, at >= 0 ? m->vals->v[at] : DefaultOf(m->vt), s);
        return VBool(at >= 0);
    }
    case K_GetValueOrDefault: {
        if (!dict || n < 1 || n > 2) break;
        int at = MapFind(m, ConvertImplicit(a[0], m->kt, true, pos));
        return at >= 0 ? m->vals->v[at] : n == 2 ? a[1] : DefaultOf(m->vt);
    }
    case K_UnionWith: case K_IntersectWith: case K_ExceptWith: {
        if (dict || n != 1) break;
        Arr *other = ToArr(a[0], pos);
        if (id == K_UnionWith) {
            for (int i = 0; i < other->n; i++) MapAdd(m, other->v[i], VNull(), false);
        } else {
            Map *om = NewMap(false);
            for (int i = 0; i < other->n; i++) MapAdd(om, other->v[i], VNull(), false);
            Arr *keep = NewArr(m->keys->n, m->keys->et);
            for (int i = 0; i < m->keys->n; i++)
                if ((MapFind(om, m->keys->v[i]) >= 0) == (id == K_IntersectWith)) ArrPush(keep, m->keys->v[i]);
            m->keys->n = 0;
            m->cap = 0;
            m->slots = NULL;
            for (int i = 0; i < keep->n; i++) MapAdd(m, keep->v[i], VNull(), false);
        }
        return VVoid();
    }
    case K_IsSubsetOf: case K_IsSupersetOf: case K_Overlaps: case K_SetEquals: {
        if (dict || n != 1) break;
        Arr *other = ToArr(a[0], pos);
        Map *om = NewMap(false);
        for (int i = 0; i < other->n; i++) MapAdd(om, other->v[i], VNull(), false);
        if (id == K_Overlaps) {
            for (int i = 0; i < om->keys->n; i++) if (MapFind(m, om->keys->v[i]) >= 0) return VBool(true);
            return VBool(false);
        }
        bool sub = true, super = true;
        for (int i = 0; i < m->keys->n; i++) if (MapFind(om, m->keys->v[i]) < 0) sub = false;
        for (int i = 0; i < om->keys->n; i++) if (MapFind(m, om->keys->v[i]) < 0) super = false;
        return VBool(id == K_IsSubsetOf ? sub : id == K_IsSupersetOf ? super : sub && super);
    }
    default:
        break;
    }
    bool found;
    Value r = Linq(o, id, a, n, call, pos, &found);
    if (found) return r;
    NoMember(o, nm, pos);
}

Value InstanceCall(Value o, Name *nm, Value *a, int n, Node *call, Scope *s)
{
    int pos = PosOf(call);
    int id = nm->id;
    // every object
    switch (id) {
    case K_ToString:
        if (o.t == T_SB) break;
        if (n == 0) return VStr(ToStr(o));
        if (n == 1 && a[0].t == T_STRING) return VStr(FormatValue(o, a[0].s, pos));
        if (n == 1 && a[0].t == T_NULL) return VStr(ToStr(o));
        NoOverload(nm, n, pos);
    case K_Equals:
        if (n == 1) return VBool(o.t == T_STRING || a[0].t == T_STRING ? OpEquals(o, a[0], pos) && a[0].t == o.t : ValEquals(o, a[0]));
        break;
    case K_CompareTo:
        if (n == 1 && o.t != T_STRING) {
            int c = Compare(o, a[0], pos);
            return VInt(c < 0 ? -1 : c > 0 ? 1 : 0);
        }
        break;
    case K_GetType:
        if (n == 0) return VTy(o.t == T_ARRAY ? TY_ARRAYT : o.t == T_LIST ? TY_LIST : o.t);
        break;
    case K_Dump:
        if (n > 1) NoOverload(nm, n, pos);
        Dump(o, n == 1 && a[0].t == T_STRING ? a[0].s : NULL, false);
        return o;
    default:
        break;
    }
    switch (o.t) {
    case T_STRING:
        return StringCall(o, nm, a, n, pos);
    case T_LIST:
        return ListCall(o, nm, a, n, pos);
    case T_DICT:
    case T_SET:
        return MapCall(o, nm, a, n, call, s, pos);
    case T_SB:
        return SbCall(o, nm, a, n, pos);
    case T_FUNC:
        if (id == K_Invoke) return CallFunc(o.fn, a, n, pos);
        break;
    default:
        break;
    }
    bool found;
    Value r = Linq(o, id, a, n, call, pos, &found);
    if (found) return r;
    if (IsNumeric(o.t) && (id == K_PopCount || id == K_IsPow2))
        ThrowAt(pos, "InvalidOperationException", "Use %s.%s(x) or BitOperations.%s(x)", TyName(o.t), nm->s, nm->s);
    NoMember(o, nm, pos);
}

} // namespace rs
