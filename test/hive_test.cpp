// Original source:
// https://github.com/mattreecebentley/plf_hive/blob/7b7763f/plf_hive_test_suite.cpp

#if __cplusplus >= 201703

#include <sg14/plf_hive.h>

#include <gtest/gtest.h>

#include <algorithm>
#if __has_include(<concepts>)
#include <concepts>
#endif
#include <functional>
#include <iterator>
#include <memory>
#if __has_include(<memory_resource>)
#include <memory_resource>
#endif
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

template<class T> struct hivet : testing::Test {};

using hivet_types = testing::Types<
    plf::hive<int>
    , plf::hive<int, std::allocator<int>, plf::hive_priority::performance>
    , plf::hive<int, std::allocator<int>, plf::hive_priority::memory_use>
    , plf::hive<std::string>            // non-trivial construction/destruction
#if __cpp_lib_memory_resource >= 201603
    , plf::hive<int, std::pmr::polymorphic_allocator<int>>                            // pmr allocator
    , plf::hive<std::pmr::string, std::pmr::polymorphic_allocator<std::pmr::string>>  // uses-allocator construction
#endif
>;
TYPED_TEST_SUITE(hivet, hivet_types);

template<class> struct hivet_setup;
template<class A, class P> struct hivet_setup<plf::hive<int, A, P>> {
    static int value(int i) { return i; }
    static bool int_eq_t(int i, int v) { return v == i; }
};
template<class A, class P> struct hivet_setup<plf::hive<std::string, A, P>> {
    static std::string value(int i) { return std::to_string(i); }
    static bool int_eq_t(int i, const std::string& v) { return v == std::to_string(i); }
};
#if __cpp_lib_memory_resource >= 201603
template<class A, class P> struct hivet_setup<plf::hive<std::pmr::string, A, P>> {
    static std::pmr::string value(int i) { return std::to_string(i).c_str(); }
    static bool int_eq_t(int i, std::string_view v) { return v == std::to_string(i); }
};
#endif

#define EXPECT_INVARIANTS(h) \
    EXPECT_EQ(h.empty(), (h.size() == 0)); \
    EXPECT_EQ(h.empty(), (h.begin() == h.end())); \
    EXPECT_GE(h.max_size(), h.capacity()); \
    EXPECT_GE(h.capacity(), h.size()); \
    EXPECT_EQ(std::distance(h.begin(), h.end()), h.size()); \
    EXPECT_EQ(h.begin().distance(h.end()), h.size()); \
    EXPECT_EQ(h.begin().next(h.size()), h.end()); \
    EXPECT_EQ(h.end().prev(h.size()), h.begin());

namespace {
template<class BidirIt>
struct Adaptor {
    using value_type = typename BidirIt::value_type;
    using difference_type = typename BidirIt::difference_type;
    using iterator_category = std::random_access_iterator_tag;
    using pointer = typename BidirIt::pointer;
    using reference = typename BidirIt::reference;
    BidirIt it_;
    reference operator*() const { return *it_; }
    reference operator[](difference_type n) const { return *(*this + n); }
    auto& operator++() { ++it_; return *this; }
    auto& operator++(int) { auto copy = *this; ++it_; return copy; }
    auto& operator--() { --it_; return *this; }
    auto& operator--(int) { auto copy = *this; --it_; return copy; }
    auto& operator+=(difference_type n) { it_.advance(n); return *this; }
    auto& operator-=(difference_type n) { it_.advance(-n); return *this; }
    friend Adaptor operator+(Adaptor a, difference_type n) { a += n; return a; }
    friend Adaptor operator+(difference_type n, Adaptor a) { a += n; return a; }
    friend Adaptor operator-(Adaptor a, difference_type n) { a -= n; return a; }
    friend difference_type operator-(const Adaptor& a, const Adaptor& b) { return b.it_.distance(a.it_); }
    friend bool operator==(const Adaptor& a, const Adaptor& b) { return a.it_ == b.it_; }
    friend bool operator!=(const Adaptor& a, const Adaptor& b) { return a.it_ != b.it_; }
    friend bool operator<(const Adaptor& a, const Adaptor& b) { return a.it_ < b.it_; }
    friend bool operator>(const Adaptor& a, const Adaptor& b) { return a.it_ > b.it_; }
    friend bool operator<=(const Adaptor& a, const Adaptor& b) { return a.it_ <= b.it_; }
    friend bool operator>=(const Adaptor& a, const Adaptor& b) { return a.it_ >= b.it_; }
};
template<class BidirIt>
static Adaptor<BidirIt> adapt_iterator(BidirIt it) {
    return Adaptor<BidirIt>{it};
}
} // anonymous namespace

TYPED_TEST(hivet, BasicInsertClear)
{
    using Hive = TypeParam;

    Hive h;
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);

    auto x = hivet_setup<Hive>::value(42);
    h.insert(x);
    EXPECT_EQ(h.size(), 1u);
    EXPECT_INVARIANTS(h);

    EXPECT_EQ(*h.begin(), x);

    h.clear();
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);
}

TYPED_TEST(hivet, CustomAdvanceForward)
{
    using Hive = TypeParam;
    Hive h(400);
    EXPECT_EQ(h.size(), 400u);
    EXPECT_INVARIANTS(h);

    auto it = h.begin();
    auto jt = h.begin();
    auto kt = h.cbegin();

    std::advance(it, 20);
    jt.advance(20);
    kt.advance(20);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.begin().next(20));
    EXPECT_EQ(it, h.cbegin().next(20));

    std::advance(it, 37);
    jt.advance(37);
    kt.advance(37);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.begin().next(57));
    EXPECT_EQ(it, h.cbegin().next(57));

    std::advance(it, 101);
    jt.advance(101);
    kt.advance(101);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.begin().next(158));
    EXPECT_EQ(it, h.cbegin().next(158));

    std::advance(it, 1);
    jt.advance(1);
    kt.advance(1);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.begin().next(159));
    EXPECT_EQ(it, h.cbegin().next(159));

    std::advance(it, 400 - 159);
    jt.advance(400 - 159);
    kt.advance(400 - 159);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.begin().next(400));
    EXPECT_EQ(it, h.cbegin().next(400));
    EXPECT_EQ(it, h.end());
    EXPECT_EQ(jt, h.end());
    EXPECT_EQ(kt, h.end());
}

TYPED_TEST(hivet, CustomAdvanceBackward)
{
    using Hive = TypeParam;
    Hive h(400);
    EXPECT_EQ(h.size(), 400u);
    EXPECT_INVARIANTS(h);

    auto it = h.end();
    auto jt = h.end();
    auto kt = h.cend();

    std::advance(it, -20);
    jt.advance(-20);
    kt.advance(-20);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.end().prev(20));
    EXPECT_EQ(it, h.cend().prev(20));

    std::advance(it, -37);
    jt.advance(-37);
    kt.advance(-37);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.end().prev(57));
    EXPECT_EQ(it, h.cend().prev(57));

    std::advance(it, -101);
    jt.advance(-101);
    kt.advance(-101);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.end().prev(158));
    EXPECT_EQ(it, h.cend().prev(158));

    std::advance(it, -1);
    jt.advance(-1);
    kt.advance(-1);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.end().prev(159));
    EXPECT_EQ(it, h.cend().prev(159));

    std::advance(it, 159 - 400);
    jt.advance(159 - 400);
    kt.advance(159 - 400);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.end().prev(400));
    EXPECT_EQ(it, h.cend().prev(400));
    EXPECT_EQ(it, h.begin());
    EXPECT_EQ(jt, h.begin());
    EXPECT_EQ(kt, h.begin());
}

TYPED_TEST(hivet, CustomDistanceFunction)
{
    using Hive = TypeParam;
    Hive h(400);
    EXPECT_EQ(h.size(), 400u);
    EXPECT_INVARIANTS(h);

    auto plus20 = h.begin();
    std::advance(plus20, 20);
    auto plus200 = h.begin();
    std::advance(plus200, 200);
    EXPECT_EQ(std::distance(h.begin(), plus20), 20);
    EXPECT_EQ(std::distance(h.begin(), plus200), 200);
    EXPECT_EQ(std::distance(plus20, plus200), 180);

    EXPECT_EQ(h.begin().distance(plus20), 20);
    EXPECT_EQ(h.begin().distance(plus200), 200);
    EXPECT_EQ(plus20.distance(plus200), 180);
    EXPECT_EQ(plus20.distance(h.begin()), -20);
    EXPECT_EQ(plus200.distance(h.begin()), -200);
    EXPECT_EQ(plus200.distance(plus20), -180);
    EXPECT_EQ(plus200.distance(plus200), 0);

    // Test const iterators also
    typename Hive::const_iterator c20 = plus20;
    typename Hive::const_iterator c200 = plus200;
    EXPECT_EQ(h.cbegin().distance(c20), 20);
    EXPECT_EQ(h.cbegin().distance(c200), 200);
    EXPECT_EQ(c20.distance(c200), 180);
    EXPECT_EQ(c20.distance(h.cbegin()), -20);
    EXPECT_EQ(c200.distance(h.cbegin()), -200);
    EXPECT_EQ(c200.distance(c20), -180);
    EXPECT_EQ(c200.distance(c200), 0);
}

