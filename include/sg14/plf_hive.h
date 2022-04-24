// Copyright (c) 2022, Matthew Bentley (mattreecebentley@gmail.com) www.plflib.org
// Modified by Arthur O'Dwyer, 2022. Original source:
// https://github.com/mattreecebentley/plf_hive/blob/7b7763f/plf_hive.h

// zLib license (https://www.zlib.net/zlib_license.html):
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//  claim that you wrote the original software. If you use this software
//  in a product, an acknowledgement in the product documentation would be
//  appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//  misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

#ifndef PLF_HIVE_H
#define PLF_HIVE_H

#ifndef PLF_HIVE_RANDOM_ACCESS_ITERATORS
 #define PLF_HIVE_RANDOM_ACCESS_ITERATORS 0
#endif

#include <algorithm>
#if __has_include(<bit>)
#include <bit>
#endif
#include <cassert>
#if __has_include(<compare>)
#include <compare>
#endif
#if __has_include(<concepts>)
#include <concepts>
#endif
#include <cstddef>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#if __has_include(<ranges>)
#include <ranges>
#endif
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace plf {

// Polyfill std::type_identity_t
template<class T> struct hive_identity { using type = T; };
template<class T> using hive_identity_t = typename hive_identity<T>::type;

template<class R>
struct hive_txn {
    R& rollback_;
    bool done_ = false;
    explicit hive_txn(R& rollback) : rollback_(rollback) {}
    ~hive_txn() { if (!done_) rollback_(); }
};

template<class F, class R>
static inline void hive_try_rollback(F&& task, R&& rollback) {
    hive_txn<R> txn(rollback);
    task();
    txn.done_ = true;
}

struct hive_limits {
    constexpr hive_limits(size_t mn, size_t mx) noexcept : min(mn), max(mx) {}
    size_t min, max;
};

namespace hive_priority {
    struct performance {
        using skipfield_type = unsigned short;
    };
    struct memory_use {
        using skipfield_type = unsigned char;
    };
}

template <class T, class allocator_type = std::allocator<T>, class priority = plf::hive_priority::performance>
class hive {
    template<bool IsConst> class hive_iterator;
    template<bool IsConst> class hive_reverse_iterator;
    friend class hive_iterator<false>;
    friend class hive_iterator<true>;

    using skipfield_type = typename priority::skipfield_type;
    using AllocTraits = std::allocator_traits<allocator_type>;

public:
    using value_type = T;
    using aligned_T = std::aligned_storage_t<
        sizeof(T),
        (alignof(T) > 2 * sizeof(skipfield_type)) ? alignof(T) : 2 * sizeof(skipfield_type)
    >;
    using size_type = typename AllocTraits::size_type;
    using difference_type = typename AllocTraits::difference_type;
    using pointer = typename AllocTraits::pointer;
    using const_pointer = typename AllocTraits::const_pointer;
    using reference = T&;
    using const_reference = const T&;
    using iterator = hive_iterator<false>;
    using const_iterator = hive_iterator<true>;
    using reverse_iterator = hive_reverse_iterator<false>;
    using const_reverse_iterator = hive_reverse_iterator<true>;

private:
    struct alignas(alignof(aligned_T)) aligned_allocation_struct {
        char data[alignof(aligned_T)];
    };

    // Calculate the capacity of a groups' memory block when expressed in multiples of the value_type's alignment.
    // We also check to see if alignment is larger than sizeof value_type and use alignment size if so:
    static inline size_type get_aligned_block_capacity(skipfield_type elements_per_group) {
        return ((elements_per_group * (((sizeof(aligned_T) >= alignof(aligned_T)) ?
            sizeof(aligned_T) : alignof(aligned_T)) + sizeof(skipfield_type))) + sizeof(skipfield_type) + alignof(aligned_T) - 1)
            / alignof(aligned_T);
    }

    template <class D, class S>
    static constexpr D bitcast_pointer(S source_pointer) {
#if __cpp_lib_bit_cast >= 201806 && __cpp_lib_to_address >= 201711
        return std::bit_cast<D>(std::to_address(source_pointer));
#elif __cpp_lib_to_address >= 201711
        return reinterpret_cast<D>(std::to_address(source_pointer));
#else
        return reinterpret_cast<D>(source_pointer); // reject fancy pointer types
#endif
    }

    // forward declarations for typedefs below
    struct group;
    struct item_index_tuple; // for use in sort()

    typedef typename std::allocator_traits<allocator_type>::template rebind_alloc<aligned_T>     aligned_element_allocator_type;
    typedef typename std::allocator_traits<allocator_type>::template rebind_alloc<group>                     group_allocator_type;
    typedef typename std::allocator_traits<allocator_type>::template rebind_alloc<skipfield_type>            skipfield_allocator_type;
    typedef typename std::allocator_traits<allocator_type>::template rebind_alloc<aligned_allocation_struct> aligned_struct_allocator_type;
    typedef typename std::allocator_traits<allocator_type>::template rebind_alloc<item_index_tuple>          tuple_allocator_type;

    typedef typename std::allocator_traits<aligned_element_allocator_type>::pointer aligned_pointer_type; // pointer to the overaligned element type, not the original element type
    typedef typename std::allocator_traits<group_allocator_type>::pointer               group_pointer_type;
    typedef typename std::allocator_traits<skipfield_allocator_type>::pointer           skipfield_pointer_type;
    typedef typename std::allocator_traits<aligned_struct_allocator_type>::pointer  aligned_struct_pointer_type;
    typedef typename std::allocator_traits<tuple_allocator_type>::pointer               tuple_pointer_type;

    // group == element memory block + skipfield + block metadata
    struct group {
        aligned_pointer_type          last_endpoint;          // The address which is one-past the highest cell number that's been used so far in this group - does not change via erasure but may change via insertion/emplacement/assignment (if no previously-erased locations are available to insert to). This variable is necessary because an iterator cannot access the hive's end_. It is probably the most-used variable in general hive usage (being heavily used in operator ++, --), so is first in struct. If all cells in the group have been inserted into at some point, it will be == reinterpret_cast<aligned_pointer_type>(skipfield).
        group_pointer_type            next_group;             // Next group in the intrusive list of all groups. nullptr if no next group.
        const aligned_pointer_type    elements;               // Element storage.
        const skipfield_pointer_type  skipfield;              // Skipfield storage. The element and skipfield arrays are allocated contiguously, in a single allocation, in this implementation, hence the skipfield pointer also functions as a 'one-past-end' pointer for the elements array. There will always be one additional skipfield node allocated compared to the number of elements. This is to ensure a faster ++ iterator operation (fewer checks are required when this is present). The extra node is unused and always zero, but checked, and not having it will result in out-of-bounds memory errors.
        group_pointer_type            previous_group;         // Previous group in the linked list of all groups. nullptr if no preceding group.
        skipfield_type                free_list_head;         // The index of the last erased element in the group. The last erased element will, in turn, contain the number of the index of the next erased element, and so on. If this is == maximum skipfield_type value then free_list is empty ie. no erasures have occurred in the group (or if they have, the erased locations have subsequently been reused via insert/emplace/assign).
        const skipfield_type          capacity;               // The element capacity of this particular group - can also be calculated from reinterpret_cast<aligned_pointer_type>(group->skipfield) - group->elements, however this space is effectively free due to struct padding and the sizeof(skipfield_type), and calculating it once is faster in benchmarking.
        skipfield_type                size;                   // The total number of active elements in group - changes with insert and erase commands - used to check for empty group in erase function, as an indication to remove the group. Also used in combination with capacity to check if group is full, which is used in the next/previous/advance/distance overloads, and range-erase.
        group_pointer_type            erasures_list_next_group; // The next group in the singly-linked list of groups with erasures ie. with active erased-element free lists. nullptr if no next group.
        size_type                     group_number;           // Used for comparison (> < >= <= <=>) iterator operators (used by distance function and user).


        // Group elements allocation explanation: memory has to be allocated as an aligned type in order to align with memory boundaries correctly (as opposed to being allocated as char or uint_8). Unfortunately this makes combining the element memory block and the skipfield memory block into one allocation (which increases performance) a little more tricky. Specifically it means in many cases the allocation will amass more memory than is needed, particularly if the element type is large.

        explicit group(aligned_struct_allocator_type aa, skipfield_type elements_per_group, group_pointer_type previous):
            last_endpoint(bitcast_pointer<aligned_pointer_type>(
                std::allocator_traits<aligned_struct_allocator_type>::allocate(aa, get_aligned_block_capacity(elements_per_group), (previous == nullptr) ? 0 : previous->elements))),
 /* Because this variable occurs first in the struct, we allocate here initially, then increment its value in the element initialisation below. As opposed to doing a secondary assignment in the code */
            next_group(nullptr),
            elements(last_endpoint++), // we increment here because in 99% of cases, a group allocation occurs because of an insertion, so this saves a ++ call later
            skipfield(bitcast_pointer<skipfield_pointer_type>(elements + elements_per_group)),
            previous_group(previous),
            free_list_head(std::numeric_limits<skipfield_type>::max()),
            capacity(elements_per_group),
            size(1),
            erasures_list_next_group(nullptr),
            group_number((previous == nullptr) ? 0 : previous->group_number + 1u)
        {
            // Static casts to unsigned int from short not necessary as C++ automatically promotes lesser types for arithmetic purposes.
            std::memset(bitcast_pointer<void *>(skipfield), 0, sizeof(skipfield_type) * (static_cast<size_type>(elements_per_group) + 1u));
        }

        void reset(skipfield_type increment, group_pointer_type next, group_pointer_type previous, size_type group_num) {
            last_endpoint = elements + increment;
            next_group = next;
            free_list_head = std::numeric_limits<skipfield_type>::max();
            previous_group = previous;
            size = increment;
            erasures_list_next_group = nullptr;
            group_number = group_num;

            std::memset(bitcast_pointer<void *>(skipfield), 0, sizeof(skipfield_type) * static_cast<size_type>(capacity));
            // capacity + 1 is not necessary here as the end skipfield is never written to after initialization
        }
    };

    template <bool IsConst>
    class hive_iterator {
        group_pointer_type group_pointer = group_pointer_type();
        aligned_pointer_type element_pointer = aligned_pointer_type();
        skipfield_pointer_type skipfield_pointer = skipfield_pointer_type();

    public:
#if PLF_HIVE_RANDOM_ACCESS_ITERATORS
        using iterator_category = std::random_access_iterator_tag;
#else
        using iterator_category = std::bidirectional_iterator_tag;
#endif
        using value_type = typename hive::value_type;
        using difference_type = typename hive::difference_type;
        using pointer = std::conditional_t<IsConst, typename hive::const_pointer, typename hive::pointer>;
        using reference = std::conditional_t<IsConst, typename hive::const_reference, typename hive::reference>;

        friend class hive;
        friend class hive_reverse_iterator<false>;
        friend class hive_reverse_iterator<true>;

        explicit hive_iterator() = default;
        hive_iterator(hive_iterator&&) = default;
        hive_iterator(const hive_iterator&) = default;
        hive_iterator& operator=(hive_iterator&&) = default;
        hive_iterator& operator=(const hive_iterator&) = default;

        template<bool IsConst_ = IsConst, class = std::enable_if_t<IsConst_>>
        hive_iterator(const hive_iterator<false>& rhs) :
            group_pointer(rhs.group_pointer),
            element_pointer(rhs.element_pointer),
            skipfield_pointer(rhs.skipfield_pointer)
        {}

        template<bool IsConst_ = IsConst, class = std::enable_if_t<IsConst_>>
        hive_iterator(hive_iterator<false>&& rhs) :
            group_pointer(std::move(rhs.group_pointer)),
            element_pointer(std::move(rhs.element_pointer)),
            skipfield_pointer(std::move(rhs.skipfield_pointer))
        {}

        friend void swap(hive_iterator& a, hive_iterator& b) noexcept {
            using std::swap;
            swap(a.group_pointer, b.group_pointer);
            swap(a.element_pointer, b.element_pointer);
            swap(a.skipfield_pointer, b.skipfield_pointer);
        }

    private:
        template<bool IsConst_ = IsConst, class = std::enable_if_t<IsConst_>>
        hive_iterator<false> unconst() const {
            hive_iterator<false> it;
            it.group_pointer = group_pointer;
            it.element_pointer = element_pointer;
            it.skipfield_pointer = skipfield_pointer;
            return it;
        }

    public:
        friend bool operator==(const hive_iterator& a, const hive_iterator& b) {
            return a.element_pointer == b.element_pointer;
        }

#if __cpp_impl_three_way_comparison >= 201907
        friend std::strong_ordering operator<=>(const hive_iterator& a, const hive_iterator& b) {
            // TODO: what about fancy pointer types that don't support <=> natively?
            return a.group_pointer == b.group_pointer ?
                a.element_pointer <=> b.element_pointer :
                a.group_pointer->group_number <=> b.group_pointer->group_number;
        }
#else
        friend bool operator!=(const hive_iterator& a, const hive_iterator& b) {
            return a.element_pointer != b.element_pointer;
        }

        friend bool operator<(const hive_iterator& a, const hive_iterator& b) {
            return a.group_pointer == b.group_pointer ?
                a.element_pointer < b.element_pointer :
                a.group_pointer->group_number < b.group_pointer->group_number;
        }

        friend bool operator>(const hive_iterator& a, const hive_iterator& b) {
            return a.group_pointer == b.group_pointer ?
                a.element_pointer > b.element_pointer :
                a.group_pointer->group_number > b.group_pointer->group_number;
        }

        friend bool operator<=(const hive_iterator& a, const hive_iterator& b) {
            return a.group_pointer == b.group_pointer ?
                a.element_pointer <= b.element_pointer :
                a.group_pointer->group_number < b.group_pointer->group_number;
        }

        friend bool operator>=(const hive_iterator& a, const hive_iterator& b) {
            return a.group_pointer == b.group_pointer ?
                a.element_pointer >= b.element_pointer :
                a.group_pointer->group_number > b.group_pointer->group_number;
        }
#endif

        inline reference operator*() const noexcept {
            return *bitcast_pointer<pointer>(element_pointer);
        }

        inline pointer operator->() const noexcept {
            return bitcast_pointer<pointer>(element_pointer);
        }

        hive_iterator& operator++() {
            assert(group_pointer != nullptr);
            skipfield_type skip = *(++skipfield_pointer);
            element_pointer += static_cast<size_type>(skip) + 1u;
            if (element_pointer == group_pointer->last_endpoint && group_pointer->next_group != nullptr) {
                group_pointer = group_pointer->next_group;
                const aligned_pointer_type elements = group_pointer->elements;
                const skipfield_pointer_type skipfield = group_pointer->skipfield;
                skip = *skipfield;
                element_pointer = elements + skip;
                skipfield_pointer = skipfield;
            }
            skipfield_pointer += skip;
            return *this;
        }

        hive_iterator& operator--() {
            assert(group_pointer != nullptr);
            if (element_pointer != group_pointer->elements) {
                // ie. not already at beginning of group
                const skipfield_type skip = *(--skipfield_pointer);
                skipfield_pointer -= skip;
                if ((element_pointer -= static_cast<size_type>(skip) + 1u) != group_pointer->elements - 1) {
                    // ie. iterator was not already at beginning of hive (with some previous consecutive deleted elements), and skipfield does not takes us into the previous group)
                    return *this;
                }
            }
            group_pointer = group_pointer->previous_group;
            const skipfield_pointer_type skipfield = group_pointer->skipfield + group_pointer->capacity - 1;
            const skipfield_type skip = *skipfield;
            element_pointer = (bitcast_pointer<hive::aligned_pointer_type>(group_pointer->skipfield) - 1) - skip;
            skipfield_pointer = skipfield - skip;
            return *this;
        }

        inline hive_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
        inline hive_iterator operator--(int) { auto copy = *this; --*this; return copy; }

    private:
        explicit hive_iterator(group_pointer_type g, aligned_pointer_type e, skipfield_pointer_type s) :
            group_pointer(std::move(g)), element_pointer(std::move(e)), skipfield_pointer(std::move(s)) {}

        void advance_forward(difference_type n) {
            // Code explanation:
            // For the initial state of the iterator, we don't know which elements have been erased before that element in that group.
            // So for the first group, we follow the following logic:
            // 1. If no elements have been erased in the group, we do simple pointer addition to progress, either to within the group
            // (if the distance is small enough) or the end of the group and subtract from distance accordingly.
            // 2. If any of the first group's elements have been erased, we manually iterate, as we don't know whether
            // the erased elements occur before or after the initial iterator position, and we subtract 1 from the distance
            // amount each time we iterate. Iteration continues until either distance becomes zero, or we reach the end of the group.

            // For all subsequent groups, we follow this logic:
            // 1. If distance is larger than the total number of non-erased elements in a group, we skip that group and subtract
            //    the number of elements in that group from distance.
            // 2. If distance is smaller than the total number of non-erased elements in a group, then:
            //   a. If there are no erased elements in the group we simply add distance to group->elements to find the new location for the iterator.
            //   b. If there are erased elements in the group, we manually iterate and subtract 1 from distance on each iteration,
            //      until the new iterator location is found ie. distance = 0.

            // Note: incrementing element_pointer is avoided until necessary to avoid needless calculations.

            // Check that we're not already at end()
            assert(!(element_pointer == group_pointer->last_endpoint && group_pointer->next_group == nullptr));

            // Special case for initial element pointer and initial group (we don't know how far into the group the element pointer is)
            if (element_pointer != group_pointer->elements + group_pointer->skipfield[0]) {
                // ie. != first non-erased element in group
                const difference_type distance_from_end = static_cast<difference_type>(group_pointer->last_endpoint - element_pointer);

                if (group_pointer->size == static_cast<skipfield_type>(distance_from_end)) {
                    // ie. if there are no erasures in the group (using endpoint - elements_start to determine number of elements in group just in case this is the last group of the hive, in which case group->last_endpoint != group->elements + group->capacity)
                    if (n < distance_from_end) {
                        element_pointer += n;
                        skipfield_pointer += n;
                        return;
                    } else if (group_pointer->next_group == nullptr) {
                        // either we've reached end() or gone beyond it, so bound to end()
                        element_pointer = group_pointer->last_endpoint;
                        skipfield_pointer += distance_from_end;
                        return;
                    } else {
                        n -= distance_from_end;
                    }
                } else {
                    const skipfield_pointer_type endpoint = skipfield_pointer + distance_from_end;

                    while (true) {
                        ++skipfield_pointer;
                        skipfield_pointer += skipfield_pointer[0];
                        --n;

                        if (skipfield_pointer == endpoint) {
                            break;
                        } else if (n == 0) {
                            element_pointer = group_pointer->elements + (skipfield_pointer - group_pointer->skipfield);
                            return;
                        }
                    }

                    if (group_pointer->next_group == nullptr) {
                        // either we've reached end() or gone beyond it, so bound to end()
                        element_pointer = group_pointer->last_endpoint;
                        return;
                    }
                }

                group_pointer = group_pointer->next_group;

                if (n == 0) {
                    element_pointer = group_pointer->elements + group_pointer->skipfield[0];
                    skipfield_pointer = group_pointer->skipfield + group_pointer->skipfield[0];
                    return;
                }
            }

            // Intermediary groups - at the start of this code block and the subsequent block, the position of the iterator is assumed to be the first non-erased element in the current group:
            while (static_cast<difference_type>(group_pointer->size) <= n) {
                if (group_pointer->next_group == nullptr) {
                    // either we've reached end() or gone beyond it, so bound to end()
                    element_pointer = group_pointer->last_endpoint;
                    skipfield_pointer = group_pointer->skipfield + (group_pointer->last_endpoint - group_pointer->elements);
                    return;
                } else {
                    n -= group_pointer->size;
                    group_pointer = group_pointer->next_group;
                    if (n == 0) {
                        element_pointer = group_pointer->elements + group_pointer->skipfield[0];
                        skipfield_pointer = group_pointer->skipfield + group_pointer->skipfield[0];
                        return;
                    }
                }
            }

            // Final group (if not already reached):
            if (group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                // No erasures in this group, use straight pointer addition
                element_pointer = group_pointer->elements + n;
                skipfield_pointer = group_pointer->skipfield + n;
            } else {
                // ie. size > n - safe to ignore endpoint check condition while incrementing:
                skipfield_pointer = group_pointer->skipfield + group_pointer->skipfield[0];
                do {
                    ++skipfield_pointer;
                    skipfield_pointer += *skipfield_pointer;
                } while (--n != 0);
                element_pointer = group_pointer->elements + (skipfield_pointer - group_pointer->skipfield);
            }
        }

        void advance_backward(difference_type n) {
            assert(n < 0);
            assert(!((element_pointer == group_pointer->elements + *(group_pointer->skipfield)) && group_pointer->previous_group == nullptr)); // check that we're not already at begin()

            // Special case for initial element pointer and initial group (we don't know how far into the group the element pointer is)
            if (element_pointer != group_pointer->last_endpoint) {
                if (group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                    // ie. no prior erasures have occurred in this group
                    difference_type distance_from_beginning = static_cast<difference_type>(group_pointer->elements - element_pointer);

                    if (n >= distance_from_beginning) {
                        element_pointer += n;
                        skipfield_pointer += n;
                        return;
                    } else if (group_pointer->previous_group == nullptr) {
                        // ie. we've gone before begin(), so bound to begin()
                        element_pointer = group_pointer->elements;
                        skipfield_pointer = group_pointer->skipfield;
                        return;
                    } else {
                        n -= distance_from_beginning;
                    }
                } else {
                    const skipfield_pointer_type beginning_point = group_pointer->skipfield + group_pointer->skipfield[0];
                    while (skipfield_pointer != beginning_point) {
                        --skipfield_pointer;
                        skipfield_pointer -= skipfield_pointer[0];
                        if (++n == 0) {
                            element_pointer = group_pointer->elements + (skipfield_pointer - group_pointer->skipfield);
                            return;
                        }
                    }

                    if (group_pointer->previous_group == nullptr) {
                        element_pointer = group_pointer->elements + group_pointer->skipfield[0]; // This is first group, so bound to begin() (just in case final decrement took us before begin())
                        skipfield_pointer = group_pointer->skipfield + group_pointer->skipfield[0];
                        return;
                    }
                }
                group_pointer = group_pointer->previous_group;
            }

            // Intermediary groups - at the start of this code block and the subsequent block, the position of the iterator is assumed to be either the first non-erased element in the next group over, or end():
            while (n < -static_cast<difference_type>(group_pointer->size)) {
                if (group_pointer->previous_group == nullptr) {
                    // we've gone beyond begin(), so bound to it
                    element_pointer = group_pointer->elements + group_pointer->skipfield[0];
                    skipfield_pointer = group_pointer->skipfield + group_pointer->skipfield[0];
                    return;
                }
                n += group_pointer->size;
                group_pointer = group_pointer->previous_group;
            }

            // Final group (if not already reached):
            if (n == -static_cast<difference_type>(group_pointer->size)) {
                element_pointer = group_pointer->elements + group_pointer->skipfield[0];
                skipfield_pointer = group_pointer->skipfield + group_pointer->skipfield[0];
            } else if (group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                // ie. no erased elements in this group
                element_pointer = reinterpret_cast<aligned_pointer_type>(group_pointer->skipfield) + n;
                skipfield_pointer = (group_pointer->skipfield + group_pointer->capacity) + n;
            } else {
                // ie. no more groups to traverse but there are erased elements in this group
                skipfield_pointer = group_pointer->skipfield + group_pointer->capacity;
                do {
                    --skipfield_pointer;
                    skipfield_pointer -= skipfield_pointer[0];
                } while (++n != 0);
                element_pointer = group_pointer->elements + (skipfield_pointer - group_pointer->skipfield);
            }
        }

    public:
        inline void advance(difference_type n) {
            if (n > 0) {
                advance_forward(n);
            } else if (n < 0) {
                advance_backward(n);
            }
        }

        inline hive_iterator next(difference_type n) const {
            auto copy = *this;
            copy.advance(n);
            return copy;
        }

        inline hive_iterator prev(difference_type n) const {
            auto copy = *this;
            copy.advance(-n);
            return copy;
        }

        difference_type distance(hive_iterator last) const {
            auto first = *this;
            if (first == last) {
                return 0;
            }

            const bool should_swap = first > last;
            if (should_swap) {
                using std::swap;
                swap(first, last);
            }

            difference_type distance = 0;
            if (first.group_pointer != last.group_pointer) {
                if (first.group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                    // If no prior erasures have occured in this group we can do simple addition
                    distance += static_cast<difference_type>(first.group_pointer->last_endpoint - first.element_pointer);
                } else if (first.element_pointer == first.group_pointer->elements + first.group_pointer->skipfield[0]) {
                    // ie. element is at start of group - rare case
                    distance += static_cast<difference_type>(first.group_pointer->size);
                } else {
                    // Manually iterate to find distance to end of group:
                    const skipfield_pointer_type endpoint = first.skipfield_pointer + (first.group_pointer->last_endpoint - first.element_pointer);

                    while (first.skipfield_pointer != endpoint) {
                        ++first.skipfield_pointer;
                        first.skipfield_pointer += first.skipfield_pointer[0];
                        ++distance;
                    }
                }

                first.group_pointer = first.group_pointer->next_group;
                while (first.group_pointer != last.group_pointer) {
                    distance += static_cast<difference_type>(first.group_pointer->size);
                    first.group_pointer = first.group_pointer->next_group;
                }
                first.skipfield_pointer = first.group_pointer->skipfield;
            }

            if (last.group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                distance += last.skipfield_pointer - first.skipfield_pointer;
            } else if (last.group_pointer->last_endpoint - 1 >= last.element_pointer || last.element_pointer + *(last.skipfield_pointer + 1) == last.group_pointer->last_endpoint) {
                // ie. if last is .end() or the last element in the block
                distance += static_cast<difference_type>(last.group_pointer->size) - (last.group_pointer->last_endpoint - last.element_pointer);
            } else {
                while (first.skipfield_pointer != last.skipfield_pointer) {
                    ++first.skipfield_pointer;
                    first.skipfield_pointer += *first.skipfield_pointer;
                    ++distance;
                }
            }
            return should_swap ? -distance : distance;
        }

#if PLF_HIVE_RANDOM_ACCESS_ITERATORS
        friend hive_iterator& operator+=(hive_iterator& a, difference_type n) { a.advance(n); return a; }
        friend hive_iterator& operator-=(hive_iterator& a, difference_type n) { a.advance(-n); return a; }
        friend hive_iterator operator+(const hive_iterator& a, difference_type n) { return a.next(n); }
        friend hive_iterator operator+(difference_type n, const hive_iterator& a) { return a.next(n); }
        friend hive_iterator operator-(const hive_iterator& a, difference_type n) { return a.prev(n); }
        friend difference_type operator-(const hive_iterator& a, const hive_iterator& b) { return b.distance(a); }
#endif
    }; // class hive_iterator

    template <bool IsConst>
    class hive_reverse_iterator {
        hive_iterator<IsConst> it_;

    public:
#if PLF_HIVE_RANDOM_ACCESS_ITERATORS
        using iterator_category = std::random_access_iterator_tag;
#else
        using iterator_category = std::bidirectional_iterator_tag;
#endif
        using value_type = typename hive::value_type;
        using difference_type = typename hive::difference_type;
        using pointer = std::conditional_t<IsConst, typename hive::const_pointer, typename hive::pointer>;
        using reference = std::conditional_t<IsConst, typename hive::const_reference, typename hive::reference>;

        hive_reverse_iterator() = default;
        hive_reverse_iterator(hive_reverse_iterator&&) = default;
        hive_reverse_iterator(const hive_reverse_iterator&) = default;
        hive_reverse_iterator& operator=(hive_reverse_iterator&&) = default;
        hive_reverse_iterator& operator=(const hive_reverse_iterator&) = default;

        template<bool IsConst_ = IsConst, class = std::enable_if_t<IsConst_>>
        hive_reverse_iterator(const hive_reverse_iterator<false>& rhs) :
            it_(rhs.base())
        {}

        explicit hive_reverse_iterator(hive_iterator<IsConst>&& rhs) : it_(std::move(rhs)) {}
        explicit hive_reverse_iterator(const hive_iterator<IsConst>& rhs) : it_(rhs) {}

#if __cpp_impl_three_way_comparison >= 201907
        friend bool operator==(const hive_reverse_iterator& a, const hive_reverse_iterator& b) = default;
        friend std::strong_ordering operator<=>(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return (b.it_ <=> a.it_); }
#else
        friend bool operator==(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.it_ == a.it_; }
        friend bool operator!=(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.it_ != a.it_; }
        friend bool operator<(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.it_ < a.it_; }
        friend bool operator>(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.it_ > a.it_; }
        friend bool operator<=(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.it_ <= a.it_; }
        friend bool operator>=(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.it_ >= a.it_; }
#endif

        inline reference operator*() const noexcept { auto jt = it_; --jt; return *jt; }
        inline pointer operator->() const noexcept { auto jt = it_; --jt; return jt.operator->(); }
        hive_reverse_iterator& operator++() { --it_; return *this; }
        hive_reverse_iterator operator++(int) { auto copy = *this; --it_; return copy; }
        hive_reverse_iterator& operator--() { ++it_; return *this; }
        hive_reverse_iterator operator--(int) { auto copy = *this; ++it_; return copy; }

        hive_iterator<IsConst> base() const noexcept { return it_; }

        hive_reverse_iterator next(difference_type n) const {
            auto copy = *this;
            copy.it_.advance(-n);
            return copy;
        }

        hive_reverse_iterator prev(difference_type n) const {
            auto copy = *this;
            copy.it_.advance(n);
            return copy;
        }

        difference_type distance(const hive_reverse_iterator &last) const {
            return last.it_.distance(it_);
        }

        void advance(difference_type n) {
            it_.advance(-n);
        }

#if PLF_HIVE_RANDOM_ACCESS_ITERATORS
        friend hive_reverse_iterator& operator+=(hive_reverse_iterator& a, difference_type n) { a.advance(n); return a; }
        friend hive_reverse_iterator& operator-=(hive_reverse_iterator& a, difference_type n) { a.advance(-n); return a; }
        friend hive_reverse_iterator operator+(const hive_reverse_iterator& a, difference_type n) { return a.next(n); }
        friend hive_reverse_iterator operator+(difference_type n, const hive_reverse_iterator& a) { return a.next(n); }
        friend hive_reverse_iterator operator-(const hive_reverse_iterator& a, difference_type n) { return a.prev(n); }
        friend difference_type operator-(const hive_reverse_iterator& a, const hive_reverse_iterator& b) { return b.distance(a); }
#endif
    }; // hive_reverse_iterator

private:
    iterator end_;
    iterator begin_;
    group_pointer_type groups_with_erasures_list_head = group_pointer_type();
        // Head of the singly-linked list of groups which have erased-element memory locations available for re-use
    group_pointer_type unused_groups_head = group_pointer_type();
       // Head of singly-linked list of groups retained by erase()/clear() or created by reserve()
    size_type size_ = 0;
    size_type capacity_ = 0;
    allocator_type allocator_;
    skipfield_type min_group_capacity_ = get_minimum_block_capacity();
    skipfield_type max_group_capacity_ = std::numeric_limits<skipfield_type>::max();

    // An adaptive minimum based around sizeof(aligned_T), sizeof(group) and sizeof(hive):
    static constexpr inline skipfield_type get_minimum_block_capacity() {
        return static_cast<skipfield_type>((sizeof(aligned_T) * 8 > (sizeof(plf::hive<T>) + sizeof(group)) * 2) ?
            8 : (((sizeof(plf::hive<T>) + sizeof(group)) * 2) / sizeof(aligned_T)));
    }

    static inline void check_limits(plf::hive_limits soft) {
        auto hard = block_capacity_hard_limits();
        if (!(hard.min <= soft.min && soft.min <= soft.max && soft.max <= hard.max)) {
            throw std::length_error("Supplied limits are outside the allowable range");
        }
    }

public:
    hive() = default;

    explicit hive(plf::hive_limits limits) :
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
    }

    explicit hive(const allocator_type &alloc) : allocator_(alloc) {}

    hive(plf::hive_limits limits, const allocator_type &alloc) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
    }

    hive(const hive& source) :
        allocator_(std::allocator_traits<allocator_type>::select_on_container_copy_construction(source.allocator_)),
        min_group_capacity_(static_cast<skipfield_type>((source.min_group_capacity_ > source.size_) ? source.min_group_capacity_ : ((source.size_ > source.max_group_capacity_) ? source.max_group_capacity_ : source.size_))),
            // min group size is set to value closest to total number of elements in source hive, in order to not create
            // unnecessary small groups in the range-insert below, then reverts to the original min group size afterwards.
            // This effectively saves a call to reserve.
        max_group_capacity_(source.max_group_capacity_)
    {
        // can skip checking for skipfield conformance here as the skipfields must be equal between the destination and source,
        // and source will have already had theirs checked. Same applies for other copy and move constructors below
        reserve(source.size());
        range_assign_impl(source.begin(), source.end());
        min_group_capacity_ = source.min_group_capacity_;
    }

    hive(const hive& source, const hive_identity_t<allocator_type>& alloc) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>((source.min_group_capacity_ > source.size_) ? source.min_group_capacity_ : ((source.size_ > source.max_group_capacity_) ? source.max_group_capacity_ : source.size_))),
        max_group_capacity_(source.max_group_capacity_)
    {
        reserve(source.size());
        range_assign_impl(source.begin(), source.end());
        min_group_capacity_ = source.min_group_capacity_;
    }

