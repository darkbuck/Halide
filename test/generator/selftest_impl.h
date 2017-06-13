#include <sstream>
#include <tuple>

class expect {
protected:
    const bool condition;
    std::ostringstream str;

public:
    expect(bool condition) : condition(condition) {}

    template<typename T>
    expect &operator<<(T&& x) {
        if (!condition) {
            str << std::forward<T>(x);
        }
        return *this;
    }

    ~expect() {
        if (!condition) {
          if (!str.str().empty() && str.str().back() != '\n') str << '\n';
          #ifdef _halide_user_assert
          _halide_user_assert(condition) << str.str();
          #else
          halide_error(nullptr, str.str().c_str());
          #endif
        }
    }
};

class expect_eq : public expect {
public:
    template<typename T>
    expect_eq(const T &expected, const T &actual) : expect(expected == actual) {
        if (!condition) {
            str << "expect_eq(" << expected << ", " << actual << ") ";
        }
    }
};

namespace Halide {

class Testable {
public:
  template <typename... Args>
  int operator()(Args... args) {
    prepare_call();

    // Capture in a Tuple to ensure all are copied to stack
    auto all = std::make_tuple<Args...>(std::forward<Args>(args)...);
    expect_eq(num_inputs + num_outputs, std::tuple_size<decltype(all)>::value);

    build_inputs_and_outputs(all);

    return finish_call();
  }

protected:
  size_t num_inputs, num_outputs;

  Testable() = default;
  virtual ~Testable() = default;

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index < sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(std::tuple<Args...>& t) { 
      set_io(index, std::get<index>(t));
      build_inputs_and_outputs<index+1, Args...>(t);
  }

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index == sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(std::tuple<Args...>& t) { 
      // nothing
  }

  virtual void prepare_call() = 0;
  virtual int finish_call() = 0;

#define TESTABLE_BASE_TYPED_SETTER(TYPE) \
    virtual void set_io(size_t i, TYPE &value) { expect(false) << "Unimplemented setter for " #TYPE ; }

    TESTABLE_BASE_TYPED_SETTER(bool)
    TESTABLE_BASE_TYPED_SETTER(int8_t)
    TESTABLE_BASE_TYPED_SETTER(int16_t)
    TESTABLE_BASE_TYPED_SETTER(int32_t)
    TESTABLE_BASE_TYPED_SETTER(int64_t)
    TESTABLE_BASE_TYPED_SETTER(uint8_t)
    TESTABLE_BASE_TYPED_SETTER(uint16_t)
    TESTABLE_BASE_TYPED_SETTER(uint32_t)
    TESTABLE_BASE_TYPED_SETTER(uint64_t)
    TESTABLE_BASE_TYPED_SETTER(float)
    TESTABLE_BASE_TYPED_SETTER(double)
    TESTABLE_BASE_TYPED_SETTER(halide_buffer_t)

#undef TESTABLE_BASE_TYPED_SETTER

    // allow ptr->ref conversion to allow easy Halide::Runtime::Buffer usage
    void set_io(size_t i, halide_buffer_t *value) { set_io(i, *value); }

private:
    explicit Testable(const Testable &) = delete;
    void operator=(const Testable &) = delete;
};

namespace Internal {

template<typename T>
struct is_buffer {
  static constexpr bool value = std::is_convertible<typename std::decay<T>::type, halide_buffer_t>::value;
};

#ifdef _halide_user_assert
// Can only define this class if Halide.h is around
class Testable_JIT : public Testable {
  const std::string name;
  const GeneratorContext &context;
  std::vector<std::vector<StubInput>> inputs;
  std::vector<Buffer<>> outputs;
  std::unique_ptr<GeneratorBase> generator;

  template<typename T,
           typename std::enable_if<!is_buffer<T>::value>::type * = nullptr>
  void set_input(size_t i, T &arg) {
    // We must use an explicit Expr() ctor to preserve the type
    Expr e(arg);
    StubInput si(e);
    _halide_user_assert(inputs.size() == i);
    inputs.push_back({si});
  }