TYPED_TEST(hivet, CustomAdvanceForwardRev)
{
    using Hive = TypeParam;
    Hive h(400);
    EXPECT_EQ(h.size(), 400u);
    EXPECT_INVARIANTS(h);

    auto it = h.rbegin();
    auto jt = h.rbegin();
    auto kt = h.crbegin();

    std::advance(it, 20);
    jt.advance(20);
    kt.advance(20);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rbegin().next(20));
    EXPECT_EQ(it, h.crbegin().next(20));

    std::advance(it, 37);
    jt.advance(37);
    kt.advance(37);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rbegin().next(57));
    EXPECT_EQ(it, h.crbegin().next(57));

    std::advance(it, 101);
    jt.advance(101);
    kt.advance(101);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rbegin().next(158));
    EXPECT_EQ(it, h.crbegin().next(158));

    std::advance(it, 1);
    jt.advance(1);
    kt.advance(1);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rbegin().next(159));
    EXPECT_EQ(it, h.crbegin().next(159));

    std::advance(it, 400 - 159);
    jt.advance(400 - 159);
    kt.advance(400 - 159);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rbegin().next(400));
    EXPECT_EQ(it, h.crbegin().next(400));
    EXPECT_EQ(it, h.rend());
    EXPECT_EQ(jt, h.rend());
    EXPECT_EQ(kt, h.rend());
}

TYPED_TEST(hivet, CustomAdvanceBackwardRev)
{
    using Hive = TypeParam;
    Hive h(400);
    EXPECT_EQ(h.size(), 400u);
    EXPECT_INVARIANTS(h);

    auto it = h.rend();
    auto jt = h.rend();
    auto kt = h.crend();

    std::advance(it, -20);
    jt.advance(-20);
    kt.advance(-20);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rend().prev(20));
    EXPECT_EQ(it, h.crend().prev(20));

    std::advance(it, -37);
    jt.advance(-37);
    kt.advance(-37);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rend().prev(57));
    EXPECT_EQ(it, h.crend().prev(57));

    std::advance(it, -101);
    jt.advance(-101);
    kt.advance(-101);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rend().prev(158));
    EXPECT_EQ(it, h.crend().prev(158));

    std::advance(it, -1);
    jt.advance(-1);
    kt.advance(-1);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rend().prev(159));
    EXPECT_EQ(it, h.crend().prev(159));

    std::advance(it, 159 - 400);
    jt.advance(159 - 400);
    kt.advance(159 - 400);
    EXPECT_EQ(it, jt);
    EXPECT_EQ(it, kt);
    EXPECT_EQ(it, h.rend().prev(400));
    EXPECT_EQ(it, h.crend().prev(400));
    EXPECT_EQ(it, h.rbegin());
    EXPECT_EQ(jt, h.rbegin());
    EXPECT_EQ(kt, h.rbegin());
}

TYPED_TEST(hivet, CustomDistanceFunctionRev)
{
    using Hive = TypeParam;
    Hive h(400);
    EXPECT_EQ(h.size(), 400u);
    EXPECT_INVARIANTS(h);

    auto plus20 = h.rbegin();
    std::advance(plus20, 20);
    auto plus200 = h.rbegin();
    std::advance(plus200, 200);
    EXPECT_EQ(std::distance(h.rbegin(), plus20), 20);
    EXPECT_EQ(std::distance(h.rbegin(), plus200), 200);
    EXPECT_EQ(std::distance(plus20, plus200), 180);

    EXPECT_EQ(h.rbegin().distance(plus20), 20);
    EXPECT_EQ(h.rbegin().distance(plus200), 200);
    EXPECT_EQ(plus20.distance(plus200), 180);
    EXPECT_EQ(plus20.distance(h.rbegin()), -20);
    EXPECT_EQ(plus200.distance(h.rbegin()), -200);
    EXPECT_EQ(plus200.distance(plus20), -180);
    EXPECT_EQ(plus200.distance(plus200), 0);

    // Test const iterators also
    typename Hive::const_reverse_iterator c20 = plus20;
    typename Hive::const_reverse_iterator c200 = plus200;
    EXPECT_EQ(h.crbegin().distance(c20), 20);
    EXPECT_EQ(h.crbegin().distance(c200), 200);
    EXPECT_EQ(c20.distance(c200), 180);
    EXPECT_EQ(c20.distance(h.crbegin()), -20);
    EXPECT_EQ(c200.distance(h.crbegin()), -200);
    EXPECT_EQ(c200.distance(c20), -180);
    EXPECT_EQ(c200.distance(c200), 0);
}

