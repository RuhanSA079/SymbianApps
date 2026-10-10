/*
 * rs_lex.cpp: the arena, interned names, errors and the lexer.
 */
#include "rs_int.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace rs {

State g;

// ---------------------------------------------------------------------------
// arena: blocks of 64 KB (or one block per big request), freed after a run

struct Block { Block *next; size_t size; };
const size_t KBlock = 64 * 1024;

void ArenaInit()
{
    g.blocks = NULL;
    g.cur = NULL;
    g.left = g.used = 0;
}

void ArenaFree()
{
    void (*release)(void *) = g.opt && g.opt->release ? g.opt->release : free;
    for (Block *b = g.blocks; b; ) {
        Block *next = b->next;
        release(b);
        b = next;
    }
    g.blocks = NULL;
    g.cur = NULL;
    g.left = 0;
}

void *Alloc(size_t aSize)
{
    aSize = (aSize + 7) & ~(size_t)7;
    if (aSize > g.left) {
        size_t size = aSize + sizeof(Block) + 8 > KBlock ? aSize + sizeof(Block) + 8 : KBlock;
        if (g.used + size > g.limit)
            Throw("OutOfMemoryException", "rSharp's memory limit (%u MB) was reached.",
                  (unsigned)(g.limit >> 20));
        void *(*alloc)(size_t) = g.opt && g.opt->alloc ? g.opt->alloc : malloc;
        Block *b = (Block *)alloc(size);
        if (!b)
            Throw("OutOfMemoryException", "The phone is out of memory.");
        g.used += size;
        b->size = size;
        b->next = g.blocks;
        g.blocks = b;
        g.cur = (char *)b + ((sizeof(Block) + 7) & ~(size_t)7);
        g.left = size - (g.cur - (char *)b);
    }
    void *p = g.cur;
    g.cur += aSize;
    g.left -= aSize;
    memset(p, 0, aSize);
    return p;
}

// ---------------------------------------------------------------------------
// names

static const char *const KNames[] = {
    "",
#define RS_X(n) #n,
    RS_NAMES(RS_X)
#undef RS_X
};

const char *NameText(int aId) { return aId > 0 && aId < K_COUNT ? KNames[aId] : "?"; }

