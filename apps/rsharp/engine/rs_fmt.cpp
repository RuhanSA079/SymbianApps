/*
 * rs_fmt.cpp: text. ToString() as .NET does it (shortest round-trip
 * doubles, True/False, { a = 1 } for anonymous objects), number formats
 * ("X8", "N2", "0.00", ...), string.Format, parsing numbers, the
 * LINQPad-style Dump() of results, and rs_run().
 */
#include "rs_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace rs {

// ---------------------------------------------------------------------------
// string builder

void SbAppendChars(SB *b, const rs_char *c, int n)
{
    if (n <= 0) return;
    if (b->n + n > b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->n + n) cap *= 2;
        rs_char *nc = (rs_char *)Alloc(cap * sizeof(rs_char));
        if (b->n) memcpy(nc, b->c, b->n * sizeof(rs_char));
        b->c = nc;
        b->cap = cap;
    }
    memcpy(b->c + b->n, c, n * sizeof(rs_char));
    b->n += n;
}

void SbAppend(SB *b, const Str *s)
{
    SbAppendChars(b, s->c, s->n);
}

void SbAppendAscii(SB *b, const char *s)
{
    while (*s) {
        rs_char c = (unsigned char)*s++;
        SbAppendChars(b, &c, 1);
    }
}

// UTF-8 text (for ∞ and friends)
static void SbAppendUtf8(SB *b, const char *s)
{
    Str *x = StrFromUtf8(s, strlen(s));
    SbAppend(b, x);
}

Str *SbStr(SB *b)
{
    Str *s = NewStr(b->n);
    if (b->n) memcpy(s->c, b->c, b->n * sizeof(rs_char));
    return s;
}

Str *Align(Str *s, int aWidth)
{
    int w = aWidth < 0 ? -aWidth : aWidth;
    if (s->n >= w) return s;
    Str *r = NewStr(w);
    int pad = w - s->n;
    for (int i = 0; i < w; i++) {
        if (aWidth > 0) r->c[i] = i < pad ? ' ' : s->c[i - pad];
        else r->c[i] = i < s->n ? s->c[i] : ' ';
    }
    return r;
}

// ---------------------------------------------------------------------------
// numbers to text

// Shortest digits that read back as the same double (float): digits and
// the decimal exponent of the first digit.
static void Shortest(double d, bool aFloat, char *aDigits, int *aExp)
{
    char buf[40];
    int maxp = aFloat ? 9 : 17;
    for (int p = 1; p <= maxp; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, d);
        double back = strtod(buf, NULL);
        if (aFloat ? (float)back == (float)d : back == d) break;
    }
    // buf: [-]d.ddde[+-]xx
    char *p = buf;
    if (*p == '-') p++;
    int n = 0;
    for (; *p && *p != 'e'; p++)
        if (*p >= '0' && *p <= '9') aDigits[n++] = *p;
    while (n > 1 && aDigits[n - 1] == '0') n--;
    aDigits[n] = 0;
    *aExp = *p == 'e' ? atoi(p + 1) : 0;
}