private:
    inline void blank() {
        end_.group_pointer = nullptr;
        end_.element_pointer = nullptr;
        end_.skipfield_pointer = nullptr;
        begin_.group_pointer = nullptr;
        begin_.element_pointer = nullptr;
        begin_.skipfield_pointer = nullptr;
        groups_with_erasures_list_head = nullptr;
        unused_groups_head = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

public:
    hive(hive&& source) noexcept :
        end_(std::move(source.end_)),
        begin_(std::move(source.begin_)),
        groups_with_erasures_list_head(std::move(source.groups_with_erasures_list_head)),
        unused_groups_head(std::move(source.unused_groups_head)),
        size_(source.size_),
        capacity_(source.capacity_),
        allocator_(source.get_allocator()),
        min_group_capacity_(source.min_group_capacity_),
        max_group_capacity_(source.max_group_capacity_)
    {
        assert(&source != this);
        source.blank();
    }

    hive(hive&& source, const hive_identity_t<allocator_type>& alloc):
        end_(std::move(source.end_)),
        begin_(std::move(source.begin_)),
        groups_with_erasures_list_head(std::move(source.groups_with_erasures_list_head)),
        unused_groups_head(std::move(source.unused_groups_head)),
        size_(source.size_),
        capacity_(source.capacity_),
        allocator_(alloc),
        min_group_capacity_(source.min_group_capacity_),
        max_group_capacity_(source.max_group_capacity_)
    {
        assert(&source != this);
        source.blank();
    }

    hive(size_type n, const T& value, const allocator_type &alloc = allocator_type()) :
        allocator_(alloc)
    {
        assign(n, value);
    }

    hive(size_type n, const T& value, plf::hive_limits limits, const allocator_type &alloc = allocator_type()) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
        assign(n, value);
    }

    explicit hive(size_type n) { assign(n, T()); }

    hive(size_type n, const allocator_type &alloc) :
        allocator_(alloc)
    {
        assign(n, T());
    }

    hive(size_type n, plf::hive_limits limits, const allocator_type &alloc = allocator_type()) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
        assign(n, T());
    }