TYPED_TEST(hivet, CopyConstructor)
{
    using Hive = TypeParam;
    Hive h(7, hivet_setup<Hive>::value(1));
    h.insert(10'000, hivet_setup<Hive>::value(2));

    Hive h2 = h;
    EXPECT_EQ(h2.size(), 10'007);
    EXPECT_INVARIANTS(h2);
    EXPECT_TRUE(std::equal(h.begin(), h.end(), h2.begin(), h2.end()));

    Hive h3(h, h.get_allocator());
    EXPECT_EQ(h3.size(), 10'007);
    EXPECT_INVARIANTS(h3);
    EXPECT_TRUE(std::equal(h.begin(), h.end(), h3.begin(), h3.end()));
}

TEST(hive, MoveConstructor)
{
    plf::hive<int> h = {1, 2, 3, 4, 5, 6, 7};
    h.insert(10'000, 42);

    plf::hive<int> copy = h;

    plf::hive<int> h2 = std::move(h);
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(h2.size(), 10'007);
    EXPECT_INVARIANTS(h2);
    EXPECT_TRUE(std::equal(copy.begin(), copy.end(), h2.begin(), h2.end()));

    h = copy;
    plf::hive<int> h3(std::move(h), copy.get_allocator());
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(h3.size(), 10'007);
    EXPECT_INVARIANTS(h3);
    EXPECT_TRUE(std::equal(copy.begin(), copy.end(), h3.begin(), h3.end()));
}

TEST(hive, ReverseIterator)
{
    plf::hive<int> h = {1, 2, 3, 4, 5};
    std::vector<int> expected = {1, 2, 3, 4, 5};
    EXPECT_TRUE(std::equal(h.begin(), h.end(), expected.begin(), expected.end()));
    EXPECT_TRUE(std::equal(h.cbegin(), h.cend(), expected.begin(), expected.end()));
    EXPECT_TRUE(std::equal(h.rbegin(), h.rend(), expected.rbegin(), expected.rend()));
    EXPECT_TRUE(std::equal(h.crbegin(), h.crend(), expected.rbegin(), expected.rend()));
}

TEST(hive, ReverseIteratorBase)
{
    plf::hive<int> h = {1, 2, 3, 4, 5};
    EXPECT_EQ(h.rend().base(), h.begin());
    EXPECT_EQ(h.crend().base(), h.cbegin());
    EXPECT_EQ(h.rbegin().base(), h.end());
    EXPECT_EQ(h.crbegin().base(), h.cend());

    auto rit = h.rbegin();
    auto crit = h.crbegin();
    static_assert(std::is_same<decltype(rit.base()), plf::hive<int>::iterator>::value, "");
    static_assert(std::is_same<decltype(crit.base()), plf::hive<int>::const_iterator>::value, "");
    static_assert(noexcept(rit.base()), "");
    static_assert(noexcept(crit.base()), "");
}

TEST(hive, ShrinkToFit)
{
    plf::hive<int> h = {1, 2, 3, 4, 5};
    size_t oldcap = h.capacity();

    h.shrink_to_fit();
    EXPECT_EQ(h.size(), 5u);
    EXPECT_LE(h.capacity(), oldcap);
    EXPECT_INVARIANTS(h);
}

TEST(hive, InsertInMovedFromContainer)
{
    plf::hive<int> h = {1, 2, 3, 4, 5};
    auto dummy = std::move(h);
    EXPECT_TRUE(h.empty());
    h.insert(42);
    EXPECT_EQ(h.size(), 1u);
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(*h.begin(), 42);
}

TEST(hive, Swap)
{
    plf::hive<int> h1 = {1, 2, 3, 4, 5};
    plf::hive<int> h2 = {3, 1, 4};

    h1.swap(h2);
    EXPECT_EQ(h1.size(), 3u);
    EXPECT_EQ(h2.size(), 5u);

    h1.assign(100'000, 42);
    h1.swap(h2);
    EXPECT_EQ(h1.size(), 5u);
    EXPECT_EQ(h2.size(), 100'000u);

    using std::swap;
    swap(h1, h2);
    EXPECT_EQ(h1.size(), 100'000u);
    EXPECT_EQ(h2.size(), 5u);
}

TEST(hive, MaxSize)
{
    plf::hive<int> h1 = {1, 2, 3, 4, 5};
    EXPECT_GE(h1.max_size(), 100'000u);
    static_assert(noexcept(h1.max_size()), "");
    static_assert(std::is_same<decltype(h1.max_size()), size_t>::value, "");
}

TEST(hive, IteratorConvertibility)
{
    using H = plf::hive<int>;
    using It = H::iterator;
    using CIt = H::const_iterator;
    using RIt = H::reverse_iterator;
    using CRIt = H::const_reverse_iterator;
    static_assert( std::is_constructible<It, It>::value, "");
    static_assert(!std::is_constructible<It, CIt>::value, "");
    static_assert(!std::is_constructible<It, RIt>::value, "");
    static_assert(!std::is_constructible<It, CRIt>::value, "");
    static_assert( std::is_constructible<CIt, It>::value, "");
    static_assert( std::is_constructible<CIt, CIt>::value, "");
    static_assert(!std::is_constructible<CIt, RIt>::value, "");
    static_assert(!std::is_constructible<CIt, CRIt>::value, "");
    static_assert( std::is_constructible<RIt, It>::value, "");
    static_assert(!std::is_constructible<RIt, CIt>::value, "");
    static_assert( std::is_constructible<RIt, RIt>::value, "");
    static_assert(!std::is_constructible<RIt, CRIt>::value, "");
    static_assert( std::is_constructible<CRIt, It>::value, "");
    static_assert( std::is_constructible<CRIt, CIt>::value, "");
    static_assert( std::is_constructible<CRIt, RIt>::value, "");
    static_assert( std::is_constructible<CRIt, CRIt>::value, "");
    static_assert( std::is_convertible<It, It>::value, "");
    static_assert( std::is_convertible<It, CIt>::value, "");
    static_assert(!std::is_convertible<It, RIt>::value, "");
    static_assert(!std::is_convertible<It, CRIt>::value, "");
    static_assert(!std::is_convertible<CIt, It>::value, "");
    static_assert( std::is_convertible<CIt, CIt>::value, "");
    static_assert(!std::is_convertible<CIt, RIt>::value, "");
    static_assert(!std::is_convertible<CIt, CRIt>::value, "");
    static_assert(!std::is_convertible<RIt, It>::value, "");
    static_assert(!std::is_convertible<RIt, CIt>::value, "");
    static_assert( std::is_convertible<RIt, RIt>::value, "");
    static_assert( std::is_convertible<RIt, CRIt>::value, "");
    static_assert(!std::is_convertible<CRIt, It>::value, "");
    static_assert(!std::is_convertible<CRIt, CIt>::value, "");
    static_assert(!std::is_convertible<CRIt, RIt>::value, "");
    static_assert( std::is_convertible<CRIt, CRIt>::value, "");
}

template<class T> using Tag = typename std::iterator_traits<T>::iterator_category;

TEST(hive, IteratorCategory)
{
    using H = plf::hive<int>;
    using It = H::iterator;
    using CIt = H::const_iterator;
    using RIt = H::reverse_iterator;
    using CRIt = H::const_reverse_iterator;

    static_assert(std::is_base_of<std::bidirectional_iterator_tag, Tag<It>>::value, "");
    static_assert(std::is_base_of<std::bidirectional_iterator_tag, Tag<CIt>>::value, "");
    static_assert(std::is_base_of<std::bidirectional_iterator_tag, Tag<RIt>>::value, "");
    static_assert(std::is_base_of<std::bidirectional_iterator_tag, Tag<CRIt>>::value, "");
#if PLF_HIVE_RANDOM_ACCESS_ITERATORS
    static_assert(std::is_base_of<std::random_access_iterator_tag, Tag<It>>::value, "");
    static_assert(std::is_base_of<std::random_access_iterator_tag, Tag<CIt>>::value, "");
    static_assert(std::is_base_of<std::random_access_iterator_tag, Tag<RIt>>::value, "");
    static_assert(std::is_base_of<std::random_access_iterator_tag, Tag<CRIt>>::value, "");
#else
    static_assert(!std::is_base_of<std::random_access_iterator_tag, Tag<It>>::value, "");
    static_assert(!std::is_base_of<std::random_access_iterator_tag, Tag<CIt>>::value, "");
    static_assert(!std::is_base_of<std::random_access_iterator_tag, Tag<RIt>>::value, "");
    static_assert(!std::is_base_of<std::random_access_iterator_tag, Tag<CRIt>>::value, "");
#endif
}

#if __cpp_lib_ranges >= 201911
TEST(hive, RangeConcepts)
{
    using H = plf::hive<int>;
    using It = H::iterator;
    using CIt = H::const_iterator;
    using RIt = H::reverse_iterator;
    using CRIt = H::const_reverse_iterator;

    static_assert(std::bidirectional_iterator<It>, "");
    static_assert(std::bidirectional_iterator<CIt>, "");
    static_assert(std::bidirectional_iterator<RIt>, "");
    static_assert(std::bidirectional_iterator<CRIt>, "");
    static_assert(std::ranges::bidirectional_range<H>, "");
#if PLF_HIVE_RANDOM_ACCESS_ITERATORS
    static_assert(std::random_access_iterator<It>, "");
    static_assert(std::random_access_iterator<CIt>, "");
    static_assert(std::random_access_iterator<RIt>, "");
    static_assert(std::random_access_iterator<CRIt>, "");
    static_assert(std::ranges::random_access_range<H>, "");
#else
    static_assert(!std::random_access_iterator<It>, "");
    static_assert(!std::random_access_iterator<CIt>, "");
    static_assert(!std::random_access_iterator<RIt>, "");
    static_assert(!std::random_access_iterator<CRIt>, "");
    static_assert(!std::ranges::random_access_range<H>, "");
#endif
    static_assert(!std::contiguous_iterator<It>, "");
    static_assert(!std::contiguous_iterator<CIt>, "");
    static_assert(!std::contiguous_iterator<RIt>, "");
    static_assert(!std::contiguous_iterator<CRIt>, "");
    static_assert(!std::ranges::contiguous_range<H>, "");

    static_assert(std::ranges::sized_range<H>, "");
    static_assert(std::ranges::sized_range<const H>, "");
    static_assert(!std::ranges::view<H>, "");
    static_assert(!std::ranges::view<const H>, "");
}
#endif

TYPED_TEST(hivet, IteratorComparison)
{
    using Hive = TypeParam;

    for (int n : {5, 30, 10'000}) {
        Hive h(n, hivet_setup<Hive>::value(42));
        typename Hive::iterator it1 = h.begin();
        typename Hive::iterator it2 = h.end();
        std::advance(it1, n / 10);
        std::advance(it2, -2);

        EXPECT_EQ((it1 < it2), true);
        EXPECT_EQ((it1 <= it2), true);
        EXPECT_EQ((it1 > it2), false);
        EXPECT_EQ((it1 >= it2), false);
        EXPECT_EQ((it1 == it2), false);
        EXPECT_EQ((it1 != it2), true);

        EXPECT_EQ((it2 < it1), false);
        EXPECT_EQ((it2 <= it1), false);
        EXPECT_EQ((it2 > it1), true);
        EXPECT_EQ((it2 >= it1), true);
        EXPECT_EQ((it2 == it1), false);
        EXPECT_EQ((it2 != it1), true);

#if __cpp_impl_three_way_comparison >= 201907
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::less);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::greater);
        it2 = it1;
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::equal);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::equal);
#endif
    }

#if __cpp_lib_concepts >= 202002
    static_assert(std::totally_ordered<typename Hive::iterator>);
#endif
#if __cpp_impl_three_way_comparison >= 201907 && __cpp_lib_concepts >= 202002
    static_assert(std::three_way_comparable<typename Hive::iterator>);
#endif
}

TYPED_TEST(hivet, ConstIteratorComparison)
{
    using Hive = TypeParam;

    for (int n : {5, 30, 10'000}) {
        Hive h(n, hivet_setup<Hive>::value(42));
        typename Hive::const_iterator it1 = h.cbegin();
        typename Hive::const_iterator it2 = h.cend();
        std::advance(it1, n / 10);
        std::advance(it2, -2);

        EXPECT_EQ((it1 < it2), true);
        EXPECT_EQ((it1 <= it2), true);
        EXPECT_EQ((it1 > it2), false);
        EXPECT_EQ((it1 >= it2), false);
        EXPECT_EQ((it1 == it2), false);
        EXPECT_EQ((it1 != it2), true);

        EXPECT_EQ((it2 < it1), false);
        EXPECT_EQ((it2 <= it1), false);
        EXPECT_EQ((it2 > it1), true);
        EXPECT_EQ((it2 >= it1), true);
        EXPECT_EQ((it2 == it1), false);
        EXPECT_EQ((it2 != it1), true);

#if __cpp_impl_three_way_comparison >= 201907
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::less);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::greater);
        it2 = it1;
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::equal);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::equal);
#endif
    }

#if __cpp_lib_concepts >= 202002
    static_assert(std::totally_ordered<typename Hive::const_iterator>);
#endif
#if __cpp_impl_three_way_comparison >= 201907 && __cpp_lib_concepts >= 202002
    static_assert(std::three_way_comparable<typename Hive::const_iterator>);
#endif
}

TYPED_TEST(hivet, MixedIteratorComparison)
{
    using Hive = TypeParam;

    for (int n : {5, 30, 10'000}) {
        Hive h(n, hivet_setup<Hive>::value(42));
        typename Hive::iterator it1 = h.begin();
        typename Hive::const_iterator it2 = h.cend();
        std::advance(it1, n / 10);
        std::advance(it2, -2);

        EXPECT_EQ((it1 < it2), true);
        EXPECT_EQ((it1 <= it2), true);
        EXPECT_EQ((it1 > it2), false);
        EXPECT_EQ((it1 >= it2), false);
        EXPECT_EQ((it1 == it2), false);
        EXPECT_EQ((it1 != it2), true);

        EXPECT_EQ((it2 < it1), false);
        EXPECT_EQ((it2 <= it1), false);
        EXPECT_EQ((it2 > it1), true);
        EXPECT_EQ((it2 >= it1), true);
        EXPECT_EQ((it2 == it1), false);
        EXPECT_EQ((it2 != it1), true);

#if __cpp_impl_three_way_comparison >= 201907
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::less);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::greater);
        it2 = it1;
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::equal);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::equal);
#endif
    }

#if __cpp_lib_concepts >= 202002
    static_assert(std::totally_ordered_with<
        typename Hive::iterator,
        typename Hive::const_iterator
    >);
#endif
#if __cpp_impl_three_way_comparison >= 201907 && __cpp_lib_concepts >= 202002
    static_assert(std::three_way_comparable_with<
        typename Hive::iterator,
        typename Hive::const_iterator
    >);
#endif
}

TYPED_TEST(hivet, ReverseIteratorComparison)
{
    using Hive = TypeParam;

    for (int n : {5, 30, 10'000}) {
        Hive h(n, hivet_setup<Hive>::value(42));
        typename Hive::reverse_iterator it1 = h.rbegin();
        typename Hive::reverse_iterator it2 = h.rend();
        std::advance(it1, n / 10);
        std::advance(it2, -2);

        EXPECT_EQ((it1 < it2), true);
        EXPECT_EQ((it1 <= it2), true);
        EXPECT_EQ((it1 > it2), false);
        EXPECT_EQ((it1 >= it2), false);
        EXPECT_EQ((it1 == it2), false);
        EXPECT_EQ((it1 != it2), true);

        EXPECT_EQ((it2 < it1), false);
        EXPECT_EQ((it2 <= it1), false);
        EXPECT_EQ((it2 > it1), true);
        EXPECT_EQ((it2 >= it1), true);
        EXPECT_EQ((it2 == it1), false);
        EXPECT_EQ((it2 != it1), true);

#if __cpp_impl_three_way_comparison >= 201907
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::less);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::greater);
        it2 = it1;
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::equal);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::equal);
#endif
    }

