/*
 * rs_int.h: rSharp interpreter internals, shared by the engine's files.
 *
 *  rs_lex.cpp    names, tokens, the arena, errors
 *  rs_parse.cpp  tokens -> syntax tree
 *  rs_eval.cpp   values, operators, conversions, statements, expressions
 *  rs_lib.cpp    the library: Math, string, Console, collections, ...
 *  rs_linq.cpp   lazy sequences: LINQ methods and query expressions
 *  rs_fmt.cpp    ToString, number formats, Dump output, rs_run()
 *
 * Errors (compile or runtime) longjmp back to rs_run(); all memory is in an
 * arena, so nothing needs unwinding.
 */
#ifndef RS_INT_H
#define RS_INT_H

#include "rs.h"
#include <setjmp.h>
#include <string.h>

// The Symbian build compiles C++ as gnu++98 (env/patch-sdk-gcce.sh), so
// the engine is C++98: no [[noreturn]], lambdas or auto.
#define RS_NORETURN __attribute__((noreturn))

namespace rs {

typedef int64_t i64;
typedef uint64_t u64;

// ---------------------------------------------------------------------------
// arena

void *Alloc(size_t aSize);                      // zeroed, 8-byte aligned
template <class T> inline T *New() { return (T *)Alloc(sizeof(T)); }
template <class T> inline T *NewArr(int aCount) { return (T *)Alloc(sizeof(T) * (aCount > 0 ? aCount : 1)); }

} // namespace rs

// Arena placement new (for classes with virtual functions).
inline void *operator new(size_t aSize, rs::i64 *, int) { return rs::Alloc(aSize); }
#define RS_NEW(T) new ((rs::i64 *)0, 0) T

