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
namespace Internal {

template<template<typename BufType = void> class Buffer, typename T>
struct is_buffer_impl {
  static constexpr bool value = std::is_convertible<typename std::decay<T>::type, Buffer<>>::value;
};

class TestableBase {
public:

protected:
  TestableBase() {}
  virtual ~TestableBase() {}

  virtual std::pair<size_t, size_t> io_count() const = 0;

private:
    explicit TestableBase(const TestableBase &) = delete;
    void operator=(const TestableBase &) = delete;
};

#ifdef _halide_user_assert
// Can only define this class if Halide.h is around
class Testable_JIT : public TestableBase {
  template<typename T = void>
  using Buffer = Halide::Buffer<T>;

  template<typename T>
  using is_buffer = is_buffer_impl<Buffer, T>;

  const std::string name;
  const GeneratorContext &context;
  std::vector<std::vector<StubInput>> inputs;
  std::vector<Buffer<>> outputs;
  std::unique_ptr<GeneratorBase> generator;

  template<typename T,
           typename std::enable_if<!is_buffer<T>::value>::type * = nullptr>
  void set_input(size_t i, T&& arg) {
    // We must use an explicit Expr() ctor to preserve the type
    Expr e(arg);
    StubInput si(e);
    _halide_user_assert(inputs.size() == i);
    inputs.push_back({si});
  }

  template<typename T,
           typename std::enable_if<is_buffer<T>::value>::type * = nullptr>
  void set_input(size_t i, T&& arg) {
    Buffer<> b = arg;
    StubInputBuffer<> sib(b);
    StubInput si(sib);
    _halide_user_assert(inputs.size() == i);
    inputs.push_back({si});
  }

  template<typename T,
           typename std::enable_if<!is_buffer<T>::value>::type * = nullptr>
  void set_output(size_t i, T&& arg) {
    _halide_user_assert(false) << "set_output(" << i << ") should not be called for non-Buffers";
  }

  template<typename T,
           typename std::enable_if<is_buffer<T>::value>::type * = nullptr>
  void set_output(size_t i, T&& arg) {
      Buffer<> b = arg;
      _halide_user_assert(outputs.size() == i);
      outputs.push_back(b);
  }

  template<typename T>
  void set_io(size_t i, T&& arg) {
    const auto counts = io_count();
    if (i < counts.first) {
      set_input(i, arg);
      return;
    }
    i -= counts.first;
    if (i < counts.second) {
      set_output(i, arg);
      return;
    }
    _halide_user_assert(false) << "Bad index " << i;
  }

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index < sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(const std::tuple<Args...>& t) { 
    set_io(index, std::get<index>(t));
    build_inputs_and_outputs<index+1, Args...>(t);
  }

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index == sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(const std::tuple<Args...>& t) { 
      // nothing
  }

protected:
  std::pair<size_t, size_t> io_count() const override {
    _halide_user_assert(generator);
    const auto &p = generator->param_info();
    return { p.filter_inputs.size(), p.filter_outputs.size() };
  }

public:
  Testable_JIT(const std::string &name, const GeneratorContext &context) 
    : name(name), context(context) {
      // nothing
  }

  template <typename... Args>
  int operator()(Args... args) {
    generator = Halide::Internal::GeneratorRegistry::create(name, context, {});
    _halide_user_assert(generator->param_info().filter_params.empty()) << "Can only test new-style Generators";
    auto all = std::make_tuple<Args...>(std::forward<Args>(args)...);
    build_inputs_and_outputs(all);

    generator->set_inputs_vector(inputs);
    generator->call_generate();
    generator->call_schedule();

    _halide_user_assert(outputs.size() == generator->param_info().filter_outputs.size());
    Realization r(outputs);
    generator->realize(r);
    return 0;
  }

private:
    explicit Testable_JIT(const Testable_JIT &) = delete;
    void operator=(const Testable_JIT &) = delete;
};
#endif   // _halide_user_assert

class Testable_AOT : public TestableBase {
  template<typename T = void>
  using Buffer = Halide::Runtime::Buffer<T>;

