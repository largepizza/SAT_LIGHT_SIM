#pragma once
// SegVec: a vector in fixed-size segments (2026-10-21, the hitch pass). Growing never moves what is already there:
// a std::vector of ~10M Kessler events (84 B each) reallocated and copied ~0.4-0.8 GB in one frame each time it
// doubled late in a cascade. Random access is one shift and one mask; the elements of a segment are contiguous.
// (std::deque would do, but MSVC's keeps one element per block for elements over 8 bytes.)

#include <cstddef>
#include <memory>
#include <vector>

template <class T, unsigned Bits = 16>
class SegVec
{
  public:
    static constexpr size_t kSeg = size_t(1) << Bits;
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T &operator[](size_t i) { return segs_[i >> Bits][i & (kSeg - 1)]; }
    const T &operator[](size_t i) const { return segs_[i >> Bits][i & (kSeg - 1)]; }
    T &back() { return (*this)[n_ - 1]; }
    void clear()
    {
        segs_.clear();
        n_ = 0;
    }
    void push_back(const T &v)
    {
        if ((n_ >> Bits) >= segs_.size())
            segs_.emplace_back(new T[kSeg]);
        (*this)[n_++] = v;
    }
    template <class It>
    void append(It first, It last)
    {
        for (; first != last; ++first)
            push_back(*first);
    }
    // Shrinks only (what the rewind paths need), or grows with default elements.
    void resize(size_t n)
    {
        while (n_ < n)
            push_back(T{});
        n_ = n;
        segs_.resize((n_ + kSeg - 1) >> Bits);
    }
    template <class It>
    void assign(It first, It last)
    {
        clear();
        append(first, last);
    }
    // The first index in [0, size) whose element is not less than key (less(element, key)), as std::lower_bound.
    template <class K, class Less>
    size_t lowerBound(const K &key, Less less) const
    {
        size_t lo = 0, hi = n_;
        while (lo < hi)
        {
            const size_t mid = lo + (hi - lo) / 2;
            if (less((*this)[mid], key))
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

  private:
    std::vector<std::unique_ptr<T[]>> segs_;
    size_t n_ = 0;
};