#if __cpp_lib_ranges >= 201911
    template <std::input_or_output_iterator It, std::sentinel_for<It> Sent>
    hive(It first, Sent last)
    {
        assign(std::move(first), std::move(last));
    }

    template <std::input_or_output_iterator It, std::sentinel_for<It> Sent>
    hive(It first, Sent last, const allocator_type &alloc) :
        allocator_(alloc)
    {
        assign(std::move(first), std::move(last));
    }

    template <std::input_or_output_iterator It, std::sentinel_for<It> Sent>
    hive(It first, Sent last, plf::hive_limits limits, const allocator_type &alloc = allocator_type()) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
        assign(std::move(first), std::move(last));
    }
#else
    template<class It, class = std::enable_if_t<!std::is_integral<It>::value>>
    hive(It first, It last)
    {
        assign(std::move(first), std::move(last));
    }

    template<class It, class = std::enable_if_t<!std::is_integral<It>::value>>
    hive(It first, It last, const allocator_type &alloc) :
        allocator_(alloc)
    {
        assign(std::move(first), std::move(last));
    }

    template<class It, class = std::enable_if_t<!std::is_integral<It>::value>>
    hive(It first, It last, plf::hive_limits limits, const allocator_type &alloc = allocator_type()) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
        assign(std::move(first), std::move(last));
    }
#endif

    hive(std::initializer_list<T> il, const hive_identity_t<allocator_type>& alloc = allocator_type()) :
        allocator_(alloc)
    {
        assign(il.begin(), il.end());
    }

    hive(std::initializer_list<T> il, plf::hive_limits limits, const allocator_type &alloc = allocator_type()):
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
        assign(il.begin(), il.end());
    }

#if __cpp_lib_ranges >= 201911 && __cpp_lib_ranges_to_container >= 202202
    template<std::ranges::range R>
    hive(std::from_range_t, R&& rg)
    {
        assign_range(std::forward<R>(rg));
    }

    template<std::ranges::range R>
    hive(std::from_range_t, R&& rg, const allocator_type &alloc) :
        allocator_(alloc)
    {
        assign_range(std::forward<R>(rg));
    }

    template<std::ranges::range R>
    explicit hive(std::from_range_t, R&& rg, plf::hive_limits limits, const allocator_type &alloc = allocator_type()) :
        allocator_(alloc),
        min_group_capacity_(static_cast<skipfield_type>(limits.min)),
        max_group_capacity_(static_cast<skipfield_type>(limits.max))
    {
        check_limits(limits);
        assign_range(std::forward<R>(rg));
    }
#endif

    ~hive() {
        destroy_all_data();
    }

    inline iterator begin() noexcept { return begin_; }
    inline const_iterator begin() const noexcept { return begin_; }
    inline const_iterator cbegin() const noexcept { return begin_; }
    inline iterator end() noexcept { return end_; }
    inline const_iterator end() const noexcept { return end_; }
    inline const_iterator cend() const noexcept { return end_; }

    inline reverse_iterator rbegin() noexcept { return reverse_iterator(end_); }
    inline const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end_); }
    inline const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end_); }
    inline reverse_iterator rend() noexcept { return reverse_iterator(begin_); }
    inline const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin_); }
    inline const_reverse_iterator crend() const noexcept { return const_reverse_iterator(begin_); }

private:
    group_pointer_type allocate_new_group(skipfield_type elements_per_group, group_pointer_type previous = nullptr) {
        auto ga = group_allocator_type(get_allocator());
        auto new_group = std::allocator_traits<group_allocator_type>::allocate(ga, 1, 0);
        hive_try_rollback([&]() {
            std::allocator_traits<group_allocator_type>::construct(ga, new_group, get_allocator(), elements_per_group, previous);
        }, [&]() {
            std::allocator_traits<group_allocator_type>::deallocate(ga, new_group, 1);
        });
        return new_group;
    }

    inline void deallocate_group(group_pointer_type g) {
        auto ga = group_allocator_type(get_allocator());
        auto aa = aligned_struct_allocator_type(get_allocator());
        std::allocator_traits<aligned_struct_allocator_type>::deallocate(aa, bitcast_pointer<aligned_struct_pointer_type>(g->elements), get_aligned_block_capacity(g->capacity));
        std::allocator_traits<group_allocator_type>::deallocate(ga, g, 1);
    }

    void destroy_all_data() {
        if (begin_.group_pointer != nullptr) {
            end_.group_pointer->next_group = unused_groups_head;

            if constexpr (!std::is_trivially_destructible<T>::value) {
                if (size_ != 0) {
                    while (true) {
                        // Erase elements without bothering to update skipfield - much faster:
                        const aligned_pointer_type end_pointer = begin_.group_pointer->last_endpoint;
                        do {
                            std::allocator_traits<allocator_type>::destroy(allocator_, bitcast_pointer<pointer>(begin_.element_pointer));
                            begin_.element_pointer += static_cast<size_type>(*++begin_.skipfield_pointer) + 1u;
                            begin_.skipfield_pointer += *begin_.skipfield_pointer;
                        } while (begin_.element_pointer != end_pointer); // ie. beyond end of available data

                        const group_pointer_type next_group = begin_.group_pointer->next_group;
                        deallocate_group(begin_.group_pointer);
                        begin_.group_pointer = next_group;

                        if (next_group == unused_groups_head) {
                            break;
                        }
                        begin_.element_pointer = next_group->elements + next_group->skipfield[0];
                        begin_.skipfield_pointer = next_group->skipfield + next_group->skipfield[0];
                    }
                }
            }

            while (begin_.group_pointer != nullptr) {
                const group_pointer_type next_group = begin_.group_pointer->next_group;
                deallocate_group(begin_.group_pointer);
                begin_.group_pointer = next_group;
            }
        }
    }

    void initialize(const skipfield_type first_group_size) {
        end_.group_pointer = begin_.group_pointer = allocate_new_group(first_group_size);
        end_.element_pointer = begin_.element_pointer = begin_.group_pointer->elements;
        end_.skipfield_pointer = begin_.skipfield_pointer = begin_.group_pointer->skipfield;
        capacity_ = first_group_size;
    }

    void update_skipblock(const iterator &new_location, skipfield_type prev_free_list_index) {
        const skipfield_type new_value = static_cast<skipfield_type>(*(new_location.skipfield_pointer) - 1);

        if (new_value != 0) // ie. skipfield was not 1, ie. a single-node skipblock, with no additional nodes to update
        {
            // set (new) start and (original) end of skipblock to new value:
            *(new_location.skipfield_pointer + new_value) = *(new_location.skipfield_pointer + 1) = new_value;

            // transfer free list node to new start node:
            ++(groups_with_erasures_list_head->free_list_head);

            if (prev_free_list_index != std::numeric_limits<skipfield_type>::max()) // ie. not the tail free list node
            {
                *(bitcast_pointer<skipfield_pointer_type>(new_location.group_pointer->elements + prev_free_list_index) + 1) = groups_with_erasures_list_head->free_list_head;
            }

            *(bitcast_pointer<skipfield_pointer_type>(new_location.element_pointer + 1)) = prev_free_list_index;
            *(bitcast_pointer<skipfield_pointer_type>(new_location.element_pointer + 1) + 1) = std::numeric_limits<skipfield_type>::max();
        }
        else // single-node skipblock, remove skipblock
        {
            groups_with_erasures_list_head->free_list_head = prev_free_list_index;

            if (prev_free_list_index != std::numeric_limits<skipfield_type>::max()) // ie. not the last free list node
            {
                *(bitcast_pointer<skipfield_pointer_type>(new_location.group_pointer->elements + prev_free_list_index) + 1) = std::numeric_limits<skipfield_type>::max();
            } else {
                // remove this group from the list of groups with erasures
                groups_with_erasures_list_head = groups_with_erasures_list_head->erasures_list_next_group;
            }
        }

        *(new_location.skipfield_pointer) = 0;
        ++(new_location.group_pointer->size);

        if (new_location.group_pointer == begin_.group_pointer && new_location.element_pointer < begin_.element_pointer)
        { /* ie. begin_ was moved forwards as the result of an erasure at some point, this erased element is before the current begin, hence, set current begin iterator to this element */
            begin_ = new_location;
        }

        ++size_;
    }

    inline void reset() {
        destroy_all_data();
        blank();
    }

