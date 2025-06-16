#pragma once

namespace ox {
constexpr static auto nullopt = std::nullopt;

template <typename T>
concept is_enum_with_invalid_val = requires(T v) {
  std::is_enum_v<T>;
  T::Invalid;
};

template <typename, typename = void>
struct option_flag_val {
  constexpr static auto nullopt = ox::nullopt;
};

template <is_enum_with_invalid_val T>
struct option_flag_val<T> {
  constexpr static auto nullopt = T::Invalid;
};

template <>
struct option_flag_val<u8> {
  constexpr static auto nullopt = std::numeric_limits<u8>::max();
};

template <>
struct option_flag_val<u16> {
  constexpr static auto nullopt = std::numeric_limits<u16>::max();
};

template <>
struct option_flag_val<u32> {
  constexpr static auto nullopt = std::numeric_limits<u32>::max();
};

template <>
struct option_flag_val<usize> {
  constexpr static auto nullopt = std::numeric_limits<usize>::max();
};

template <>
struct option_flag_val<f32, std::enable_if_t<std::numeric_limits<f32>::is_iec559>> {
  constexpr static auto nullopt = static_cast<f32>(0x7fedb6db);
};

template <>
struct option_flag_val<f64, std::enable_if_t<std::numeric_limits<double>::is_iec559>> {
  constexpr static auto nullopt = static_cast<f64>(0x7ffdb6db6db6db6d);
};

template <typename T>
struct option_flag {
private:
  static_assert(!std::is_same_v<std::nullopt_t, std::remove_cvref_t<decltype(option_flag_val<T>::nullopt)>>,
                "To use option_flag, `T` must have option_flag_val defined!");

  struct Dummy {
    // Make sure it's non-trivial, we don't want to initialize value with its default value.
    constexpr Dummy() noexcept {}
  };

  union {
    Dummy dummy;
    std::remove_const_t<T> value_;
  };

public:
  constexpr option_flag() noexcept { reset(); }
  constexpr option_flag(std::nullopt_t) noexcept { reset(); }
  template <typename U = T, std::enable_if_t<std::is_copy_constructible_v<T>, int> = 0>
  constexpr option_flag(const option_flag<U>& other) {
    if (other.has_value()) {
      new (std::addressof(value_)) T(*other);
    } else {
      reset();
    }
  }
  template <typename U = T, std::enable_if_t<std::is_move_constructible_v<T>, int> = 0>
  constexpr option_flag(option_flag<U>&& other) {
    if (other.has_value()) {
      new (std::addressof(value_)) T(std::move(*other));
    } else {
      reset();
    }
  }

  template <typename U = T, std::enable_if_t<std::is_constructible_v<T, U&&>, int> = 0>
  constexpr option_flag(U&& v) noexcept(std::is_nothrow_assignable_v<T&, U> && std::is_nothrow_constructible_v<T, U>) {
    new (std::addressof(value_)) T(std::forward<U>(v));
  }

  ~option_flag() { reset(); }

  constexpr bool has_value(this const auto& self) { return self.value_ != option_flag_val<T>::nullopt; }
  constexpr void reset() noexcept { this->value_ = option_flag_val<T>::nullopt; }

  template <typename SelfT>
  [[nodiscard]]
  constexpr T&& value(this SelfT&& self) {
    if (!self.has_value()) {
      throw std::bad_optional_access();
    }

    return std::forward<option_flag<T>>(self).value_;
  }

  template <typename U, typename SelfT>
  [[nodiscard]]
  constexpr T value_or(this SelfT&& self, U&& default_value) {
    return self.has_value() ? self.value_ : static_cast<T>(std::forward<U>(default_value));
  }

  template <typename SelfT, typename FuncT>
  constexpr auto&& or_else(this SelfT&& self, FuncT&& fun) {
    if (self) {
      return self;
    }

    return std::forward<FuncT>(fun);
  }

  void swap(option_flag<T>& other) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_swappable_v<T>) {
    static_assert(std::is_move_constructible_v<T>);
    if (has_value() && other.has_value()) {
      std::swap(value_, other.value_);
    } else if (has_value() && !other.has_value()) {
      other.value_ = std::move(value_);
    } else if (!has_value() && other.has_value()) {
      value_ = std::move(other.value_);
    }
  }

  // operators
  template <typename SelfT>
  constexpr T&& operator*(this SelfT&& self) {
    if (!self.has_value()) {
      throw std::bad_optional_access();
    }

    return std::forward<option_flag<T>>(self).value_;
  }

  constexpr option_flag& operator=(std::nullopt_t) noexcept {
    reset();
    return *this;
  }

  template <typename U = T, std::enable_if_t<std::is_constructible_v<T, U&&>, int> = 0>
  option_flag& operator=(U&& _new) noexcept(std::is_nothrow_assignable_v<T&, U> &&
                                            std::is_nothrow_constructible_v<T, U>) {
    if (has_value()) {
      value_ = std::forward<U>(_new);
    } else {
      new (std::addressof(value_)) T(std::forward<U>(_new));
    }
    return *this;
  }

  template <typename U,
            std::enable_if_t<std::conjunction_v<std::is_constructible<T, const U&>, std::is_assignable<T&, const U&>>,
                             int> = 0>
  option_flag& operator=(const option_flag<U>& other) {
    if (other.has_value()) {
      if (has_value()) {
        value_ = other.value_;
      } else {
        new (std::addressof(value_)) T(other.value_);
      }
    } else {
      reset();
    }
    return *this;
  }

  template <typename U,
            std::enable_if_t<std::conjunction_v<std::is_constructible<T, U>, std::is_assignable<T&, U>>, int> = 0>
  option_flag& operator=(option_flag<U>&& other) noexcept(std::is_nothrow_assignable_v<T&, T> &&
                                                          std::is_nothrow_constructible_v<T, T>) {
    if (other.has_value()) {
      if (has_value()) {
        value_ = std::move(other.value_);
      } else {
        new (std::addressof(value_)) T(other.value_);
      }
    } else {
      reset();
    }
    return *this;
  }

  constexpr auto operator->(this auto&& self) { return std::addressof(self.value_); }
  constexpr explicit operator bool() const noexcept { return has_value(); }
};

template <typename T, typename U>
bool operator==(const option_flag<T>& opt, const U& value) {
  return opt.has_value() && (*opt) == value;
}

template <typename T, typename U>
bool operator!=(const option_flag<T>& opt, const U& value) {
  return !(opt == value);
}

template <typename T, typename U>
bool operator<(const option_flag<T>& opt, const U& value) {
  return opt.has_value() && *opt < value;
}

template <typename T, typename U>
bool operator<=(const option_flag<T>& opt, const U& value) {
  return opt.has_value() && *opt <= value;
}

template <typename T, typename U>
bool operator>(const option_flag<T>& opt, const U& value) {
  return opt.has_value() && *opt > value;
}

template <typename T, typename U>
bool operator>=(const option_flag<T>& opt, const U& value) {
  return opt.has_value() && *opt >= value;
}

template <typename T>
using option =
    std::conditional_t<!std::is_same_v<std::nullopt_t, std::remove_const_t<decltype(option_flag_val<T>::nullopt)>>,
                       option_flag<T>,
                       std::optional<T>>;

template <typename T>
using option_ref = option<std::reference_wrapper<T>>;

} // namespace ox
