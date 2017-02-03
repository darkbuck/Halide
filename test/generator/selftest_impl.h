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

#ifdef _halide_user_assert
using Halide::Buffer;
#else
using Halide::Runtime::Buffer;
#endif

namespace Halide {
namespace Internal {

template<typename T>
struct is_buffer {
  static constexpr bool value = std::is_convertible<typename std::decay<T>::type, Buffer<>>::value;
};

#ifdef _halide_user_assert
class Testable_JIT {
  const std::string name;
  const Target target;
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
    auto &inputs = generator->filter_inputs;
    auto &outputs = generator->filter_outputs;
    if (i < inputs.size()) {
      set_input(i, arg);
      return;
    }
    i -= inputs.size();
    if (i < outputs.size()) {
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

public:
  Testable_JIT(const std::string &name, Target target = get_jit_target_from_environment()) 
    : name(name), target(target) {
      // nothing
  }

  template <typename... Args>
  int operator()(Args... args) {
    const std::map<std::string, std::string> generator_params = {};
    generator = Halide::Internal::GeneratorRegistry::create(name, generator_params);
    build_inputs_and_outputs(std::make_tuple<Args...>(std::forward<Args>(args)...));

    generator->target.set(target);
    generator->set_inputs(inputs);
    generator->call_generate();

    const std::map<std::string, std::string> schedule_params = {};
    const std::map<std::string, LoopLevel> schedule_params_looplevels = {};
    generator->set_schedule_param_values(schedule_params, schedule_params_looplevels);
    generator->call_schedule();

    _halide_user_assert(outputs.size() == generator->filter_outputs.size());
    Realization r(outputs);
    generator->produce_pipeline().realize(r, generator->get_target());
    return 0;
  }

private:
    explicit Testable_JIT(const Testable_JIT &) = delete;
    void operator=(const Testable_JIT &) = delete;
};
#endif   // _halide_user_assert

class Testable_AOT {
  int (*func)(void **);
  const struct halide_filter_metadata_t* md;
  std::vector<void*> addresses;

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index < sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(const std::tuple<Args...>& t) { 
      using arg_type = typename std::decay<decltype(std::get<index>(t))>::type;
      const halide_type_t type = halide_type_of<arg_type>();
      const bool is_buf = is_buffer<arg_type>::value;

      const halide_type_t expected_type = md->arguments[index].type;
      const bool expected_is_buffer = (md->arguments[index].kind != halide_argument_kind_input_scalar);
      expect_eq(expected_type, type) << "Type mismatch for argument #" << index << " " << md->arguments[index].name;
      expect_eq(expected_is_buffer, is_buf) << "IsBuffer mismatch for argument #" << index << " " << md->arguments[index].name;

      addresses[index] = (void *) &std::get<index>(t);

      build_inputs_and_outputs<index+1, Args...>(t);
  }

  template<size_t index = 0,
           typename... Args, 
           typename std::enable_if<index == sizeof...(Args)>::type * = nullptr>
  void build_inputs_and_outputs(const std::tuple<Args...>& t) { 
      // nothing
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

namespace {

template<typename T>
struct halide_type_of_helper<Buffer<T>> {
    operator halide_type_t() { return halide_type_of_helper<T>(); }
};

}  // namespace


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