public:
    inline iterator insert(const T& value) { return emplace(value); }
    inline iterator insert(T&& value) { return emplace(std::move(value)); }

    template<class... Args>
    iterator emplace(Args&&... args) {
        if (end_.element_pointer != nullptr) {
            if (groups_with_erasures_list_head == nullptr) {
                if (end_.element_pointer != bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield)) {
                    const iterator return_iterator = end_;
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(end_.element_pointer), static_cast<Args&&>(args)...);
                    ++end_.element_pointer;
                    end_.group_pointer->last_endpoint = end_.element_pointer;
                    ++(end_.group_pointer->size);
                    ++end_.skipfield_pointer;
                    ++size_;
                    return return_iterator;
                }
                group_pointer_type next_group;
                if (unused_groups_head == nullptr) {
                    const skipfield_type new_group_size = (size_ < static_cast<size_type>(max_group_capacity_)) ? static_cast<skipfield_type>(size_) : max_group_capacity_;
                    next_group = allocate_new_group(new_group_size, end_.group_pointer);
                    hive_try_rollback([&]() {
                        std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(next_group->elements), static_cast<Args&&>(args)...);
                    }, [&]() {
                        deallocate_group(next_group);
                    });
                    capacity_ += new_group_size;
                } else {
                    next_group = unused_groups_head;
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(next_group->elements), static_cast<Args&&>(args)...);
                    unused_groups_head = next_group->next_group;
                    next_group->reset(1, nullptr, end_.group_pointer, end_.group_pointer->group_number + 1u);
                }

                end_.group_pointer->next_group = next_group;
                end_.group_pointer = next_group;
                end_.element_pointer = next_group->last_endpoint;
                end_.skipfield_pointer = next_group->skipfield + 1;
                ++size_;

                return iterator(next_group, next_group->elements, next_group->skipfield);
            } else {
                auto new_location = iterator(groups_with_erasures_list_head, groups_with_erasures_list_head->elements + groups_with_erasures_list_head->free_list_head, groups_with_erasures_list_head->skipfield + groups_with_erasures_list_head->free_list_head);

                skipfield_type prev_free_list_index = *(bitcast_pointer<skipfield_pointer_type>(new_location.element_pointer));
                std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(new_location.element_pointer), static_cast<Args&&>(args)...);
                update_skipblock(new_location, prev_free_list_index);

                return new_location;
            }
        } else {
            initialize(min_group_capacity_);
            hive_try_rollback([&]() {
                std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(end_.element_pointer++), static_cast<Args&&>(args)...);
            }, [&]() {
                reset();
            });
            ++end_.skipfield_pointer;
            size_ = 1;
            return begin_;
        }
    }

private:
    void recover_from_partial_fill() {
        if constexpr (!std::is_nothrow_copy_constructible<T>::value) {
            end_.group_pointer->last_endpoint = end_.element_pointer;
            auto elements_constructed_before_exception = static_cast<skipfield_type>(end_.element_pointer - end_.group_pointer->elements);
            end_.group_pointer->size = elements_constructed_before_exception;
            end_.skipfield_pointer = end_.group_pointer->skipfield + elements_constructed_before_exception;
            size_ += elements_constructed_before_exception;
            unused_groups_head = end_.group_pointer->next_group;
            end_.group_pointer->next_group = nullptr;
        }
    }

    void fill(const T& element, skipfield_type n) {
        if constexpr (std::is_nothrow_copy_constructible<T>::value) {
            if constexpr (std::is_trivially_copyable<T>::value && std::is_trivially_copy_constructible<T>::value) {
                // ie. we can get away with using the cheaper fill_n here if there is no chance of an exception being thrown:
                if constexpr (sizeof(aligned_T) != sizeof(T)) {
                    // to avoid potentially violating memory boundaries in line below, create an initial object copy of same (but aligned) type
                    alignas(aligned_T) T aligned_copy = element;
                    std::fill_n(end_.element_pointer, n, *bitcast_pointer<aligned_pointer_type>(&aligned_copy));
                } else {
                    std::fill_n(bitcast_pointer<pointer>(end_.element_pointer), n, element);
                }
                end_.element_pointer += n;
            } else {
                const aligned_pointer_type fill_end = end_.element_pointer + n;
                do {
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(end_.element_pointer), element);
                } while (++end_.element_pointer != fill_end);
            }
        } else {
            const aligned_pointer_type fill_end = end_.element_pointer + n;
            do {
                hive_try_rollback([&]() {
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(end_.element_pointer), element);
                }, [&]() {
                    recover_from_partial_fill();
                });
            } while (++end_.element_pointer != fill_end);
        }
        size_ += n;
    }

    // For catch blocks in range_fill_skipblock and fill_skipblock
    void recover_from_partial_skipblock_fill(aligned_pointer_type location, aligned_pointer_type current_location, skipfield_pointer_type skipfield_pointer, skipfield_type prev_free_list_node) {
        if constexpr (!std::is_nothrow_copy_constructible<T>::value) {
            // Reconstruct existing skipblock and free-list indexes to reflect partially-reused skipblock:
            const skipfield_type elements_constructed_before_exception = static_cast<skipfield_type>((current_location - 1) - location);
            groups_with_erasures_list_head->size = static_cast<skipfield_type>(groups_with_erasures_list_head->size + elements_constructed_before_exception);
            size_ += elements_constructed_before_exception;

            std::memset(skipfield_pointer, 0, elements_constructed_before_exception * sizeof(skipfield_type));

            *(bitcast_pointer<skipfield_pointer_type>(location + elements_constructed_before_exception)) = prev_free_list_node;
            *(bitcast_pointer<skipfield_pointer_type>(location + elements_constructed_before_exception) + 1) = std::numeric_limits<skipfield_type>::max();

            const skipfield_type new_skipblock_head_index = static_cast<skipfield_type>((location - groups_with_erasures_list_head->elements) + elements_constructed_before_exception);
            groups_with_erasures_list_head->free_list_head = new_skipblock_head_index;

            if (prev_free_list_node != std::numeric_limits<skipfield_type>::max()) {
                *(bitcast_pointer<skipfield_pointer_type>(groups_with_erasures_list_head->elements + prev_free_list_node) + 1) = new_skipblock_head_index;
            }
        }
    }

    void fill_skipblock(const T &element, aligned_pointer_type location, skipfield_pointer_type skipfield_pointer, skipfield_type size) {
        if constexpr (std::is_nothrow_copy_constructible<T>::value) {
            if constexpr (std::is_trivially_copyable<T>::value && std::is_trivially_copy_constructible<T>::value) {
                if constexpr (sizeof(aligned_T) != sizeof(T)) {
                    alignas (alignof(aligned_T)) T aligned_copy = element;
                    std::fill_n(location, size, *(bitcast_pointer<aligned_pointer_type>(&aligned_copy)));
                } else {
                    std::fill_n(bitcast_pointer<pointer>(location), size, element);
                }
            } else {
                const aligned_pointer_type fill_end = location + size;
                for (aligned_pointer_type p = location; p != fill_end; ++p) {
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(p), element);
                }
            }
        } else {
            const aligned_pointer_type fill_end = location + size;
            const skipfield_type prev_free_list_node = *(bitcast_pointer<skipfield_pointer_type>(location)); // in case of exception, grabbing indexes before free_list node is reused

            for (aligned_pointer_type current_location = location; current_location != fill_end; ++current_location) {
                hive_try_rollback([&]() {
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(current_location), element);
                }, [&]() {
                    recover_from_partial_skipblock_fill(location, current_location, skipfield_pointer, prev_free_list_node);
                });
            }
        }

        std::memset(skipfield_pointer, 0, size * sizeof(skipfield_type)); // reset skipfield nodes within skipblock to 0
        groups_with_erasures_list_head->size = static_cast<skipfield_type>(groups_with_erasures_list_head->size + size);
        size_ += size;
    }

    void fill_unused_groups(size_type size, const T& element, size_type group_number, group_pointer_type previous_group, group_pointer_type current_group) {
        end_.group_pointer = current_group;
        for (; end_.group_pointer->capacity < size; end_.group_pointer = end_.group_pointer->next_group) {
            const skipfield_type capacity = end_.group_pointer->capacity;
            end_.group_pointer->reset(capacity, end_.group_pointer->next_group, previous_group, group_number++);
            previous_group = end_.group_pointer;
            size -= static_cast<size_type>(capacity);
            end_.element_pointer = end_.group_pointer->elements;
            fill(element, capacity);
        }

        // Deal with final group (partial fill)
        unused_groups_head = end_.group_pointer->next_group;
        end_.group_pointer->reset(static_cast<skipfield_type>(size), nullptr, previous_group, group_number);
        end_.element_pointer = end_.group_pointer->elements;
        end_.skipfield_pointer = end_.group_pointer->skipfield + size;
        fill(element, static_cast<skipfield_type>(size));
    }

public:
    void insert(size_type size, const T &element) {
        if (size == 0) {
            return;
        } else if (size == 1) {
            insert(element);
            return;
        } else if (size_ == 0) {
            assign(size, element);
            return;
        }

        reserve(size_ + size);

        // Use up erased locations if available:
        while (groups_with_erasures_list_head != nullptr) {
            // skipblock loop: breaks when hive is exhausted of reusable skipblocks, or returns if size == 0
            aligned_pointer_type const element_pointer = groups_with_erasures_list_head->elements + groups_with_erasures_list_head->free_list_head;
            skipfield_pointer_type const skipfield_pointer = groups_with_erasures_list_head->skipfield + groups_with_erasures_list_head->free_list_head;
            const skipfield_type skipblock_size = *skipfield_pointer;

            if (groups_with_erasures_list_head == begin_.group_pointer && element_pointer < begin_.element_pointer)
            {
                begin_.element_pointer = element_pointer;
                begin_.skipfield_pointer = skipfield_pointer;
            }

            if (skipblock_size <= size)
            {
                groups_with_erasures_list_head->free_list_head = *(bitcast_pointer<skipfield_pointer_type>(element_pointer)); // set free list head to previous free list node
                fill_skipblock(element, element_pointer, skipfield_pointer, skipblock_size);
                size -= skipblock_size;

                if (groups_with_erasures_list_head->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                    // there are more skipblocks to be filled in this group
                    *(bitcast_pointer<skipfield_pointer_type>(groups_with_erasures_list_head->elements + groups_with_erasures_list_head->free_list_head) + 1) = std::numeric_limits<skipfield_type>::max(); // set 'next' index of new free list head to 'end' (numeric max)
                } else {
                    groups_with_erasures_list_head = groups_with_erasures_list_head->erasures_list_next_group; // change groups
                }

                if (size == 0) {
                    return;
                }
            } else {
                // skipblock is larger than remaining number of elements
                const skipfield_type prev_index = *(bitcast_pointer<skipfield_pointer_type>(element_pointer)); // save before element location is overwritten
                fill_skipblock(element, element_pointer, skipfield_pointer, static_cast<skipfield_type>(size));
                const skipfield_type new_skipblock_size = static_cast<skipfield_type>(skipblock_size - size);

                // Update skipfield (earlier nodes already memset'd in fill_skipblock function):
                *(skipfield_pointer + size) = new_skipblock_size;
                *(skipfield_pointer + skipblock_size - 1) = new_skipblock_size;
                groups_with_erasures_list_head->free_list_head = static_cast<skipfield_type>(groups_with_erasures_list_head->free_list_head + size); // set free list head to new start node

                // Update free list with new head:
                *(bitcast_pointer<skipfield_pointer_type>(element_pointer + size)) = prev_index;
                *(bitcast_pointer<skipfield_pointer_type>(element_pointer + size) + 1) = std::numeric_limits<skipfield_type>::max();

                if (prev_index != std::numeric_limits<skipfield_type>::max())
                {
                    *(bitcast_pointer<skipfield_pointer_type>(groups_with_erasures_list_head->elements + prev_index) + 1) = groups_with_erasures_list_head->free_list_head; // set 'next' index of previous skipblock to new start of skipblock
                }

                return;
            }
        }


        // Use up remaining available element locations in end group:
        // This variable is either the remaining capacity of the group or the number of elements yet to be filled, whichever is smaller:
        const skipfield_type group_remainder = (static_cast<skipfield_type>(
            bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield) - end_.element_pointer) >= size) ?
            static_cast<skipfield_type>(size) :
            static_cast<skipfield_type>(bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield) - end_.element_pointer);

        if (group_remainder != 0)
        {
            fill(element, group_remainder);
            end_.group_pointer->last_endpoint = end_.element_pointer;
            end_.group_pointer->size = static_cast<skipfield_type>(end_.group_pointer->size + group_remainder);

            if (size == group_remainder) // Ie. remaining capacity was >= remaining elements to be filled
            {
                end_.skipfield_pointer = end_.group_pointer->skipfield + end_.group_pointer->size;
                return;
            }

            size -= group_remainder;
        }


        // Use unused groups:
        end_.group_pointer->next_group = unused_groups_head;
        fill_unused_groups(size, element, end_.group_pointer->group_number + 1, end_.group_pointer, unused_groups_head);
    }