// a double (float) as .NET writes it; returns the length. Infinity is the
// one non-ASCII case: it is written as "inf" / "-inf" and the caller maps it.
static int RealToAscii(double d, bool aFloat, char *out)
{
    if (d != d) return sprintf(out, "NaN");
    if (d == INFINITY) return sprintf(out, "inf");
    if (d == -INFINITY) return sprintf(out, "-inf");
    if (d == 0) return sprintf(out, signbit(d) ? "-0" : "0");
    char digits[24];
    int e;
    Shortest(d, aFloat, digits, &e);
    int n = 0;
    if (d < 0) out[n++] = '-';
    int nd = strlen(digits);
    // as .NET's "G" for the shortest digits: scientific when the decimal
    // point would be past max(digits, 15 (float: 7)) or before 0.0001
    int maxDigits = nd > (aFloat ? 7 : 15) ? nd : (aFloat ? 7 : 15);
    if (e + 1 > maxDigits || e < -4) {
        out[n++] = digits[0];
        if (nd > 1) {
            out[n++] = '.';
            for (int i = 1; i < nd; i++) out[n++] = digits[i];
        }
        n += sprintf(out + n, "E%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (e >= 0) {
        for (int i = 0; i <= e; i++) out[n++] = i < nd ? digits[i] : '0';
        if (nd > e + 1) {
            out[n++] = '.';
            for (int i = e + 1; i < nd; i++) out[n++] = digits[i];
        }
    } else {
        out[n++] = '0';
        out[n++] = '.';
        for (int i = 0; i < -e - 1; i++) out[n++] = '0';
        for (int i = 0; i < nd; i++) out[n++] = digits[i];
    }
    out[n] = 0;
    return n;
}

static bool IsScalar(const Value &v)
{
    return v.t == T_BOOL || IsNumeric(v.t);
}

// A number, bool or char as text, without allocating; returns the length.
static int ScalarChars(const Value &v, rs_char *out)
{
    char buf[48];
    int n;
    if (v.t == T_BOOL) n = sprintf(buf, v.i ? "True" : "False");
    else if (v.t == T_CHAR) { out[0] = (rs_char)v.u; return 1; }
    else if (IsReal(v.t)) {
        n = RealToAscii(v.d, v.t == T_FLOAT, buf);
        if (!strcmp(buf, "inf") || !strcmp(buf, "-inf")) {
            int k = 0;
            if (buf[0] == '-') out[k++] = '-';
            out[k++] = 0x221E;
            return k;
        }
    }
    else if (IsUnsigned(v.t)) n = sprintf(buf, "%llu", (unsigned long long)v.u);
    else n = sprintf(buf, "%lld", (long long)v.i);
    for (int i = 0; i < n; i++) out[i] = (unsigned char)buf[i];
    return n;
}

void SbAppendValue(SB *b, Value v)
{
    if (IsScalar(v)) {
        rs_char buf[48];
        SbAppendChars(b, buf, ScalarChars(v, buf));
    } else if (v.t != T_NULL) {
        SbAppend(b, ToStr(v));
    }
}

static void RealToSb(SB *b, double d, bool aFloat)
{
    SbAppendValue(b, VReal(aFloat ? T_FLOAT : T_DOUBLE, d));
}

static const char *ClrName(int t)
{
    switch (t) {
    case T_BOOL: return "System.Boolean";
    case T_CHAR: return "System.Char";
    case T_SBYTE: return "System.SByte";
    case T_BYTE: return "System.Byte";
    case T_SHORT: return "System.Int16";
    case T_USHORT: return "System.UInt16";
    case T_INT: return "System.Int32";
    case T_UINT: return "System.UInt32";
    case T_LONG: return "System.Int64";
    case T_ULONG: return "System.UInt64";
    case T_FLOAT: return "System.Single";
    case T_DOUBLE: return "System.Double";
    case T_STRING: return "System.String";
    default: return "System.Object";
    }
}

static const char *TypeIdName(int aId)
{
    if (aId < T_COUNT) return ClrName(aId);
    switch (aId) {
    case TY_MATH: return "System.Math";
    case TY_CONSOLE: return "System.Console";
    case TY_ENUMERABLE: return "System.Linq.Enumerable";
    case TY_CONVERT: return "System.Convert";
    case TY_BITOPS: return "System.Numerics.BitOperations";
    case TY_ARRAYT: return "System.Array";
    case TY_LIST: return "System.Collections.Generic.List`1";
    case TY_DICT: return "System.Collections.Generic.Dictionary`2";
    case TY_SET: return "System.Collections.Generic.HashSet`1";
    case TY_SB: return "System.Text.StringBuilder";
    case TY_EXCEPTION: return "System.Exception";
    default: return "System.Type";
    }
}

static void ObjToSb(SB *b, Value v, int aDepth);

Str *ToStr(Value v)
{
    if (v.t == T_STRING) return v.s;
    if (IsScalar(v)) {
        rs_char buf[48];
        int n = ScalarChars(v, buf);
        Str *s = NewStr(n);
        memcpy(s->c, buf, n * sizeof(rs_char));
        return s;
    }
    SB b = { NULL, 0, 0 };
    ObjToSb(&b, v, 0);
    return SbStr(&b);
}

static void ObjToSb(SB *b, Value v, int aDepth)
{
    char buf[200];
    switch (v.t) {
    case T_NULL: return;
    case T_BOOL: SbAppendAscii(b, v.i ? "True" : "False"); return;
    case T_CHAR: { rs_char c = (rs_char)v.u; SbAppendChars(b, &c, 1); return; }
    case T_FLOAT: RealToSb(b, v.d, true); return;
    case T_DOUBLE: RealToSb(b, v.d, false); return;
    case T_STRING: SbAppend(b, v.s); return;
    case T_ARRAY:
        snprintf(buf, sizeof buf, "%s[]", ClrName(v.a->et));
        SbAppendAscii(b, buf);
        return;
    case T_LIST:
        snprintf(buf, sizeof buf, "System.Collections.Generic.List`1[%s]", ClrName(v.a->et));
        SbAppendAscii(b, buf);
        return;
    case T_SEQ:
        snprintf(buf, sizeof buf, "System.Linq.Enumerable+Iterator`1[%s]", ClrName(v.q->TyHint()));
        SbAppendAscii(b, buf);
        return;
    case T_GROUP:
        SbAppendAscii(b, "System.Linq.Grouping`2[System.Object,System.Object]");
        return;
    case T_DICT:
        snprintf(buf, sizeof buf, "System.Collections.Generic.Dictionary`2[%s,%s]", ClrName(v.m->kt), ClrName(v.m->vt));
        SbAppendAscii(b, buf);
        return;
    case T_SET:
        snprintf(buf, sizeof buf, "System.Collections.Generic.HashSet`1[%s]", ClrName(v.m->kt));
        SbAppendAscii(b, buf);
        return;
    case T_FUNC: {
        int ar = v.fn->node ? v.fn->node->n : 1;
        snprintf(buf, sizeof buf, "System.Func`%d[", ar + 1);
        SbAppendAscii(b, buf);
        for (int i = 0; i <= ar; i++) SbAppendAscii(b, i ? ",System.Object" : "System.Object");
        SbAppendAscii(b, "]");
        return;
    }
    case T_SB: SbAppendChars(b, v.sb->c, v.sb->n); return;
    case T_ENUM: {
        int ty = (int)(v.i >> 8), x = (int)(v.i & 0xFF);
        static const char *const split[] = { "None", "RemoveEmptyEntries", "TrimEntries", "RemoveEmptyEntries, TrimEntries" };
        static const char *const mid[] = { "ToEven", "AwayFromZero", "ToZero", "ToNegativeInfinity", "ToPositiveInfinity" };
        SbAppendAscii(b, ty == TY_SPLITOPT ? split[x & 3] : mid[x < 5 ? x : 0]);
        return;
    }
    case T_TYPE: SbAppendAscii(b, TypeIdName((int)v.i)); return;
    case T_NS: SbAppendAscii(b, "System"); return;
    case T_OBJ: {
        Obj *o = v.o;
        if (aDepth > 20) { SbAppendAscii(b, "..."); return; }
        switch (o->kind) {
        case OK_ANON:
            if (!o->n) { SbAppendAscii(b, "{ }"); return; }
            SbAppendAscii(b, "{ ");
            for (int i = 0; i < o->n; i++) {
                if (i) SbAppendAscii(b, ", ");
                SbAppendAscii(b, o->names[i]->s);
                SbAppendAscii(b, " = ");
                ObjToSb(b, o->f[i], aDepth + 1);
            }
            SbAppendAscii(b, " }");
            return;
        case OK_TUPLE:
            SbAppendAscii(b, "(");
            for (int i = 0; i < o->n; i++) {
                if (i) SbAppendAscii(b, ", ");
                ObjToSb(b, o->f[i], aDepth + 1);
            }
            SbAppendAscii(b, ")");
            return;
        case OK_KVP:
            SbAppendAscii(b, "[");
            ObjToSb(b, o->f[0], aDepth + 1);
            SbAppendAscii(b, ", ");
            ObjToSb(b, o->f[1], aDepth + 1);
            SbAppendAscii(b, "]");
            return;
        default:
            SbAppendAscii(b, strncmp(o->names[0]->s, "System.", 7) ? "System." : "");
            SbAppendAscii(b, o->names[0]->s);
            SbAppendAscii(b, ": ");
            ObjToSb(b, o->f[0], aDepth + 1);
            return;
        }
    }
    default:
        if (IsIntegral(v.t)) SbAppendValue(b, v);
        return;
    }
}

// ---------------------------------------------------------------------------
// format strings

static void Group3(char *aInt, char *aOut)
{
    // "1234567" -> "1,234,567" (aInt may start with '-')
    int n = strlen(aInt), o = 0, start = 0;
    if (aInt[0] == '-') { aOut[o++] = '-'; start = 1; }
    int digits = n - start;
    for (int i = 0; i < digits; i++) {
        if (i && (digits - i) % 3 == 0) aOut[o++] = ',';
        aOut[o++] = aInt[start + i];
    }
    aOut[o] = 0;
}

// fixed decimals, with optional grouping: "1,234.50"
static void FixedText(double d, int aDecimals, bool aGroup, char *aOut, int aSize)
{
    char tmp[400];
    snprintf(tmp, sizeof tmp, "%.*f", aDecimals, d);
    if (!strcmp(tmp, "-0") || (tmp[0] == '-' && strspn(tmp + 1, "0.") == strlen(tmp + 1)))
        memmove(tmp, tmp + 1, strlen(tmp));          // .NET 3.0+: no "-0.00"
    if (!aGroup) {
        snprintf(aOut, aSize, "%s", tmp);
        return;
    }
    char *dot = strchr(tmp, '.');
    char frac[340] = "";
    if (dot) {
        snprintf(frac, sizeof frac, "%s", dot);
        *dot = 0;
    }
    char grouped[420];
    Group3(tmp, grouped);
    snprintf(aOut, aSize, "%s%s", grouped, frac);
}

static Str *CustomFormat(double d, const Str *f, int aPos)
{
    // [prefix] [#,0]+ [. [#0]+] [suffix]; '%' in the suffix scales by 100
    char *fmt = StrUtf8(f);
    int len = strlen(fmt);
    int i = 0;
    SB pre = { NULL, 0, 0 }, post = { NULL, 0, 0 };
    while (i < len && !strchr("#0.,", fmt[i])) { char c[2] = { fmt[i], 0 }; SbAppendAscii(&pre, c); i++; }
    int intZeros = 0, decZeros = 0, decHashes = 0;
    bool group = false, dot = false;
    for (; i < len && strchr("#0.,", fmt[i]); i++) {
        char c = fmt[i];
        if (c == '.') dot = true;
        else if (c == ',') { if (!dot) group = true; }
        else if (!dot) { if (c == '0') intZeros++; }
        else { if (c == '0') decZeros++; else decHashes++; }
    }
    bool percent = false;
    for (; i < len; i++) {
        if (fmt[i] == '%') percent = true;
        char c[2] = { fmt[i], 0 };
        SbAppendAscii(&post, c);
    }
    if (percent) d *= 100;
    char num[440];
    FixedText(d, decZeros + decHashes, group, num, sizeof num);
    // drop optional (#) decimals that are zero
    char *dp = strchr(num, '.');
    if (dp) {
        char *end = num + strlen(num);
        int optional = decHashes;
        while (optional > 0 && end > dp + 1 + decZeros && end[-1] == '0') { *--end = 0; optional--; }
        if (end == dp + 1) *dp = 0;
    }
    // leading zeros: "0000"
    char *digitsStart = num[0] == '-' ? num + 1 : num;
    int intLen = 0;
    for (char *p = digitsStart; *p && *p != '.'; p++) if (*p >= '0' && *p <= '9') intLen++;
    SB out = { NULL, 0, 0 };
    SbAppend(&out, SbStr(&pre));
    if (num[0] == '-') SbAppendAscii(&out, "-");
    for (int k = intLen; k < intZeros; k++) SbAppendAscii(&out, "0");
    if (intZeros == 0 && intLen == 1 && digitsStart[0] == '0' && (digitsStart[1] == '.' )) {
        // "#.##" drops the leading zero: .5
        SbAppendAscii(&out, digitsStart + 1);
    } else if (intZeros == 0 && !strcmp(digitsStart, "0")) {
        // "#" of 0 is empty
    } else {
        SbAppendAscii(&out, digitsStart);
    }
    SbAppend(&out, SbStr(&post));
    (void)aPos;
    return SbStr(&out);
}

Str *FormatValue(Value v, const Str *f, int aPos)
{
    if (!f || !f->n || !IsNumeric(v.t) || v.t == T_CHAR)
        return ToStr(v);
    char spec = (char)f->c[0];
    int prec = -1;
    bool standard = (spec >= 'A' && spec <= 'Z') || (spec >= 'a' && spec <= 'z');
    if (standard && f->n > 1) {
        prec = 0;
        for (int i = 1; i < f->n; i++) {
            if (f->c[i] < '0' || f->c[i] > '9') { standard = false; break; }
            prec = prec * 10 + (f->c[i] - '0');
        }
        if (prec > 999) standard = false;
    }
    if (!standard)
        return CustomFormat(ToDouble(v), f, aPos);
    char buf[1100];
    bool integral = IsIntegral(v.t);
    bool upper = spec >= 'A' && spec <= 'Z';
    switch (spec) {
    case 'X': case 'x': case 'B': case 'b': {
        if (!integral)
            ThrowAt(aPos, "FormatException", "Format specifier '%c' is only for integers.", spec);
        int bits = Bits(v.t);
        u64 x = v.u;
        if (bits < 64) x &= (1ull << bits) - 1;
        bool bin = spec == 'B' || spec == 'b';
        char tmp[70];
        int n = 0;
        do {
            tmp[n++] = bin ? (char)('0' + (x & 1)) : (upper ? "0123456789ABCDEF" : "0123456789abcdef")[x & 15];
            x = bin ? x >> 1 : x >> 4;
        } while (x);
        int o = 0;
        for (int k = n; k < prec && o < 1000; k++) buf[o++] = '0';
        while (n) buf[o++] = tmp[--n];
        buf[o] = 0;
        return StrFromAscii(buf);
    }
    case 'D': case 'd': {
        if (!integral)
            ThrowAt(aPos, "FormatException", "Format specifier 'D' is only for integers.");
        char digits[32];
        bool neg = !IsUnsigned(v.t) && v.i < 0;
        if (IsUnsigned(v.t)) snprintf(digits, sizeof digits, "%llu", (unsigned long long)v.u);
        else snprintf(digits, sizeof digits, "%llu", (unsigned long long)(neg ? (u64)0 - v.u : v.u));
        int o = 0;
        if (neg) buf[o++] = '-';
        for (int k = strlen(digits); k < prec && o < 1000; k++) buf[o++] = '0';
        snprintf(buf + o, sizeof buf - o, "%s", digits);
        return StrFromAscii(buf);
    }
    case 'N': case 'n': case 'F': case 'f': case 'P': case 'p': case 'C': case 'c': {
        double d = ToDouble(v);
        if (spec == 'P' || spec == 'p') d *= 100;
        if (d != d) return StrFromAscii("NaN");
        if (d == INFINITY || d == -INFINITY) return ToStr(VDouble(d));
        int decimals = prec >= 0 ? prec : 2;
        if (decimals > 300) decimals = 300;
        char num[1000];
        FixedText(d, decimals, spec != 'F' && spec != 'f', num, sizeof num);
        if (spec == 'P' || spec == 'p') snprintf(buf, sizeof buf, "%s %%", num);
        else if (spec == 'C' || spec == 'c') {
            if (num[0] == '-') snprintf(buf, sizeof buf, "-\xC2\xA4%s", num + 1);
            else snprintf(buf, sizeof buf, "\xC2\xA4%s", num);
            return StrFromUtf8(buf, strlen(buf));
        }
        else snprintf(buf, sizeof buf, "%s", num);
        return StrFromAscii(buf);
    }
    case 'E': case 'e': {
        double d = ToDouble(v);
        int p = prec >= 0 ? (prec > 300 ? 300 : prec) : 6;
        char tmp[400];
        snprintf(tmp, sizeof tmp, "%.*e", p, d);
        // .NET: at least three exponent digits: 1.234560E+003
        char *e = strchr(tmp, 'e');
        if (!e) return StrFromAscii(tmp);
        *e = 0;
        int ex = atoi(e + 1);
        snprintf(buf, sizeof buf, "%s%c%c%03d", tmp, upper ? 'E' : 'e', ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
        return StrFromAscii(buf);
    }
    case 'G': case 'g': case 'R': case 'r': {
        if (prec <= 0 || spec == 'R' || spec == 'r' || integral) {
            Str *s = ToStr(v);
            if (!upper) for (int i = 0; i < s->n; i++) if (s->c[i] == 'E') s->c[i] = 'e';
            return s;
        }
        double d = ToDouble(v);
        char tmp[400];
        snprintf(tmp, sizeof tmp, "%.*G", prec > 300 ? 300 : prec, d);
        // .NET writes E+XX with two digits at least, as printf does
        if (!upper) for (char *q = tmp; *q; q++) if (*q == 'E') *q = 'e';
        return StrFromAscii(tmp);
    }
    default:
        ThrowAt(aPos, "FormatException", "Format specifier '%c' was invalid.", spec);
    }
}

Str *FormatString(const Str *f, Value *a, int n, int aPos)
{
    SB b = { NULL, 0, 0 };
    for (int i = 0; i < f->n; ) {
        rs_char c = f->c[i];
        if (c == '{' && i + 1 < f->n && f->c[i + 1] == '{') { SbAppendChars(&b, &c, 1); i += 2; continue; }
        if (c == '}' && i + 1 < f->n && f->c[i + 1] == '}') { SbAppendChars(&b, &c, 1); i += 2; continue; }
        if (c == '}')
            ThrowAt(aPos, "FormatException", "Input string was not in a correct format (unexpected '}').");
        if (c != '{') { SbAppendChars(&b, &c, 1); i++; continue; }
        i++;
        int idx = 0, digits = 0;
        while (i < f->n && f->c[i] >= '0' && f->c[i] <= '9') { idx = idx * 10 + (f->c[i] - '0'); i++; digits++; }
        if (!digits)
            ThrowAt(aPos, "FormatException", "Input string was not in a correct format (a number must follow '{').");
        int align = 0;
        if (i < f->n && f->c[i] == ',') {
            i++;
            bool neg = false;
            if (i < f->n && f->c[i] == '-') { neg = true; i++; }
            while (i < f->n && f->c[i] >= '0' && f->c[i] <= '9') { align = align * 10 + (f->c[i] - '0'); i++; }
            if (neg) align = -align;
        }
        Str *fmt = NULL;
        if (i < f->n && f->c[i] == ':') {
            int from = ++i;
            while (i < f->n && f->c[i] != '}') i++;
            fmt = StrSub(f, from, i - from);
        }
        if (i >= f->n || f->c[i] != '}')
            ThrowAt(aPos, "FormatException", "Input string was not in a correct format (missing '}').");
        i++;
        if (idx >= n)
            ThrowAt(aPos, "FormatException", "Index (zero based) must be greater than or equal to zero and less than the size of the argument list.");
        Str *s = a[idx].t == T_NULL ? (Str *)EmptyStr() : fmt ? FormatValue(a[idx], fmt, aPos) : ToStr(a[idx]);
        if (align) s = Align(s, align);
        SbAppend(&b, s);
    }
    return SbStr(&b);
}

// ---------------------------------------------------------------------------
// parsing: int.Parse, double.Parse (invariant culture)

bool ParseNumber(const Str *s, int aTy, Value &aOut)
{
    char buf[128];
    int n = 0;
    int i = 0, end = s->n;
    while (i < end && (s->c[i] == ' ' || (s->c[i] >= 9 && s->c[i] <= 13))) i++;
    while (end > i && (s->c[end - 1] == ' ' || (s->c[end - 1] >= 9 && s->c[end - 1] <= 13))) end--;
    if (i >= end || end - i > 120) return false;
    for (int k = i; k < end; k++) {
        rs_char c = s->c[k];
        if (c > 0x7F) {
            if (c == 0x221E && IsReal(aTy)) { buf[n++] = 'i'; buf[n++] = 'n'; buf[n++] = 'f'; continue; }
            return false;
        }
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    if (IsReal(aTy)) {
        // digits, one '.', exponent, thousands separators; NaN, Infinity
        char clean[128];
        int m = 0;
        const char *p = buf;
        if (*p == '+' || *p == '-') clean[m++] = *p++;
        if (!strcmp(p, "NaN") || !strcmp(p, "Infinity") || !strcmp(p, "inf")) {
            double v = p[0] == 'N' ? NAN : INFINITY;
            if (buf[0] == '-') v = -v;
            aOut = VReal(aTy, v);
            return true;
        }
        bool digit = false, dot = false, exp = false;
        for (; *p; p++) {
            char c = *p;
            if (c >= '0' && c <= '9') { clean[m++] = c; digit = true; }
            else if (c == ',' && !dot && !exp) continue;
            else if (c == '.' && !dot && !exp) { clean[m++] = c; dot = true; }
            else if ((c == 'e' || c == 'E') && digit && !exp) {
                clean[m++] = 'e';
                exp = true;
                if (p[1] == '+' || p[1] == '-') clean[m++] = *++p;
                if (!(p[1] >= '0' && p[1] <= '9')) return false;
            } else {
                return false;
            }
        }
        if (!digit) return false;
        clean[m] = 0;
        double v = strtod(clean, NULL);
        if (aTy == T_FLOAT && fabs(v) > 3.4028234663852886e38 && v == v && fabs(v) != INFINITY)
            v = v > 0 ? INFINITY : -INFINITY;
        aOut = VReal(aTy, v);
        return true;
    }
    if (!IsIntegral(aTy) || aTy == T_CHAR) return false;
    const char *p = buf;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (!*p) return false;
    u64 x = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        u64 nx = x * 10 + (*p - '0');
        if ((nx - (*p - '0')) / 10 != x) return false;
        x = nx;
    }
    if (IsUnsigned(aTy)) {
        if (neg && x) return false;
        bool fits = aTy == T_ULONG || (x <= ((1ull << Bits(aTy)) - 1));
        if (!fits) return false;
        aOut = VNum(aTy, (i64)x);
        return true;
    }
    if (neg ? x > 0x8000000000000000ull : x > 0x7FFFFFFFFFFFFFFFull) return false;
    i64 sv = neg ? (i64)(0 - x) : (i64)x;
    int bits = Bits(aTy);
    if (bits < 64) {
        i64 lo = -(1LL << (bits - 1)), hi = (1LL << (bits - 1)) - 1;
        if (sv < lo || sv > hi) return false;
    }
    aOut = VNum(aTy, sv);
    return true;
}

// ---------------------------------------------------------------------------
// Dump: results as LINQPad shows them, in plain text

static const int KMaxItems = 1000;

static void Render(SB *b, Value v, int aDepth);

static void RenderSeq(SB *b, Value v, int aDepth)
{
    Iter *it = OpenSeq(v, -1);
    Value x;
    SbAppendAscii(b, "[");
    int i = 0;
    while (it->Next(x)) {
        if (i == 100) { SbAppendAscii(b, ", ..."); break; }
        if (i++) SbAppendAscii(b, ", ");
        Render(b, x, aDepth + 1);
    }
    SbAppendAscii(b, "]");
}

static void Render(SB *b, Value v, int aDepth)
{
    if (aDepth > 12) { SbAppendAscii(b, "..."); return; }
    switch (v.t) {
    case T_NULL:
        SbAppendAscii(b, "null");
        return;
    case T_ARRAY: case T_LIST: case T_SEQ: case T_SET:
        RenderSeq(b, v, aDepth);
        return;
    case T_DICT: {
        SbAppendAscii(b, "{");
        for (int i = 0; i < v.m->keys->n; i++) {
            if (i == 100) { SbAppendAscii(b, ", ..."); break; }
            SbAppendAscii(b, i ? ", " : " ");
            Render(b, v.m->keys->v[i], aDepth + 1);
            SbAppendAscii(b, ": ");
            Render(b, v.m->vals->v[i], aDepth + 1);
        }
        SbAppendAscii(b, v.m->keys->n ? " }" : "}");
        return;
    }
    case T_GROUP:
        RenderSeq(b, v, aDepth);
        return;
    case T_OBJ: {
        Obj *o = v.o;
        if (o->kind == OK_EXC) break;
        const char *open = o->kind == OK_ANON ? (o->n ? "{ " : "{") : o->kind == OK_TUPLE ? "(" : "[";
        const char *close = o->kind == OK_ANON ? (o->n ? " }" : "}") : o->kind == OK_TUPLE ? ")" : "]";
        SbAppendAscii(b, open);
        for (int i = 0; i < o->n; i++) {
            if (i) SbAppendAscii(b, ", ");
            if (o->kind == OK_ANON) {
                SbAppendAscii(b, o->names[i]->s);
                SbAppendAscii(b, " = ");
            }
            Render(b, o->f[i], aDepth + 1);
        }
        SbAppendAscii(b, close);
        return;
    }
    default:
        break;
    }
    ObjToSb(b, v, aDepth);
}

static bool Scalar(const Value &v)
{
    return v.t == T_NULL || v.t == T_BOOL || IsNumeric(v.t) || v.t == T_ENUM;
}

// "int", "string", "{ Name, Age }", ...
static void ElemTypeName(SB *b, Arr *items)
{
    int t = -1;
    for (int i = 0; i < items->n; i++) {
        int it = items->v[i].t;
        if (it == T_NULL) continue;
        if (t < 0) t = it;
        else if (t != it) { t = T_OBJECT; break; }
    }
    if (t < 0 || t == T_OBJECT || t == T_COUNT) { SbAppendAscii(b, "object"); return; }
    if (t == T_OBJ) {
        Obj *o = items->v[0].o;
        SbAppendAscii(b, o->kind == OK_TUPLE ? "(" : o->kind == OK_KVP ? "KeyValuePair<" : "{ ");
        for (int i = 0; i < o->n; i++) {
            if (i) SbAppendAscii(b, ", ");
            if (o->kind == OK_ANON) SbAppendAscii(b, o->names[i]->s);
            else SbAppendAscii(b, ValTyName(o->f[i]));
        }
        SbAppendAscii(b, o->kind == OK_TUPLE ? ")" : o->kind == OK_KVP ? ">" : " }");
        return;
    }
    if (t == T_GROUP) {
        Group *g0 = items->v[0].g;
        SbAppendAscii(b, "IGrouping<");
        SbAppendAscii(b, ValTyName(g0->key));
        SbAppendAscii(b, ", ");
        SbAppendAscii(b, TyName(g0->items->et));
        SbAppendAscii(b, ">");
        return;
    }
    if (t == T_ARRAY) { SbAppendAscii(b, TyName(items->v[0].a->et == T_OBJECT ? T_OBJECT : items->v[0].a->et)); SbAppendAscii(b, "[]"); return; }
    SbAppendAscii(b, TyName(t));
}

static void Binary4(SB *b, u64 x, int bits)
{
    // 0b1010_1100: groups of four, leading zero groups dropped
    int top = bits - 1;
    while (top > 3 && !((x >> top) & 1)) top--;
    top = (top / 4) * 4 + 3;
    if (top >= bits) top = bits - 1;
    SbAppendAscii(b, "0b");
    for (int k = top; k >= 0; k--) {
        SbAppendAscii(b, (x >> k) & 1 ? "1" : "0");
        if (k && k % 4 == 0) SbAppendAscii(b, "_");
    }
}

static void DumpScalarInfo(SB *b, Value v)
{
    char buf[80];
    if (IsIntegral(v.t) && v.t != T_CHAR) {
        int bits = Bits(v.t);
        u64 x = v.u;
        if (bits < 64) x &= (1ull << bits) - 1;
        snprintf(buf, sizeof buf, "\n%s \xC2\xB7 0x%llX\n", TyName(v.t), (unsigned long long)x);
        SbAppendUtf8(b, buf);
        Binary4(b, x, bits);
    } else if (v.t == T_CHAR) {
        snprintf(buf, sizeof buf, "\nchar \xC2\xB7 U+%04X \xC2\xB7 %u", (unsigned)v.u, (unsigned)v.u);
        SbAppendUtf8(b, buf);
    } else if (IsReal(v.t) || v.t == T_BOOL) {
        SbAppendAscii(b, "\n");
        SbAppendAscii(b, TyName(v.t));
    }
}

void Dump(Value v, const Str *aTitle, bool aResult)
{
    if (IsVoid(v)) return;
    SB b = { NULL, 0, 0 };
    if (g.out.n && g.out.c[g.out.n - 1] != '\n') SbAppendAscii(&b, "\n");
    if (g.out.n) SbAppendAscii(&b, "\n");
    if (aTitle) {
        SbAppend(&b, aTitle);
        SbAppendAscii(&b, "\n");
    }
    bool views = g.opt && g.opt->int_views;
    if (v.t == T_STRING) {
        SbAppend(&b, v.s);
    } else if (Scalar(v)) {
        Render(&b, v, 0);
        if (aResult && views && v.t != T_NULL) DumpScalarInfo(&b, v);
    } else if (v.t == T_ARRAY || v.t == T_LIST || v.t == T_SEQ || v.t == T_SET || v.t == T_DICT || v.t == T_GROUP) {
        // materialise (at most KMaxItems + 1)
        Arr *items = NewArr(16, T_OBJECT);
        bool more = false;
        Iter *it = OpenSeq(v, -1);
        Value x;
        while (it->Next(x)) {
            if (items->n == KMaxItems) { more = true; break; }
            ArrPush(items, x);
        }
        char buf[80];
        if (v.t == T_GROUP) {
            SbAppendAscii(&b, "Key = ");
            Render(&b, v.g->key, 1);
            SbAppendAscii(&b, "\n");
        }
        SB tn = { NULL, 0, 0 };
        if (v.t == T_DICT) {
            SbAppendAscii(&tn, "Dictionary<");
            SbAppendAscii(&tn, TyName(v.m->kt == T_OBJECT ? T_OBJECT : v.m->kt));
            SbAppendAscii(&tn, ", ");
            SbAppendAscii(&tn, TyName(v.m->vt == T_OBJECT ? T_OBJECT : v.m->vt));
            SbAppendAscii(&tn, ">");
        } else {
            SB et = { NULL, 0, 0 };
            ElemTypeName(&et, items);
            Str *ets = SbStr(&et);
            if (v.t == T_ARRAY) { SbAppend(&tn, ets); SbAppendAscii(&tn, "[]"); }
            else {
                SbAppendAscii(&tn, v.t == T_LIST ? "List<" : v.t == T_SET ? "HashSet<" : "IEnumerable<");
                SbAppend(&tn, ets);
                SbAppendAscii(&tn, ">");
            }
        }
        SbAppend(&b, SbStr(&tn));
        snprintf(buf, sizeof buf, more ? " (first %d items)" : items->n == 1 ? " (1 item)" : " (%d items)", items->n);
        SbAppendAscii(&b, buf);
        bool allScalar = true;
        for (int i = 0; i < items->n; i++) if (!Scalar(items->v[i])) allScalar = false;
        if (allScalar && items->n) {
            SbAppendAscii(&b, "\n");
            for (int i = 0; i < items->n; i++) {
                if (i) SbAppendAscii(&b, ", ");
                Render(&b, items->v[i], 1);
            }
        } else {
            for (int i = 0; i < items->n; i++) {
                SbAppendAscii(&b, "\n");
                Value e = items->v[i];
                if (e.t == T_GROUP) {
                    Render(&b, e.g->key, 1);
                    SbAppendAscii(&b, ": ");
                    RenderSeq(&b, e, 1);
                } else if (v.t == T_DICT) {
                    Render(&b, e.o->f[0], 1);
                    SbAppendAscii(&b, ": ");
                    Render(&b, e.o->f[1], 1);
                } else {
                    Render(&b, e, 1);
                }
            }
        }
        if (more) SbAppendAscii(&b, "\n...");
    } else {
        Render(&b, v, 0);
    }
    SbAppendAscii(&b, "\n");
    Out(SbStr(&b));
}

} // namespace rs

// ---------------------------------------------------------------------------
// rs_run

using namespace rs;

static rs_char *gOutput;
static int gOutputLen;
static int gErrorPos = -1;
static int gErrorLen;

int rs_error_pos(void) { return gErrorPos; }
int rs_error_length(void) { return gErrorLen; }
static void (*gOutputRelease)(void *);

void rs_free_output(void)
{
    if (gOutput) (gOutputRelease ? gOutputRelease : free)(gOutput);
    gOutput = NULL;
    gOutputLen = 0;
}

int rs_run(const rs_char *aSrc, int aLen, const rs_options *aOpt, const rs_char **aOut, int *aOutLen)
{
    rs_free_output();
    static rs_options defaults;
    const rs_options *opt = aOpt ? aOpt : &defaults;
    memset(&g, 0, sizeof g);
    g.opt = opt;
    g.limit = opt->mem_limit ? opt->mem_limit : 16u << 20;
    g.outLimit = opt->out_limit ? opt->out_limit : 200000;
    g.stackLimit = opt->stack_limit ? opt->stack_limit : 256u << 10;
    g.src = aSrc;
    g.srcLen = aLen;
    g.status = RS_OK;
    g.errPos = -1;
    gErrorPos = -1;
    gErrorLen = 0;
    gChecked = false;
    ResetEvalFlags();
    g.stackBase = (char *)__builtin_frame_address(0);
    ArenaInit();
    if (setjmp(g.jb) == 0) {
        NamesInit();
        Program p = Parse(aSrc, aLen);
        RunProgram(p);
    }
    // the output, outside the arena
    int status = g.status;
    gErrorPos = status == RS_COMPILE_ERROR || status == RS_RUNTIME_ERROR ? g.errPos : -1;
    int msgLen = status != RS_OK ? strlen(g.msg) : 0;
    int outN = status == RS_COMPILE_ERROR ? 0 : g.out.n;
    bool sep = outN && msgLen;
    int total = outN + (sep ? 2 : 0) + msgLen + (msgLen ? 1 : 0);
    void *(*alloc)(size_t) = opt->alloc ? opt->alloc : malloc;
    gOutputRelease = opt->release;
    gOutput = (rs_char *)alloc((total + 1) * sizeof(rs_char));
    if (gOutput) {
        int n = 0;
        if (outN) memcpy(gOutput, g.out.c, outN * sizeof(rs_char));
        n = outN;
        if (sep) {
            if (gOutput[n - 1] != '\n') gOutput[n++] = '\n';
            gOutput[n++] = '\n';
        }
        // the message is UTF-8
        int msgStart = n;
        for (int i = 0; i < msgLen; ) {
            unsigned char c = g.msg[i];
            unsigned cp;
            if (c < 0x80) { cp = c; i++; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < msgLen) { cp = ((c & 0x1F) << 6) | (g.msg[i + 1] & 0x3F); i += 2; }
            else if ((c & 0xF0) == 0xE0 && i + 2 < msgLen) { cp = ((c & 0x0F) << 12) | ((g.msg[i + 1] & 0x3F) << 6) | (g.msg[i + 2] & 0x3F); i += 3; }
            else { cp = '?'; i++; }
            gOutput[n++] = (rs_char)cp;
        }
        if (msgLen) gOutput[n++] = '\n';
        gOutputLen = n;
        gErrorLen = status == RS_COMPILE_ERROR || status == RS_RUNTIME_ERROR ? n - msgStart : 0;
    }
    ArenaFree();
    g.freeScopes = NULL;
    *aOut = gOutput;
    *aOutLen = gOutputLen;
    return status;
}