#if __cpp_lib_concepts >= 202002
    static_assert(std::totally_ordered<typename Hive::reverse_iterator>);
#endif
#if __cpp_impl_three_way_comparison >= 201907 && __cpp_lib_concepts >= 202002
    static_assert(std::three_way_comparable<typename Hive::reverse_iterator>);
#endif
}

TYPED_TEST(hivet, ConstReverseIteratorComparison)
{
    using Hive = TypeParam;

    for (int n : {5, 30, 10'000}) {
        Hive h(n, hivet_setup<Hive>::value(42));
        typename Hive::const_reverse_iterator it1 = h.rbegin();
        typename Hive::const_reverse_iterator it2 = h.rend();
        std::advance(it1, n / 10);
        std::advance(it2, -2);

        EXPECT_EQ((it1 < it2), true);
        EXPECT_EQ((it1 <= it2), true);
        EXPECT_EQ((it1 > it2), false);
        EXPECT_EQ((it1 >= it2), false);
        EXPECT_EQ((it1 == it2), false);
        EXPECT_EQ((it1 != it2), true);

        EXPECT_EQ((it2 < it1), false);
        EXPECT_EQ((it2 <= it1), false);
        EXPECT_EQ((it2 > it1), true);
        EXPECT_EQ((it2 >= it1), true);
        EXPECT_EQ((it2 == it1), false);
        EXPECT_EQ((it2 != it1), true);

#if __cpp_impl_three_way_comparison >= 201907
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::less);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::greater);
        it2 = it1;
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::equal);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::equal);
#endif
    }

#if __cpp_lib_concepts >= 202002
    static_assert(std::totally_ordered<typename Hive::const_reverse_iterator>);
#endif
#if __cpp_impl_three_way_comparison >= 201907 && __cpp_lib_concepts >= 202002
    static_assert(std::three_way_comparable<typename Hive::const_reverse_iterator>);
#endif
}

TYPED_TEST(hivet, MixedReverseIteratorComparison)
{
    using Hive = TypeParam;

    for (int n : {5, 30, 10'000}) {
        Hive h(n, hivet_setup<Hive>::value(42));
        typename Hive::reverse_iterator it1 = h.rbegin();
        typename Hive::const_reverse_iterator it2 = h.crend();
        std::advance(it1, n / 10);
        std::advance(it2, -2);

        EXPECT_EQ((it1 < it2), true);
        EXPECT_EQ((it1 <= it2), true);
        EXPECT_EQ((it1 > it2), false);
        EXPECT_EQ((it1 >= it2), false);
        EXPECT_EQ((it1 == it2), false);
        EXPECT_EQ((it1 != it2), true);

        EXPECT_EQ((it2 < it1), false);
        EXPECT_EQ((it2 <= it1), false);
        EXPECT_EQ((it2 > it1), true);
        EXPECT_EQ((it2 >= it1), true);
        EXPECT_EQ((it2 == it1), false);
        EXPECT_EQ((it2 != it1), true);

#if __cpp_impl_three_way_comparison >= 201907
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::less);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::greater);
        it2 = it1;
        EXPECT_EQ(it1 <=> it2, std::strong_ordering::equal);
        EXPECT_EQ(it2 <=> it1, std::strong_ordering::equal);
#endif
    }

#if __cpp_lib_concepts >= 202002
    static_assert(std::totally_ordered_with<
        typename Hive::reverse_iterator,
        typename Hive::const_reverse_iterator
    >);
#endif
#if __cpp_impl_three_way_comparison >= 201907 && __cpp_lib_concepts >= 202002
    static_assert(std::three_way_comparable_with<
        typename Hive::reverse_iterator,
        typename Hive::const_reverse_iterator
    >);
#endif
}