private:

    template <class iterator_type>
    iterator_type range_fill(iterator_type it, skipfield_type size) {
        if constexpr (std::is_nothrow_copy_constructible<T>::value) {
            const aligned_pointer_type fill_end = end_.element_pointer + size;
            do {
                std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(end_.element_pointer), *it++);
            } while (++end_.element_pointer != fill_end);
        } else {
            const aligned_pointer_type fill_end = end_.element_pointer + size;
            do {
                hive_try_rollback([&]() {
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(end_.element_pointer), *it++);
                }, [&]() {
                    recover_from_partial_fill();
                });
            } while (++end_.element_pointer != fill_end);
        }

        size_ += size;
        return it;
    }

    template<class It>
    It range_fill_skipblock(It it, aligned_pointer_type location, skipfield_pointer_type skipfield_pointer, skipfield_type n) {
        const aligned_pointer_type fill_end = location + n;
        if constexpr (std::is_nothrow_copy_constructible<T>::value) {
            for (aligned_pointer_type current_location = location; current_location != fill_end; ++current_location) {
                std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(current_location), *it++);
            }
        } else {
            // in case of exception, grabbing indexes before free_list node is reused
            const skipfield_type prev_free_list_node = *bitcast_pointer<skipfield_pointer_type>(location);
            for (aligned_pointer_type current_location = location; current_location != fill_end; ++current_location) {
                hive_try_rollback([&]() {
                    std::allocator_traits<allocator_type>::construct(elt_allocator(), bitcast_pointer<pointer>(current_location), *it++);
                }, [&]() {
                    recover_from_partial_skipblock_fill(location, current_location, skipfield_pointer, prev_free_list_node);
                });
            }
        }
        std::memset(skipfield_pointer, 0, n * sizeof(skipfield_type)); // reset skipfield nodes within skipblock to 0
        groups_with_erasures_list_head->size = static_cast<skipfield_type>(groups_with_erasures_list_head->size + n);
        size_ += n;
        return it;
    }

    template<class It>
    void range_fill_unused_groups(size_type size, It it, size_type group_number, group_pointer_type previous_group, group_pointer_type current_group) {
        end_.group_pointer = current_group;

        for (; end_.group_pointer->capacity < size; end_.group_pointer = end_.group_pointer->next_group)
        {
            const skipfield_type capacity = end_.group_pointer->capacity;
            end_.group_pointer->reset(capacity, end_.group_pointer->next_group, previous_group, group_number++);
            previous_group = end_.group_pointer;
            size -= static_cast<size_type>(capacity);
            end_.element_pointer = end_.group_pointer->elements;
            it = range_fill(it, capacity);
        }

        // Deal with final group (partial fill)
        unused_groups_head = end_.group_pointer->next_group;
        end_.group_pointer->reset(static_cast<skipfield_type>(size), nullptr, previous_group, group_number);
        end_.element_pointer = end_.group_pointer->elements;
        end_.skipfield_pointer = end_.group_pointer->skipfield + size;
        range_fill(it, static_cast<skipfield_type>(size));
    }

    template<class It, class Sent>
    void range_insert_impl(It first, Sent last) {
        if (first == last) {
            return;
        } else if (size_ == 0) {
            assign(std::move(first), std::move(last));
#if __cpp_lib_ranges >= 201911
        } else if constexpr (!std::forward_iterator<It>) {
            for ( ; first != last; ++first) {
                insert(*first);
            }
#endif
        } else {
#if __cpp_lib_ranges >= 201911
            size_type n = std::ranges::distance(first, last);
#else
            size_type n = std::distance(first, last);
#endif
            reserve(size_ + n);
            while (groups_with_erasures_list_head != nullptr) {
                aligned_pointer_type element_pointer = groups_with_erasures_list_head->elements + groups_with_erasures_list_head->free_list_head;
                skipfield_pointer_type skipfield_pointer = groups_with_erasures_list_head->skipfield + groups_with_erasures_list_head->free_list_head;
                skipfield_type skipblock_size = *skipfield_pointer;

                if (groups_with_erasures_list_head == begin_.group_pointer && element_pointer < begin_.element_pointer) {
                    begin_.element_pointer = element_pointer;
                    begin_.skipfield_pointer = skipfield_pointer;
                }

                if (skipblock_size <= n) {
                    groups_with_erasures_list_head->free_list_head = *(bitcast_pointer<skipfield_pointer_type>(element_pointer));
                    first = range_fill_skipblock(std::move(first), element_pointer, skipfield_pointer, skipblock_size);
                    n -= skipblock_size;
                    if (groups_with_erasures_list_head->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                        *(bitcast_pointer<skipfield_pointer_type>(groups_with_erasures_list_head->elements + groups_with_erasures_list_head->free_list_head) + 1) = std::numeric_limits<skipfield_type>::max();
                    } else {
                        groups_with_erasures_list_head = groups_with_erasures_list_head->erasures_list_next_group;
                    }
                    if (n == 0) {
                        return;
                    }
                } else {
                    const skipfield_type prev_index = *(bitcast_pointer<skipfield_pointer_type>(element_pointer));
                    first = range_fill_skipblock(std::move(first), element_pointer, skipfield_pointer, static_cast<skipfield_type>(n));
                    const skipfield_type new_skipblock_size = static_cast<skipfield_type>(skipblock_size - n);
                    skipfield_pointer[n] = new_skipblock_size;
                    skipfield_pointer[skipblock_size - 1] = new_skipblock_size;
                    groups_with_erasures_list_head->free_list_head = static_cast<skipfield_type>(groups_with_erasures_list_head->free_list_head + n);
                    *(bitcast_pointer<skipfield_pointer_type>(element_pointer + n)) = prev_index;
                    *(bitcast_pointer<skipfield_pointer_type>(element_pointer + n) + 1) = std::numeric_limits<skipfield_type>::max();
                    if (prev_index != std::numeric_limits<skipfield_type>::max()) {
                        *(bitcast_pointer<skipfield_pointer_type>(groups_with_erasures_list_head->elements + prev_index) + 1) = groups_with_erasures_list_head->free_list_head;
                    }
                    return;
                }
            }
            const skipfield_type group_remainder = (static_cast<skipfield_type>(
                bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield) - end_.element_pointer) >= n) ?
                static_cast<skipfield_type>(n) :
                static_cast<skipfield_type>(bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield) - end_.element_pointer);

            if (group_remainder != 0) {
                first = range_fill(std::move(first), group_remainder);
                end_.group_pointer->last_endpoint = end_.element_pointer;
                end_.group_pointer->size = static_cast<skipfield_type>(end_.group_pointer->size + group_remainder);

                if (n == group_remainder) {
                    end_.skipfield_pointer = end_.group_pointer->skipfield + end_.group_pointer->size;
                    return;
                }
                n -= group_remainder;
            }
            end_.group_pointer->next_group = unused_groups_head;
            range_fill_unused_groups(n, first, end_.group_pointer->group_number + 1, end_.group_pointer, unused_groups_head);
        }
    }

public:
#if __cpp_lib_ranges >= 201911
    template<std::ranges::range R>
    inline void insert_range(R&& rg) {
        if constexpr (std::sized_range<R&>) {
            reserve(size() + std::ranges::size(rg));
        }
        insert(std::ranges::begin(rg), std::ranges::end(rg));
    }

    template<std::input_or_output_iterator It, std::sentinel_for<It> Sent>
    inline void insert(It first, Sent last) {
        range_insert_impl(std::move(first), std::move(last));
    }
#else
    template<class It, std::enable_if_t<!std::is_integral<It>::value>* = nullptr>
    inline void insert(It first, It last) {
        range_insert_impl(std::move(first), std::move(last));
    }
#endif

    inline void insert(std::initializer_list<T> il) {
        range_insert_impl(il.begin(), il.end());
    }

private:
    inline void update_subsequent_group_numbers(group_pointer_type current_group) {
        do {
            --(current_group->group_number);
            current_group = current_group->next_group;
        } while (current_group != nullptr);
    }

    void remove_from_groups_with_erasures_list(group_pointer_type group_to_remove) {
        if (group_to_remove == groups_with_erasures_list_head) {
            groups_with_erasures_list_head = groups_with_erasures_list_head->erasures_list_next_group;
            return;
        }

        group_pointer_type previous_group = groups_with_erasures_list_head, current_group = groups_with_erasures_list_head->erasures_list_next_group;

        while (group_to_remove != current_group)
        {
            previous_group = current_group;
            current_group = current_group->erasures_list_next_group;
        }

        previous_group->erasures_list_next_group = current_group->erasures_list_next_group;
    }

    inline void reset_only_group_left(group_pointer_type const group_pointer) {
        groups_with_erasures_list_head = nullptr;
        group_pointer->reset(0, nullptr, nullptr, 0);

        // Reset begin and end iterators:
        end_.element_pointer = begin_.element_pointer = group_pointer->last_endpoint;
        end_.skipfield_pointer = begin_.skipfield_pointer = group_pointer->skipfield;
    }

    inline void add_group_to_unused_groups_list(group *group_pointer) {
        group_pointer->next_group = unused_groups_head;
        unused_groups_head = group_pointer;
    }

