// { dg-options "-std=c++17 -fconcepts" }

template <class T> concept bool Copyable = requires (T t) { T(t); };
template <class T> concept bool Constructable = requires { T(); };
template <class T> concept bool Both = Copyable<T> && Constructable<T>;

template <Copyable... Ts>
constexpr int f(Ts...) { return 0; } // #1

template <Both... Ts>
constexpr int f(Ts...) { return 1; }     // #2

int main()
{
  static_assert(f(42) == 1);
}