TEST(hive, InsertAndErase)
{
    std::mt19937 g;
    plf::hive<int> h;
    for (int i = 0; i < 500'000; ++i) {
        h.insert(i);
    }
    EXPECT_EQ(h.size(), 500'000u);
    EXPECT_INVARIANTS(h);

    if (true) {
        auto it = std::find(h.begin(), h.end(), 5000);
        EXPECT_EQ(*it, 5000);

        auto rit = std::find(h.rbegin(), h.rend(), 5000);
        EXPECT_EQ(*rit, 5000);
    }

    for (auto it = h.begin(); it != h.end(); ++it) {
        it = h.erase(it);
        ASSERT_NE(it, h.end());
    }
    EXPECT_EQ(h.size(), 250'000u);
    EXPECT_INVARIANTS(h);

    while (!h.empty()) {
        for (auto it = h.begin(); it != h.end(); ) {
            if (g() % 8 == 0) {
                it = h.erase(it);
            } else {
                ++it;
            }
        }
    }
    EXPECT_INVARIANTS(h);

    h.clear();
    h.trim();
    h.reshape(plf::hive_limits(10000, h.block_capacity_limits().max));
    h.insert(30'000, 1);
    EXPECT_EQ(h.size(), 30'000u);
    EXPECT_INVARIANTS(h);

    size_t erased_count = 0;
    while (!h.empty()) {
        for (auto it = h.begin(); it != h.end(); ) {
            if (g() % 8 == 0) {
                it = h.erase(it);
                erased_count += 1;
            } else {
                ++it;
            }
        }
    }
    EXPECT_EQ(h.size(), 30'000u - erased_count);
    EXPECT_INVARIANTS(h);

    h.insert(erased_count, 1);
    EXPECT_EQ(h.size(), 30'000u);
    EXPECT_INVARIANTS(h);

    size_t sum = 0;
    for (auto it = h.begin(); it != h.end(); ) {
        if (++sum == 3) {
            sum = 0;
            it = h.erase(it);
        } else {
            h.insert(1);  // TODO: why doesn't this invalidate `it`?
            ++it;
        }
    }
    EXPECT_EQ(h.size(), 45'001u); 
    EXPECT_INVARIANTS(h);

    while (!h.empty()) {
        for (auto it = h.begin(); it != h.end(); ) {
            if (g() % 4 == 0) {
                ++it;
                h.insert(1);
            } else {
                it = h.erase(it);
            }
        }
    }
    EXPECT_INVARIANTS(h);

    h.insert(500'000, 10);
    EXPECT_EQ(h.size(), 500'000u);
    EXPECT_INVARIANTS(h);

    if (true) {
        auto it2 = h.begin();
        std::advance(it2, 250'000);

        // Yes, this is just `h.erase(it2, h.end())`
        while (it2 != h.end()) {
            it2 = h.erase(it2);
        }
        EXPECT_EQ(h.size(), 250'000u);
        EXPECT_INVARIANTS(h);
    }

    h.insert(250'000, 10);

    if (true) {
        auto it1 = h.end();
        auto it2 = h.end();
        std::advance(it1, -250'000);
        for (int i = 0; i < 250'000; ++i) {
            --it2;
        }
        EXPECT_EQ(it1, it2);

        for (auto it = h.begin(); it != it1; ) {
            it = h.erase(it);
        }
        EXPECT_EQ(h.size(), 250'000u);
        EXPECT_INVARIANTS(h);
    }

    h.insert(250'000, 10);
    EXPECT_EQ(h.size(), 500'000u);
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(std::accumulate(h.begin(), h.end(), 0), 5'000'000);
}

TEST(hive, InsertAndErase2)
{
    plf::hive<int> h(500'000, 10);
    auto first = h.begin();
    auto last = h.end();
    std::advance(first, 300'000);
    std::advance(last, -50'001);
    for (auto it = first; it != last; ) {
        it = h.erase(it);
    }
    EXPECT_EQ(h.size(), 350'001u);
    EXPECT_INVARIANTS(h);

    h.insert(100'000, 10);

    first = h.begin();
    std::advance(first, 300'001);
    for (auto it = first; it != h.end(); ) {
        it = h.erase(it);
    }
    EXPECT_EQ(h.size(), 300'001u);
    EXPECT_INVARIANTS(h);

    if (true) {
        auto temp = h.begin();
        std::advance(temp, 20);
        EXPECT_EQ(std::distance(h.begin(), temp), 20);

        h.erase(temp);
    }

    if (true) {
        // Check edge-case with advance when erasures present in initial group
        auto temp = h.begin();
        std::advance(temp, 500);
        EXPECT_EQ(std::distance(h.begin(), temp), 500);
        ASSERT_NE(temp, h.end());

        auto temp2 = h.get_iterator(&*temp);
        auto temp3 = const_cast<const plf::hive<int>&>(h).get_iterator(&*temp);
        static_assert(std::is_same<decltype(temp2), plf::hive<int>::iterator>::value, "");
        static_assert(std::is_same<decltype(temp3), plf::hive<int>::const_iterator>::value, "");
        EXPECT_EQ(temp, temp2);
        EXPECT_EQ(temp, temp3);
    }

    for (auto it = h.begin(); it != h.end(); ) {
        it = h.erase(it);
    }
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);
}

TEST(hive, Reserve)
{
    plf::hive<int> h;
    h.reshape(plf::hive_limits(3, h.block_capacity_limits().max));

    size_t cap = h.capacity();
    h.reserve(100'000);
    EXPECT_GE(h.capacity(), 100'000);
    EXPECT_GE(h.capacity(), cap);
    EXPECT_INVARIANTS(h);
}

TEST(hive, MultipleSingleInsertErase)
{
    std::mt19937 g;
    plf::hive<int> h(110'000, 1);

    size_t count = h.size();
    for (int i = 0; i < 50'000; ++i) {
        for (int j = 0; j < 10; ++j) {
            if (g() % 8 == 0) {
                h.insert(1);
                count += 1;
            }
        }
        for (auto it = h.begin(); it != h.end(); ) {
            if (g() % 8 == 0) {
                it = h.erase(it);
                count -= 1;
            } else {
                ++it;
            }
        }
        EXPECT_EQ(h.size(), count);
        EXPECT_INVARIANTS(h);
    }
}

TEST(hive, Erase)
{
    plf::hive<int> h;
    for (int i = 0; i < 1000; ++i) {
        h.insert(i);
    }

    auto it1 = h.begin();
    auto it2 = h.begin();
    std::advance(it1, 500);
    std::advance(it2, 800);

    h.erase(it1, it2);
    EXPECT_EQ(h.size(), 700u);
    EXPECT_INVARIANTS(h);

    it1 = h.begin();
    it2 = h.begin();
    std::advance(it1, 400);
    std::advance(it2, 500);

    h.erase(it1, it2);
    EXPECT_EQ(h.size(), 600u);
    EXPECT_INVARIANTS(h);

    it1 = h.begin();
    it2 = h.begin();
    std::advance(it1, 4);
    std::advance(it2, 9);

    h.erase(it1, it2);
    EXPECT_EQ(h.size(), 595u);
    EXPECT_INVARIANTS(h);

    it1 = h.begin();
    it2 = h.begin();
    std::advance(it2, 50);

    h.erase(it1, it2);
    EXPECT_EQ(h.size(), 545u);
    EXPECT_INVARIANTS(h);

    it1 = h.begin();
    std::advance(it1, 345);

    h.erase(it1, h.end());
    EXPECT_EQ(h.size(), 345u);
    EXPECT_INVARIANTS(h);
}

TEST(hive, RangeEraseHalfErasedAlternating)
{
    plf::hive<int> v;
    for (int i = 0; i < 3000; ++i) {
        v.insert(i);
    }
    for (auto it = v.begin(); it != v.end(); ++it) {
        it = v.erase(it);
        ASSERT_NE(it, v.end());
    }
    auto it1 = v.begin();
    auto it2 = v.begin();
    std::advance(it1, 4);
    std::advance(it2, 600);

    v.erase(it1, it2);
    EXPECT_EQ(v.size(), 904u);
    EXPECT_INVARIANTS(v);
}

TEST(hive, RangeEraseThirdErasedRandomized)
{
    std::mt19937 g;
    plf::hive<int> v(3000, 42);
    for (auto it = v.begin(); it != v.end(); ) {
        if (g() % 2 == 0) {
            it = v.erase(it);
        } else {
            ++it;
        }
    }
    ASSERT_GE(v.size(), 400u);
    auto it1 = v.begin();
    std::advance(it1, 400);

    v.erase(it1, v.end());
    EXPECT_EQ(v.size(), 400u);
    EXPECT_INVARIANTS(v);
}    

TYPED_TEST(hivet, DISABLED_EraseRandomlyUntilEmpty)
{
    using Hive = TypeParam;

    std::mt19937 g;
    Hive h;
    for (int t = 0; t < 10; ++t) {
        h.clear();
        for (int i = 0; i < 1000; ++i) {
            h.insert(hivet_setup<Hive>::value(i));
        }
        for (int i = 0; i < 50 && !h.empty(); ++i) {
            auto it1 = h.begin();
            auto it2 = h.begin();
            size_t n = h.size();
            size_t offset = g() % (n + 1);
            size_t len = g() % (n + 1 - offset);
            std::advance(it1, offset);
            std::advance(it2, offset + len);
            EXPECT_EQ(it1.distance(it2), len);
            EXPECT_EQ(std::distance(it1, it2), len);
            h.erase(it1, it2);
            EXPECT_EQ(h.size(), n - len);
            EXPECT_INVARIANTS(h);

            // Test to make sure our stored erased_locations are valid
            h.insert(hivet_setup<Hive>::value(1));
            h.insert(hivet_setup<Hive>::value(10));
            EXPECT_EQ(h.size(), n - len + 2);
            EXPECT_INVARIANTS(h);
        }
        EXPECT_INVARIANTS(h);
    }
}

TYPED_TEST(hivet, DISABLED_EraseInsertRandomly)
{
    using Hive = TypeParam;

    std::mt19937 g;
    Hive h;
    for (int t = 0; t < 10; ++t) {
        h.assign(10'000, hivet_setup<Hive>::value(42));
        for (int i = 0; i < 50 && !h.empty(); ++i) {
            auto it1 = h.begin();
            auto it2 = h.begin();
            size_t n = h.size();
            size_t offset = g() % (n + 1);
            size_t len = g() % (n + 1 - offset);
            std::advance(it1, offset);
            std::advance(it2, offset + len);
            EXPECT_EQ(std::distance(it1, it2), len);
            h.erase(it1, it2);
            EXPECT_EQ(h.size(), n - len);
            EXPECT_INVARIANTS(h);

            // Test to make sure our stored erased_locations are valid & fill-insert is functioning properly in these scenarios
            size_t extra = g() % 10'000;
            h.insert(extra, hivet_setup<Hive>::value(5));
            EXPECT_EQ(h.size(), n - len + extra);
            EXPECT_INVARIANTS(h);
        }
    }
}

TYPED_TEST(hivet, EraseEmptyRange)
{
    using Hive = TypeParam;

    Hive h;
    h.erase(h.begin(), h.end());
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);

    h.insert(10, hivet_setup<Hive>::value(1));
    EXPECT_EQ(h.size(), 10u);
    EXPECT_INVARIANTS(h);

    h.erase(h.begin(), h.begin());
    h.erase(h.end(), h.end());
    EXPECT_EQ(h.size(), 10u);
    EXPECT_INVARIANTS(h);
}

TEST(hive, DISABLED_RegressionTestIssue8)
{
    plf::hive<int> h = {1,2,3,4,5};
    h.erase(h.begin());
    h.erase(h.begin());
    h.insert(6);
    assert(h.size() == 4);  // {6,3,4,5}
    EXPECT_INVARIANTS(h);

    auto it = h.begin();
    for (int i = 0; i < 4; ++i, ++it) {
        EXPECT_EQ(h.begin().distance(it), i);
        EXPECT_EQ(h.end().distance(it), i - 4);
    }
}

TEST(hive, RegressionTestIssue14)
{
    struct S {
        std::shared_ptr<int> p_;
        S(int i) : p_(std::make_shared<int>(i)) {
            if (i == 3) throw 42;
        }
    };
    static_assert(std::is_nothrow_copy_constructible<S>::value, "");

    plf::hive<S> h;
    int a[] = {1, 2, 3, 4, 5};
    ASSERT_THROW(h.assign(a, a + 5), int);
    EXPECT_INVARIANTS(h);
}

TYPED_TEST(hivet, RegressionTestIssue15)
{
    using Hive = TypeParam;
    int a[] = {1, 2, 1, 0, 2, 1, 0, 1, 2, 0};
    Hive h;
    for (int i : {1, 2, 1, 0, 2, 1, 0, 1, 2, 0}) {
        h.insert(hivet_setup<Hive>::value(i));
    }
    h.unique();
    EXPECT_INVARIANTS(h);
    EXPECT_TRUE(std::equal(a, a + 10, h.begin(), h.end(), hivet_setup<Hive>::int_eq_t));
}

TEST(hive, RegressionTestIssue16)
{
    for (int n = 0; n < 15; ++n) {
        plf::hive<char> h(plf::hive_limits(4, 4));
        h.insert(n, 'x');
        for (int i = 0; i <= n; ++i) {
            for (int j = 0; j <= n - i; ++j) {
                auto it = h.begin().next(i);
                auto jt = it.next(j);
                EXPECT_EQ(it.distance(jt), j);
                EXPECT_EQ(jt.distance(it), -j);

                auto kt = h.end().prev(i);
                auto lt = kt.prev(j);
                EXPECT_EQ(lt.distance(kt), j);
                EXPECT_EQ(kt.distance(lt), -j);
            }
        }
    }
}

TYPED_TEST(hivet, Sort)
{
    using Hive = TypeParam;

    std::mt19937 g;
    Hive h;
    for (int i = 0; i < 50'000; ++i) {
        h.insert(hivet_setup<Hive>::value(g() % 65536));
    }
    ASSERT_EQ(h.size(), 50'000u);
    ASSERT_TRUE(!std::is_sorted(h.begin(), h.end()));
    Hive h2 = h;
    h2.sort();
    EXPECT_EQ(h.size(), 50'000u);
    EXPECT_FALSE(std::is_sorted(h.begin(), h.end()));
    EXPECT_EQ(h2.size(), 50'000u);
    EXPECT_TRUE(std::is_sorted(h2.begin(), h2.end()));
    std::sort(adapt_iterator(h.begin()), adapt_iterator(h.end()));
    EXPECT_TRUE(std::equal(h2.begin(), h2.end(), h.begin(), h.end()));
    EXPECT_INVARIANTS(h);
    EXPECT_INVARIANTS(h2);
}

TYPED_TEST(hivet, SortGreater)
{
    using Hive = TypeParam;
    using Value = typename Hive::value_type;

    std::mt19937 g;
    Hive h;
    for (int i = 0; i < 50'000; ++i) {
        h.insert(hivet_setup<Hive>::value(g() % 65536));
    }
    Hive h2 = h;
    h2.sort(std::greater<Value>());
    EXPECT_EQ(h.size(), 50'000u);
    EXPECT_FALSE(std::is_sorted(h.begin(), h.end()));
    EXPECT_EQ(h2.size(), 50'000u);
    EXPECT_TRUE(std::is_sorted(h2.begin(), h2.end(), std::greater<Value>()));
    std::sort(adapt_iterator(h.begin()), adapt_iterator(h.end()), std::greater<Value>());
    EXPECT_TRUE(std::equal(h2.begin(), h2.end(), h.begin(), h.end()));
    EXPECT_INVARIANTS(h);
    EXPECT_INVARIANTS(h2);
}

TYPED_TEST(hivet, SortAndUnique)
{
    using Hive = TypeParam;
    using Value = typename Hive::value_type;

    std::mt19937 g;
    for (int n : {1, 2, 3, 10, 100, 500, 50'000}) {
        std::vector<Value> v;
        for (int i = 0; i < n; ++i) {
            v.push_back(hivet_setup<Hive>::value(g() % 65536));
        }
        auto h = Hive(v.begin(), v.end());
        h.sort();
        h.unique();
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        EXPECT_TRUE(std::is_sorted(h.begin(), h.end()));
        EXPECT_TRUE(std::equal(h.begin(), h.end(), v.begin(), v.end()));
        EXPECT_INVARIANTS(h);
    }
}

TYPED_TEST(hivet, ConstructFromInitializerList)
{
    using Hive = TypeParam;
    if (true) {
        Hive h = {
            hivet_setup<Hive>::value(1),
            hivet_setup<Hive>::value(2),
            hivet_setup<Hive>::value(3),
        };
        EXPECT_EQ(h.size(), 3u);
        EXPECT_INVARIANTS(h);
    }
    if (true) {
        Hive h = {
            hivet_setup<Hive>::value(1),
            hivet_setup<Hive>::value(2),
        };
        EXPECT_EQ(h.size(), 2u);
        EXPECT_INVARIANTS(h);
    }
    if (true) {
        Hive h = {
            hivet_setup<Hive>::value(1),
        };
        EXPECT_EQ(h.size(), 1u);
        EXPECT_INVARIANTS(h);
    }
    if (true) {
        std::initializer_list<typename Hive::value_type> il = {
            hivet_setup<Hive>::value(1),
            hivet_setup<Hive>::value(2),
        };
        Hive h = il;
        EXPECT_EQ(h.size(), 2u);
        EXPECT_INVARIANTS(h);
    }
}

TYPED_TEST(hivet, ConstructFromIteratorPair)
{
    using Hive = TypeParam;
    using V = typename Hive::value_type;
    std::vector<V> v = {
        hivet_setup<Hive>::value(1),
        hivet_setup<Hive>::value(2),
        hivet_setup<Hive>::value(3),
    };
    Hive h(v.begin(), v.end());
    EXPECT_EQ(h.size(), 3u);
    EXPECT_INVARIANTS(h);
}

TEST(hive, ConstructFromVectorBoolIteratorPair)
{
    std::vector<bool> v = { true, false, true, false, true };
    auto h = plf::hive<bool>(v.begin(), v.end());
    EXPECT_EQ(h.size(), 5u);
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(std::count(h.begin(), h.end(), true), 3);
    EXPECT_EQ(std::count(h.begin(), h.end(), false), 2);
}

#if __cpp_lib_ranges >= 201911 && __cpp_lib_ranges_to_container >= 202202
TEST(hive, ConstructFromRange)
{
    plf::hive<int> v = {1, 2, 3};
    auto r = v | std::views::take(2);
    plf::hive<int> h(std::from_range, r);
    EXPECT_EQ(h.size(), 2u);
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(*h.begin(), 1);
    EXPECT_EQ(*std::next(h.begin()), 2);
}
#endif

TYPED_TEST(hivet, InsertOverloads)
{
    using Hive = TypeParam;
    using Value = typename Hive::value_type;

    Hive h;

    // single-element insert
    Value one = hivet_setup<Hive>::value(1);
    h.insert(one);
    h.insert(hivet_setup<Hive>::value(2));

    // fill-insert
    Value three = hivet_setup<Hive>::value(3);
    h.insert(3, three);
    h.insert(4, hivet_setup<Hive>::value(4));

    // iterator-pair
    std::vector<Value> v(3, hivet_setup<Hive>::value(5));
    h.insert(v.begin(), v.end());

    // initializer_list
    std::initializer_list<Value> il = {
        hivet_setup<Hive>::value(6),
        hivet_setup<Hive>::value(7),
    };
    h.insert(il);
    h.insert({
        hivet_setup<Hive>::value(8),
        hivet_setup<Hive>::value(9),
    });

    std::vector<Value> expected = {
        hivet_setup<Hive>::value(1),
        hivet_setup<Hive>::value(2),
        hivet_setup<Hive>::value(3),
        hivet_setup<Hive>::value(3),
        hivet_setup<Hive>::value(3),
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(5),
        hivet_setup<Hive>::value(5),
        hivet_setup<Hive>::value(5),
        hivet_setup<Hive>::value(6),
        hivet_setup<Hive>::value(7),
        hivet_setup<Hive>::value(8),
        hivet_setup<Hive>::value(9),
    };
    EXPECT_TRUE(std::is_permutation(h.begin(), h.end(), expected.begin(), expected.end()));
}

#if __cpp_lib_ranges >= 201911
TYPED_TEST(hivet, InsertOverloadsForRanges)
{
    using Hive = TypeParam;
    using Value = typename Hive::value_type;

    Hive h = {
        hivet_setup<Hive>::value(0),
    };

    // iterator-sentinel pair
    std::vector<Value> v = {
        hivet_setup<Hive>::value(1),
        hivet_setup<Hive>::value(2),
        hivet_setup<Hive>::value(3),
    };
    h.insert(v.begin(), v.cend());

    // insert_range
    std::vector<Value> v = {
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(5),
        hivet_setup<Hive>::value(6),
        hivet_setup<Hive>::value(7),
    };
    h.insert_range(v);
    h.insert_range(v | std::views::take(2));

    std::vector<Value> expected = {
        hivet_setup<Hive>::value(0),
        hivet_setup<Hive>::value(1),
        hivet_setup<Hive>::value(2),
        hivet_setup<Hive>::value(3),
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(5),
        hivet_setup<Hive>::value(6),
        hivet_setup<Hive>::value(7),
        hivet_setup<Hive>::value(4),
        hivet_setup<Hive>::value(5),
    };
    EXPECT_TRUE(std::is_permutation(h.begin(), h.end(), expected.begin(), expected.end()));
}
#endif // __cpp_lib_ranges >= 201911

TEST(hive, ReserveAndFill)
{
    plf::hive<int> v;
    v.trim();
    v.reserve(50'000);
    v.insert(60'000, 1);
    EXPECT_EQ(v.size(), 60'000u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 60'000);
}

TEST(hive, ReserveAndFill2)
{
    plf::hive<int> v;
    v.reserve(50'000);
    v.insert(60, 1);
    EXPECT_EQ(v.size(), 60u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 60);

    v.insert(6000, 1);
    EXPECT_EQ(v.size(), 6060u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 6060);

    v.reserve(18'000);
    v.insert(6000, 1);
    EXPECT_EQ(v.size(), 12060u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 12060);

    v.clear();
    v.insert(6000, 2);
    EXPECT_EQ(v.size(), 6000u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 12000);
}

TEST(hive, Assign)
{
    plf::hive<int> v(50, 2);
    v.assign(50, 1);
    EXPECT_EQ(v.size(), 50u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 50);

    v.assign(10, 2);
    EXPECT_EQ(v.size(), 10u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 20);

    v.assign(2000, 20);
    EXPECT_EQ(v.size(), 2000u);
    EXPECT_INVARIANTS(v);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 40000);
}

TEST(hive, AssignFuzz)
{
    std::mt19937 g;
    plf::hive<int> v;
    for (int t = 0; t < 10; ++t) {
        size_t n = g() % 100'000;
        int x = g() % 20;
        v.assign(n, x);
        EXPECT_EQ(v.size(), n);
        EXPECT_INVARIANTS(v);
        EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0u), n * x);
    }
}

TEST(hive, RangeAssign)
{
    std::vector<int> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    plf::hive<int> h;
    h.assign(v.begin(), v.end());
    EXPECT_TRUE(std::equal(h.begin(), h.end(), v.begin(), v.end()));
    EXPECT_INVARIANTS(h);
}

TEST(hive, RangeAssignFuzz)
{
    std::mt19937 g;
    plf::hive<int> h;
    for (int t = 0; t < 10; ++t) {
        size_t n = g() % 100'000;
        int x = g() % 20;
        auto v = std::vector<int>(n, x);
        h.assign(v.begin(), v.end());
        EXPECT_EQ(h.size(), n);
        EXPECT_INVARIANTS(h);
        EXPECT_TRUE(std::equal(h.begin(), h.end(), v.begin(), v.end()));
    }
}

TEST(hive, AssignInitializerList)
{
    plf::hive<int> h;
    h.assign({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    plf::hive<int> h2 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT_INVARIANTS(h);
    EXPECT_INVARIANTS(h2);
    EXPECT_TRUE(std::equal(h.begin(), h.end(), h2.begin(), h2.end()));
}

TEST(hive, PerfectForwarding)
{
    struct S {
        bool success = false;
        explicit S(int&&, int& i) : success(true) { i = 1; }
    };

    plf::hive<S> v;
    int i = 0;
    v.emplace(7, i);
    EXPECT_EQ(v.size(), 1u);
    EXPECT_INVARIANTS(v);
    EXPECT_TRUE(v.begin()->success);
    EXPECT_EQ(i, 1);
}

TEST(hive, BasicEmplace)
{
    struct S {
        double *empty_field_1;
        double unused_number;
        unsigned int empty_field2;
        double *empty_field_3;
        int number;
        unsigned int empty_field4;

        explicit S(int n) : number(n) {}
    };

    plf::hive<S> v;
    for (int i = 0; i < 100; ++i) {
        v.emplace(i);
    }
    int total = 0;
    for (S& s : v) {
        total += s.number;
    }
    EXPECT_EQ(total, 4950);
    EXPECT_EQ(v.size(), 100u);
    EXPECT_INVARIANTS(v);
}

TEST(hive, MoveOnly)
{
    plf::hive<std::unique_ptr<int>> h;
    h.emplace(std::make_unique<int>(1));
    h.emplace(std::make_unique<int>(2));
    EXPECT_EQ(h.size(), 2u);
    EXPECT_INVARIANTS(h);
}

TEST(hive, NonCopyable)
{
    struct S {
        int m;
        explicit S(int i) : m(i) {}
        S(const S&) = delete;
        S& operator=(const S&) = delete;
    };
    plf::hive<S> h;
    h.emplace(1);
    h.emplace(2);
    EXPECT_EQ(h.size(), 2u);
    EXPECT_INVARIANTS(h);
    EXPECT_EQ(h.begin()->m, 1);
    EXPECT_EQ((++h.begin())->m, 2);
}

TEST(hive, Reshape)
{
    plf::hive<int> h;
    h.reshape(plf::hive_limits(50, 100));
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);

    h.insert(27);
    EXPECT_EQ(h.size(), 1u);
    EXPECT_EQ(h.capacity(), 50u);
    EXPECT_INVARIANTS(h);

    for (int i = 0; i < 100; ++i) {
        h.insert(i);
    }
    EXPECT_EQ(h.size(), 101u);
    EXPECT_EQ(h.capacity(), 200u);
    EXPECT_INVARIANTS(h);

    h.clear();
    h.reshape(plf::hive_limits(200, 2000));
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);

    h.insert(27);
    EXPECT_EQ(h.size(), 1u);
    EXPECT_EQ(h.capacity(), 200u);
    EXPECT_INVARIANTS(h);

    static_assert(noexcept(h.block_capacity_limits()), "");
    plf::hive_limits soft = h.block_capacity_limits();
    EXPECT_EQ(soft.min, 200u);
    EXPECT_EQ(soft.max, 2000u);

    static_assert(noexcept(decltype(h)::block_capacity_hard_limits()), "");
    plf::hive_limits hard = decltype(h)::block_capacity_hard_limits();
    EXPECT_EQ(hard.min, 3u);
    EXPECT_EQ(hard.max, 65535u);

    for (int i = 0; i < 3300; ++i) {
        h.insert(i);
    }
    EXPECT_EQ(h.size(), 3301u);
    EXPECT_EQ(h.capacity(), 5200u);
    EXPECT_INVARIANTS(h);

    h.reshape(plf::hive_limits(500, 500));
    EXPECT_EQ(h.size(), 3301u);
    EXPECT_EQ(h.capacity(), 3500u);
    EXPECT_INVARIANTS(h);

    h.reshape(plf::hive_limits(200, 200));
    EXPECT_EQ(h.size(), 3301u);
    EXPECT_EQ(h.capacity(), 3400u);
    EXPECT_INVARIANTS(h);
}

TEST(hive, Splice)
{
    std::vector<int> v1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<int> v2 = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    plf::hive<int> h1(v1.begin(), v1.end());
    plf::hive<int> h2(v2.begin(), v2.end());

    h1.splice(h2);
    v1.insert(v1.end(), v2.begin(), v2.end());
    EXPECT_TRUE(std::equal(h1.begin(), h1.end(), v1.begin(), v1.end()));
    EXPECT_TRUE(h2.empty());
    EXPECT_INVARIANTS(h1);
    EXPECT_INVARIANTS(h2);
}

TEST(hive, SpliceRvalue)
{
    std::vector<int> v1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<int> v2 = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    plf::hive<int> h1(v1.begin(), v1.end());
    plf::hive<int> h2(v2.begin(), v2.end());

    h1.splice(std::move(h2));
    v1.insert(v1.end(), v2.begin(), v2.end());
    EXPECT_TRUE(std::equal(h1.begin(), h1.end(), v1.begin(), v1.end()));
    EXPECT_TRUE(h2.empty());
    EXPECT_INVARIANTS(h1);
    EXPECT_INVARIANTS(h2);
}

TEST(hive, SpliceLargeRandom)
{
    std::mt19937 g;
    plf::hive<int> h1(100'000, 1);

    for (int t = 0; t < 10; ++t) {
        for (auto it = h1.begin(); it != h1.end(); ++it) {
            if (g() & 1) {
                it = h1.erase(it);
                if (it == h1.end()) break;
            }
        }
        EXPECT_INVARIANTS(h1);

        plf::hive<int> h2(10'000, t);
        for (auto it = h2.begin(); it != h2.end(); ++it) {
            if (g() & 1) {
                it = h2.erase(it);
                if (it == h2.end()) break;
            }
        }
        EXPECT_INVARIANTS(h2);

        auto expected = std::vector<int>(h1.begin(), h1.end());
        expected.insert(expected.end(), h2.begin(), h2.end());

        h1.splice(h2);
        EXPECT_TRUE(h2.empty());
        EXPECT_TRUE(std::is_permutation(h1.begin(), h1.end(), expected.begin(), expected.end()));
        EXPECT_INVARIANTS(h1);
        EXPECT_INVARIANTS(h2);
    }
}

TEST(hive, SpliceUnequalSize1)
{
    auto h1 = plf::hive<int>(plf::hive_limits(200, 200));
    auto h2 = plf::hive<int>(plf::hive_limits(200, 200));
    std::vector<int> expected;

    for (int i = 0; i < 150; ++i) {
        h1.insert(i);
        expected.push_back(i);
    }
    for (int i = 150; i < 250; ++i) {
        h2.insert(i);
        expected.push_back(i);
    }
    h1.splice(h2);
    EXPECT_TRUE(h2.empty());
    EXPECT_TRUE(std::is_permutation(h1.begin(), h1.end(), expected.begin(), expected.end()));
    EXPECT_INVARIANTS(h1);
    EXPECT_INVARIANTS(h2);
}

TEST(hive, SpliceUnequalSize2)
{
    auto h1 = plf::hive<int>(plf::hive_limits(200, 200));
    auto h2 = plf::hive<int>(plf::hive_limits(200, 200));
    std::vector<int> expected;

    for (int i = 0; i < 150; ++i) {
        h2.insert(i);
        expected.push_back(i);
    }
    for (int i = 150; i < 250; ++i) {
        h1.insert(i);
        expected.push_back(i);
    }
    h1.splice(h2);
    EXPECT_TRUE(h2.empty());
    EXPECT_TRUE(std::is_permutation(h1.begin(), h1.end(), expected.begin(), expected.end()));
    EXPECT_INVARIANTS(h1);
    EXPECT_INVARIANTS(h2);
}

TEST(hive, TrimDoesntMove)
{
    struct S {
        int i;
        explicit S(int i) : i(i) {}
        S(S&&) { throw 42; }
    };
    plf::hive<S> h({10, 10});
    for (int i=0; i < 100; ++i) {
        h.emplace(i);
    }
    for (auto it = h.begin(); it != h.end(); ) {
        if (it->i % 3 == 0 || (35 < it->i && it->i < 65)) {
            it = h.erase(it);
        } else {
            ++it;
        }
    }
    size_t oldcap = h.capacity();
    h.reserve(oldcap + 100);
    EXPECT_GE(h.capacity(), oldcap + 100);
    h.trim();
    EXPECT_LE(h.capacity(), oldcap);
}

TEST(hive, StdErase)
{
    std::mt19937 g;
    plf::hive<int> h1;
    for (int count = 0; count != 1000; ++count) {
        h1.insert(g() & 1);
    }
    plf::hive<int> h2 = h1;
    ASSERT_EQ(h1.size(), 1000u);

    int count0 = std::count(h1.begin(), h1.end(), 0);
    int count1 = std::count(h1.begin(), h1.end(), 1);
    ASSERT_EQ(count0 + count1, 1000);

    erase(h1, 0);
    erase(h2, 1);

    EXPECT_EQ(h1.size(), count1);
    EXPECT_INVARIANTS(h1);
    EXPECT_TRUE(std::all_of(h1.begin(), h1.end(), [](int i){ return i == 1; }));

    EXPECT_EQ(h2.size(), count0);
    EXPECT_INVARIANTS(h2);
    EXPECT_TRUE(std::all_of(h2.begin(), h2.end(), [](int i){ return i == 0; }));
}

TEST(hive, StdErase2)
{
    auto h = plf::hive<int>(100, 100);
    h.insert(100, 200);
    plf::hive<int> h2 = h;
    ASSERT_EQ(h.size(), 200u);

    erase(h, 100);
    EXPECT_EQ(std::accumulate(h.begin(), h.end(), 0), 20000);
    EXPECT_INVARIANTS(h);

    erase(h2, 200);
    EXPECT_EQ(std::accumulate(h2.begin(), h2.end(), 0), 10000);
    EXPECT_INVARIANTS(h2);

    erase(h, 200);
    EXPECT_TRUE(h.empty());
    EXPECT_INVARIANTS(h);

    erase(h2, 100);
    EXPECT_TRUE(h2.empty());
    EXPECT_INVARIANTS(h2);
}

TEST(hive, StdEraseIf)
{
    plf::hive<int> h;
    for (int count = 0; count != 1000; ++count) {
        h.insert(count);
    }
    erase_if(h, [](int i){ return i >= 500; });
    EXPECT_EQ(h.size(), 500u);
    EXPECT_INVARIANTS(h);
    EXPECT_TRUE(std::all_of(h.begin(), h.end(), [](int i){ return i < 500; }));
}

#if __cpp_lib_memory_resource >= 201603
struct PmrGuard {
    std::pmr::memory_resource *m_;
    explicit PmrGuard() : m_(std::pmr::set_default_resource(std::pmr::null_memory_resource())) {}
    ~PmrGuard() { std::pmr::set_default_resource(m_); }
};

TEST(hive, DefaultCtorDoesntAllocate)
{
    using Hive = plf::hive<int, std::pmr::polymorphic_allocator<int>>;
    PmrGuard guard;
    Hive h;  // should not allocate
}

TEST(hive, PmrCorrectness)
{
    std::pmr::monotonic_buffer_resource mr(10'000);
    int a[] = {1, 2, 3, 4};

    PmrGuard guard;

    using Hive = plf::hive<int, std::pmr::polymorphic_allocator<int>>;
    Hive h1(&mr);
    Hive h2({10, 10}, &mr);
    Hive h3(plf::hive_limits(10, 10), &mr);
    Hive h4(100, &mr);
    Hive h5(100, {10, 10}, &mr);
    Hive h6(100, plf::hive_limits(10, 10), &mr);
    Hive h7(100, 42, &mr);
    Hive h8(100, 42, {10, 10}, &mr);
    Hive h9(100, 42, plf::hive_limits(10, 10), &mr);
    Hive ha(a, a + 4, &mr);
    Hive hb({1, 2, 3, 4}, &mr);
    Hive hc({1, 2, 3, 4}, {10, 10}, &mr);
    Hive hd({1, 2, 3, 4}, plf::hive_limits(10, 10), &mr);
    Hive he(h1, &mr);
    Hive hf(Hive(&mr), &mr);

    h1.insert(100, 42);
    h2.insert(100, 42);
    h3.insert(100, 42);
    h4.insert(100, 42);
    h5.insert(100, 42);
    h6.insert(100, 42);
    h7.insert(100, 42);
    h8.insert(100, 42);
    h9.insert(100, 42);
    ha.insert(100, 42);
    hb.insert(100, 42);
    hc.insert(100, 42);
    hd.insert(100, 42);
    he.insert(100, 42);
    hf.insert(100, 42);

#if __cpp_lib_ranges >= 201911
    Hive hg(std::counted_iterator(a, 2), std::default_sentinel, &mr);
    Hive hh(std::counted_iterator(a, 2), std::default_sentinel, {10, 10}, &mr);
    Hive hi(std::counted_iterator(a, 2), std::default_sentinel, plf::hive_limits(10, 10), &mr);
    hg.insert(100, 42);
    hh.insert(100, 42);
    hi.insert(100, 42);
#endif

#if __cpp_lib_ranges >= 201911 && __cpp_lib_ranges_to_container >= 202202
    Hive hj(std::from_range, a, &mr);
    Hive hk(std::from_range, a | std::views::take(2), &mr);
    hj.insert(100, 42);
    hk.insert(100, 42);
#endif
}
#endif // __cpp_lib_memory_resource >= 201603

#endif // __cplusplus >= 201703