namespace rs {

// ---------------------------------------------------------------------------
// names: identifiers and keywords, interned. Known names have an id.

#define RS_NAMES(X) \
    X(var) X(int) X(uint) X(long) X(ulong) X(short) X(ushort) X(byte) X(sbyte) \
    X(char) X(bool) X(float) X(double) X(decimal) X(string) X(object) X(dynamic) \
    X(void) X(true) X(false) X(null) X(new) X(if) X(else) X(while) X(do) X(for) \
    X(foreach) X(in) X(break) X(continue) X(return) X(is) X(as) X(not) X(and) \
    X(or) X(default) X(checked) X(unchecked) X(typeof) X(nameof) X(out) X(ref) \
    X(using) X(switch) X(case) X(when) X(throw) X(from) X(where) X(select) X(let) \
    X(join) X(on) X(equals) X(into) X(orderby) X(ascending) X(descending) \
    X(group) X(by) X(static) X(const) X(params) X(this) X(base) X(namespace) \
    X(class) X(struct) X(try) X(catch) X(finally) X(goto) X(lock) X(yield) \
    X(System) X(Linq) X(Collections) X(Generic) X(Text) X(Numerics) \
    X(Math) X(Console) X(Enumerable) X(Convert) X(BitOperations) X(Array) \
    X(List) X(Dictionary) X(HashSet) X(StringBuilder) X(IEnumerable) X(IList) \
    X(ICollection) X(IReadOnlyList) X(IReadOnlyCollection) X(IOrderedEnumerable) \
    X(IGrouping) X(Func) X(Action) X(Predicate) X(KeyValuePair) X(ValueTuple) X(Tuple) \
    X(Char) X(String) X(Int32) X(Int64) X(UInt32) X(UInt64) X(Int16) X(UInt16) \
    X(Byte) X(SByte) X(Double) X(Single) X(Boolean) X(Object) X(Decimal) \
    X(MaxValue) X(MinValue) X(Parse) X(TryParse) X(NaN) X(PositiveInfinity) \
    X(NegativeInfinity) X(Epsilon) X(IsNaN) X(IsInfinity) X(IsPositiveInfinity) \
    X(IsNegativeInfinity) X(IsFinite) X(PI) X(E) X(Tau) X(Abs) X(Max) X(Min) \
    X(Pow) X(Sqrt) X(Cbrt) X(Floor) X(Ceiling) X(Round) X(Truncate) X(Sign) \
    X(Sin) X(Cos) X(Tan) X(Asin) X(Acos) X(Atan) X(Atan2) X(Sinh) X(Cosh) \
    X(Tanh) X(Exp) X(Log) X(Log10) X(Log2) X(Clamp) X(DivRem) X(BigMul) \
    X(WriteLine) X(Write) X(Range) X(Repeat) X(Empty) X(ToString) \
    X(ToInt32) X(ToInt64) X(ToUInt32) X(ToUInt64) X(ToInt16) X(ToUInt16) X(ToByte) \
    X(ToSByte) X(ToDouble) X(ToSingle) X(ToChar) X(ToBoolean) X(PopCount) \
    X(LeadingZeroCount) X(TrailingZeroCount) X(RotateLeft) X(RotateRight) \
    X(IsPow2) X(Join) X(Concat) X(Format) X(IsNullOrEmpty) X(IsNullOrWhiteSpace) \
    X(Compare) X(CompareOrdinal) X(IsDigit) X(IsLetter) X(IsLetterOrDigit) \
    X(IsWhiteSpace) X(IsUpper) X(IsLower) X(IsPunctuation) X(ToUpper) X(ToLower) \
    X(Sort) X(Reverse) X(IndexOf) X(Fill) X(Create) X(Where) X(Select) \
    X(SelectMany) X(OrderBy) X(OrderByDescending) X(ThenBy) X(ThenByDescending) \
    X(Order) X(OrderDescending) X(GroupBy) X(GroupJoin) X(Distinct) X(DistinctBy) \
    X(Take) X(Skip) X(TakeWhile) X(SkipWhile) X(TakeLast) X(SkipLast) X(Zip) \
    X(Sum) X(Average) X(MinBy) X(MaxBy) X(Count) X(LongCount) X(Any) X(All) \
    X(First) X(FirstOrDefault) X(Last) X(LastOrDefault) \
    X(SingleOrDefault) X(ElementAt) X(ElementAtOrDefault) X(Contains) \
    X(Aggregate) X(ToList) X(ToArray) X(ToDictionary) X(ToHashSet) X(Union) \
    X(Intersect) X(Except) X(UnionBy) X(IntersectBy) X(ExceptBy) X(Append) \
    X(Prepend) X(Chunk) X(DefaultIfEmpty) X(SequenceEqual) X(Cast) X(OfType) \
    X(Dump) X(AsEnumerable) X(Length) X(Add) X(AddRange) X(Insert) X(Remove) \
    X(RemoveAt) X(RemoveAll) X(Clear) X(LastIndexOf) X(ForEach) X(Exists) \
    X(Find) X(FindAll) X(FindIndex) X(ConvertAll) X(TrueForAll) X(Keys) X(Values) \
    X(ContainsKey) X(ContainsValue) X(TryGetValue) X(Key) X(Value) X(Item1) \
    X(Item2) X(Item3) X(Item4) X(Item5) X(Item6) X(Item7) X(Substring) \
    X(StartsWith) X(EndsWith) X(Replace) X(Trim) X(TrimStart) X(TrimEnd) \
    X(Split) X(PadLeft) X(PadRight) X(ToCharArray) X(Equals) X(CompareTo) \
    X(AppendLine) X(UnionWith) X(IntersectWith) X(ExceptWith) X(GetType) \
    X(IsSubsetOf) X(IsSupersetOf) X(Overlaps) X(IsEven) X(IsOdd) X(Name) \
    X(Next) X(Exception) X(Message) X(StringSplitOptions) X(RemoveEmptyEntries) \
    X(TrimEntries) X(None) X(GetValueOrDefault) X(TryAdd) X(ToUpperInvariant) \
    X(ToLowerInvariant) X(GetRange) X(Capacity) X(LongLength) X(Invoke) X(SetEquals) \
    X(GetNumericValue) X(MidpointRounding) X(ToEven) X(AwayFromZero) X(ToZero) \
    X(ToNegativeInfinity) X(ToPositiveInfinity) X(IsControl) X(IsSymbol) \
    X(IsSeparator) X(Quotient) X(Remainder) X(IsEvenInteger) X(IsOddInteger) \
    X(Environment) X(NewLine) X(Resize) X(CopyTo) X(IsAscii) X(PadZero) \
    X(Hypot) X(CopySign) X(Exp2) X(MaxMagnitude) X(ScaleB) X(Index) X(CountBy)

enum TNameId
    {
    K_NONE = 0,
#define RS_X(n) K_##n,
    RS_NAMES(RS_X)
#undef RS_X
    K_COUNT
    };

struct Name
    {
    const char *s;          // UTF-8, NUL-terminated
    int len;
    int id;                 // TNameId, or K_NONE
    Name *next;             // hash chain
    };

Name *Intern(const char *aText, int aLen);
const char *NameText(int aId);

// ---------------------------------------------------------------------------
// values

enum Ty
    {
    T_NULL, T_BOOL,
    T_CHAR, T_SBYTE, T_BYTE, T_SHORT, T_USHORT, T_INT, T_UINT, T_LONG, T_ULONG,  // integral
    T_FLOAT, T_DOUBLE,
    T_STRING, T_ARRAY, T_LIST, T_SEQ, T_FUNC, T_OBJ, T_GROUP, T_DICT, T_SET,
    T_SB,                   // StringBuilder
    T_ENUM,                 // StringSplitOptions, MidpointRounding: i = type << 8 | value
    T_TYPE,                 // a type used as a value: Math, int, ... (i = TTypeId)
    T_NS,                   // a namespace: System, System.Linq, ...
    T_OBJECT,               // only as a declared type: object/var/dynamic
    T_COUNT
    };

inline bool IsIntegral(int t) { return t >= T_CHAR && t <= T_ULONG; }
inline bool IsNumeric(int t) { return t >= T_CHAR && t <= T_DOUBLE; }
inline bool IsReal(int t) { return t == T_FLOAT || t == T_DOUBLE; }
inline bool IsUnsigned(int t) { return t == T_CHAR || t == T_BYTE || t == T_USHORT || t == T_UINT || t == T_ULONG; }
int Bits(int t);                // 8, 16, 32, 64

// Types used as values (T_TYPE) that are not primitives (primitives use Ty).
enum TTypeId
    {
    TY_MATH = 100, TY_CONSOLE, TY_ENUMERABLE, TY_CONVERT, TY_BITOPS, TY_ARRAYT,
    TY_LIST, TY_DICT, TY_SET, TY_SB, TY_TUPLE, TY_KVP, TY_ENUMERABLE_T, TY_FUNC,
    TY_GROUPING, TY_EXCEPTION, TY_SPLITOPT, TY_MIDPOINT, TY_ENVIRONMENT
    };

struct Str { int n; rs_char c[1]; };
struct Value;
struct Arr { Value *v; int n, cap; uint8_t et; };      // array or List<T>; et: element Ty
struct Obj;
struct Group;
struct Map;
struct Seq;
struct Func;
struct SB { rs_char *c; int n, cap; };

struct Value
    {
    uint8_t t;              // Ty
    union
        {
        i64 i;              // bool (0/1), signed integers, T_TYPE id, T_NS level
        u64 u;              // char and unsigned integers
        double d;           // float (rounded to float) and double
        Str *s;
        Arr *a;
        Obj *o;
        Group *g;
        Map *m;
        Seq *q;
        Func *fn;
        SB *sb;
        };
    };

enum TObjKind { OK_ANON, OK_TUPLE, OK_KVP, OK_EXC };   // OK_EXC: f[0] message, names[0] type
struct Obj { uint8_t kind; int n; Name **names; Value *f; };   // names[i] NULL: ItemN
struct Group { Value key; Arr *items; };
struct Map                  // Dictionary (vals) or HashSet (vals NULL): insertion order
    {
    Arr *keys, *vals;
    int *slots;             // hash index -> position + 1; 0 empty, -1 deleted
    int cap;
    uint8_t kt, vt;         // declared key/value types
    };

// values
inline Value VNull() { Value v; v.t = T_NULL; v.i = 0; return v; }
inline Value VVoid() { Value v; v.t = T_NULL; v.i = 1; return v; }  // a void call's "value"
inline bool IsVoid(const Value &v) { return v.t == T_NULL && v.i == 1; }
inline Value VBool(bool b) { Value v; v.t = T_BOOL; v.i = b; return v; }
inline Value VInt(i64 x) { Value v; v.t = T_INT; v.i = (int32_t)x; return v; }
inline Value VLong(i64 x) { Value v; v.t = T_LONG; v.i = x; return v; }
inline Value VDouble(double x) { Value v; v.t = T_DOUBLE; v.d = x; return v; }
inline Value VChar(unsigned c) { Value v; v.t = T_CHAR; v.u = c & 0xFFFF; return v; }
Value VNum(int aTy, i64 aBits);            // integral of type aTy, wrapped
Value VReal(int aTy, double x);            // float (rounded) or double
Value VStr(Str *s);
Value VTy(int aType);
inline Value VEnum(int aType, int aVal) { Value v; v.t = T_ENUM; v.i = (aType << 8) | aVal; return v; }
inline int EnumVal(const Value &v, int aType) { return v.t == T_ENUM && (v.i >> 8) == aType ? (int)(v.i & 0xFF) : -1; }

// strings
Str *NewStr(int aLen);
Str *StrFromAscii(const char *s);
Str *StrFromUtf8(const char *s, int aLen);
Str *StrSub(const Str *s, int aFrom, int aLen);
Str *StrCat(const Str *a, const Str *b);
bool StrEq(const Str *a, const Str *b);
int StrCmpOrdinal(const Str *a, const Str *b);
int StrCmpCulture(const Str *a, const Str *b);
char *StrUtf8(const Str *s);               // arena copy, NUL-terminated
const Str *EmptyStr();

// arrays and lists
Arr *NewArr(int aCap, int aElemTy);
void ArrPush(Arr *a, Value v);
Value *ArrAt(Arr *a, i64 aIndex);          // range-checked

// maps
Map *NewMap(bool aDict);
int MapFind(Map *m, Value k);              // position or -1
int MapAdd(Map *m, Value k, Value v, bool aReplace);   // position; -1 if present and !aReplace
bool MapRemove(Map *m, Value k);

// objects
Obj *NewObj(int aKind, int aCount);
Value VTuple2(Value a, Value b);
Value VKvp(Value k, Value v);

// ---------------------------------------------------------------------------
// errors

RS_NORETURN void CompileError(int aPos, const char *aFmt, ...);
RS_NORETURN void Throw(const char *aType, const char *aFmt, ...);   // runtime exception
RS_NORETURN void ThrowAt(int aPos, const char *aType, const char *aFmt, ...);
const char *TyName(int t);                 // "int", "string", ...
const char *TooBigFor(int t);              // "an Int32" (OverflowException texts)
const char *ValTyName(const Value &v);
void Tick();                               // step counter: cancel and stack checks

// ---------------------------------------------------------------------------
// tokens

enum TTok
    {
    TK_EOF, TK_NAME, TK_NUM, TK_STR, TK_CHARLIT, TK_INTERP,
    // punctuation
    TK_LPAR, TK_RPAR, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK, TK_SEMI, TK_COMMA,
    TK_DOT, TK_QUESTION, TK_COLON, TK_ARROW,
    TK_QDOT, TK_QLBRACK, TK_QQ, TK_QQEQ, TK_RANGE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_AMP, TK_BAR, TK_CARET,
    TK_SHL, TK_SHR, TK_USHR,
    TK_EQEQ, TK_NE, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_ANDAND, TK_OROR, TK_NOT, TK_TILDE, TK_INC, TK_DEC,
    TK_EQ, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ, TK_AMPEQ,
    TK_BAREQ, TK_CARETEQ, TK_SHLEQ, TK_SHREQ, TK_USHREQ,
    TK_COUNT
    };

struct Tok
    {
    uint8_t k;
    bool verbatim;          // @name (never a keyword); @"..." / $@"..."
    int pos, end;           // source range
    Name *name;             // TK_NAME (verbatim @name: name->id is K_NONE)
    Value lit;              // TK_NUM, TK_CHARLIT, TK_STR
    };

struct Lexed { Tok *t; int n; };
Lexed Lex(const rs_char *aSrc, int aFrom, int aTo);
const char *TokText(int k);

// ---------------------------------------------------------------------------
// syntax tree

enum TNode
    {
    // expressions
    N_LIT, N_NAME, N_MEMBER, N_CALL, N_INDEX, N_UNARY, N_PREINC, N_PREDEC,
    N_POSTINC, N_POSTDEC, N_BINARY, N_ANDAND, N_OROR, N_COALESCE, N_COND,
    N_ASSIGN, N_LAMBDA, N_NEW, N_NEWARR, N_NEWANON, N_COLLECTION, N_CAST, N_IS,
    N_AS, N_DEFAULT, N_CHECKED, N_TUPLE, N_INTERP, N_HOLE, N_QUERY, N_SWITCHX,
    N_OUTARG, N_TYPEOF, N_NAMEOF, N_THROWX, N_RANGEX,
    // query clauses
    Q_FROM, Q_LET, Q_WHERE, Q_JOIN, Q_ORDERBY, Q_SELECT, Q_GROUP, Q_INTO,
    // patterns
    P_CONST, P_TYPE, P_DISCARD, P_REL, P_NOT, P_AND, P_OR, P_VAR,
    // statements
    S_BLOCK, S_VAR, S_EXPR, S_IF, S_WHILE, S_DO, S_FOR, S_FOREACH, S_BREAK,
    S_CONTINUE, S_RETURN, S_FUNC, S_EMPTY, S_SWITCH, S_THROW
    };

enum TNodeFlags
    {
    F_CONST = 1,            // constant expression (implicit narrowing allowed)
    F_RESULT = 2,           // S_EXPR: the program's last line, no ';': dump it
    F_BLOCKBODY = 4,        // lambda/function body is a block
    F_NULLCOND = 8,         // ?. or ?[
    F_DESC = 16,            // orderby key: descending
    F_OUT_DECL = 32,        // out var x
    F_ARROW = 64,           // switch section: last statement value
    F_STATICCTX = 128
    };

struct TypeRef
    {
    int id;                 // Ty (T_OBJECT for var/object/unknown) or TTypeId
    Name *name;             // as written
    TypeRef **args;
    int nargs;
    int rank;               // array dimensions ([] count)
    bool nullable;
    bool isVar;
    };

struct Node
    {
    uint8_t k;              // TNode
    uint8_t op;             // TTok of the operator; N_CHECKED: 1 checked
    uint16_t flags;
    int pos;                // source position (errors)
    Node *a, *b, *c, *d;
    Node **xs;
    int n;
    Name *name;
    Name **names;           // lambda/function parameters, anonymous members, tuple names
    TypeRef *type;
    TypeRef **targs;        // generic arguments on a member: Cast<int>
    int ntargs;
    Value lit;
    Str *fmt;               // N_HOLE format string
    };

struct Program { Node *body; };            // S_BLOCK
Program Parse(const rs_char *aSrc, int aLen);
void RunProgram(Program &p);
Node *ParseExprRange(const rs_char *aSrc, int aFrom, int aTo);  // interpolation holes

// ---------------------------------------------------------------------------
// evaluation

struct Var { Name *name; Value v; uint8_t ty; };   // ty: declared type (T_OBJECT: any)
struct Scope
    {
    Scope *up;
    Var *vars;
    int n, cap;
    bool captured;          // a closure or iterator holds it: never reused
    Var inl[4];
    };

Scope *NewScope(Scope *aUp);
void ReleaseScope(Scope *s);               // back to the free list unless captured
void Capture(Scope *s);                    // mark s and its parents captured
Var *Declare(Scope *s, Name *aName, int aTy, int aPos);
Var *Lookup(Scope *s, Name *aName);

struct Func
    {
    Node *node;             // N_LAMBDA or S_FUNC; NULL: builtin method group
    Scope *env;
    int typeId, nameId;     // builtin method group: Math.Sqrt
    TypeRef **ptypes;       // parameter types from Func<...> (else the lambda's own)
    TypeRef *ret;           // return type from Func<...>
    };

Value Eval(Node *e, Scope *s);
void SetOut(Node *aCall, int aIndex, Value v, Scope *s);   // out arguments
int TypeTy(TypeRef *t);                    // a declared type as Ty (T_OBJECT: anything)
Value CallFunc(Func *f, Value *aArgs, int aCount, int aPos);
Value Call1(const Value &f, Value a, int aPos);
Value Call2(const Value &f, Value a, Value b, int aPos);
int FuncArity(const Value &f);            // declared parameters, -1 unknown

// operators and conversions
Value Binary(int aOp, Value a, Value b, int aPos);
Value Unary(int aOp, Value a, int aPos);
Value ConvertExplicit(Value v, int aTo, int aPos);
Value ConvertImplicit(Value v, int aTo, bool aConst, int aPos);
Value DefaultOf(int aTy);
bool Truthy(Value v, int aPos);
int PromoteTypes(int a, int b);            // binary numeric promotion
Value NumTo(Value v, int aTy, bool aChecked, int aPos);   // numeric conversion
bool ValEquals(Value a, Value b);          // Equals(): structural for anonymous/tuples
bool OpEquals(Value a, Value b, int aPos); // ==
uint32_t ValHash(Value v);
int Compare(Value a, Value b, int aPos);   // Comparer<T>.Default
i64 ToI64(Value v, int aPos);              // integral value (char too)
double ToDouble(Value v);
int ToIndex(Value v, int aPos);            // int-convertible, as an index

extern bool gChecked;                      // inside checked(...)
void ResetEvalFlags();

// library (rs_lib.cpp)
Value StaticMember(int aType, Name *aName, int aPos);
Value StaticCall(int aType, Name *aName, Value *aArgs, int aCount, Node *aCall, Scope *s);
Value InstanceMember(Value aObj, Name *aName, int aPos);
Value InstanceCall(Value aObj, Name *aName, Value *aArgs, int aCount, Node *aCall, Scope *s);
Value IndexGet(Value aObj, Value *aIdx, int aCount, int aPos);
void IndexSet(Value aObj, Value *aIdx, int aCount, Value v, int aPos);
void MemberSet(Value aObj, Name *aName, Value v, int aPos);
Value NewObject(TypeRef *t, Value *aArgs, int aCount, int aPos);
int TypeIdOf(Name *aName);                 // Math -> TY_MATH, int -> T_INT, ...; -1
void Out(const Str *s);                    // Console output
void OutAscii(const char *s);

// sequences (rs_linq.cpp)
struct Iter { virtual bool Next(Value &aOut); };
struct Seq
    {
    virtual Iter *Open();
    virtual int TyHint();   // element type when known, else T_OBJECT
    virtual int Kind();     // 1: OrderBy (ThenBy may follow)
    };
Iter *OpenSeq(Value v, int aPos);          // anything enumerable
bool IsEnumerable(Value v);
Value VSeq(Seq *q);
Arr *ToArr(Value v, int aPos);             // materialise (copy)
Value Linq(Value aSrc, int aId, Value *aArgs, int aCount, Node *aCall, int aPos, bool *aFound);
Value EvalQuery(Node *q, Scope *s);
Value RangeSeq(i64 aStart, i64 aCount, int aPos);
Value RepeatSeq(Value v, i64 aCount, int aPos);
void SortValues(Value *aV, int aN, Value aCmp, int aPos);  // stable; aCmp: comparison or null

// text (rs_fmt.cpp)
Str *ToStr(Value v);                       // C# ToString()
Str *FormatValue(Value v, const Str *aFmt, int aPos);   // ToString(format)
Str *FormatString(const Str *aFmt, Value *aArgs, int aCount, int aPos);   // string.Format
Str *Align(Str *s, int aWidth);
void SbAppend(SB *b, const Str *s);
void SbAppendAscii(SB *b, const char *s);
void SbAppendChars(SB *b, const rs_char *c, int n);
void SbAppendValue(SB *b, Value v);        // ToString() of v, appended
Str *SbStr(SB *b);
void Dump(Value v, const Str *aTitle, bool aResult);
bool ParseNumber(const Str *s, int aTy, Value &aOut);

// ---------------------------------------------------------------------------
// the run's state (rs_lex.cpp)

struct Block;
struct State
    {
    jmp_buf jb;
    const rs_options *opt;
    Block *blocks;
    char *cur;
    size_t left, used, limit;
    Name *names[1024];
    int status;             // RS_*
    char msg[640];
    int errPos;
    const rs_char *src;
    int srcLen;
    unsigned steps;
    char *stackBase;
    size_t stackLimit;
    SB out;
    size_t outLimit;
    bool outFull;
    Scope *freeScopes;
    Value result;           // ret value of a `return`
    };
extern State g;

void ArenaInit();
void ArenaFree();
void NamesInit();
void LineCol(int aPos, int &aLine, int &aCol);
void StackCheck();

} // namespace rs

#endif
