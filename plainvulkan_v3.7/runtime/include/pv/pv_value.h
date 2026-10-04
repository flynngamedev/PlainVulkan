// pv_value.h -- the dynamic value type every PlainVulkan variable compiles
// to. PlainVulkan has no type annotations anywhere in its syntax (var/let/
// const declarations never name a type), so the C++ code generator targets
// this single dynamically-typed `pv::Value` class rather than trying to
// infer static C++ types. Think of it as a small, self-contained
// tagged-union value type in the spirit of a scripting language runtime
// (a bit like a minimal QVariant / JS value / Lua value).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace pv {

class Value;
using ValueArray = std::vector<Value>;
using ValueObject = std::map<std::string, Value>;

enum class ValueType { Null, Bool, Int, Float, String, Array, Object, Handle };

const char* type_name(ValueType t);

// A Handle is an opaque runtime resource id (mesh, texture, shader, sound,
// material, body, light, ...). `kind` lets debug tooling and asserts give
// a sane error ("expected a texture handle, got a mesh handle") instead of
// silently misusing a raw integer.
struct Handle {
    uint64_t id = 0;
    std::string kind;
    bool operator==(const Handle& o) const { return id == o.id && kind == o.kind; }
};

class Value {
public:
    Value() : data_(std::monostate{}) {}
    Value(std::nullptr_t) : data_(std::monostate{}) {}
    Value(bool b) : data_(b) {}
    Value(int i) : data_(static_cast<int64_t>(i)) {}
    Value(int64_t i) : data_(i) {}
    Value(size_t i) : data_(static_cast<int64_t>(i)) {}
    Value(double d) : data_(d) {}
    Value(float f) : data_(static_cast<double>(f)) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}
    Value(ValueArray arr) : data_(std::make_shared<ValueArray>(std::move(arr))) {}
    Value(Handle h) : data_(std::move(h)) {}

    // A function pointer converts implicitly to bool, so without this
    // deletion `Value(SomeFunction)` would compile and quietly store `true`.
    // Generated code emits user functions as bare C++ names, which makes
    // that an easy mistake to make (`Pv::Log(MyHandler)` instead of
    // `Pv::Log(MyHandler())`) and a very hard one to notice. pvcc rejects it
    // with a source-level diagnostic; this is the backstop that guarantees
    // it can never silently succeed even if a path there is ever missed.
    // NOTE: the type parameters are deliberately NOT short names like
    // `R`/`A`. runtime/third_party/stb_vorbis.c defines object-like macros
    // `L`, `C` and `R` and never #undefs them, and audio.cpp includes it
    // before this header -- a parameter named `R` gets macro-expanded into
    // a syntax error in exactly that one translation unit.
    template <class PvFnRet, class... PvFnArgs>
    Value(PvFnRet (*)(PvFnArgs...)) = delete;

    static Value MakeArray() { return Value(std::make_shared<ValueArray>()); }
    static Value MakeObject() { return Value(std::make_shared<ValueObject>()); }
    static Value MakeHandle(uint64_t id, std::string kind) { return Value(Handle{id, std::move(kind)}); }

    ValueType type() const;
    bool isNull() const { return type() == ValueType::Null; }
    bool isNumber() const { return type() == ValueType::Int || type() == ValueType::Float; }
    bool isArray() const { return type() == ValueType::Array; }
    bool isObject() const { return type() == ValueType::Object; }
    bool isHandle() const { return type() == ValueType::Handle; }

    bool asBool() const;
    int64_t asInt() const;
    double asFloat() const;
    std::string asString() const; // stringifies numbers/bools too, for Log()/concat convenience
    const Handle& asHandle() const;

    // Mutable access to the underlying containers. Auto-vivifies: calling
    // this on a Null value turns it into an (empty) array/object in place,
    // which is what lets `index_ref`/`member_ref` (below) support
    // `arr[i] = v` / `obj.field = v` even when `arr`/`obj` started out
    // uninitialized ("var arr;" then later indexed).
    ValueArray& arrayRef();
    ValueObject& objectRef();

    bool truthy() const;
    explicit operator bool() const { return truthy(); }

    Value operator-() const; // unary neg
    Value operator!() const; // logical not
    Value operator~() const; // bitwise not (ints only)

    bool equals(const Value& other) const;

    friend Value operator+(const Value& a, const Value& b);
    friend Value operator-(const Value& a, const Value& b);
    friend Value operator*(const Value& a, const Value& b);
    friend Value operator/(const Value& a, const Value& b);
    friend Value operator%(const Value& a, const Value& b);

    // NOTE: deliberately no operator&&/operator|| overloads. Overloaded
    // C++ operators can never short-circuit (both operands always get
    // evaluated first), so codegen emits `Value((bool)a && (bool)b)`
    // using Value's `explicit operator bool()` instead, which *does*
    // short-circuit correctly via native C++ &&/||.

    friend Value operator==(const Value& a, const Value& b);
    friend Value operator!=(const Value& a, const Value& b);
    friend Value operator<(const Value& a, const Value& b);
    friend Value operator>(const Value& a, const Value& b);
    friend Value operator<=(const Value& a, const Value& b);
    friend Value operator>=(const Value& a, const Value& b);

    friend std::ostream& operator<<(std::ostream& os, const Value& v);

    friend Value index_get(const Value& base, const Value& idx);
    friend Value member_get(const Value& base, const std::string& name);

private:
    std::variant<
        std::monostate,
        bool,
        int64_t,
        double,
        std::string,
        std::shared_ptr<ValueArray>,
        std::shared_ptr<ValueObject>,
        Handle>
        data_;

    explicit Value(std::shared_ptr<ValueArray> a) : data_(std::move(a)) {}
    explicit Value(std::shared_ptr<ValueObject> o) : data_(std::move(o)) {}
};

Value make_array(std::initializer_list<Value> elems);

// Read helpers: safe (never throw), return Null on any mismatch.
Value index_get(const Value& base, const Value& idx);
Value member_get(const Value& base, const std::string& name);

// Write/reference helpers: auto-vivify (a Null base becomes an array/
// object in place) and return a mutable reference so `a.b[0].c = v`-style
// chains compile to a single chained expression. See codegen's handling
// of `LValue` in codegen.rs for how these get chained.
Value& index_ref(Value& base, const Value& idx);
Value& member_ref(Value& base, const std::string& name);

} // namespace pv