public:
    iterator erase(const_iterator it) {
        assert(size_ != 0);
        assert(it.group_pointer != nullptr); // ie. not uninitialized iterator
        assert(it.element_pointer != it.group_pointer->last_endpoint); // ie. != end()
        assert(*(it.skipfield_pointer) == 0); // ie. element pointed to by iterator has not been erased previously

        if constexpr (!std::is_trivially_destructible<T>::value) {
            std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(it.element_pointer));
        }

        --size_;

        if (it.group_pointer->size-- != 1) {
            // ie. non-empty group at this point in time, don't consolidate
            // optimization note: GCC optimizes postfix - 1 comparison better than prefix - 1 comparison in some cases.

            // Code logic for following section:
            // ---------------------------------
            // If current skipfield node has no skipblock on either side, create new skipblock of size 1
            // If node only has skipblock on left, set current node and start node of the skipblock to left node value + 1.
            // If node only has skipblock on right, make this node the start node of the skipblock and update end node
            // If node has skipblocks on left and right, set start node of left skipblock and end node of right skipblock to the values of the left + right nodes + 1

            // Optimization explanation:
            // The contextual logic below is the same as that in the insert() functions but in this case the value of the current skipfield node will always be
            // zero (since it is not yet erased), meaning no additional manipulations are necessary for the previous skipfield node comparison - we only have to check against zero
            const char prev_skipfield = *(it.skipfield_pointer - (it.skipfield_pointer != it.group_pointer->skipfield)) != 0;
            const char after_skipfield = *(it.skipfield_pointer + 1) != 0;  // NOTE: boundary test (checking against end-of-elements) is able to be skipped due to the extra skipfield node (compared to element field) - which is present to enable faster iterator operator ++ operations
            skipfield_type update_value = 1;

            if (!(prev_skipfield | after_skipfield)) {
                // no consecutive erased elements
                *it.skipfield_pointer = 1; // solo skipped node
                const skipfield_type index = static_cast<skipfield_type>(it.element_pointer - it.group_pointer->elements);

                if (it.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                    // ie. if this group already has some erased elements
                    *(bitcast_pointer<skipfield_pointer_type>(it.group_pointer->elements + it.group_pointer->free_list_head) + 1) = index; // set prev free list head's 'next index' number to the index of the current element
                } else {
                    it.group_pointer->erasures_list_next_group = groups_with_erasures_list_head; // add it to the groups-with-erasures free list
                    groups_with_erasures_list_head = it.group_pointer;
                }

                *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer)) = it.group_pointer->free_list_head;
                *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer) + 1) = std::numeric_limits<skipfield_type>::max();
                it.group_pointer->free_list_head = index;
            } else if (prev_skipfield & (!after_skipfield)) {
                // previous erased consecutive elements, none following
                *(it.skipfield_pointer - *(it.skipfield_pointer - 1)) = *it.skipfield_pointer = static_cast<skipfield_type>(*(it.skipfield_pointer - 1) + 1);
            } else if ((!prev_skipfield) & after_skipfield) {
                // following erased consecutive elements, none preceding
                const skipfield_type following_value = static_cast<skipfield_type>(*(it.skipfield_pointer + 1) + 1);
                *(it.skipfield_pointer + following_value - 1) = *(it.skipfield_pointer) = following_value;

                const skipfield_type following_previous = *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer + 1));
                const skipfield_type following_next = *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer + 1) + 1);
                *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer)) = following_previous;
                *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer) + 1) = following_next;

                const skipfield_type index = static_cast<skipfield_type>(it.element_pointer - it.group_pointer->elements);

                if (following_previous != std::numeric_limits<skipfield_type>::max()) {
                    *(bitcast_pointer<skipfield_pointer_type>(it.group_pointer->elements + following_previous) + 1) = index; // Set next index of previous free list node to this node's 'next' index
                }

                if (following_next != std::numeric_limits<skipfield_type>::max()) {
                    *(bitcast_pointer<skipfield_pointer_type>(it.group_pointer->elements + following_next)) = index;    // Set previous index of next free list node to this node's 'previous' index
                } else {
                    it.group_pointer->free_list_head = index;
                }
                update_value = following_value;
            } else {
                // both preceding and following consecutive erased elements - erased element is between two skipblocks
                const skipfield_type preceding_value = *(it.skipfield_pointer - 1);
                const skipfield_type following_value = static_cast<skipfield_type>(*(it.skipfield_pointer + 1) + 1);

                // Join the skipblocks
                *(it.skipfield_pointer - preceding_value) = *(it.skipfield_pointer + following_value - 1) = static_cast<skipfield_type>(preceding_value + following_value);

                // Remove the following skipblock's entry from the free list
                const skipfield_type following_previous = *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer + 1));
                const skipfield_type following_next = *(bitcast_pointer<skipfield_pointer_type>(it.element_pointer + 1) + 1);

                if (following_previous != std::numeric_limits<skipfield_type>::max()) {
                    *(bitcast_pointer<skipfield_pointer_type>(it.group_pointer->elements + following_previous) + 1) = following_next; // Set next index of previous free list node to this node's 'next' index
                }

                if (following_next != std::numeric_limits<skipfield_type>::max()) {
                    *(bitcast_pointer<skipfield_pointer_type>(it.group_pointer->elements + following_next)) = following_previous; // Set previous index of next free list node to this node's 'previous' index
                } else {
                    it.group_pointer->free_list_head = following_previous;
                }
                update_value = following_value;
            }

            iterator return_iterator(it.group_pointer, it.element_pointer + update_value, it.skipfield_pointer + update_value);

            if (return_iterator.element_pointer == it.group_pointer->last_endpoint && it.group_pointer->next_group != nullptr) {
                return_iterator.group_pointer = it.group_pointer->next_group;
                const aligned_pointer_type elements = return_iterator.group_pointer->elements;
                const skipfield_pointer_type skipfield = return_iterator.group_pointer->skipfield;
                const skipfield_type skip = *skipfield;
                return_iterator.element_pointer = elements + skip;
                return_iterator.skipfield_pointer = skipfield + skip;
            }

            if (it.element_pointer == begin_.element_pointer) {
                // If original iterator was first element in hive, update it's value with the next non-erased element:
                begin_ = return_iterator;
            }
            return return_iterator;
        }

        // else: group is empty, consolidate groups
        const bool in_back_block = (it.group_pointer->next_group == nullptr), in_front_block = (it.group_pointer == begin_.group_pointer);

        if (in_back_block & in_front_block) {
            // ie. only group in hive
            // Reset skipfield and free list rather than clearing - leads to fewer allocations/deallocations:
            reset_only_group_left(it.group_pointer);
            return end_;
        } else if ((!in_back_block) & in_front_block) {
            // ie. Remove first group, change first group to next group
            it.group_pointer->next_group->previous_group = nullptr; // Cut off this group from the chain
            begin_.group_pointer = it.group_pointer->next_group; // Make the next group the first group

            update_subsequent_group_numbers(begin_.group_pointer);

            if (it.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                // Erasures present within the group, ie. was part of the linked list of groups with erasures.
                remove_from_groups_with_erasures_list(it.group_pointer);
            }

            capacity_ -= it.group_pointer->capacity;
            deallocate_group(it.group_pointer);

            // note: end iterator only needs to be changed if the deleted group was the final group in the chain ie. not in this case
            begin_.element_pointer = begin_.group_pointer->elements + *(begin_.group_pointer->skipfield); // If the beginning index has been erased (ie. skipfield != 0), skip to next non-erased element
            begin_.skipfield_pointer = begin_.group_pointer->skipfield + *(begin_.group_pointer->skipfield);

            return begin_;
        } else if (!(in_back_block | in_front_block)) {
            // this is a non-first group but not final group in chain: delete the group, then link previous group to the next group in the chain:
            it.group_pointer->next_group->previous_group = it.group_pointer->previous_group;
            const group_pointer_type return_group = it.group_pointer->previous_group->next_group = it.group_pointer->next_group; // close the chain, removing this group from it

            update_subsequent_group_numbers(return_group);

            if (it.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                remove_from_groups_with_erasures_list(it.group_pointer);
            }

            if (it.group_pointer->next_group != end_.group_pointer) {
                capacity_ -= it.group_pointer->capacity;
                deallocate_group(it.group_pointer);
            } else {
                add_group_to_unused_groups_list(it.group_pointer);
            }

            // Return next group's first non-erased element:
            return iterator(return_group, return_group->elements + *(return_group->skipfield), return_group->skipfield + *(return_group->skipfield));
        } else {
            // this is a non-first group and the final group in the chain
            if (it.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                remove_from_groups_with_erasures_list(it.group_pointer);
            }
            it.group_pointer->previous_group->next_group = nullptr;
            end_.group_pointer = it.group_pointer->previous_group; // end iterator needs to be changed as element supplied was the back element of the hive
            end_.element_pointer = bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield);
            end_.skipfield_pointer = end_.group_pointer->skipfield + end_.group_pointer->capacity;
            add_group_to_unused_groups_list(it.group_pointer);
            return end_;
        }
    }

    iterator erase(const_iterator iterator1, const_iterator iterator2) {
        // if uninitialized/invalid iterators supplied, function could generate an exception. If iterator1 > iterator2, behaviour is undefined.
        const_iterator current = iterator1;
        if (current.group_pointer != iterator2.group_pointer) {
            if (current.element_pointer != current.group_pointer->elements + *(current.group_pointer->skipfield)) {
                // if iterator1 is not the first non-erased element in it's group - most common case
                size_type number_of_group_erasures = 0;

                // Now update skipfield:
                const aligned_pointer_type end = iterator1.group_pointer->last_endpoint;

                // Schema: first erase all non-erased elements until end of group & remove all skipblocks post-iterator1 from the free_list. Then, either update preceding skipblock or create new one:

                if (std::is_trivially_destructible<T>::value && current.group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                    number_of_group_erasures += static_cast<size_type>(end - current.element_pointer);
                } else {
                    while (current.element_pointer != end) {
                        if (*current.skipfield_pointer == 0) {
                            if constexpr (!std::is_trivially_destructible<T>::value) {
                                std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer)); // Destruct element
                            }
                            ++number_of_group_erasures;
                            ++current.element_pointer;
                            ++current.skipfield_pointer;
                        } else {
                            // remove skipblock from group:
                            const skipfield_type prev_free_list_index = *(bitcast_pointer<skipfield_pointer_type>(current.element_pointer));
                            const skipfield_type next_free_list_index = *(bitcast_pointer<skipfield_pointer_type>(current.element_pointer) + 1);

                            current.element_pointer += *(current.skipfield_pointer);
                            current.skipfield_pointer += *(current.skipfield_pointer);

                            if (next_free_list_index == std::numeric_limits<skipfield_type>::max() && prev_free_list_index == std::numeric_limits<skipfield_type>::max()) {
                                // if this is the last skipblock in the free list
                                remove_from_groups_with_erasures_list(iterator1.group_pointer); // remove group from list of free-list groups - will be added back in down below, but not worth optimizing for
                                iterator1.group_pointer->free_list_head = std::numeric_limits<skipfield_type>::max();
                                number_of_group_erasures += static_cast<size_type>(end - current.element_pointer);
                                if constexpr (!std::is_trivially_destructible<T>::value) {
                                    while (current.element_pointer != end) {
                                        std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer));
                                        ++current.element_pointer;
                                    }
                                }
                                break; // end overall while loop
                            } else if (next_free_list_index == std::numeric_limits<skipfield_type>::max()) {
                                // if this is the head of the free list
                                current.group_pointer->free_list_head = prev_free_list_index; // make free list head equal to next free list node
                                *(bitcast_pointer<skipfield_pointer_type>(current.group_pointer->elements + prev_free_list_index) + 1) = std::numeric_limits<skipfield_type>::max();
                            } else {
                                // either a tail or middle free list node
                                *(bitcast_pointer<skipfield_pointer_type>(current.group_pointer->elements + next_free_list_index)) = prev_free_list_index;

                                if (prev_free_list_index != std::numeric_limits<skipfield_type>::max()) {
                                    // ie. not the tail free list node
                                    bitcast_pointer<skipfield_pointer_type>(current.group_pointer->elements + prev_free_list_index)[1] = next_free_list_index;
                                }
                            }
                        }
                    }
                }

                const skipfield_type previous_node_value = *(iterator1.skipfield_pointer - 1);
                const skipfield_type distance_to_end = static_cast<skipfield_type>(end - iterator1.element_pointer);

                if (previous_node_value == 0) {
                    // no previous skipblock
                    *iterator1.skipfield_pointer = distance_to_end; // set start node value
                    *(iterator1.skipfield_pointer + distance_to_end - 1) = distance_to_end; // set end node value

                    const skipfield_type index = static_cast<skipfield_type>(iterator1.element_pointer - iterator1.group_pointer->elements);

                    if (iterator1.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) // ie. if this group already has some erased elements
                    {
                        *(bitcast_pointer<skipfield_pointer_type>(iterator1.group_pointer->elements + iterator1.group_pointer->free_list_head) + 1) = index; // set prev free list head's 'next index' number to the index of the iterator1 element
                    }
                    else
                    {
                        iterator1.group_pointer->erasures_list_next_group = groups_with_erasures_list_head; // add it to the groups-with-erasures free list
                        groups_with_erasures_list_head = iterator1.group_pointer;
                    }

                    *(bitcast_pointer<skipfield_pointer_type>(iterator1.element_pointer)) = iterator1.group_pointer->free_list_head;
                    *(bitcast_pointer<skipfield_pointer_type>(iterator1.element_pointer) + 1) = std::numeric_limits<skipfield_type>::max();
                    iterator1.group_pointer->free_list_head = index;
                } else {
                    // update previous skipblock, no need to update free list:
                    *(iterator1.skipfield_pointer - previous_node_value) = *(iterator1.skipfield_pointer + distance_to_end - 1) = static_cast<skipfield_type>(previous_node_value + distance_to_end);
                }

                iterator1.group_pointer->size = static_cast<skipfield_type>(iterator1.group_pointer->size - number_of_group_erasures);
                size_ -= number_of_group_erasures;

                current.group_pointer = current.group_pointer->next_group;
            }


            // Intermediate groups:
            const group_pointer_type previous_group = current.group_pointer->previous_group;

            while (current.group_pointer != iterator2.group_pointer) {
                if constexpr (!std::is_trivially_destructible<T>::value) {
                    current.element_pointer = current.group_pointer->elements + *(current.group_pointer->skipfield);
                    current.skipfield_pointer = current.group_pointer->skipfield + *(current.group_pointer->skipfield);
                    const aligned_pointer_type end = current.group_pointer->last_endpoint;

                    do {
                        std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer)); // Destruct element
                        const skipfield_type skip = *(++current.skipfield_pointer);
                        current.element_pointer += static_cast<size_type>(skip) + 1u;
                        current.skipfield_pointer += skip;
                    } while (current.element_pointer != end);
                }

                if (current.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                    remove_from_groups_with_erasures_list(current.group_pointer);
                }

                size_ -= current.group_pointer->size;
                const group_pointer_type current_group = current.group_pointer;
                current.group_pointer = current.group_pointer->next_group;

                if (current_group != end_.group_pointer && current_group->next_group != end_.group_pointer) {
                    capacity_ -= current_group->capacity;
                    deallocate_group(current_group);
                } else {
                    add_group_to_unused_groups_list(current_group);
                }
            }

            current.element_pointer = current.group_pointer->elements + *(current.group_pointer->skipfield);
            current.skipfield_pointer = current.group_pointer->skipfield + *(current.group_pointer->skipfield);
            current.group_pointer->previous_group = previous_group;

            if (previous_group != nullptr) {
                previous_group->next_group = current.group_pointer;
            } else {
                // This line is included here primarily to avoid a secondary if statement within the if block below - it will not be needed in any other situation
                begin_ = iterator2.unconst();
            }
        }

        if (current.element_pointer == iterator2.element_pointer) // in case iterator2 was at beginning of it's group - also covers empty range case (first == last)
        {
            return iterator2.unconst();
        }

        // Final group:
        // Code explanation:
        // If not erasing entire final group, 1. Destruct elements (if non-trivial destructor) and add locations to group free list. 2. process skipfield.
        // If erasing entire group, 1. Destruct elements (if non-trivial destructor), 2. if no elements left in hive, reset the group 3. otherwise reset end_ and remove group from groups-with-erasures list (if free list of erasures present)

        if (iterator2.element_pointer != end_.element_pointer || current.element_pointer != current.group_pointer->elements + *(current.group_pointer->skipfield)) // ie. not erasing entire group
        {
            size_type number_of_group_erasures = 0;
            // Schema: first erased all non-erased elements until end of group & remove all skipblocks post-iterator2 from the free_list. Then, either update preceding skipblock or create new one:

            const const_iterator current_saved = current;

            if (std::is_trivially_destructible<T>::value && current.group_pointer->free_list_head == std::numeric_limits<skipfield_type>::max()) {
                number_of_group_erasures += static_cast<size_type>(iterator2.element_pointer - current.element_pointer);
            } else {
                while (current.element_pointer != iterator2.element_pointer) {
                    if (*current.skipfield_pointer == 0) {
                        if constexpr (!std::is_trivially_destructible<T>::value) {
                            std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer)); // Destruct element
                        }

                        ++number_of_group_erasures;
                        ++current.element_pointer;
                        ++current.skipfield_pointer;
                    }
                    else // remove skipblock from group:
                    {
                        const skipfield_type prev_free_list_index = *(bitcast_pointer<skipfield_pointer_type>(current.element_pointer));
                        const skipfield_type next_free_list_index = *(bitcast_pointer<skipfield_pointer_type>(current.element_pointer) + 1);

                        current.element_pointer += *(current.skipfield_pointer);
                        current.skipfield_pointer += *(current.skipfield_pointer);

                        if (next_free_list_index == std::numeric_limits<skipfield_type>::max() && prev_free_list_index == std::numeric_limits<skipfield_type>::max()) // if this is the last skipblock in the free list
                        {
                            remove_from_groups_with_erasures_list(iterator2.group_pointer); // remove group from list of free-list groups - will be added back in down below, but not worth optimizing for
                            iterator2.group_pointer->free_list_head = std::numeric_limits<skipfield_type>::max();
                            number_of_group_erasures += static_cast<size_type>(iterator2.element_pointer - current.element_pointer);

                            if constexpr (!std::is_trivially_destructible<T>::value) {
                                while (current.element_pointer != iterator2.element_pointer) {
                                    std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer++)); // Destruct element
                                }
                            }
                            break; // end overall while loop
                        }
                        else if (next_free_list_index == std::numeric_limits<skipfield_type>::max()) // if this is the head of the free list
                        {
                            current.group_pointer->free_list_head = prev_free_list_index;
                            *(bitcast_pointer<skipfield_pointer_type>(current.group_pointer->elements + prev_free_list_index) + 1) = std::numeric_limits<skipfield_type>::max();
                        }
                        else
                        {
                            *(bitcast_pointer<skipfield_pointer_type>(current.group_pointer->elements + next_free_list_index)) = prev_free_list_index;

                            if (prev_free_list_index != std::numeric_limits<skipfield_type>::max()) // ie. not the tail free list node
                            {
                                *(bitcast_pointer<skipfield_pointer_type>(current.group_pointer->elements + prev_free_list_index) + 1) = next_free_list_index;
                            }
                        }
                    }
                }
            }

            const skipfield_type distance_to_iterator2 = static_cast<skipfield_type>(iterator2.element_pointer - current_saved.element_pointer);
            const skipfield_type index = static_cast<skipfield_type>(current_saved.element_pointer - iterator2.group_pointer->elements);

            if (index == 0 || *(current_saved.skipfield_pointer - 1) == 0) // element is either at start of group or previous skipfield node is 0
            {
                *(current_saved.skipfield_pointer) = distance_to_iterator2;
                *(iterator2.skipfield_pointer - 1) = distance_to_iterator2;

                if (iterator2.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) // ie. if this group already has some erased elements
                {
                    *(bitcast_pointer<skipfield_pointer_type>(iterator2.group_pointer->elements + iterator2.group_pointer->free_list_head) + 1) = index;
                }
                else
                {
                    iterator2.group_pointer->erasures_list_next_group = groups_with_erasures_list_head; // add it to the groups-with-erasures free list
                    groups_with_erasures_list_head = iterator2.group_pointer;
                }

                *(bitcast_pointer<skipfield_pointer_type>(current_saved.element_pointer)) = iterator2.group_pointer->free_list_head;
                *(bitcast_pointer<skipfield_pointer_type>(current_saved.element_pointer) + 1) = std::numeric_limits<skipfield_type>::max();
                iterator2.group_pointer->free_list_head = index;
            }
            else // If iterator 1 & 2 are in same group, but iterator 1 was not at start of group, and previous skipfield node is an end node in a skipblock:
            {
                // Just update existing skipblock, no need to create new free list node:
                const skipfield_type prev_node_value = *(current_saved.skipfield_pointer - 1);
                *(current_saved.skipfield_pointer - prev_node_value) = static_cast<skipfield_type>(prev_node_value + distance_to_iterator2);
                *(iterator2.skipfield_pointer - 1) = static_cast<skipfield_type>(prev_node_value + distance_to_iterator2);
            }


            if (iterator1.element_pointer == begin_.element_pointer)
            {
                begin_ = iterator2.unconst();
            }

            iterator2.group_pointer->size = static_cast<skipfield_type>(iterator2.group_pointer->size - number_of_group_erasures);
            size_ -= number_of_group_erasures;
        } else {
            if constexpr (!std::is_trivially_destructible<T>::value) {
                while (current.element_pointer != iterator2.element_pointer) {
                    std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer));
                    ++current.skipfield_pointer;
                    current.element_pointer += static_cast<size_type>(*current.skipfield_pointer) + 1u;
                    current.skipfield_pointer += *current.skipfield_pointer;
                }
            }


            if ((size_ -= current.group_pointer->size) != 0) {
                // ie. either previous_group != nullptr or next_group != nullptr
                if (current.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                    remove_from_groups_with_erasures_list(current.group_pointer);
                }

                current.group_pointer->previous_group->next_group = current.group_pointer->next_group;

                if (current.group_pointer == end_.group_pointer) {
                    end_.group_pointer = current.group_pointer->previous_group;
                    end_.element_pointer = end_.group_pointer->last_endpoint;
                    end_.skipfield_pointer = end_.group_pointer->skipfield + end_.group_pointer->capacity;
                    add_group_to_unused_groups_list(current.group_pointer);
                    return end_;
                } else if (current.group_pointer == begin_.group_pointer) {
                    begin_.group_pointer = current.group_pointer->next_group;
                    const skipfield_type skip = *(begin_.group_pointer->skipfield);
                    begin_.element_pointer = begin_.group_pointer->elements + skip;
                    begin_.skipfield_pointer = begin_.group_pointer->skipfield + skip;
                }

                if (current.group_pointer->next_group != end_.group_pointer) {
                    capacity_ -= current.group_pointer->capacity;
                } else {
                    add_group_to_unused_groups_list(current.group_pointer);
                    return iterator2.unconst();
                }
            } else {
                // Reset skipfield and free list rather than clearing - leads to fewer allocations/deallocations:
                reset_only_group_left(current.group_pointer);
                return end_;
            }

            deallocate_group(current.group_pointer);
        }

        return iterator2.unconst();
    }