  template<typename T,
           typename std::enable_if<is_buffer<T>::value>::type * = nullptr>
  void set_input(size_t i, T &arg) {
    Buffer<> b(arg);
    StubInputBuffer<> sib(b);
    StubInput si(sib);
    _halide_user_assert(inputs.size() == i);
    inputs.push_back({si});
  }

  template<typename T,
           typename std::enable_if<!is_buffer<T>::value>::type * = nullptr>
  void set_output(size_t i, T &arg) {
    _halide_user_assert(false) << "set_output(" << i << ") should not be called for non-Buffers";
  }

  template<typename T,
           typename std::enable_if<is_buffer<T>::value>::type * = nullptr>
  void set_output(size_t i, T &arg) {
      Buffer<> b(arg);
      _halide_user_assert(outputs.size() == i);
      outputs.push_back(b);
  }

  template<typename T>
  void set_io_impl(size_t i, T &arg) {
    if (i < num_inputs) {
      set_input(i, arg);
      return;
    }
    i -= num_inputs;
    if (i < num_outputs) {
      set_output(i, arg);
      return;
    }
    _halide_user_assert(false) << "Bad index " << i;
  }

protected:
  void prepare_call() override {
    generator = Halide::Internal::GeneratorRegistry::create(name, context, {});
    const auto &p = generator->param_info();
    num_inputs = p.filter_inputs.size();
    num_outputs = p.filter_outputs.size();
  }

  int finish_call() override {
    generator->set_inputs_vector(inputs);
    generator->call_generate();
    generator->call_schedule();

    _halide_user_assert(outputs.size() == generator->param_info().filter_outputs.size());
    Realization r(outputs);
    generator->realize(r);

    generator.reset();
    inputs.clear();
    outputs.clear();

    return 0;
  }

    using Testable::set_io;

#define TESTABLE_BASE_TYPED_SETTER(TYPE) \
    void set_io(size_t i, TYPE &value) override { set_io_impl<TYPE>(i, value); }

    TESTABLE_BASE_TYPED_SETTER(bool)
    TESTABLE_BASE_TYPED_SETTER(int8_t)
    TESTABLE_BASE_TYPED_SETTER(int16_t)
    TESTABLE_BASE_TYPED_SETTER(int32_t)
    TESTABLE_BASE_TYPED_SETTER(int64_t)
    TESTABLE_BASE_TYPED_SETTER(uint8_t)
    TESTABLE_BASE_TYPED_SETTER(uint16_t)
    TESTABLE_BASE_TYPED_SETTER(uint32_t)
    TESTABLE_BASE_TYPED_SETTER(uint64_t)
    TESTABLE_BASE_TYPED_SETTER(float)
    TESTABLE_BASE_TYPED_SETTER(double)
    TESTABLE_BASE_TYPED_SETTER(halide_buffer_t)

#undef TESTABLE_BASE_TYPED_SETTER

public:
  Testable_JIT(const std::string &name, const GeneratorContext &context) 
    : name(name), context(context) {}

private:
    explicit Testable_JIT(const Testable_JIT &) = delete;
    void operator=(const Testable_JIT &) = delete;
};
#endif   // _halide_user_assert

class Testable_AOT : public Testable {
  using ArgvFunc = int (*)(void **);
  const ArgvFunc func;
  const struct halide_filter_metadata_t* md;
  std::vector<void*> addresses;

  template<typename T,
           typename std::enable_if<!is_buffer<T>::value>::type * = nullptr>
  static std::pair<halide_type_t, bool> type_and_isbuf(T &arg) {
    return { halide_type_of<T>(), false };
  }

  template<typename T,
           typename std::enable_if<is_buffer<T>::value>::type * = nullptr>
  static std::pair<halide_type_t, bool> type_and_isbuf(T &arg) {
    return { arg.type, true };
  }

  template<typename T>
  void set_io_impl(size_t i, T &arg) {
      auto tb = type_and_isbuf<T>(arg);
      const halide_type_t expected_type = md->arguments[i].type;
      const bool expected_is_buffer = (md->arguments[i].kind != halide_argument_kind_input_scalar);
      expect_eq(expected_type, tb.first) << "Type mismatch for argument #" << i << " " << md->arguments[i].name;
      expect_eq(expected_is_buffer, tb.second) << "IsBuffer mismatch for argument #" << i << " " << md->arguments[i].name;
      addresses[i] = (void *) &arg;
  }

protected:
    using Testable::set_io;

#define TESTABLE_BASE_TYPED_SETTER(TYPE) \
    void set_io(size_t i, TYPE &value) override { set_io_impl<TYPE>(i, value); }

