/*
 * rs_linq.cpp: sequences. LINQ methods are lazy (deferred) as in .NET:
 * Where, Select, Take, ... stream one element at a time; OrderBy, GroupBy,
 * Reverse, Join, ... buffer their input when they are enumerated. Each
 * enumeration starts again from the source, as in .NET.
 *
 * Query expressions run as a stream of "rows": each row is a scope that
 * binds the range variables (from x, let y, join z) on top of the previous
 * row, so the clauses' expressions are evaluated in that scope.
 */
#include "rs_int.h"
#include <math.h>

namespace rs {

bool Iter::Next(Value &) { return false; }
Iter *Seq::Open() { return RS_NEW(Iter); }
int Seq::TyHint() { return T_OBJECT; }
int Seq::Kind() { return 0; }

Value VSeq(Seq *q)
{
    Value v;
    v.t = T_SEQ;
    v.q = q;
    return v;
}

// ---------------------------------------------------------------------------
// enumerating containers

struct ArrIter : Iter
    {
    Arr *a;
    int i;
    bool Next(Value &aOut)
    {
        if (i >= a->n) return false;
        Tick();
        aOut = a->v[i++];
        return true;
    }
    };

struct StrIter : Iter
    {
    Str *s;
    int i;
    bool Next(Value &aOut)
    {
        if (i >= s->n) return false;
        aOut = VChar(s->c[i++]);
        return true;
    }
    };

struct MapIter : Iter
    {
    Map *m;
    int i;
    bool Next(Value &aOut)
    {
        if (i >= m->keys->n) return false;
        aOut = m->vals ? VKvp(m->keys->v[i], m->vals->v[i]) : m->keys->v[i];
        i++;
        return true;
    }
    };

static Iter *IterArr(Arr *a)
{
    ArrIter *it = RS_NEW(ArrIter);
    it->a = a;
    return it;
}

bool IsEnumerable(Value v)
{
    switch (v.t) {
    case T_ARRAY: case T_LIST: case T_STRING: case T_SEQ: case T_DICT: case T_SET: case T_GROUP:
        return true;
    default:
        return false;
    }
}

Iter *OpenSeq(Value v, int aPos)
{
    switch (v.t) {
    case T_ARRAY:
    case T_LIST:
        return IterArr(v.a);
    case T_GROUP:
        return IterArr(v.g->items);
    case T_STRING: {
        StrIter *it = RS_NEW(StrIter);
        it->s = v.s;
        return it;
    }
    case T_SEQ:
        return v.q->Open();
    case T_DICT:
    case T_SET: {
        MapIter *it = RS_NEW(MapIter);
        it->m = v.m;
        return it;
    }
    case T_NULL:
        ThrowAt(aPos, "ArgumentNullException", "Value cannot be null. (Parameter 'source')");
    default:
        ThrowAt(aPos, "InvalidOperationException", "A value of type '%s' is not a sequence (IEnumerable)", ValTyName(v));
    }
}

static int HintOf(Value v)
{
    switch (v.t) {
    case T_ARRAY: case T_LIST: return v.a->et;
    case T_GROUP: return v.g->items->et;
    case T_STRING: return T_CHAR;
    case T_SET: return v.m->kt;
    case T_SEQ: return v.q->TyHint();
    default: return T_OBJECT;
    }
}

// the element type when all elements share a primitive type
static int ElemType(Arr *a, int aHint)
{
    if (aHint != T_OBJECT) return aHint;
    if (!a->n) return T_OBJECT;
    int t = a->v[0].t;
    if (t == T_NULL || t > T_STRING) return T_OBJECT;
    for (int i = 1; i < a->n; i++)
        if (a->v[i].t != t) return T_OBJECT;
    return t;
}

Arr *ToArr(Value v, int aPos)
{
    Iter *it = OpenSeq(v, aPos);
    Arr *a = NewArr(v.t == T_ARRAY || v.t == T_LIST ? v.a->n : 8, T_OBJECT);
    Value x;
    while (it->Next(x)) ArrPush(a, x);
    a->et = (uint8_t)ElemType(a, HintOf(v));
    return a;
}

static Value VArr(Arr *a, int aKind)
{
    Value v;
    v.t = (uint8_t)aKind;
    v.a = a;
    return v;
}

// ---------------------------------------------------------------------------
// sorting: stable merge sort

struct SortCtx
    {
    Value cmp;              // comparison function, or null
    Value **keys;           // per element: its keys (OrderBy), or NULL
    bool *desc;
    int nkeys;
    int pos;
    };

static int CmpWith(SortCtx &c, int aIdx, int bIdx, Value *v)
{
    if (c.keys) {
        for (int k = 0; k < c.nkeys; k++) {
            int r = c.cmp.t == T_FUNC && k == 0 ? (int)ToI64(Call2(c.cmp, c.keys[aIdx][k], c.keys[bIdx][k], c.pos), c.pos)
                                                 : Compare(c.keys[aIdx][k], c.keys[bIdx][k], c.pos);
            if (r) return c.desc[k] ? -r : r;
        }
        return 0;
    }
    if (c.cmp.t == T_FUNC)
        return (int)ToI64(Call2(c.cmp, v[aIdx], v[bIdx], c.pos), c.pos);
    return Compare(v[aIdx], v[bIdx], c.pos);
}

// sorts the index array idx[0..n)
static void MergeSort(SortCtx &c, int *idx, int *tmp, int n, Value *v)
{
    if (n < 2) return;
    if (n <= 8) {
        for (int i = 1; i < n; i++) {
            int x = idx[i], j = i - 1;
            while (j >= 0 && CmpWith(c, idx[j], x, v) > 0) { idx[j + 1] = idx[j]; j--; }
            idx[j + 1] = x;
        }
        return;
    }
    int h = n / 2;
    MergeSort(c, idx, tmp, h, v);
    MergeSort(c, idx + h, tmp, n - h, v);
    int i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = CmpWith(c, idx[j], idx[i], v) < 0 ? idx[j++] : idx[i++];
    while (i < h) tmp[k++] = idx[i++];
    while (j < n) tmp[k++] = idx[j++];
    memcpy(idx, tmp, n * sizeof(int));
}

void SortValues(Value *aV, int aN, Value aCmp, int aPos)
{
    SortCtx c;
    c.cmp = aCmp;
    c.keys = NULL;
    c.desc = NULL;
    c.nkeys = 0;
    c.pos = aPos;
    int *idx = NewArr<int>(aN), *tmp = NewArr<int>(aN);
    for (int i = 0; i < aN; i++) idx[i] = i;
    MergeSort(c, idx, tmp, aN, aV);
    Value *copy = NewArr<Value>(aN);
    for (int i = 0; i < aN; i++) copy[i] = aV[idx[i]];
    memcpy(aV, copy, aN * sizeof(Value));
}

// ---------------------------------------------------------------------------
// streaming operators

struct RangeSeqC : Seq
    {
    i64 start, count;
    struct It : Iter
        {
        i64 cur, end;
        bool Next(Value &aOut)
        {
            if (cur >= end) return false;
            Tick();
            aOut = VInt(cur++);
            return true;
        }
        };
    Iter *Open() { It *it = RS_NEW(It); it->cur = start; it->end = start + count; return it; }
    int TyHint() { return T_INT; }
    };

Value RangeSeq(i64 aStart, i64 aCount, int aPos)
{
    if (aCount < 0 || aStart + aCount - 1 > 0x7FFFFFFFLL)
        ThrowAt(aPos, "ArgumentOutOfRangeException", "Specified argument was out of the range of valid values. (Parameter 'count')");
    RangeSeqC *q = RS_NEW(RangeSeqC);
    q->start = aStart;
    q->count = aCount;
    return VSeq(q);
}

struct RepeatSeqC : Seq
    {
    Value v;
    i64 count;
    struct It : Iter
        {
        RepeatSeqC *q;
        i64 i;
        bool Next(Value &aOut)
        {
            if (i >= q->count) return false;
            Tick();
            i++;
            aOut = q->v;
            return true;
        }
        };
    Iter *Open() { It *it = RS_NEW(It); it->q = this; return it; }
    int TyHint() { return v.t != T_NULL && v.t <= T_STRING ? v.t : T_OBJECT; }
    };

Value RepeatSeq(Value v, i64 aCount, int aPos)
{
    if (aCount < 0)
        ThrowAt(aPos, "ArgumentOutOfRangeException", "count ('%lld') must be a non-negative value. (Parameter 'count')", (long long)aCount);
    RepeatSeqC *q = RS_NEW(RepeatSeqC);
    q->v = v;
    q->count = aCount;
    return VSeq(q);
}

// one function, one source: Where, Select, TakeWhile, SkipWhile, Take, Skip, ...
enum TStreamOp { SO_WHERE, SO_SELECT, SO_TAKEWHILE, SO_SKIPWHILE, SO_TAKE, SO_SKIP, SO_CAST, SO_OFTYPE, SO_DISTINCT, SO_DEFAULTIFEMPTY, SO_INDEX };

struct StreamSeq : Seq
    {
    int op;
    Value src, f;
    bool withIndex;
    i64 n;
    int ty;                 // Cast/OfType target, element hint
    int pos;
    struct It : Iter
        {
        StreamSeq *q;
        Iter *in;
        i64 i;
        bool done, skipping, any;
        Map *seen;
        bool Next(Value &aOut)
        {
            StreamSeq *s = q;
            Value v;
            for (;;) {
                if (done || !in->Next(v)) {
                    if (s->op == SO_DEFAULTIFEMPTY && !any && !done) {
                        done = true;
                        any = true;
                        aOut = s->f.t == T_NULL && s->withIndex ? DefaultOf(s->ty) : s->f;
                        return true;
                    }
                    done = true;
                    return false;
                }
                i64 idx = i++;
                any = true;
                switch (s->op) {
                case SO_WHERE: {
                    Value r = s->withIndex ? Call2(s->f, v, VInt(idx), s->pos) : Call1(s->f, v, s->pos);
                    if (!Truthy(r, s->pos)) continue;
                    aOut = v;
                    return true;
                }
                case SO_SELECT:
                    aOut = s->withIndex ? Call2(s->f, v, VInt(idx), s->pos) : Call1(s->f, v, s->pos);
                    return true;
                case SO_TAKEWHILE: {
                    Value r = s->withIndex ? Call2(s->f, v, VInt(idx), s->pos) : Call1(s->f, v, s->pos);
                    if (!Truthy(r, s->pos)) { done = true; return false; }
                    aOut = v;
                    return true;
                }
                case SO_SKIPWHILE:
                    if (skipping) {
                        Value r = s->withIndex ? Call2(s->f, v, VInt(idx), s->pos) : Call1(s->f, v, s->pos);
                        if (Truthy(r, s->pos)) continue;
                        skipping = false;
                    }
                    aOut = v;
                    return true;
                case SO_TAKE:
                    if (idx >= s->n) { done = true; return false; }
                    aOut = v;
                    if (idx + 1 >= s->n) done = true;
                    return true;
                case SO_SKIP:
                    if (idx < s->n) continue;
                    aOut = v;
                    return true;
                case SO_CAST:
                    aOut = s->ty == T_OBJECT ? v : ConvertExplicit(v, s->ty, s->pos);
                    if (s->ty != T_OBJECT && IsNumeric(v.t) && v.t != s->ty)
                        ThrowAt(s->pos, "InvalidCastException", "Unable to cast object of type '%s' to type '%s'.",
                                TyName(v.t), TyName(s->ty));
                    return true;
                case SO_OFTYPE:
                    if (v.t == T_NULL) continue;
                    if (s->ty != T_OBJECT && v.t != s->ty) continue;
                    aOut = v;
                    return true;
                case SO_DISTINCT: {
                    Value key = s->f.t == T_FUNC ? Call1(s->f, v, s->pos) : v;
                    if (MapAdd(seen, key, VNull(), false) < 0) continue;
                    aOut = v;
                    return true;
                }
                case SO_DEFAULTIFEMPTY:
                    aOut = v;
                    return true;
                case SO_INDEX:
                    aOut = VTuple2(VInt(idx), v);
                    aOut.o->names[0] = Intern("Index", 5);
                    aOut.o->names[1] = Intern("Item", 4);
                    return true;
                }
                return false;
            }
        }
        };
    Iter *Open()
    {
        It *it = RS_NEW(It);
        it->q = this;
        it->in = OpenSeq(src, pos);
        it->skipping = true;
        if (op == SO_DISTINCT) it->seen = NewMap(false);
        if (op == SO_TAKE && n <= 0) it->done = true;
        return it;
    }
    int TyHint()
    {
        switch (op) {
        case SO_SELECT: case SO_INDEX: return T_OBJECT;
        case SO_CAST: case SO_OFTYPE: return ty;
        default: return HintOf(src);
        }
    }
    };

static Value Stream(int aOp, Value src, Value f, bool aIdx, i64 n, int ty, int pos)
{
    StreamSeq *q = RS_NEW(StreamSeq);
    q->op = aOp;
    q->src = src;
    q->f = f;
    q->withIndex = aIdx;
    q->n = n;
    q->ty = ty;
    q->pos = pos;
    return VSeq(q);
}

struct ConcatSeq : Seq
    {
    Value a, b;
    int pos;
    struct It : Iter
        {
        ConcatSeq *q;
        Iter *cur;
        bool second;
        bool Next(Value &aOut)
        {
            for (;;) {
                if (cur->Next(aOut)) return true;
                if (second) return false;
                second = true;
                cur = OpenSeq(q->b, q->pos);
            }
        }
        };
    Iter *Open() { It *it = RS_NEW(It); it->q = this; it->cur = OpenSeq(a, pos); return it; }
    int TyHint() { int x = HintOf(a); return x == HintOf(b) ? x : T_OBJECT; }
    };

struct SelectManySeq : Seq
    {
    Value src, f, res;      // res: result selector (x, y) => ...
    bool withIndex;
    int pos;
    struct It : Iter
        {
        SelectManySeq *q;
        Iter *outer, *inner;
        Value cur;
        int i;
        bool Next(Value &aOut)
        {
            for (;;) {
                if (inner) {
                    Value v;
                    if (inner->Next(v)) {
                        aOut = q->res.t == T_FUNC ? Call2(q->res, cur, v, q->pos) : v;
                        return true;
                    }
                    inner = NULL;
                }
                if (!outer->Next(cur)) return false;
                Value coll = q->withIndex ? Call2(q->f, cur, VInt(i), q->pos) : Call1(q->f, cur, q->pos);
                i++;
                inner = OpenSeq(coll, q->pos);
            }
        }
        };
    Iter *Open() { It *it = RS_NEW(It); it->q = this; it->outer = OpenSeq(src, pos); return it; }
    };

struct ZipSeq : Seq
    {
    Value a, b, c, f;       // c: third sequence (or null)
    int pos;
    struct It : Iter
        {
        ZipSeq *q;
        Iter *ia, *ib, *ic;
        bool Next(Value &aOut)
        {
            Value x, y, z;
            if (!ia->Next(x) || !ib->Next(y)) return false;
            if (ic) {
                if (!ic->Next(z)) return false;
                Obj *o = NewObj(OK_TUPLE, 3);
                o->f[0] = x;
                o->f[1] = y;
                o->f[2] = z;
                aOut.t = T_OBJ;
                aOut.o = o;
                return true;
            }
            if (q->f.t == T_FUNC) {
                aOut = Call2(q->f, x, y, q->pos);
            } else {
                aOut = VTuple2(x, y);
                aOut.o->names[0] = Intern("First", 5);
                aOut.o->names[1] = Intern("Second", 6);
            }
            return true;
        }
        };
    Iter *Open()
    {
        It *it = RS_NEW(It);
        it->q = this;
        it->ia = OpenSeq(a, pos);
        it->ib = OpenSeq(b, pos);
        it->ic = c.t != T_NULL ? OpenSeq(c, pos) : NULL;
        return it;
    }
    };

struct AppendSeq : Seq
    {
    Value src, v;
    bool prepend;
    int pos;
    struct It : Iter
        {
        AppendSeq *q;
        Iter *in;
        int state;          // prepend: 0 value, 1 rest; append: 0 rest, 1 value, 2 done
        bool Next(Value &aOut)
        {
            if (q->prepend) {
                if (state == 0) { state = 1; aOut = q->v; return true; }
                return in->Next(aOut);
            }
            if (state == 0) {
                if (in->Next(aOut)) return true;
                state = 1;
            }
            if (state == 1) { state = 2; aOut = q->v; return true; }
            return false;
        }
        };
    Iter *Open() { It *it = RS_NEW(It); it->q = this; it->in = OpenSeq(src, pos); return it; }
    int TyHint() { return HintOf(src) == v.t ? v.t : T_OBJECT; }
    };

// ---------------------------------------------------------------------------
// buffered operators: computed when enumerated

enum TBufOp { BO_REVERSE, BO_GROUPBY, BO_JOIN, BO_GROUPJOIN, BO_UNION, BO_INTERSECT, BO_EXCEPT,
              BO_CHUNK, BO_TAKELAST, BO_SKIPLAST, BO_COUNTBY };

static Value MakeGroup(Value key, Arr *items)
{
    Group *g = New<Group>();
    g->key = key;
    g->items = items;
    items->et = (uint8_t)ElemType(items, T_OBJECT);
    Value v;
    v.t = T_GROUP;
    v.g = g;
    return v;
}

// GroupBy over a materialised array: groups in order of first appearance
static Arr *GroupInto(Arr *src, Value keyFn, Value elemFn, int pos)
{
    Map *m = NewMap(true);
    Arr *groups = NewArr(8, T_GROUP);
    for (int i = 0; i < src->n; i++) {
        Value key = Call1(keyFn, src->v[i], pos);
        Value el = elemFn.t == T_FUNC ? Call1(elemFn, src->v[i], pos) : src->v[i];
        int at = MapFind(m, key);
        if (at < 0) {
            Value g = MakeGroup(key, NewArr(4, T_OBJECT));
            MapAdd(m, key, VInt(groups->n), false);
            ArrPush(groups, g);
            ArrPush(g.g->items, el);
        } else {
            ArrPush(groups->v[m->vals->v[at].i].g->items, el);
        }
    }
    for (int i = 0; i < groups->n; i++) {
        Arr *items = groups->v[i].g->items;
        items->et = (uint8_t)ElemType(items, T_OBJECT);
    }
    return groups;
}

struct BufSeq : Seq
    {
    int op;
    Value src, a1, a2, a3, a4;
    int pos;
    Arr *Compute()
    {
        Arr *in = ToArr(src, pos);
        switch (op) {
        case BO_REVERSE: {
            Arr *r = NewArr(in->n, in->et);
            for (int i = in->n - 1; i >= 0; i--) ArrPush(r, in->v[i]);
            return r;
        }
        case BO_GROUPBY: {
            Arr *groups = GroupInto(in, a1, a2, pos);
            if (a3.t == T_FUNC) {
                Arr *r = NewArr(groups->n, T_OBJECT);
                for (int i = 0; i < groups->n; i++) {
                    Value gv = groups->v[i];
                    ArrPush(r, Call2(a3, gv.g->key, VArr(gv.g->items, T_ARRAY), pos));
                }
                return r;
            }
            return groups;
        }
        case BO_COUNTBY: {
            Arr *groups = GroupInto(in, a1, VNull(), pos);
            Arr *r = NewArr(groups->n, T_OBJECT);
            for (int i = 0; i < groups->n; i++)
                ArrPush(r, VKvp(groups->v[i].g->key, VInt(groups->v[i].g->items->n)));
            return r;
        }
        case BO_JOIN:
        case BO_GROUPJOIN: {
            // a1 inner, a2 outer key, a3 inner key, a4 result
            Arr *inner = ToArr(a1, pos);
            Map *lookup = NewMap(true);
            for (int i = 0; i < inner->n; i++) {
                Value k = Call1(a3, inner->v[i], pos);
                if (k.t == T_NULL) continue;
                int at = MapFind(lookup, k);
                if (at < 0) at = MapAdd(lookup, k, VArr(NewArr(2, inner->et), T_ARRAY), false);
                ArrPush(lookup->vals->v[at].a, inner->v[i]);
            }
            Arr *r = NewArr(in->n, T_OBJECT);
            for (int i = 0; i < in->n; i++) {
                Value k = Call1(a2, in->v[i], pos);
                int at = k.t == T_NULL ? -1 : MapFind(lookup, k);
                if (op == BO_GROUPJOIN) {
                    Value matches = at >= 0 ? lookup->vals->v[at] : VArr(NewArr(0, inner->et), T_ARRAY);
                    ArrPush(r, Call2(a4, in->v[i], matches, pos));
                } else if (at >= 0) {
                    Arr *ms = lookup->vals->v[at].a;
                    for (int j = 0; j < ms->n; j++) ArrPush(r, Call2(a4, in->v[i], ms->v[j], pos));
                }
            }
            return r;
        }
        case BO_UNION:
        case BO_INTERSECT:
        case BO_EXCEPT: {
            // a1 other, a2 key selector (the By variants)
            Map *seen = NewMap(false);
            Arr *r = NewArr(in->n, in->et);
            if (op == BO_UNION) {
                Arr *other = ToArr(a1, pos);
                for (int pass = 0; pass < 2; pass++) {
                    Arr *x = pass ? other : in;
                    for (int i = 0; i < x->n; i++) {
                        Value k = a2.t == T_FUNC ? Call1(a2, x->v[i], pos) : x->v[i];
                        if (MapAdd(seen, k, VNull(), false) >= 0) ArrPush(r, x->v[i]);
                    }
                }
                return r;
            }
            Map *second = NewMap(false);
            Arr *other = ToArr(a1, pos);
            for (int i = 0; i < other->n; i++) MapAdd(second, other->v[i], VNull(), false);
            for (int i = 0; i < in->n; i++) {
                Value k = a2.t == T_FUNC ? Call1(a2, in->v[i], pos) : in->v[i];
                bool inSecond = MapFind(second, k) >= 0;
                if (inSecond != (op == BO_INTERSECT)) continue;
                if (MapAdd(seen, k, VNull(), false) >= 0) ArrPush(r, in->v[i]);
            }
            return r;
        }
        case BO_CHUNK: {
            i64 size = a1.i;
            Arr *r = NewArr(8, T_ARRAY);
            for (int i = 0; i < in->n; i += (int)size) {
                int cnt = in->n - i < size ? in->n - i : (int)size;
                Arr *c = NewArr(cnt, in->et);
                for (int j = 0; j < cnt; j++) ArrPush(c, in->v[i + j]);
                ArrPush(r, VArr(c, T_ARRAY));
            }
            return r;
        }
        case BO_TAKELAST:
        case BO_SKIPLAST: {
            int cnt = a1.i < 0 ? 0 : a1.i > in->n ? in->n : (int)a1.i;
            Arr *r = NewArr(in->n, in->et);
            int from = op == BO_TAKELAST ? in->n - cnt : 0, to = op == BO_TAKELAST ? in->n : in->n - cnt;
            for (int i = from; i < to; i++) ArrPush(r, in->v[i]);
            return r;
        }
        }
        return in;
    }
    Iter *Open() { return IterArr(Compute()); }
    int TyHint()
    {
        switch (op) {
        case BO_REVERSE: case BO_UNION: case BO_INTERSECT: case BO_EXCEPT: case BO_TAKELAST: case BO_SKIPLAST:
            return HintOf(src);
        default:
            return T_OBJECT;
        }
    }
    };

static Value Buf(int aOp, Value src, Value a1, Value a2, Value a3, Value a4, int pos)
{
    BufSeq *q = RS_NEW(BufSeq);
    q->op = aOp;
    q->src = src;
    q->a1 = a1;
    q->a2 = a2;
    q->a3 = a3;
    q->a4 = a4;
    q->pos = pos;
    return VSeq(q);
}

// OrderBy(...).ThenBy(...): keys accumulate; sorted when enumerated
struct OrderSeq : Seq
    {
    Value src;
    Value keyFns[8];        // null: the element itself (Order())
    bool desc[8];
    Value cmp;              // comparer for the first key (rare)
    int nkeys;
    int pos;
    Iter *Open()
    {
        Arr *in = ToArr(src, pos);
        int n = in->n;
        SortCtx c;
        c.cmp = cmp;
        c.desc = desc;
        c.nkeys = nkeys;
        c.pos = pos;
        c.keys = NewArr<Value *>(n);
        for (int i = 0; i < n; i++) {
            c.keys[i] = NewArr<Value>(nkeys);
            for (int k = 0; k < nkeys; k++)
                c.keys[i][k] = keyFns[k].t == T_FUNC ? Call1(keyFns[k], in->v[i], pos) : in->v[i];
        }
        int *idx = NewArr<int>(n), *tmp = NewArr<int>(n);
        for (int i = 0; i < n; i++) idx[i] = i;
        MergeSort(c, idx, tmp, n, in->v);
        Arr *r = NewArr(n, in->et);
        for (int i = 0; i < n; i++) ArrPush(r, in->v[idx[i]]);
        return IterArr(r);
    }
    int TyHint() { return HintOf(src); }
    int Kind() { return 1; }
    };

// ---------------------------------------------------------------------------
// terminal operators and the method table

static Value Default(int aTy) { return DefaultOf(aTy); }

RS_NORETURN static void NoElements(int pos, bool aMatching)
{
    ThrowAt(pos, "InvalidOperationException", aMatching ? "Sequence contains no matching element" : "Sequence contains no elements");
}

static bool NumEq(Value a, Value b)
{
    if (IsNumeric(a.t) && IsNumeric(b.t)) return OpEquals(a, b, -1);
    return ValEquals(a, b);
}

static Value SumOf(Iter *it, Value f, int pos, int aHint)
{
    Value acc;
    bool any = false;
    Value v;
    bool saved = gChecked;
    while (it->Next(v)) {
        if (f.t == T_FUNC) v = Call1(f, v, pos);
        if (v.t == T_NULL) continue;
        if (!IsNumeric(v.t))
            ThrowAt(pos, "InvalidOperationException", "Sum: '%s' is not a number", ValTyName(v));
        if (!any) {
            acc = v.t == T_INT || v.t == T_LONG || IsReal(v.t) || v.t == T_UINT || v.t == T_ULONG ? v : NumTo(v, T_INT, false, pos);
            any = true;
            continue;
        }
        gChecked = true;                    // Enumerable.Sum is checked
        acc = Binary(TK_PLUS, acc, v, pos);
        gChecked = saved;
    }
    gChecked = saved;
    if (!any) {
        int t = aHint;
        if (!IsNumeric(t) || t == T_CHAR) t = T_INT;
        return NumTo(VInt(0), t, false, pos);
    }
    return acc;
}

static Value MinMax(Iter *it, Value f, bool aMax, int pos, bool aBy)
{
    Value best, bestKey, v;
    bool any = false;
    while (it->Next(v)) {
        Value k = f.t == T_FUNC ? Call1(f, v, pos) : v;
        if (k.t == T_NULL) continue;
        if (!any) {
            best = v;
            bestKey = k;
            any = true;
            continue;
        }
        int c = Compare(k, bestKey, pos);
        if (aMax ? c > 0 : c < 0) {
            best = v;
            bestKey = k;
        }
    }
    if (!any) {
        if (aBy) return VNull();
        NoElements(pos, false);
    }
    return aBy ? best : bestKey;
}

static int TargOf(Node *aCall)
{
    Node *tg = aCall ? aCall->a : NULL;
    if (tg && tg->ntargs == 1) return TypeTy(tg->targs[0]);
    return T_OBJECT;
}

static void Need(int id, int n, int lo, int hi, int pos)
{
    if (n < lo || n > hi)
        ThrowAt(pos, "InvalidOperationException", "No overload for method '%s' takes %d argument%s",
                NameText(id), n, n == 1 ? "" : "s");
}

static Value FnArg(Value *a, int i, int id, int pos)
{
    if (a[i].t != T_FUNC)
        ThrowAt(pos, "InvalidOperationException", "%s: argument %d must be a function (x => ...)", NameText(id), i + 1);
    return a[i];
}

Value Linq(Value src, int id, Value *a, int n, Node *call, int pos, bool *aFound)
{
    *aFound = true;
    if (!IsEnumerable(src)) {
        *aFound = false;
        return VNull();
    }
    Value nul = VNull();
#define need(lo, hi) Need(id, n, (lo), (hi), pos)
#define fn(i) FnArg(a, (i), id, pos)
    switch (id) {
    // ---- streaming ----
    case K_Where: need(1, 1); return Stream(SO_WHERE, src, fn(0), FuncArity(a[0]) == 2, 0, 0, pos);
    case K_Select: need(1, 1); return Stream(SO_SELECT, src, fn(0), FuncArity(a[0]) == 2, 0, 0, pos);
    case K_TakeWhile: need(1, 1); return Stream(SO_TAKEWHILE, src, fn(0), FuncArity(a[0]) == 2, 0, 0, pos);
    case K_SkipWhile: need(1, 1); return Stream(SO_SKIPWHILE, src, fn(0), FuncArity(a[0]) == 2, 0, 0, pos);
    case K_Take:
        need(1, 1);
        if (a[0].t == T_OBJ) break;
        return Stream(SO_TAKE, src, nul, false, ToI64(a[0], pos), 0, pos);
    case K_Skip: need(1, 1); return Stream(SO_SKIP, src, nul, false, ToI64(a[0], pos), 0, pos);
    case K_Cast: need(0, 0); return Stream(SO_CAST, src, nul, false, 0, TargOf(call), pos);
    case K_OfType: need(0, 0); return Stream(SO_OFTYPE, src, nul, false, 0, TargOf(call), pos);
    case K_Distinct: need(0, 0); return Stream(SO_DISTINCT, src, nul, false, 0, 0, pos);
    case K_DistinctBy: need(1, 1); return Stream(SO_DISTINCT, src, fn(0), false, 0, 0, pos);
    case K_Index: need(0, 0); return Stream(SO_INDEX, src, nul, false, 0, 0, pos);
    case K_DefaultIfEmpty:
        need(0, 1);
        return Stream(SO_DEFAULTIFEMPTY, src, n ? a[0] : nul, n == 0, 0, HintOf(src), pos);
    case K_AsEnumerable: need(0, 0); return src;
    case K_Concat: {
        need(1, 1);
        ConcatSeq *q = RS_NEW(ConcatSeq);
        q->a = src;
        q->b = a[0];
        q->pos = pos;
        OpenSeq(a[0], pos);                 // fail early on a non-sequence
        return VSeq(q);
    }
    case K_SelectMany: {
        need(1, 2);
        SelectManySeq *q = RS_NEW(SelectManySeq);
        q->src = src;
        q->f = fn(0);
        q->res = n == 2 ? fn(1) : nul;
        q->withIndex = FuncArity(a[0]) == 2 && n == 1;
        q->pos = pos;
        return VSeq(q);
    }
    case K_Zip: {
        need(1, 2);
        ZipSeq *q = RS_NEW(ZipSeq);
        q->a = src;
        q->b = a[0];
        q->c = n == 2 && a[1].t != T_FUNC ? a[1] : nul;
        q->f = n == 2 && a[1].t == T_FUNC ? a[1] : nul;
        q->pos = pos;
        return VSeq(q);
    }
    case K_Append:
    case K_Prepend: {
        need(1, 1);
        AppendSeq *q = RS_NEW(AppendSeq);
        q->src = src;
        q->v = a[0];
        q->prepend = id == K_Prepend;
        q->pos = pos;
        return VSeq(q);
    }
    // ---- ordering ----
    case K_OrderBy: case K_OrderByDescending: case K_Order: case K_OrderDescending: {
        bool byKey = id == K_OrderBy || id == K_OrderByDescending;
        need(byKey ? 1 : 0, byKey ? 2 : 1);
        OrderSeq *q = RS_NEW(OrderSeq);
        q->src = src;
        q->nkeys = 1;
        q->keyFns[0] = byKey ? fn(0) : nul;
        q->desc[0] = id == K_OrderByDescending || id == K_OrderDescending;
        q->cmp = nul;
        q->pos = pos;
        return VSeq(q);
    }
    case K_ThenBy: case K_ThenByDescending: {
        need(1, 1);
        if (src.t != T_SEQ || src.q->Kind() != 1)
            ThrowAt(pos, "InvalidOperationException", "%s must follow OrderBy or OrderByDescending", NameText(id));
        OrderSeq *o = (OrderSeq *)src.q;
        if (o->nkeys == 8) ThrowAt(pos, "InvalidOperationException", "Too many ThenBy keys");
        OrderSeq *q = RS_NEW(OrderSeq);
        *q = *o;
        q->keyFns[q->nkeys] = fn(0);
        q->desc[q->nkeys] = id == K_ThenByDescending;
        q->nkeys++;
        return VSeq(q);
    }
    case K_Reverse: need(0, 0); return Buf(BO_REVERSE, src, nul, nul, nul, nul, pos);
    // ---- grouping and joining ----
    case K_GroupBy: {
        need(1, 3);
        Value key = fn(0), elem = nul, res = nul;
        if (n == 2) {
            // GroupBy(key, element) or GroupBy(key, (k, items) => ...)
            if (FuncArity(a[1]) == 2) res = fn(1);
            else elem = fn(1);
        } else if (n == 3) {
            elem = fn(1);
            res = fn(2);
        }
        return Buf(BO_GROUPBY, src, key, elem, res, nul, pos);
    }
    case K_CountBy: need(1, 1); return Buf(BO_COUNTBY, src, fn(0), nul, nul, nul, pos);
    case K_Join:
    case K_GroupJoin:
        need(4, 4);
        OpenSeq(a[0], pos);
        return Buf(id == K_Join ? BO_JOIN : BO_GROUPJOIN, src, a[0], fn(1), fn(2), fn(3), pos);
    case K_Union: case K_Intersect: case K_Except:
        need(1, 1);
        return Buf(id == K_Union ? BO_UNION : id == K_Intersect ? BO_INTERSECT : BO_EXCEPT, src, a[0], nul, nul, nul, pos);
    case K_UnionBy: case K_IntersectBy: case K_ExceptBy:
        need(2, 2);
        return Buf(id == K_UnionBy ? BO_UNION : id == K_IntersectBy ? BO_INTERSECT : BO_EXCEPT, src, a[0], fn(1), nul, nul, pos);
    case K_Chunk: {
        need(1, 1);
        i64 size = ToI64(a[0], pos);
        if (size < 1) ThrowAt(pos, "ArgumentOutOfRangeException", "size ('%lld') must be greater than '0'. (Parameter 'size')", (long long)size);
        return Buf(BO_CHUNK, src, VLong(size), nul, nul, nul, pos);
    }
    case K_TakeLast: need(1, 1); return Buf(BO_TAKELAST, src, VLong(ToI64(a[0], pos)), nul, nul, nul, pos);
    case K_SkipLast: need(1, 1); return Buf(BO_SKIPLAST, src, VLong(ToI64(a[0], pos)), nul, nul, nul, pos);
    default:
        break;
    }

    // ---- terminal ----
    switch (id) {
    case K_Count:
    case K_LongCount: {
        need(0, 1);
        i64 c = 0;
        if (n == 0 && (src.t == T_ARRAY || src.t == T_LIST)) c = src.a->n;
        else if (n == 0 && src.t == T_STRING) c = src.s->n;
        else {
            Iter *it = OpenSeq(src, pos);
            Value v;
            while (it->Next(v))
                if (!n || Truthy(Call1(fn(0), v, pos), pos)) c++;
        }
        return id == K_Count ? VInt(c) : VLong(c);
    }
    case K_Sum:
        need(0, 1);
        return SumOf(OpenSeq(src, pos), n ? fn(0) : nul, pos, HintOf(src));
    case K_Average: {
        need(0, 1);
        Iter *it = OpenSeq(src, pos);
        Value v;
        double sum = 0;
        i64 cnt = 0;
        bool isFloat = true;
        while (it->Next(v)) {
            if (n) v = Call1(fn(0), v, pos);
            if (v.t == T_NULL) continue;
            if (!IsNumeric(v.t)) ThrowAt(pos, "InvalidOperationException", "Average: '%s' is not a number", ValTyName(v));
            if (v.t != T_FLOAT) isFloat = false;
            sum += ToDouble(v);
            cnt++;
        }
        if (!cnt) NoElements(pos, false);
        return VReal(isFloat ? T_FLOAT : T_DOUBLE, sum / cnt);
    }
    case K_Min: case K_Max:
        need(0, 1);
        return MinMax(OpenSeq(src, pos), n ? fn(0) : nul, id == K_Max, pos, false);
    case K_MinBy: case K_MaxBy:
        need(1, 1);
        return MinMax(OpenSeq(src, pos), fn(0), id == K_MaxBy, pos, true);
    case K_First: case K_FirstOrDefault: case K_Last: case K_LastOrDefault:
    case K_Single: case K_SingleOrDefault: {
        bool orDefault = id == K_FirstOrDefault || id == K_LastOrDefault || id == K_SingleOrDefault;
        need(0, orDefault ? 2 : 1);
        Value pred = n >= 1 && a[0].t == T_FUNC ? a[0] : nul;
        Value dflt = n == 2 ? a[1] : (n == 1 && pred.t == T_NULL ? a[0] : nul);
        bool hasDflt = n == 2 || (n == 1 && pred.t == T_NULL);
        Iter *it = OpenSeq(src, pos);
        Value v, found;
        int count = 0, firstTy = -1;
        while (it->Next(v)) {
            if (firstTy < 0) firstTy = v.t;
            if (pred.t == T_FUNC && !Truthy(Call1(pred, v, pos), pos)) continue;
            found = v;
            count++;
            if (id == K_First || id == K_FirstOrDefault) break;
            if ((id == K_Single || id == K_SingleOrDefault) && count > 1)
                ThrowAt(pos, "InvalidOperationException", pred.t == T_FUNC ? "Sequence contains more than one matching element"
                                                                         : "Sequence contains more than one element");
        }
        if (count) return found;
        if (!orDefault) NoElements(pos, pred.t == T_FUNC);
        if (hasDflt) return dflt;
        int t = HintOf(src);
        if (t == T_OBJECT && firstTy >= 0 && firstTy < T_STRING) t = firstTy;
        return Default(t);
    }
    case K_ElementAt: case K_ElementAtOrDefault: {
        need(1, 1);
        i64 want = ToI64(a[0], pos);
        Iter *it = OpenSeq(src, pos);
        Value v;
        int firstTy = -1;
        for (i64 i = 0; it->Next(v); i++) {
            if (firstTy < 0) firstTy = v.t;
            if (i == want && want >= 0) return v;
        }
        if (id == K_ElementAt)
            ThrowAt(pos, "ArgumentOutOfRangeException", "Index was out of range. Must be non-negative and less than the size of the collection. (Parameter 'index')");
        int t = HintOf(src);
        if (t == T_OBJECT && firstTy >= 0 && firstTy < T_STRING) t = firstTy;
        return Default(t);
    }
    case K_Any: {
        need(0, 1);
        Iter *it = OpenSeq(src, pos);
        Value v;
        while (it->Next(v))
            if (!n || Truthy(Call1(fn(0), v, pos), pos)) return VBool(true);
        return VBool(false);
    }
    case K_All: {
        need(1, 1);
        Iter *it = OpenSeq(src, pos);
        Value v;
        while (it->Next(v))
            if (!Truthy(Call1(fn(0), v, pos), pos)) return VBool(false);
        return VBool(true);
    }
    case K_Contains: {
        need(1, 1);
        if (src.t == T_STRING) break;
        Iter *it = OpenSeq(src, pos);
        Value v;
        while (it->Next(v))
            if (NumEq(v, a[0])) return VBool(true);
        return VBool(false);
    }
    case K_Aggregate: {
        need(1, 3);
        Iter *it = OpenSeq(src, pos);
        Value acc, v;
        Value f = n == 1 ? fn(0) : fn(1);
        if (n == 1) {
            if (!it->Next(acc)) NoElements(pos, false);
        } else {
            acc = a[0];
        }
        while (it->Next(v)) {
            Value r = Call2(f, acc, v, pos);
            acc = IsNumeric(acc.t) && IsNumeric(r.t) && r.t != acc.t && n > 1 ? NumTo(r, acc.t, false, pos) : r;
        }
        if (n == 3) acc = Call1(fn(2), acc, pos);
        return acc;
    }
    case K_ToList:
    case K_ToArray: {
        need(0, 0);
        return VArr(ToArr(src, pos), id == K_ToList ? T_LIST : T_ARRAY);
    }
    case K_ToHashSet: {
        need(0, 0);
        Map *m = NewMap(false);
        Iter *it = OpenSeq(src, pos);
        Value v;
        while (it->Next(v)) MapAdd(m, v, nul, false);
        m->kt = (uint8_t)ElemType(m->keys, HintOf(src));
        Value r;
        r.t = T_SET;
        r.m = m;
        return r;
    }
    case K_ToDictionary: {
        need(0, 2);
        Map *m = NewMap(true);
        Iter *it = OpenSeq(src, pos);
        Value v;
        while (it->Next(v)) {
            Value k, val;
            if (n == 0) {
                if (v.t != T_OBJ || (v.o->kind != OK_KVP && !(v.o->kind == OK_TUPLE && v.o->n == 2)))
                    ThrowAt(pos, "InvalidOperationException", "ToDictionary() needs key/value pairs; use ToDictionary(x => key, x => value)");
                k = v.o->f[0];
                val = v.o->f[1];
            } else {
                k = Call1(fn(0), v, pos);
                val = n == 2 ? Call1(fn(1), v, pos) : v;
            }
            if (MapAdd(m, k, val, false) < 0)
                ThrowAt(pos, "ArgumentException", "An item with the same key has already been added. Key: %s", StrUtf8(ToStr(k)));
        }
        m->kt = (uint8_t)ElemType(m->keys, T_OBJECT);
        m->vt = (uint8_t)ElemType(m->vals, T_OBJECT);
        Value r;
        r.t = T_DICT;
        r.m = m;
        return r;
    }
    case K_SequenceEqual: {
        need(1, 1);
        Iter *x = OpenSeq(src, pos), *y = OpenSeq(a[0], pos);
        Value u, w;
        for (;;) {
            bool hu = x->Next(u), hw = y->Next(w);
            if (hu != hw) return VBool(false);
            if (!hu) return VBool(true);
            if (!ValEquals(u, w)) return VBool(false);
        }
    }
    default:
        break;
    }
    *aFound = false;
    return VNull();
#undef need
#undef fn
}

// ---------------------------------------------------------------------------
// query expressions

static Value VRow(Scope *s)
{
    Value v;
    v.t = T_COUNT;          // internal: a row
    v.i = (i64)(intptr_t)s;
    return v;
}

static Scope *RowOf(const Value &v) { return (Scope *)(intptr_t)v.i; }

// a row that no later clause can see again (only its own scope, not the
// rows it was built on: a second from still uses those)
static void ReleaseRow(const Value &v) { ReleaseScope(RowOf(v)); }

// rows -> rows (or values for select/group)
struct QStage : Seq
    {
    Node *c;                // the clause
    Scope *env;             // the query's scope (first from, join sources)
    Value src;              // the previous stage (rows) or, for a first from over a continuation, values
    bool srcIsValues;
    int pos;