private:
    void prepare_groups_for_assign(size_type size) {
        // Destroy all elements if non-trivial:
        if constexpr (!std::is_trivially_destructible<T>::value) {
            for (iterator current = begin_; current != end_; ++current) {
                std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer)); // Destruct element
            }
        }

        if (size < capacity_ && (capacity_ - size) >= min_group_capacity_) {
            size_type difference = capacity_ - size;
            end_.group_pointer->next_group = unused_groups_head;

            // Remove surplus groups which're under the difference limit:
            group_pointer_type current_group = begin_.group_pointer;
            group_pointer_type previous_group = nullptr;

            do {
                const group_pointer_type next_group = current_group->next_group;

                if (current_group->capacity <= difference) {
                    // Remove group:
                    difference -= current_group->capacity;
                    capacity_ -= current_group->capacity;
                    deallocate_group(current_group);
                    if (current_group == begin_.group_pointer) {
                        begin_.group_pointer = next_group;
                    }
                } else {
                    if (previous_group != nullptr) {
                        previous_group->next_group = current_group;
                    }
                    previous_group = current_group;
                }
                current_group = next_group;
            } while (current_group != nullptr);

            previous_group->next_group = nullptr;
        } else {
            if (size > capacity_) {
                reserve(size);
            }

            // Join all unused_groups to main chain:
            end_.group_pointer->next_group = unused_groups_head;
        }

        begin_.element_pointer = begin_.group_pointer->elements;
        begin_.skipfield_pointer = begin_.group_pointer->skipfield;
        groups_with_erasures_list_head = nullptr;
        size_ = 0;
    }

private:
    template<class It, class Sent>
    inline void range_assign_impl(It first, Sent last) {
        if (first == last) {
            reset();
        } else {
#if __cpp_lib_ranges >= 201911
            size_type n = std::ranges::distance(first, last);
#else
            size_type n = std::distance(first, last);
#endif
            prepare_groups_for_assign(n);
            range_fill_unused_groups(n, std::move(first), 0, nullptr, begin_.group_pointer);
        }
    }

public:
    inline void assign(size_type n, const T& value) {
        if (n == 0) {
            reset();
        } else {
            prepare_groups_for_assign(n);
            fill_unused_groups(n, value, 0, nullptr, begin_.group_pointer);
        }
    }

    inline void assign(std::initializer_list<T> il) {
        range_assign_impl(il.begin(), il.end());
    }

#if __cpp_lib_ranges >= 201911
    template <std::input_or_output_iterator It, std::sentinel_for<It> Sent>
    inline void assign(It first, Sent last) {
        range_assign_impl(std::move(first), std::move(last));
    }

    template<std::ranges::range R>
    inline void assign_range(R&& rg) {
        range_assign_impl(std::ranges::begin(rg), std::ranges::end(rg));
    }
#else
    template <class It, class = std::enable_if_t<!std::is_integral<It>::value>>
    inline void assign(It first, It last) {
        range_assign_impl(std::move(first), std::move(last));
    }
#endif

    [[nodiscard]] inline bool empty() const noexcept { return size_ == 0; }
    inline size_type size() const noexcept { return size_; }
    inline size_type max_size() const noexcept { return std::allocator_traits<allocator_type>::max_size(get_allocator()); }
    inline size_type capacity() const noexcept { return capacity_; }

private:
    // get all elements contiguous in memory and shrink to fit, remove erasures and erasure free lists. Invalidates all iterators and pointers to elements.
    void consolidate() {
        hive temp(plf::hive_limits(min_group_capacity_, max_group_capacity_));
        temp.range_assign_impl(std::make_move_iterator(begin()), std::make_move_iterator(end()));
        this->swap(temp);
    }