    TESTABLE_BASE_TYPED_SETTER(bool)
    TESTABLE_BASE_TYPED_SETTER(int8_t)
    TESTABLE_BASE_TYPED_SETTER(int16_t)
    TESTABLE_BASE_TYPED_SETTER(int32_t)
    TESTABLE_BASE_TYPED_SETTER(int64_t)
    TESTABLE_BASE_TYPED_SETTER(uint8_t)
    TESTABLE_BASE_TYPED_SETTER(uint16_t)
    TESTABLE_BASE_TYPED_SETTER(uint32_t)
    TESTABLE_BASE_TYPED_SETTER(uint64_t)
    TESTABLE_BASE_TYPED_SETTER(float)
    TESTABLE_BASE_TYPED_SETTER(double)
    TESTABLE_BASE_TYPED_SETTER(halide_buffer_t)

#undef TESTABLE_BASE_TYPED_SETTER

  void prepare_call() override {
    num_inputs = 0;
    num_outputs = 0;
    for (int i = 0; i < md->num_arguments; ++i) {
      if (md->arguments[i].kind == halide_argument_kind_output_buffer) {
        num_outputs++;
      } else {
        expect_eq(num_outputs, (size_t) 0) << "All inputs must come before any outputs";
        num_inputs++;
      }
    }
    addresses.resize(num_inputs + num_outputs, nullptr);
  }

  int finish_call() override {
    return func(addresses.data());
  }

public:
  Testable_AOT(ArgvFunc f, const struct halide_filter_metadata_t* (*md_getter)()) : func(f), md(md_getter()) {}

private:
    explicit Testable_AOT(const Testable_AOT &) = delete;
    void operator=(const Testable_AOT &) = delete;
};

}  // namespace Internal
}  // namespace Halide

inline std::ostream &operator<<(std::ostream &o, const halide_type_t &t) {
    switch(t.code) {
    case halide_type_int:
        o << "int";
        break;
    case halide_type_uint:
        o << "uint";
        break;
    case halide_type_float:
        o << "float";
        break;
    case halide_type_handle:
        o << "handle";
        break;
    }
    o << (int) t.bits;
    if (t.lanes != 1) {
      o << "x" << (int) t.lanes;
    }
    return o;
}

void selftest_test(Halide::Testable &testable) {
  using Halide::Runtime::Buffer;
  const int kSize = 32;

  Buffer<uint8_t> input(kSize, kSize);
  input.for_each_element([&](int x, int y) {
    input(x, y) = (uint8_t) ((x + y) & 0xff);
  });

  Buffer<uint8_t> output_xor(kSize, kSize);
  Buffer<uint8_t> output_add(kSize, kSize);

  const uint8_t value = 0xA5;
  testable(input, value, output_xor, output_add);

  output_xor.for_each_element([&](int x, int y) {
      const uint8_t expected_xor = ((uint8_t) ((x + y) & 0xff)) ^ value;
      expect_eq(expected_xor, output_xor(x, y)) << "Failure @ " << x << " " << y;
  });

  output_add.for_each_element([&](int x, int y) {
      const uint8_t expected_add = ((uint8_t) ((x + y + value) & 0xff));
      expect_eq(expected_add, output_add(x, y)) << "Failure @ " << x << " " << y;
  });

  const uint8_t value2 = 0xE6;
  testable(input, value2, output_xor, output_add);

  output_xor.for_each_element([&](int x, int y) {
      const uint8_t expected_xor = ((uint8_t) ((x + y) & 0xff)) ^ value2;
      expect_eq(expected_xor, output_xor(x, y)) << "Failure @ " << x << " " << y;
  });

  output_add.for_each_element([&](int x, int y) {
      const uint8_t expected_add = ((uint8_t) ((x + y + value2) & 0xff));
      expect_eq(expected_add, output_add(x, y)) << "Failure @ " << x << " " << y;
  });
}