  template<typename T>
  using is_buffer = is_buffer_impl<Buffer, T>;

  int (*func)(void **);
  const struct halide_filter_metadata_t* md;
  std::vector<void*> addresses;

  template<typename T2,
           typename std::enable_if<!is_buffer<T2>::value>::type * = nullptr>
  static halide_type_t get_arg_type(const T2 &) {
    return halide_type_of<T2>();
  }

  template<typename T2,
           typename std::enable_if<is_buffer<T2>::value>::type * = nullptr>
  static halide_type_t get_arg_type(const T2 &a) {
    // TODO: should this be the static type?
    return a.type();
  }

  template<typename T>
  void set_io(size_t i, T&& arg) {
      using arg_type = typename std::decay<decltype(arg)>::type;
      const halide_type_t type = get_arg_type<arg_type>(arg);
      const bool is_buf = is_buffer<arg_type>::value;

      const halide_type_t expected_type = md->arguments[i].type;
      const bool expected_is_buffer = (md->arguments[i].kind != halide_argument_kind_input_scalar);
      expect_eq(expected_type, type) << "Type mismatch for argument #" << i << " " << md->arguments[i].name;
      expect_eq(expected_is_buffer, is_buf) << "IsBuffer mismatch for argument #" << i << " " << md->arguments[i].name;

      addresses[i] = const_cast<void *>((const void *) &arg);
  }

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index < sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(const std::tuple<Args...>& t) { 
      set_io(index, std::get<index>(t));
      build_inputs_and_outputs<index+1, Args...>(t);
  }

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index == sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(const std::tuple<Args...>& t) { 
      // nothing
  }

protected:
  std::pair<size_t, size_t> io_count() const override {
    size_t inputs = 0, outputs = 0;
    for (int i = 0; i < md->num_arguments; ++i) {
      if (md->arguments[i].kind == halide_argument_kind_output_buffer) {
        outputs++;
      } else {
        expect_eq(outputs, (size_t) 0) << "All inputs must come before any outputs";
        inputs++;
      }
    }
    return { inputs, outputs };
  }

public:
  Testable_AOT(int (*f)(void **), const struct halide_filter_metadata_t* (*md_getter)()) : func(f), md(md_getter()) {}

  template <typename... Args>
  int operator()(Args... args) { 
    auto all = std::make_tuple<Args...>(std::forward<Args>(args)...);
    constexpr auto tuple_size = std::tuple_size<decltype(all)>::value;
    expect_eq((size_t) md->num_arguments, tuple_size);
    addresses.resize(tuple_size, nullptr);
    build_inputs_and_outputs(all);
    return func(addresses.data());
  }

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

// TODO(srj): yuck
#ifdef _halide_user_assert
using Halide::Buffer;
#else
using Halide::Runtime::Buffer;
#endif


template<typename Testable>
void selftest_test(Testable &testable) {
  const int kSize = 32;

  Buffer<uint8_t> input(kSize, kSize);
  input.for_each_element([&](int x, int y) {
    input(x, y) = (uint8_t) ((x + y) & 0xff);
  });

  const uint8_t value = 0xA5;

  Buffer<uint8_t> output_xor(kSize, kSize);
  Buffer<uint8_t> output_add(kSize, kSize);

  testable(input, value, output_xor, output_add);

  output_xor.for_each_element([&](int x, int y) {
      const uint8_t expected_xor = ((uint8_t) ((x + y) & 0xff)) ^ value;
      expect_eq(expected_xor, output_xor(x, y)) << "Failure @ " << x << " " << y;
  });

  output_add.for_each_element([&](int x, int y) {
      const uint8_t expected_add = ((uint8_t) ((x + y + value) & 0xff));
      expect_eq(expected_add, output_add(x, y)) << "Failure @ " << x << " " << y;
  });
}