public:
    void reshape(plf::hive_limits limits) {
        static_assert(std::is_move_constructible<T>::value, "");
        check_limits(limits);
        min_group_capacity_ = static_cast<skipfield_type>(limits.min);
        max_group_capacity_ = static_cast<skipfield_type>(limits.max);

        // Need to check all group sizes here, because splice might append smaller blocks to the end of a larger block:
        for (group_pointer_type current = begin_.group_pointer; current != end_.group_pointer; current = current->next_group) {
            if (current->capacity < min_group_capacity_ || current->capacity > max_group_capacity_) {
                consolidate();
                return;
            }
        }
    }

    inline plf::hive_limits block_capacity_limits() const noexcept {
        return plf::hive_limits(min_group_capacity_, max_group_capacity_);
    }

    constexpr static inline plf::hive_limits block_capacity_hard_limits() noexcept {
        return plf::hive_limits(3, std::numeric_limits<skipfield_type>::max());
    }

    void clear() noexcept {
        if (size_ == 0) {
            return;
        }

        // Destroy all elements if element type is non-trivial:
        if constexpr (!std::is_trivially_destructible<T>::value) {
            for (iterator current = begin_; current != end_; ++current) {
                std::allocator_traits<allocator_type>::destroy(elt_allocator(), bitcast_pointer<pointer>(current.element_pointer));
            }
        }

        if (begin_.group_pointer != end_.group_pointer) {
            // Move all other groups onto the unused_groups list
            end_.group_pointer->next_group = unused_groups_head;
            unused_groups_head = begin_.group_pointer->next_group;
            end_.group_pointer = begin_.group_pointer; // other parts of iterator reset in the function below
        }

        reset_only_group_left(begin_.group_pointer);
        groups_with_erasures_list_head = nullptr;
        size_ = 0;
    }

    hive& operator=(const hive& source) {
        if constexpr (std::allocator_traits<allocator_type>::propagate_on_container_copy_assignment::value) {
            allocator_type source_allocator(source);
            if (!std::allocator_traits<allocator_type>::is_always_equal::value && get_allocator() != source.get_allocator()) {
                // Deallocate existing blocks as source allocator is not necessarily able to do so
                reset();
            }
            static_cast<allocator_type &>(*this) = source.get_allocator();
        }
        range_assign_impl(source.begin(), source.end());
        return *this;
    }

    hive& operator=(hive&& source)
        noexcept(std::allocator_traits<allocator_type>::propagate_on_container_move_assignment::value || std::allocator_traits<allocator_type>::is_always_equal::value)
    {
        assert(&source != this);
        destroy_all_data();

        bool should_use_source_allocator = (
            std::allocator_traits<allocator_type>::propagate_on_container_move_assignment::value ||
            std::allocator_traits<allocator_type>::is_always_equal::value ||
            this->get_allocator() == source.get_allocator()
        );
        if (should_use_source_allocator) {
            constexpr bool can_just_memcpy = (
                std::is_trivially_copyable<allocator_type>::value &&
                std::is_trivial<group_pointer_type>::value &&
                std::is_trivial<aligned_pointer_type>::value &&
                std::is_trivial<skipfield_pointer_type>::value
            );
            if constexpr (can_just_memcpy) {
                std::memcpy(static_cast<void *>(this), &source, sizeof(hive));
            } else {
                end_ = std::move(source.end_);
                begin_ = std::move(source.begin_);
                groups_with_erasures_list_head = std::move(source.groups_with_erasures_list_head);
                unused_groups_head = std::move(source.unused_groups_head);
                size_ = source.size_;
                capacity_ = source.capacity_;
                min_group_capacity_ = source.min_group_capacity_;
                max_group_capacity_ = source.max_group_capacity_;

                if constexpr (std::allocator_traits<allocator_type>::propagate_on_container_move_assignment::value) {
                    static_cast<allocator_type &>(*this) = std::move(static_cast<allocator_type &>(source));
                }
            }
        } else {
            reserve(source.size());
            range_assign_impl(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
            source.destroy_all_data();
        }

        source.blank();
        return *this;
    }

    inline hive& operator=(std::initializer_list<T> il) {
        range_assign_impl(il.begin(), il.end());
        return *this;
    }

    void shrink_to_fit() {
        static_assert(std::is_move_constructible<T>::value, "");
        if (size_ == 0) {
            reset();
        } else if (size_ != capacity_) {
            consolidate();
        }
    }

    void trim() noexcept {
        while (unused_groups_head != nullptr) {
            capacity_ -= unused_groups_head->capacity;
            const group_pointer_type next_group = unused_groups_head->next_group;
            deallocate_group(unused_groups_head);
            unused_groups_head = next_group;
        }
    }

    void reserve(size_type new_capacity) {
        if (new_capacity <= capacity_) {
            return;
        }

        if (new_capacity > max_size()) {
            throw std::length_error("Capacity requested via reserve() greater than max_size()");
        }

        new_capacity -= capacity_;

        size_type number_of_max_groups = new_capacity / max_group_capacity_;
        skipfield_type remainder = static_cast<skipfield_type>(new_capacity - (number_of_max_groups * max_group_capacity_));

        if (remainder == 0) {
            remainder = max_group_capacity_;
            --number_of_max_groups;
        } else if (remainder < min_group_capacity_) {
            remainder = min_group_capacity_;
        }

        group_pointer_type current_group;
        group_pointer_type first_unused_group;

        if (begin_.group_pointer == nullptr) {
            initialize(remainder);
            begin_.group_pointer->last_endpoint = begin_.group_pointer->elements; // last_endpoint initially == elements + 1 via default constructor
            begin_.group_pointer->size = 0; // 1 by default

            if (number_of_max_groups == 0) {
                return;
            } else {
                first_unused_group = current_group = allocate_new_group(max_group_capacity_, begin_.group_pointer);
                capacity_ += max_group_capacity_;
                --number_of_max_groups;
            }
        } else {
            first_unused_group = current_group = allocate_new_group(remainder, end_.group_pointer);
            capacity_ += remainder;
        }

        while (number_of_max_groups != 0) {
            hive_try_rollback([&]() {
                current_group->next_group = allocate_new_group(max_group_capacity_, current_group);
            }, [&]() {
                deallocate_group(current_group->next_group);
                current_group->next_group = unused_groups_head;
                unused_groups_head = first_unused_group;
            });
            current_group = current_group->next_group;
            capacity_ += max_group_capacity_;
            --number_of_max_groups;
        }
        current_group->next_group = unused_groups_head;
        unused_groups_head = first_unused_group;
    }

private:
    iterator get_it(const_pointer p) const {
        if (size_ != 0) {
            // Necessary here to prevent a pointer matching to an empty hive with one memory block retained with the skipfield wiped (see erase())
            // Start with last group first, as will be the largest group in most cases:
            for (group_pointer_type current_group = end_.group_pointer; current_group != nullptr; current_group = current_group->previous_group) {
                auto ap = bitcast_pointer<aligned_pointer_type>(pointer(p));
                if (current_group->elements <= ap && ap < bitcast_pointer<aligned_pointer_type>(current_group->skipfield)) {
                    auto skipfield_pointer = current_group->skipfield + (ap - current_group->elements);
                    return (*skipfield_pointer == 0) ? iterator(current_group, ap, skipfield_pointer) : end_;
                }
            }
        }
        return end_;
    }

    inline allocator_type& elt_allocator() { return allocator_; }

public:
    inline iterator get_iterator(const_pointer p) noexcept { return get_it(p); }
    inline const_iterator get_iterator(const_pointer p) const noexcept { return get_it(p); }
    inline allocator_type get_allocator() const noexcept { return allocator_; }

    void splice(hive& source) {
        // Process: if there are unused memory spaces at the end of the current back group of the chain, convert them
        // to skipped elements and add the locations to the group's free list.
        // Then link the destination's groups to the source's groups and nullify the source.
        // If the source has more unused memory spaces in the back group than the destination,
        // swap them before processing to reduce the number of locations added to a free list and also subsequent jumps during iteration.

        assert(&source != this);

        if (source.size_ == 0) {
            return;
        } else if (size_ == 0) {
            *this = std::move(source);
            return;
        }

        // If there's more unused element locations in back memory block of destination than in back memory block of source, swap with source to reduce number of skipped elements during iteration, and reduce size of free-list:
        if ((bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield) - end_.element_pointer) > (bitcast_pointer<aligned_pointer_type>(source.end_.group_pointer->skipfield) - source.end_.element_pointer)) {
            swap(source);
        }

        // Throw if incompatible group capacity found:
        if (source.min_group_capacity_ < min_group_capacity_ || source.max_group_capacity_ > max_group_capacity_) {
            for (group_pointer_type current_group = source.begin_.group_pointer; current_group != nullptr; current_group = current_group->next_group) {
                if (current_group->capacity < min_group_capacity_ || current_group->capacity > max_group_capacity_) {
                    throw std::length_error("A source memory block capacity is outside of the destination's minimum or maximum memory block capacity limits - please change either the source or the destination's min/max block capacity limits using reshape() before calling splice() in this case");
                }
            }
        }

        // Add source list of groups-with-erasures to destination list of groups-with-erasures:
        if (source.groups_with_erasures_list_head != nullptr) {
            if (groups_with_erasures_list_head != nullptr) {
                group_pointer_type tail_group = groups_with_erasures_list_head;
                while (tail_group->erasures_list_next_group != nullptr) {
                    tail_group = tail_group->erasures_list_next_group;
                }
                tail_group->erasures_list_next_group = source.groups_with_erasures_list_head;
            } else {
                groups_with_erasures_list_head = source.groups_with_erasures_list_head;
            }
        }

        const skipfield_type distance_to_end = static_cast<skipfield_type>(bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield) - end_.element_pointer);

        if (distance_to_end != 0) {
            // Mark unused element memory locations from back group as skipped/erased:
            // Update skipfield:
            const skipfield_type previous_node_value = *(end_.skipfield_pointer - 1);
            end_.group_pointer->last_endpoint = bitcast_pointer<aligned_pointer_type>(end_.group_pointer->skipfield);

            if (previous_node_value == 0) {
                end_.skipfield_pointer[0] = distance_to_end;
                end_.skipfield_pointer[distance_to_end - 1] = distance_to_end;

                const skipfield_type index = static_cast<skipfield_type>(end_.element_pointer - end_.group_pointer->elements);

                if (end_.group_pointer->free_list_head != std::numeric_limits<skipfield_type>::max()) {
                    // ie. if this group already has some erased elements
                    *(bitcast_pointer<skipfield_pointer_type>(end_.group_pointer->elements + end_.group_pointer->free_list_head) + 1) = index; // set prev free list head's 'next index' number to the index of the current element
                } else {
                    end_.group_pointer->erasures_list_next_group = groups_with_erasures_list_head; // add it to the groups-with-erasures free list
                    groups_with_erasures_list_head = end_.group_pointer;
                }

                bitcast_pointer<skipfield_pointer_type>(end_.element_pointer)[0] = end_.group_pointer->free_list_head;
                bitcast_pointer<skipfield_pointer_type>(end_.element_pointer)[1] = std::numeric_limits<skipfield_type>::max();
                end_.group_pointer->free_list_head = index;
            } else {
                // update previous skipblock, no need to update free list:
                *(end_.skipfield_pointer - previous_node_value) = *(end_.skipfield_pointer + distance_to_end - 1) = static_cast<skipfield_type>(previous_node_value + distance_to_end);
            }
        }

        // Update subsequent group numbers:
        group_pointer_type current_group = source.begin_.group_pointer;
        size_type current_group_number = end_.group_pointer->group_number;

        do {
            current_group->group_number = ++current_group_number;
            current_group = current_group->next_group;
        } while (current_group != nullptr);

        // Join the destination and source group chains:
        end_.group_pointer->next_group = source.begin_.group_pointer;
        source.begin_.group_pointer->previous_group = end_.group_pointer;
        end_ = source.end_;
        size_ += source.size_;
        capacity_ += source.capacity_;

        // Remove source unused groups:
        source.trim();
        source.blank();
    }

    inline void splice(hive&& source) { this->splice(source); }

private:
    struct item_index_tuple {
        pointer original_location;
        size_type original_index;

        item_index_tuple(pointer _item, size_type _index) :
            original_location(_item),
            original_index(_index)
        {}
    };

public:
    template <class Comp>
    void sort(Comp less) {
        if (size_ <= 1) {
            return;
        }

        auto tuple_allocator = tuple_allocator_type(get_allocator());
        tuple_pointer_type sort_array = std::allocator_traits<tuple_allocator_type>::allocate(tuple_allocator, size_, nullptr);
        tuple_pointer_type tuple_pointer = sort_array;

        // Construct pointers to all elements in the sequence:
        size_type index = 0;

        for (auto it = begin_; it != end_; ++it, ++tuple_pointer, ++index) {
            std::allocator_traits<tuple_allocator_type>::construct(tuple_allocator, tuple_pointer, std::addressof(*it), index);
        }

        // Now, sort the pointers by the values they point to:
        std::sort(sort_array, tuple_pointer, [&](const auto& a, const auto& b) { return less(*a.original_location, *b.original_location); });

        // Sort the actual elements via the tuple array:
        index = 0;

        for (tuple_pointer_type current_tuple = sort_array; current_tuple != tuple_pointer; ++current_tuple, ++index) {
            if (current_tuple->original_index != index) {
                T end_value = std::move(*(current_tuple->original_location));
                size_type destination_index = index;
                size_type source_index = current_tuple->original_index;

                do {
                    *(sort_array[destination_index].original_location) = std::move(*(sort_array[source_index].original_location));
                    destination_index = source_index;
                    source_index = sort_array[destination_index].original_index;
                    sort_array[destination_index].original_index = destination_index;
                } while (source_index != index);

                *(sort_array[destination_index].original_location) = std::move(end_value);
            }
        }
        std::allocator_traits<tuple_allocator_type>::deallocate(tuple_allocator, sort_array, size_);
    }

    inline void sort() { sort(std::less<T>()); }

    template<class Comp>
    size_type unique(Comp eq) {
        size_type count = 0;
        auto end = cend();
        for (auto it = cbegin(); it != end; ) {
            auto previous = it++;
            if (eq(*it, *previous)) {
                auto orig = ++count;
                auto last = it;
                while (++last != end && eq(*last, *previous)) {
                    ++count;
                }
                if (count != orig) {
                    it = erase(it, last);
                } else {
                    it = erase(it);
                }
                end = cend();
            }
        }
        return count;
    }

    inline size_type unique() { return unique(std::equal_to<T>()); }

    friend void swap(hive& a, hive& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

    void swap(hive &source)
        noexcept(std::allocator_traits<allocator_type>::propagate_on_container_swap::value || std::allocator_traits<allocator_type>::is_always_equal::value)
    {
        if constexpr (std::allocator_traits<allocator_type>::is_always_equal::value && std::is_trivial<group_pointer_type>::value && std::is_trivial<aligned_pointer_type>::value && std::is_trivial<skipfield_pointer_type>::value) {
            // if all pointer types are trivial we can just copy using memcpy - avoids constructors/destructors etc and is faster
            char temp[sizeof(hive)];
            std::memcpy(&temp, static_cast<void *>(this), sizeof(hive));
            std::memcpy(static_cast<void *>(this), static_cast<void *>(&source), sizeof(hive));
            std::memcpy(static_cast<void *>(&source), &temp, sizeof(hive));
        } else if constexpr (std::is_move_assignable<group_pointer_type>::value && std::is_move_assignable<aligned_pointer_type>::value && std::is_move_assignable<skipfield_pointer_type>::value && std::is_move_constructible<group_pointer_type>::value && std::is_move_constructible<aligned_pointer_type>::value && std::is_move_constructible<skipfield_pointer_type>::value) {
            hive temp = std::move(source);
            source = std::move(*this);
            *this = std::move(temp);
        } else {
            const iterator swap_end_ = end_;
            const iterator swap_begin_ = begin_;
            const group_pointer_type swap_groups_with_erasures_list_head = groups_with_erasures_list_head;
            const group_pointer_type swap_unused_groups_head = unused_groups_head;
            const size_type swap_size_ = size_;
            const size_type swap_capacity_ = capacity_;
            const skipfield_type swap_min_group_capacity = min_group_capacity_;
            const skipfield_type swap_max_group_capacity = max_group_capacity_;

            end_ = source.end_;
            begin_ = source.begin_;
            groups_with_erasures_list_head = source.groups_with_erasures_list_head;
            unused_groups_head = source.unused_groups_head;
            size_ = source.size_;
            capacity_ = source.capacity_;
            min_group_capacity_ = source.min_group_capacity_;
            max_group_capacity_ = source.max_group_capacity_;

            source.end_ = swap_end_;
            source.begin_ = swap_begin_;
            source.groups_with_erasures_list_head = swap_groups_with_erasures_list_head;
            source.unused_groups_head = swap_unused_groups_head;
            source.size_ = swap_size_;
            source.capacity_ = swap_capacity_;
            source.min_group_capacity_ = swap_min_group_capacity;
            source.max_group_capacity_ = swap_max_group_capacity;

            if constexpr (std::allocator_traits<allocator_type>::propagate_on_container_swap::value && !std::allocator_traits<allocator_type>::is_always_equal::value) {
                using std::swap;
                swap(static_cast<allocator_type &>(*this), static_cast<allocator_type &>(source));
            }
        }
    }
}; // hive

} // namespace plf

namespace std {

    template<class T, class A, class P, class Pred>
    typename plf::hive<T, A, P>::size_type erase_if(plf::hive<T, A, P>& h, Pred pred) {
        typename plf::hive<T, A, P>::size_type count = 0;
        auto end = h.end();
        for (auto it = h.begin(); it != end; ++it) {
            if (pred(*it)) {
                auto orig = ++count;
                auto last = it;
                while (++last != end && pred(*last)) {
                    ++count;
                }
                if (count != orig) {
                    it = h.erase(it, last);
                } else {
                    it = h.erase(it);
                }
                end = h.end();
                if (it == end) {
                    break;
                }
            }
        }
        return count;
    }

    template<class T, class A, class P>
    inline typename plf::hive<T, A, P>::size_type erase(plf::hive<T, A, P>& h, const T& value) {
        return std::erase_if(h, [&](const T &x) { return x == value; });
    }
} // namespace std

#endif // PLF_HIVE_H
