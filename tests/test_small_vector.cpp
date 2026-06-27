#include <gtest/gtest.h>
#include <string>
#include <utility>

#include "utils/SmallVector.h"

using namespace usbipdcpp;

// ============== SmallVector Tests ==============

TEST(SmallVector, EmptyVector) {
    SmallVector<int, 4> vec;

    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_EQ(vec.capacity(), 4u);
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, PushBackStack) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    EXPECT_FALSE(vec.empty());
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec.capacity(), 4u);
    EXPECT_FALSE(vec.on_heap());

    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
}

TEST(SmallVector, PushBackHeap) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    EXPECT_FALSE(vec.on_heap());

    vec.push_back(2);
    EXPECT_FALSE(vec.on_heap());

    vec.push_back(3); // spills to heap
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 3u);

    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
}

TEST(SmallVector, EmplaceBack) {
    SmallVector<std::pair<int, std::string>, 2> vec;

    vec.emplace_back(1, "one");
    vec.emplace_back(2, "two");
    EXPECT_FALSE(vec.on_heap());

    vec.emplace_back(3, "three");
    EXPECT_TRUE(vec.on_heap());

    EXPECT_EQ(vec[0].first, 1);
    EXPECT_EQ(vec[0].second, "one");
    EXPECT_EQ(vec[2].first, 3);
    EXPECT_EQ(vec[2].second, "three");
}

TEST(SmallVector, PopBack) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    vec.pop_back();
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec.back(), 2);

    vec.pop_back();
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec.back(), 1);
}

TEST(SmallVector, Clear) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    vec.clear();
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_FALSE(vec.on_heap()); // back on stack after clearing
}

TEST(SmallVector, Resize) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);

    // Expand (still on stack)
    vec.resize(4);
    EXPECT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap());

    // Expand to heap
    vec.resize(10);
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 10u);

    // Shrink back to stack
    vec.resize(2);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 2u);
}

TEST(SmallVector, Iterators) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    int sum = 0;
    for (auto& v : vec) {
        sum += v;
    }
    EXPECT_EQ(sum, 6);

    // const iterator
    const auto& cvec = vec;
    sum = 0;
    for (auto it = cvec.begin(); it != cvec.end(); ++it) {
        sum += *it;
    }
    EXPECT_EQ(sum, 6);
}

TEST(SmallVector, At) {
    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);

    EXPECT_EQ(vec.at(0), 10);
    EXPECT_EQ(vec.at(1), 20);

    EXPECT_THROW((void)vec.at(2), std::out_of_range);
    EXPECT_THROW((void)vec.at(100), std::out_of_range);
}

TEST(SmallVector, FrontBack) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    EXPECT_EQ(vec.front(), 1);
    EXPECT_EQ(vec.back(), 3);
}

TEST(SmallVector, Data) {
    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);

    int* ptr = vec.data();
    EXPECT_EQ(ptr[0], 10);
    EXPECT_EQ(ptr[1], 20);
}

TEST(SmallVector, CopyConstructor) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3); // heap

    SmallVector<int, 2> vec2(vec1);
    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec2[0], 1);
    EXPECT_EQ(vec2[1], 2);
    EXPECT_EQ(vec2[2], 3);
}

TEST(SmallVector, MoveConstructor) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2(std::move(vec1));
    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec1.size(), 0u);
    EXPECT_FALSE(vec1.on_heap());
}

TEST(SmallVector, CopyAssignment) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2;
    vec2 = vec1;

    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec2[0], 1);
    EXPECT_EQ(vec2[2], 3);
}

TEST(SmallVector, MoveAssignment) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2;
    vec2 = std::move(vec1);

    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_EQ(vec1.size(), 0u);
}

TEST(SmallVector, Reserve) {
    SmallVector<int, 2> vec;

    vec.reserve(10); // greater than N, migrate to heap
    EXPECT_TRUE(vec.on_heap());
    EXPECT_GE(vec.capacity(), 10u);
    EXPECT_EQ(vec.size(), 0u);

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_EQ(vec.size(), 3u);
}

TEST(SmallVector, ComplexType) {
    struct NonTrivial {
        std::string s;
        int* p = nullptr;

        NonTrivial() = default;
        NonTrivial(std::string str, int* ptr) : s(std::move(str)), p(ptr) {}

        // Ensure copy/move are correct
        NonTrivial(const NonTrivial& other) : s(other.s), p(other.p) {}
        NonTrivial& operator=(const NonTrivial& other) {
            s = other.s;
            p = other.p;
            return *this;
        }
    };

    int x = 42;
    SmallVector<NonTrivial, 2> vec;

    vec.emplace_back("first", &x);
    vec.emplace_back("second", &x);
    EXPECT_FALSE(vec.on_heap());

    vec.emplace_back("third", &x);
    EXPECT_TRUE(vec.on_heap());

    EXPECT_EQ(vec[0].s, "first");
    EXPECT_EQ(vec[0].p, &x);
    EXPECT_EQ(vec[2].s, "third");
}

// ============== Edge Case Tests ==============

TEST(SmallVector, ExactlyNElements) {
    // Exactly at the stack capacity boundary
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    vec.push_back(4); // exactly full

    EXPECT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap()); // still on stack

    vec.push_back(5); // overflow
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 5u);
}

TEST(SmallVector, ResizeToZero) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    vec.resize(0);
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_TRUE(vec.empty());
    EXPECT_FALSE(vec.on_heap()); // resize(0) returns to stack

    // Can be reused
    vec.push_back(10);
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 10);
}

TEST(SmallVector, EmptyOperations) {
    SmallVector<int, 4> vec;

    // Empty container operations
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_EQ(vec.begin(), vec.end());

    vec.pop_back(); // pop_back on empty container (undefined behavior, but must not crash)
    vec.clear();    // clear on empty container

    EXPECT_TRUE(vec.empty());
}

TEST(SmallVector, LargeData) {
    SmallVector<int, 4> vec;

    // Large amount of data
    for (int i = 0; i < 1000; ++i) {
        vec.push_back(i);
    }

    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 1000u);

    // Verify data is correct
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(vec[i], i);
    }
}

TEST(SmallVector, SelfAssignment) {
    SmallVector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(2);

    // Self-assignment protection
    vec = vec;
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
}

TEST(SmallVector, ReserveOnStack) {
    SmallVector<int, 4> vec;

    vec.reserve(2); // less than N, still on stack
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.capacity(), 4u); // stack capacity unchanged

    vec.reserve(4); // equal to N, still on stack
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, MultipleStackHeapTransitions) {
    SmallVector<int, 2> vec;

    // stack -> heap
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    // heap -> stack (via resize)
    vec.resize(1);
    EXPECT_FALSE(vec.on_heap());

    // stack -> heap
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    // heap -> stack (via clear)
    vec.clear();
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, StressTest) {
    SmallVector<int, 4> vec;

    // Many push/pop operations
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 100; ++i) {
            vec.push_back(i);
        }
        for (int i = 0; i < 50; ++i) {
            vec.pop_back();
        }
    }

    EXPECT_GT(vec.size(), 0u);
}