    Scope *Bind(Scope *aUp, Name *aName, Value v, TypeRef *t)
    {
        // Rows are freed after select (or a failing where) unless a lambda
        // captured them; nothing else frees them.
        Scope *r = NewScope(aUp);
        Var *var = Declare(r, aName, t ? TypeTy(t) : T_OBJECT, c->pos);
        var->v = t && !t->isVar ? ConvertExplicit(v, TypeTy(t), c->pos) : v;
        return r;
    }

    struct It : Iter
        {
        QStage *q;
        Iter *in;           // rows (or values)
        Iter *inner;        // second from: the current row's collection
        Scope *row;
        Arr *buf;           // join matches, orderby result
        int bi;
        Map *lookup;
        bool Next(Value &aOut);
        };

    Iter *Open();
    };

Iter *QStage::Open()
{
    It *it = RS_NEW(It);
    it->q = this;
    switch (c->k) {
    case Q_FROM:
        if (src.t == T_NULL && !srcIsValues) {
            // the first from: its collection, in the query's scope
            it->in = OpenSeq(Eval(c->a, env), c->pos);
        } else if (srcIsValues) {
            it->in = OpenSeq(src, c->pos);
        } else {
            it->in = OpenSeq(src, c->pos);
        }
        break;
    case Q_JOIN: {
        it->in = OpenSeq(src, c->pos);
        // the inner side, keyed (evaluated in the query's scope)
        Arr *inner = ToArr(Eval(c->a, env), c->pos);
        Map *lookup = NewMap(true);
        for (int i = 0; i < inner->n; i++) {
            Scope *tmp = Bind(env, c->name, inner->v[i], c->type);
            Value k = Eval(c->c, tmp);
            int at = MapFind(lookup, k);
            if (at < 0) {
                Value arr;
                arr.t = T_ARRAY;
                arr.a = NewArr(2, inner->et);
                at = MapAdd(lookup, k, arr, false);
            }
            ArrPush(lookup->vals->v[at].a, inner->v[i]);
        }
        it->lookup = lookup;
        break;
    }
    case Q_ORDERBY: {
        Arr *rows = ToArr(src, c->pos);
        int n = rows->n, nk = c->n;
        SortCtx sc;
        sc.cmp = VNull();
        sc.nkeys = nk;
        sc.pos = c->pos;
        sc.desc = NewArr<bool>(nk);
        for (int k = 0; k < nk; k++) sc.desc[k] = (c->xs[k]->flags & F_DESC) != 0;
        sc.keys = NewArr<Value *>(n);
        for (int i = 0; i < n; i++) {
            sc.keys[i] = NewArr<Value>(nk);
            for (int k = 0; k < nk; k++) sc.keys[i][k] = Eval(c->xs[k]->a, RowOf(rows->v[i]));
        }
        int *idx = NewArr<int>(n), *tmp = NewArr<int>(n);
        for (int i = 0; i < n; i++) idx[i] = i;
        MergeSort(sc, idx, tmp, n, rows->v);
        it->buf = NewArr(n, T_OBJECT);
        for (int i = 0; i < n; i++) ArrPush(it->buf, rows->v[idx[i]]);
        break;
    }
    case Q_GROUP: {
        Arr *rows = ToArr(src, c->pos);
        Map *m = NewMap(true);
        Arr *groups = NewArr(8, T_GROUP);
        for (int i = 0; i < rows->n; i++) {
            Scope *r = RowOf(rows->v[i]);
            Value key = Eval(c->b, r);
            Value el = Eval(c->a, r);
            ReleaseRow(rows->v[i]);
            int at = MapFind(m, key);
            if (at < 0) {
                Value g = MakeGroup(key, NewArr(4, T_OBJECT));
                MapAdd(m, key, VInt(groups->n), false);
                ArrPush(groups, g);
                ArrPush(g.g->items, el);
            } else {
                ArrPush(groups->v[m->vals->v[at].i].g->items, el);
            }
        }
        for (int i = 0; i < groups->n; i++) {
            Arr *items = groups->v[i].g->items;
            items->et = (uint8_t)ElemType(items, T_OBJECT);
        }
        it->buf = groups;
        break;
    }
    default:
        it->in = OpenSeq(src, c->pos);
        break;
    }
    return it;
}

bool QStage::It::Next(Value &aOut)
{
    Node *c = q->c;
    for (;;) {
        Value v;
        switch (c->k) {
        case Q_FROM:
            if (q->src.t == T_NULL || q->srcIsValues) {
                // the first from (or into): values -> rows
                if (!in->Next(v)) return false;
                aOut = VRow(q->Bind(q->env, c->name, v, c->type));
                return true;
            }
            // a later from: for each row, its collection
            if (inner) {
                if (inner->Next(v)) {
                    aOut = VRow(q->Bind(row, c->name, v, c->type));
                    return true;
                }
                inner = NULL;
            }
            if (!in->Next(v)) return false;
            row = RowOf(v);
            inner = OpenSeq(Eval(c->a, row), c->pos);
            continue;
        case Q_LET:
            if (!in->Next(v)) return false;
            aOut = VRow(q->Bind(RowOf(v), c->name, Eval(c->a, RowOf(v)), NULL));
            return true;
        case Q_WHERE:
            if (!in->Next(v)) return false;
            if (!Truthy(Eval(c->a, RowOf(v)), c->pos)) {
                ReleaseRow(v);
                continue;
            }
            aOut = v;
            return true;
        case Q_JOIN:
            if (buf && bi < buf->n && !c->names) {
                aOut = VRow(q->Bind(row, c->name, buf->v[bi++], c->type));
                return true;
            }
            if (!in->Next(v)) return false;
            row = RowOf(v);
            {
                Value k = Eval(c->b, row);
                int at = MapFind(lookup, k);
                if (c->names) {
                    // join ... into g: one row, g = the matches
                    Value matches;
                    matches.t = T_ARRAY;
                    matches.a = at >= 0 ? lookup->vals->v[at].a : NewArr(0, T_OBJECT);
                    aOut = VRow(q->Bind(row, c->names[0], matches, NULL));
                    return true;
                }
                buf = at >= 0 ? lookup->vals->v[at].a : NULL;
                bi = 0;
            }
            continue;
        case Q_ORDERBY:
        case Q_GROUP:
            if (bi >= buf->n) return false;
            aOut = buf->v[bi++];
            return true;
        case Q_SELECT:
            if (!in->Next(v)) return false;
            aOut = Eval(c->a, RowOf(v));
            ReleaseRow(v);
            return true;
        default:
            return false;
        }
    }
}

Value EvalQuery(Node *qn, Scope *s)
{
    Capture(s);
    Value cur = VNull();
    for (int i = 0; i < qn->n; i++) {
        Node *c = qn->xs[i];
        if (c->k == Q_INTO) {
            // continuation: from <name> in <the values so far>
            Node *from = New<Node>();
            *from = *c;
            from->k = Q_FROM;
            from->type = NULL;
            QStage *q = RS_NEW(QStage);
            q->c = from;
            q->env = s;
            q->src = cur;
            q->srcIsValues = true;
            q->pos = c->pos;
            cur = VSeq(q);
            continue;
        }
        QStage *q = RS_NEW(QStage);
        q->c = c;
        q->env = s;
        q->src = cur;
        q->srcIsValues = false;
        q->pos = c->pos;
        cur = VSeq(q);
    }
    return cur;
}

} // namespace rs