static unsigned HashText(const char *s, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

Name *Intern(const char *aText, int aLen)
{
    unsigned h = HashText(aText, aLen) & 1023;
    for (Name *n = g.names[h]; n; n = n->next)
        if (n->len == aLen && !memcmp(n->s, aText, aLen))
            return n;
    Name *n = New<Name>();
    char *s = (char *)Alloc(aLen + 1);
    memcpy(s, aText, aLen);
    n->s = s;
    n->len = aLen;
    n->next = g.names[h];
    g.names[h] = n;
    return n;
}

void NamesInit()
{
    memset(g.names, 0, sizeof g.names);
    for (int i = 1; i < K_COUNT; i++)
        Intern(KNames[i], strlen(KNames[i]))->id = i;
}

// ---------------------------------------------------------------------------
// errors

void LineCol(int aPos, int &aLine, int &aCol)
{
    aLine = 1;
    aCol = 1;
    for (int i = 0; i < aPos && i < g.srcLen; i++) {
        if (g.src[i] == '\n') {
            aLine++;
            aCol = 1;
        } else {
            aCol++;
        }
    }
}

void CompileError(int aPos, const char *aFmt, ...)
{
    int n = 0;
    if (aPos >= 0) {
        int line, col;
        LineCol(aPos, line, col);
        n = snprintf(g.msg, sizeof g.msg, "Line %d, column %d: ", line, col);
    }
    va_list ap;
    va_start(ap, aFmt);
    vsnprintf(g.msg + n, sizeof g.msg - n, aFmt, ap);
    va_end(ap);
    g.errPos = aPos;
    g.status = RS_COMPILE_ERROR;
    longjmp(g.jb, 1);
}

RS_NORETURN static void VThrow(int aPos, const char *aType, const char *aFmt, va_list ap)
{
    int n = snprintf(g.msg, sizeof g.msg, "%s%s: ", strncmp(aType, "System.", 7) ? "System." : "", aType);
    n += vsnprintf(g.msg + n, sizeof g.msg - n, aFmt, ap);
    if (aPos >= 0 && n < (int)sizeof g.msg - 32) {
        int line, col;
        LineCol(aPos, line, col);
        snprintf(g.msg + n, sizeof g.msg - n, "\n   at line %d", line);
    }
    g.errPos = aPos;
    g.status = RS_RUNTIME_ERROR;
    longjmp(g.jb, 1);
}

void Throw(const char *aType, const char *aFmt, ...)
{
    va_list ap;
    va_start(ap, aFmt);
    VThrow(-1, aType, aFmt, ap);
    va_end(ap);
}

void ThrowAt(int aPos, const char *aType, const char *aFmt, ...)
{
    va_list ap;
    va_start(ap, aFmt);
    VThrow(aPos, aType, aFmt, ap);
    va_end(ap);
}

void StackCheck()
{
    char *probe = (char *)__builtin_frame_address(0);
    if ((size_t)(g.stackBase - probe) > g.stackLimit)
        Throw("InsufficientExecutionStackException",
              "Insufficient stack to continue: the recursion is too deep.");
}

void Tick()
{
    if ((++g.steps & 1023) == 0 && g.opt && g.opt->cancel && *g.opt->cancel) {
        g.status = RS_CANCELLED;
        snprintf(g.msg, sizeof g.msg, "Stopped.");
        longjmp(g.jb, 1);
    }
}

// ---------------------------------------------------------------------------
// lexer

static const char *const KTokText[TK_COUNT] = {
    "end of input", "name", "number", "string", "character", "interpolated string",
    "(", ")", "{", "}", "[", "]", ";", ",", ".", "?", ":", "=>",
    "?.", "?[", "??", "?\?=", "..",
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", ">>>",
    "==", "!=", "<", ">", "<=", ">=", "&&", "||", "!", "~", "++", "--",
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>="
};

const char *TokText(int k) { return k >= 0 && k < TK_COUNT ? KTokText[k] : "?"; }

struct Lexer
    {
    const rs_char *s;
    int i, end;
    Tok *t;
    int n, cap;
    };

static void Push(Lexer &L, const Tok &aTok)
{
    if (L.n == L.cap) {
        int cap = L.cap ? L.cap * 2 : 256;
        Tok *t = NewArr<Tok>(cap);
        if (L.n)
            memcpy(t, L.t, L.n * sizeof(Tok));
        L.t = t;
        L.cap = cap;
    }
    L.t[L.n++] = aTok;
}

static bool IdStart(rs_char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
static bool IdPart(rs_char c) { return IdStart(c) || (c >= '0' && c <= '9'); }
static int HexVal(rs_char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// An escape after '\' at L.i; returns the character, advances L.i.
static unsigned Escape(Lexer &L)
{
    int at = L.i - 1;
    if (L.i >= L.end)
        CompileError(at, "Unterminated escape sequence");
    rs_char c = L.s[L.i++];
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'e': return 27;
    case '\\': case '\'': case '"': return c;
    case 'u': case 'x': case 'U': {
        int max = c == 'U' ? 8 : 4, digits = 0;
        unsigned v = 0;
        while (digits < max && L.i < L.end && HexVal(L.s[L.i]) >= 0) {
            v = v * 16 + HexVal(L.s[L.i++]);
            digits++;
        }
        if (!digits || (c != 'x' && digits != max))
            CompileError(at, "Unrecognized escape sequence");
        return v;
    }
    default:
        CompileError(at, "Unrecognized escape sequence");
    }
}

static void AppendChar(SB &b, unsigned c)
{
    if (c >= 0x10000) {
        c -= 0x10000;
        rs_char pair[2] = { (rs_char)(0xD800 + (c >> 10)), (rs_char)(0xDC00 + (c & 0x3FF)) };
        SbAppendChars(&b, pair, 2);
    } else {
        rs_char ch = (rs_char)c;
        SbAppendChars(&b, &ch, 1);
    }
}

static void LexNumber(Lexer &L, Tok &t)
{
    const rs_char *s = L.s;
    int i = L.i;
    char buf[80];
    int n = 0;
    bool real = false;
    int base = 10;
    if (s[i] == '0' && i + 1 < L.end && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
        while (i < L.end && (HexVal(s[i]) >= 0 || s[i] == '_')) {
            if (s[i] != '_' && n < 70) buf[n++] = (char)s[i];
            i++;
        }
    } else if (s[i] == '0' && i + 1 < L.end && (s[i + 1] == 'b' || s[i + 1] == 'B')) {
        base = 2;
        i += 2;
        while (i < L.end && (s[i] == '0' || s[i] == '1' || s[i] == '_')) {
            if (s[i] != '_' && n < 70) buf[n++] = (char)s[i];
            i++;
        }
    } else {
        while (i < L.end && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            if (s[i] != '_' && n < 70) buf[n++] = (char)s[i];
            i++;
        }
        if (i + 1 < L.end && s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9') {
            real = true;
            buf[n++] = '.';
            i++;
            while (i < L.end && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
                if (s[i] != '_' && n < 70) buf[n++] = (char)s[i];
                i++;
            }
        }
        if (i < L.end && (s[i] == 'e' || s[i] == 'E')) {
            int j = i + 1;
            if (j < L.end && (s[j] == '+' || s[j] == '-')) j++;
            if (j < L.end && s[j] >= '0' && s[j] <= '9') {
                real = true;
                buf[n++] = 'e';
                i++;
                if (s[i] == '+' || s[i] == '-') buf[n++] = (char)s[i++];
                while (i < L.end && s[i] >= '0' && s[i] <= '9') {
                    if (n < 78) buf[n++] = (char)s[i];
                    i++;
                }
            }
        }
    }
    buf[n] = 0;
    if (!n)
        CompileError(L.i, "Invalid number");
    // suffix
    bool sU = false, sL = false, sF = false, sD = false, sM = false;
    while (i < L.end) {
        rs_char c = s[i];
        if ((c == 'u' || c == 'U') && !sU && base != 0) sU = true;
        else if ((c == 'l' || c == 'L') && !sL) sL = true;
        else if ((c == 'f' || c == 'F') && base == 10 && !sU && !sL) sF = true;
        else if ((c == 'd' || c == 'D') && base == 10 && !sU && !sL) sD = true;
        else if ((c == 'm' || c == 'M') && base == 10 && !sU && !sL) sM = true;
        else break;
        i++;
        if (sF || sD || sM) break;
    }
    if (i < L.end && IdPart(s[i]))
        CompileError(L.i, "Invalid number");
    t.k = TK_NUM;
    if (sM)
        CompileError(L.i, "decimal is not supported in rSharp; use double");
    if (real || sF || sD) {
        if (base != 10)
            CompileError(L.i, "Invalid number");
        double d = strtod(buf, NULL);
        t.lit = VReal(sF ? T_FLOAT : T_DOUBLE, d);
    } else {
        u64 v = 0;
        bool over = false;
        for (int k = 0; k < n; k++) {
            int dv = HexVal(buf[k]);
            u64 nv = v * base + dv;
            if ((nv - dv) / base != v) over = true;
            v = nv;
        }
        if (over)
            CompileError(L.i, "Integral constant is too large");
        int ty;
        if (!sU && !sL)
            ty = v <= 0x7FFFFFFF ? T_INT : v <= 0xFFFFFFFFu ? T_UINT :
                 v <= 0x7FFFFFFFFFFFFFFFull ? T_LONG : T_ULONG;
        else if (sU && !sL)
            ty = v <= 0xFFFFFFFFu ? T_UINT : T_ULONG;
        else if (sL && !sU)
            ty = v <= 0x7FFFFFFFFFFFFFFFull ? T_LONG : T_ULONG;
        else
            ty = T_ULONG;
        t.lit.t = (uint8_t)ty;
        t.lit.u = v;
    }
    L.i = i;
}

// A regular or verbatim string, L.i just after the opening quote.
static Str *LexString(Lexer &L, bool aVerbatim, int aStart)
{
    SB b = { NULL, 0, 0 };
    for (;;) {
        if (L.i >= L.end || (!aVerbatim && L.s[L.i] == '\n'))
            CompileError(aStart, "Newline in constant (missing closing \")");
        rs_char c = L.s[L.i++];
        if (c == '"') {
            if (aVerbatim && L.i < L.end && L.s[L.i] == '"') {
                L.i++;
                SbAppendChars(&b, &c, 1);
                continue;
            }
            break;
        }
        if (c == '\\' && !aVerbatim) {
            AppendChar(b, Escape(L));
            continue;
        }
        SbAppendChars(&b, &c, 1);
    }
    return SbStr(&b);
}

// $"...": skips to the closing quote, minding {holes} with strings inside.
static void SkipInterp(Lexer &L, bool aVerbatim, int aStart)
{
    int depth = 0;
    for (;;) {
        if (L.i >= L.end)
            CompileError(aStart, "Unterminated interpolated string");
        rs_char c = L.s[L.i++];
        if (depth == 0) {
            if (c == '"') {
                if (aVerbatim && L.i < L.end && L.s[L.i] == '"') { L.i++; continue; }
                return;
            }
            if (c == '\\' && !aVerbatim) { L.i++; continue; }
            if (c == '{') {
                if (L.i < L.end && L.s[L.i] == '{') { L.i++; continue; }
                depth = 1;
            } else if (c == '}' && L.i < L.end && L.s[L.i] == '}') {
                L.i++;
            } else if (c == '\n' && !aVerbatim) {
                CompileError(aStart, "Newline in interpolated string");
            }
        } else {
            if (c == '{' || c == '(' || c == '[') depth++;
            else if (c == '}' || c == ')' || c == ']') depth--;
            else if (c == '"') {
                bool vb = L.i >= 2 && L.s[L.i - 2] == '@';
                LexString(L, vb, L.i - 1);
            } else if (c == '\'') {
                while (L.i < L.end && L.s[L.i] != '\'') {
                    if (L.s[L.i] == '\\') L.i++;
                    L.i++;
                }
                L.i++;
            }
        }
    }
}

Lexed Lex(const rs_char *aSrc, int aFrom, int aTo)
{
    Lexer L;
    L.s = aSrc;
    L.i = aFrom;
    L.end = aTo;
    L.t = NULL;
    L.n = L.cap = 0;
    for (;;) {
        // white space and comments
        while (L.i < L.end) {
            rs_char c = L.s[L.i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0 || c == 0x2029 || c == 0xFEFF) {
                L.i++;
            } else if (c == '/' && L.i + 1 < L.end && L.s[L.i + 1] == '/') {
                while (L.i < L.end && L.s[L.i] != '\n') L.i++;
            } else if (c == '/' && L.i + 1 < L.end && L.s[L.i + 1] == '*') {
                int at = L.i;
                L.i += 2;
                while (L.i + 1 < L.end && !(L.s[L.i] == '*' && L.s[L.i + 1] == '/')) L.i++;
                if (L.i + 1 >= L.end)
                    CompileError(at, "End-of-file found, '*/' expected");
                L.i += 2;
            } else {
                break;
            }
        }
        Tok t;
        memset(&t, 0, sizeof t);
        t.pos = L.i;
        if (L.i >= L.end) {
            t.k = TK_EOF;
            t.end = L.i;
            Push(L, t);
            break;
        }
        const rs_char *s = L.s;
        rs_char c = s[L.i];
        rs_char c1 = L.i + 1 < L.end ? s[L.i + 1] : 0;
        rs_char c2 = L.i + 2 < L.end ? s[L.i + 2] : 0;
        if (IdStart(c) || (c == '@' && IdStart(c1))) {
            if (c == '@') {
                t.verbatim = true;
                L.i++;
            }
            char buf[256];
            int n = 0;
            while (L.i < L.end && IdPart(s[L.i])) {
                rs_char ch = s[L.i++];
                if (n > 240) continue;
                if (ch < 0x80) buf[n++] = (char)ch;
                else if (ch < 0x800) { buf[n++] = (char)(0xC0 | (ch >> 6)); buf[n++] = (char)(0x80 | (ch & 0x3F)); }
                else { buf[n++] = (char)(0xE0 | (ch >> 12)); buf[n++] = (char)(0x80 | ((ch >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (ch & 0x3F)); }
            }
            t.k = TK_NAME;
            t.name = Intern(buf, n);
        } else if ((c >= '0' && c <= '9') || (c == '.' && c1 >= '0' && c1 <= '9')) {
            if (c == '.') {
                // .5
                char buf[80];
                int n = 0;
                buf[n++] = '0';
                while (L.i < L.end && n < 70 && ((s[L.i] >= '0' && s[L.i] <= '9') || s[L.i] == '.' || s[L.i] == '_')) {
                    if (s[L.i] != '_') buf[n++] = (char)s[L.i];
                    L.i++;
                }
                buf[n] = 0;
                bool f = L.i < L.end && (s[L.i] == 'f' || s[L.i] == 'F');
                if (L.i < L.end && (s[L.i] == 'f' || s[L.i] == 'F' || s[L.i] == 'd' || s[L.i] == 'D')) L.i++;
                t.k = TK_NUM;
                t.lit = VReal(f ? T_FLOAT : T_DOUBLE, strtod(buf, NULL));
            } else {
                LexNumber(L, t);
            }
        } else if (c == '"' || (c == '@' && c1 == '"')) {
            bool vb = c == '@';
            L.i += vb ? 2 : 1;
            t.k = TK_STR;
            t.verbatim = vb;
            t.lit.t = T_STRING;
            t.lit.s = LexString(L, vb, t.pos);
        } else if ((c == '$' && (c1 == '"' || (c1 == '@' && c2 == '"'))) || (c == '@' && c1 == '$' && c2 == '"')) {
            bool vb = c1 == '@' || c == '@';
            L.i += vb ? 3 : 2;
            t.k = TK_INTERP;
            t.verbatim = vb;
            t.lit.t = T_INT;
            t.lit.i = L.i;                  // content starts here
            SkipInterp(L, vb, t.pos);
        } else if (c == '\'') {
            L.i++;
            if (L.i >= L.end || s[L.i] == '\'' || s[L.i] == '\n')
                CompileError(t.pos, "Empty character literal");
            unsigned ch = s[L.i] == '\\' ? (L.i++, Escape(L)) : s[L.i++];
            if (L.i >= L.end || s[L.i] != '\'')
                CompileError(t.pos, "Too many characters in character literal");
            L.i++;
            t.k = TK_CHARLIT;
            t.lit = VChar(ch);
        } else {
            int k = -1, len = 1;
            switch (c) {
            case '(': k = TK_LPAR; break;
            case ')': k = TK_RPAR; break;
            case '{': k = TK_LBRACE; break;
            case '}': k = TK_RBRACE; break;
            case '[': k = TK_LBRACK; break;
            case ']': k = TK_RBRACK; break;
            case ';': k = TK_SEMI; break;
            case ',': k = TK_COMMA; break;
            case ':': k = TK_COLON; break;
            case '~': k = TK_TILDE; break;
            case '.': if (c1 == '.') { k = TK_RANGE; len = 2; } else k = TK_DOT; break;
            case '?':
                if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) { k = TK_QDOT; len = 2; }
                else if (c1 == '[') { k = TK_QLBRACK; len = 2; }
                else if (c1 == '?' && c2 == '=') { k = TK_QQEQ; len = 3; }
                else if (c1 == '?') { k = TK_QQ; len = 2; }
                else k = TK_QUESTION;
                break;
            case '=':
                if (c1 == '=') { k = TK_EQEQ; len = 2; }
                else if (c1 == '>') { k = TK_ARROW; len = 2; }
                else k = TK_EQ;
                break;
            case '!': if (c1 == '=') { k = TK_NE; len = 2; } else k = TK_NOT; break;
            case '+':
                if (c1 == '+') { k = TK_INC; len = 2; }
                else if (c1 == '=') { k = TK_PLUSEQ; len = 2; }
                else k = TK_PLUS;
                break;
            case '-':
                if (c1 == '-') { k = TK_DEC; len = 2; }
                else if (c1 == '=') { k = TK_MINUSEQ; len = 2; }
                else if (c1 == '>') CompileError(t.pos, "'->' (pointers) is not supported");
                else k = TK_MINUS;
                break;
            case '*': if (c1 == '=') { k = TK_STAREQ; len = 2; } else k = TK_STAR; break;
            case '/': if (c1 == '=') { k = TK_SLASHEQ; len = 2; } else k = TK_SLASH; break;
            case '%': if (c1 == '=') { k = TK_PERCENTEQ; len = 2; } else k = TK_PERCENT; break;
            case '^': if (c1 == '=') { k = TK_CARETEQ; len = 2; } else k = TK_CARET; break;
            case '&':
                if (c1 == '&') { k = TK_ANDAND; len = 2; }
                else if (c1 == '=') { k = TK_AMPEQ; len = 2; }
                else k = TK_AMP;
                break;
            case '|':
                if (c1 == '|') { k = TK_OROR; len = 2; }
                else if (c1 == '=') { k = TK_BAREQ; len = 2; }
                else k = TK_BAR;
                break;
            case '<':
                if (c1 == '<' && c2 == '=') { k = TK_SHLEQ; len = 3; }
                else if (c1 == '<') { k = TK_SHL; len = 2; }
                else if (c1 == '=') { k = TK_LE; len = 2; }
                else k = TK_LT;
                break;
            case '>':
                // '>>' is put together by the parser (generics: List<List<int>>)
                if (c1 == '=') { k = TK_GE; len = 2; }
                else k = TK_GT;
                break;
            default:
                break;
            }
            if (k < 0) {
                if (c == '#')
                    CompileError(t.pos, "Preprocessor directives are not supported");
                CompileError(t.pos, "Unexpected character '%c'", c < 0x80 && c >= 0x20 ? (char)c : '?');
            }
            t.k = (uint8_t)k;
            L.i += len;
        }
        t.end = L.i;
        Push(L, t);
    }
    Lexed r;
    r.t = L.t;
    r.n = L.n;
    return r;
}

} // namespace rs
