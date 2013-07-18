/******************************************************************************
 * Copyright © 2012-2013 Institut für Nachrichtentechnik, Universität Rostock *
 * Copyright © 2006-2012 Quality & Usability Lab,                             *
 *                       Telekom Innovation Laboratories, TU Berlin           *
 *                                                                            *
 * This file is part of the Audio Processing Framework (APF).                 *
 *                                                                            *
 * The APF is free software:  you can redistribute it and/or modify it  under *
 * the terms of the  GNU  General  Public  License  as published by the  Free *
 * Software Foundation, either version 3 of the License,  or (at your option) *
 * any later version.                                                         *
 *                                                                            *
 * The APF is distributed in the hope that it will be useful, but WITHOUT ANY *
 * WARRANTY;  without even the implied warranty of MERCHANTABILITY or FITNESS *
 * FOR A PARTICULAR PURPOSE.                                                  *
 * See the GNU General Public License for more details.                       *
 *                                                                            *
 * You should  have received a copy  of the GNU General Public License  along *
 * with this program.  If not, see <http://www.gnu.org/licenses/>.            *
 *                                                                            *
 *                                 http://AudioProcessingFramework.github.com *
 ******************************************************************************/

/// @file
/// Convolution engine.

#ifndef APF_CONVOLVER_H
#define APF_CONVOLVER_H

#include <utility>  // for std::pair
#include <algorithm>  // for std::transform()
#include <cassert>

#ifdef __SSE__
#include <xmmintrin.h>  // for SSE instrinsics
#endif

#include "apf/math.h"
#include "apf/fftwtools.h"  // for fftw_allocator and fftw traits
#include "apf/container.h"  // for fixed_vector, fixed_list
#include "apf/iterator.h"  // for make_*_iterator()

namespace apf
{

/** Convolution engine.
 * A convolution engine normally consists of an Input, a Filter and
 * an Output / StaticOutput.
 * There are also combinations:
 * Input + Output = Convolver; Input + StaticOutput = StaticConvolver
 *
 * Uses (uniformly) partitioned convolution.
 *
 * TODO: describe thread (un)safety
 **/
namespace conv
{

/// Calculate necessary number of partitions for a given filter length
static size_t min_partitions(size_t block_size, size_t filter_size)
{
  return (filter_size + block_size - 1) / block_size;
}

/// Two blocks of time-domain or FFT (half-complex) data.
struct fft_node : fixed_vector<float, fftw_allocator<float> >
{
  explicit fft_node(size_t n)
    : fixed_vector<float, fftw_allocator<float> >(n)
    , zero(true)
  {}

  fft_node& operator=(const fft_node& rhs)
  {
    assert(this->size() == rhs.size());

    if (rhs.zero)
    {
      this->zero = true;
    }
    else
    {
      std::copy(rhs.begin(), rhs.end(), this->begin());
      this->zero = false;
    }
    return *this;
  }

  // WARNING: The 'zero' flag allows saving computation power, but it also
  // raises the risk of programming errors! Handle with care!

  /// To avoid unnecessary FFTs and filling buffers with zeros.
  /// @note If zero == true, the buffer itself is not necessarily all zeros!
  bool zero;
};

/// Container holding a number of FFT blocks.
struct Filter : fixed_vector<fft_node>
{
  /// Constructor; create empty filter.
  Filter(size_t block_size_, size_t partitions_)
    : fixed_vector<fft_node>(std::make_pair(partitions_, block_size_ * 2))
  {
    assert(this->partitions() > 0);
  }

  /// Constructor from std::pair, forwarded to fixed_vector constructor.
  template<typename Size, typename Initializer>
  Filter(const std::pair<Size, Initializer>& arg)
    : fixed_vector<fft_node>(arg)
  {
    assert(this->partitions() > 0);
  }

  /// Constructor from time domain coefficients.
  template<typename In>
  Filter(size_t block_size_, In first, In last, size_t partitions_ = 0);
  // Implementation below, after definition of Transform

  size_t block_size() const { return front().size() / 2; }
  size_t partition_size() const { return front().size(); }
  size_t partitions() const { return this->size(); }
};

/// Forward-FFT-related functions
class TransformBase
{
  public:
    template<typename In>
    void prepare_filter(In first, In last, Filter& filter) const;

    size_t block_size() const { return _block_size; }
    size_t partition_size() const { return _partition_size; }

    template<typename In>
    In prepare_partition(In first, In last, fft_node& partition) const;

  protected:
    explicit TransformBase(size_t block_size_);

    ~TransformBase() { fftw<float>::destroy_plan(_fft_plan); }

    fftw<float>::plan _create_plan(float* array) const;

    /// In-place FFT
    void _fft(float* first) const
    {
      fftw<float>::execute_r2r(_fft_plan, first, first);
      _sort_coefficients(first);
    }

    fftw<float>::plan _fft_plan;

  private:
    void _sort_coefficients(float* first) const;

    const size_t _block_size;
    const size_t _partition_size;
};

TransformBase::TransformBase(size_t block_size_)
  : _fft_plan(0)
  , _block_size(block_size_)
  , _partition_size(2 * _block_size)
{
  if (_block_size % 8 != 0)
  {
    throw std::logic_error("Convolver: block size must be a multiple of 8!");
  }
}

/** Create in-place FFT plan for halfcomplex data format.
 * @note FFT plans are not re-entrant except when using FFTW_THREADSAFE!
 * @note Once a plan of a certain size exists, creating further plans
 * is very fast because "wisdom" is shared (and therefore the creation of
 * plans is not thread-safe).
 * It is not necessary to re-use plans in other convolver instances.
  **/
fftw<float>::plan
TransformBase::_create_plan(float* array) const
{
  return fftw<float>::plan_r2r_1d(int(_partition_size)
      , array, array, FFTW_R2HC, FFTW_PATIENT);
}

/** %Transform time-domain samples.
 * If there are too few input samples, the rest is zero-padded, if there are
 * too few blocks in the container @p c, the rest of the samples is ignored.
 * @param first Iterator to first time-domain sample
 * @param last Past-the-end iterator
 * @param[out] filter Target container
 **/
template<typename In>
void
TransformBase::prepare_filter(In first, In last, Filter& filter) const
{
  for (Filter::iterator it = filter.begin(); it != filter.end(); ++it)
  {
    first = this->prepare_partition(first, last, *it);
  }
}

/** FFT of one block.
 * If there are too few coefficients, the rest is zero-padded.
 * @param first Iterator to first coefficient
 * @param last Past-the-end iterator
 * @param[out] partition Target partition
 * @tparam In Forward iterator
 * @return Iterator to the first coefficient of the next block (for the next
 *   iteration, if needed)
 **/
template<typename In>
In
TransformBase::prepare_partition(In first, In last, fft_node& partition) const
{
  assert(size_t(std::distance(partition.begin(), partition.end()))
      == _partition_size);

  size_t chunk = std::min(_block_size, size_t(std::distance(first, last)));

  if (chunk == 0)
  {
    partition.zero = true;
    // No FFT has to be done (FFT of zero is also zero)
  }
  else
  {
    std::copy(first, first + chunk, partition.begin());
    std::fill(partition.begin() + chunk, partition.end(), 0.0f); // zero padding
    _fft(partition.begin());
    partition.zero = false;
  }
  return first + chunk;
}

/** Sort the FFT coefficients to be in proper place for the efficient 
 * multiplication of the spectra.
 **/
void
TransformBase::_sort_coefficients(float* data) const
{
  fixed_vector<float> buffer(_partition_size);

  int base = 8;

  buffer[0] = data[0];
  buffer[1] = data[1];
  buffer[2] = data[2];
  buffer[3] = data[3];
  buffer[4] = data[_block_size];
  buffer[5] = data[_partition_size - 1];
  buffer[6] = data[_partition_size - 2];
  buffer[7] = data[_partition_size - 3];

  for (size_t i = 0; i < (_partition_size / 8-1); i++)
  {
    for (int ii = 0; ii < 4; ii++)
    {
      buffer[base+ii] = data[base/2+ii];
    }

    for (int ii = 0; ii < 4; ii++)
    {
      buffer[base+4+ii] = data[_partition_size-base/2-ii];
    }

    base += 8;
  }

  std::copy(buffer.begin(), buffer.end(), data);
}

/// Helper class to prepare filters
struct Transform : TransformBase
{
  Transform(size_t block_size_)
    : TransformBase(block_size_)
  {
    // Temporary memory area for FFTW planning routines
    fft_node planning_space(this->partition_size());
    _fft_plan = _create_plan(planning_space.begin());
  }
};

template<typename In>
Filter::Filter(size_t block_size_, In first, In last, size_t partitions_)
  : fixed_vector<fft_node>(std::make_pair(partitions_ ? partitions_
        : min_partitions(block_size_, std::distance(first, last))
        , block_size_ * 2))
{
  assert(this->partitions() > 0);

  Transform(block_size_).prepare_filter(first, last, *this);
}

/** %Input stage of convolution.
 * New audio data is fed in here, further processing happens in Output.
 **/
struct Input : TransformBase
{
  /// @param block_size_ audio block size
  /// @param partitions_ number of partitions
  Input(size_t block_size_, size_t partitions_)
    : TransformBase(block_size_)
    // One additional list element for preparing the upcoming partition:
    , spectra(std::make_pair(partitions_+1, this->partition_size()))
  {
    assert(partitions_ > 0);

    _fft_plan = _create_plan(spectra.front().begin());
  }

  template<typename In>
  void add_block(In first);

  size_t partitions() const { return spectra.size()-1; }

  /// Spectra of the partitions (double-blocks) of the input signal to be
  /// convolved. The first element is the most recent signal chunk.
  fixed_list<fft_node> spectra;
};

/** Add a block of time-domain input samples.
 * @param first Iterator to first sample.
 * @tparam In Forward iterator
 **/
template<typename In>
void
Input::add_block(In first)
{
  In last = first;
  std::advance(last, this->block_size());

  // rotate buffers (this->spectra.size() is always at least 2)
  this->spectra.move(--this->spectra.end(), this->spectra.begin());

  fft_node& current = this->spectra.front();
  fft_node& next = this->spectra.back();

  if (math::has_only_zeros(first, last))
  {
    next.zero = true;

    if (current.zero)
    {
      // Nothing to be done, actual data is ignored
    }
    else
    {
      // If first half is not zero, second half must be filled with zeros
      std::fill(current.begin() + this->block_size(), current.end(), 0.0f);
    }
  }
  else
  {
    if (current.zero)
    {
      // First half must be actually filled with zeros
      std::fill(current.begin(), current.begin() + this->block_size(), 0.0f);
    }

    // Copy data to second half of the current partition
    std::copy(first, last, current.begin() + this->block_size());
    current.zero = false;
    // Copy data to first half of the upcoming partition
    std::copy(first, last, next.begin());
    next.zero = false;
  }

  if (current.zero)
  {
    // Nothing to be done, FFT of zero is also zero
  }
  else
  {
    _fft(current.begin());
  }
}

/// Base class for Output and StaticOutput
class OutputBase
{
  public:
    float* convolve(float weight = 1.0f);

    size_t block_size() const { return _input.block_size(); }
    size_t partitions() const { return _filter_ptrs.size(); }

  protected:
    OutputBase(const Input& input);
    ~OutputBase() { fftw<float>::destroy_plan(_ifft_plan); }

    const fft_node _empty_partition;

    typedef fixed_vector<const fft_node*> filter_ptrs_t;
    filter_ptrs_t _filter_ptrs;

  private:
    void _multiply_spectra();
    void _multiply_partition_cpp(const float* signal, const float* filter);
#ifdef __SSE__
    void _multiply_partition_simd(const float* signal, const float* filter);
#endif

    void _unsort_coefficients();

    void _ifft();

    const Input& _input;

    const size_t _partition_size;

    fft_node _output_buffer;
    fftw<float>::plan _ifft_plan;
};

OutputBase::OutputBase(const Input& input)
  : _empty_partition(0)
  // Initialize with empty partition
  , _filter_ptrs(input.partitions(), &_empty_partition)
  , _input(input)
  , _partition_size(input.partition_size())
  , _output_buffer(_partition_size)
  , _ifft_plan(fftw<float>::plan_r2r_1d(int(_partition_size)
      , _output_buffer.begin()
      , _output_buffer.begin(), FFTW_HC2R, FFTW_PATIENT))
{
  assert(_filter_ptrs.size() > 0);
}

/** Fast convolution of one audio block.
 * %Input data has to be supplied with Input::add_block().
 * @param weight amplitude weighting factor for current audio block.
 * The filter has to be set in the constructor of StaticOutput or via
 * Output::set_filter().
 * @return pointer to the first sample of the convolved (and weighted) signal
 **/
float*
OutputBase::convolve(float weight)
{
  _multiply_spectra();

  // The first half will be discarded
  float* second_half = _output_buffer.begin() + _input.block_size();

  if (_output_buffer.zero)
  {
    // Nothing to be done, IFFT of zero is also zero.
    // _output_buffer was already reset to zero in _multiply_spectra().
  }
  else
  {
    _ifft();

    // normalize buffer (fftw3 does not do this)
    const float norm = weight / float(_partition_size);
    std::transform(second_half, _output_buffer.end(), second_half
        , std::bind1st(std::multiplies<float>(), norm));
  }
  return second_half;
}

void
OutputBase::_multiply_partition_cpp(const float* signal, const float* filter)
{
  // see http://www.ludd.luth.se/~torger/brutefir.html#bruteconv_4

  float d1s = _output_buffer[0] + signal[0] * filter[0];
  float d2s = _output_buffer[4] + signal[4] * filter[4];

  for (size_t nn = 0; nn < _partition_size; nn += 8)
  {
    // real parts
    _output_buffer[nn+0] += signal[nn+0] * filter[nn + 0] -
                            signal[nn+4] * filter[nn + 4];
    _output_buffer[nn+1] += signal[nn+1] * filter[nn + 1] -
                            signal[nn+5] * filter[nn + 5];
    _output_buffer[nn+2] += signal[nn+2] * filter[nn + 2] -
                            signal[nn+6] * filter[nn + 6];
    _output_buffer[nn+3] += signal[nn+3] * filter[nn + 3] -
                            signal[nn+7] * filter[nn + 7];

    // imaginary parts
    _output_buffer[nn+4] += signal[nn+0] * filter[nn + 4] +
                            signal[nn+4] * filter[nn + 0];
    _output_buffer[nn+5] += signal[nn+1] * filter[nn + 5] +
                            signal[nn+5] * filter[nn + 1];
    _output_buffer[nn+6] += signal[nn+2] * filter[nn + 6] +
                            signal[nn+6] * filter[nn + 2];
    _output_buffer[nn+7] += signal[nn+3] * filter[nn + 7] +
                            signal[nn+7] * filter[nn + 3];

  } // for

  _output_buffer[0] = d1s;
  _output_buffer[4] = d2s;
}

#ifdef __SSE__
void
OutputBase::_multiply_partition_simd(const float* signal, const float* filter)
{
  // 16 byte alignment is needed for _mm_load_ps()!
  // This should be the case anyway because fftwf_malloc() is used.

  float dc = _output_buffer[0] + signal[0] * filter[0];
  float ny = _output_buffer[4] + signal[4] * filter[4];

  for(size_t i = 0; i < _partition_size; i += 8)
  {
    // load real and imaginary parts of signal and filter
    __m128 sigr = _mm_load_ps(signal + i);
    __m128 sigi = _mm_load_ps(signal + i + 4);
    __m128 filtr = _mm_load_ps(filter + i);
    __m128 filti = _mm_load_ps(filter + i + 4);

    // multiply and subtract
    __m128 res1 = _mm_sub_ps(_mm_mul_ps(sigr, filtr), _mm_mul_ps(sigi, filti));

    // multiply and add
    __m128 res2 = _mm_add_ps(_mm_mul_ps(sigr, filti), _mm_mul_ps(sigi, filtr));

    // load output data for accumulation
    __m128 acc1 = _mm_load_ps(&_output_buffer[i]);
    __m128 acc2 = _mm_load_ps(&_output_buffer[i + 4]);

    // accumulate
    acc1 = _mm_add_ps(acc1, res1);
    acc2 = _mm_add_ps(acc2, res2);

    // store output data
    _mm_store_ps(&_output_buffer[i], acc1);
    _mm_store_ps(&_output_buffer[i + 4], acc2);
  }

  _output_buffer[0] = dc;
  _output_buffer[4] = ny;
}
#endif

/// Complex multiplication of input and filter spectra
void
OutputBase::_multiply_spectra()
{
  // Clear IFFT buffer
  std::fill(_output_buffer.begin(), _output_buffer.end(), 0.0f);
  _output_buffer.zero = true;

  fixed_list<fft_node>::const_iterator input = _input.spectra.begin();

  for (filter_ptrs_t::const_iterator ptr = _filter_ptrs.begin()
      ; ptr != _filter_ptrs.end()
      ; ++ptr, ++input)
  {
    const fft_node* filter = *ptr;

    assert(filter != 0);

    if (input->zero || filter->zero) continue;

#ifdef __SSE__
    _multiply_partition_simd(input->begin(), filter->begin());
#else
    _multiply_partition_cpp(input->begin(), filter->begin());
#endif

    _output_buffer.zero = false;
  }
}

void
OutputBase::_unsort_coefficients()
{
  fixed_vector<float> buffer(_partition_size);

  int base = 8;

  buffer[0]                 = _output_buffer[0];
  buffer[1]                 = _output_buffer[1];
  buffer[2]                 = _output_buffer[2];
  buffer[3]                 = _output_buffer[3];
  buffer[_input.block_size()] = _output_buffer[4];
  buffer[_partition_size-1] = _output_buffer[5];
  buffer[_partition_size-2] = _output_buffer[6];
  buffer[_partition_size-3] = _output_buffer[7];

  for (size_t i=0; i < (_partition_size / 8-1); i++)
  {
    for (int ii = 0; ii < 4; ii++)
    {
      buffer[base/2+ii] = _output_buffer[base+ii];
    }

    for (int ii = 0; ii < 4; ii++)
    {
      buffer[_partition_size-base/2-ii] = _output_buffer[base+4+ii];
    }

    base += 8;
  }

  std::copy(buffer.begin(), buffer.end(), _output_buffer.begin());
}

void
OutputBase::_ifft()
{
  _unsort_coefficients();
  fftw<float>::execute(_ifft_plan);
}

/** Convolution engine (output part).
 * @see Input, StaticOutput
 **/
class Output : public OutputBase
{
  public:
    Output(const Input& input)
      : OutputBase(input)
      , _queues(apf::make_index_iterator(size_t(1))
              , apf::make_index_iterator(input.partitions()))
    {}

    void set_filter(const Filter& filter);

    bool queues_empty() const;
    void rotate_queues();

  private:
    fixed_vector<filter_ptrs_t> _queues;
};

/** Set a new filter.
 * The first filter partition is updated immediately, the later partitions are
 * updated with rotate_queues().
 * @param filter Container with filter partitions. If too few partitions are
 *   given, the rest is set to zero, if too many are given, the rest is ignored.
 **/
void
Output::set_filter(const Filter& filter)
{
  Filter::const_iterator partition = filter.begin();

  // First partition has no queue and is updated immediately
  if (partition != filter.end())
  {
    _filter_ptrs.front() = &*partition++;
  }

  for (size_t i = 0; i < _queues.size(); ++i)
  {
    _queues[i][i]
      = (partition == filter.end()) ? &_empty_partition : &*partition++;
  }
}

/** Check if there are still valid partitions in the queues.
 * If this function returns @b false, rotate_queues() should be called.
 * @note This is important for crossfades: even if set_filter() wasn't used,
 *   older partitions may still change! If the queues are empty, no crossfade is
 *   necessary (except @p weight was changed in convolve()).
 **/
bool
Output::queues_empty() const
{
  if (_queues.empty()) return true;

  // It may not be obvious, but that's what the following code does:
  // If some non-null pointer is found in the last queue, return false

  filter_ptrs_t::const_iterator first = _queues.rbegin()->begin();
  filter_ptrs_t::const_iterator last  = _queues.rbegin()->end();
  return std::find_if(first, last, math::identity<const fft_node*>()) == last;
}

/** Update filter queues.
 * If queues_empty() returns @b true, calling this function is unnecessary.
 * @note This can lead to artifacts, so a crossfade is recommended.
 **/
void
Output::rotate_queues()
{
  filter_ptrs_t::iterator target = _filter_ptrs.begin();
  // Skip first element, it doesn't have a queue
  ++target;

  for (fixed_vector<filter_ptrs_t>::iterator it = _queues.begin()
      ; it != _queues.end()
      ; ++it, ++target)
  {
    // If first element is valid, use it
    if (it->front()) *target = it->front();

    std::copy(it->begin() + 1, it->end(), it->begin());
    *it->rbegin() = 0;
  }
}

/** %Convolver output stage with static filter.
 * The filter coefficients are set in the constructor(s) and cannot be changed.
 * @see Output
 **/
class StaticOutput : public OutputBase
{
  public:
    /// Constructor from time domain samples
    template<typename In>
    StaticOutput(const Input& input, In first, In last)
      : OutputBase(input)
    {
      _filter.reset(new Filter(input.block_size(), first, last
            , input.partitions()));

      _set_filter(*_filter);
    }

    /// Constructor from existing frequency domain filter coefficients.
    /// @attention The filter coefficients are not copied, their lifetime must
    ///   exceed that of the StaticOutput!
    StaticOutput(const Input& input, const Filter& filter)
      : OutputBase(input)
    {
      _set_filter(filter);
    }

  private:
    void _set_filter(const Filter& filter)
    {
      Filter::const_iterator from = filter.begin();

      for (filter_ptrs_t::iterator to = _filter_ptrs.begin()
          ; to != _filter_ptrs.end()
          ; ++to)
      {
        // If less partitions are given, the rest is set to zero
        *to = (from == filter.end()) ? &_empty_partition : &*from++;
      }
      // If further partitions are available, they are ignored
    }

    // This is only used for the first constructor!
    std::auto_ptr<Filter> _filter;
};

/// Combination of Input and Output
struct Convolver : Input, Output
{
  Convolver(size_t block_size_, size_t partitions_)
    : Input(block_size_, partitions_)
    // static_cast to resolve ambiguity
    , Output(*static_cast<Input*>(this))
  {}
};

/// Combination of Input and StaticOutput
struct StaticConvolver : Input, StaticOutput
{
  template<typename In>
  StaticConvolver(size_t block_size_, In first, In last, size_t partitions_ = 0)
    : Input(block_size_, partitions_ ? partitions_
        : min_partitions(block_size_, std::distance(first, last)))
    , StaticOutput(*this, first, last)
  {}

  StaticConvolver(const Filter& filter, size_t partitions_ = 0)
    : Input(filter.block_size()
        , partitions_ ? partitions_ : filter.partitions())
    , StaticOutput(*this, filter)
  {}
};

/// Apply @c std::transform to a container of fft_node%s
template<typename BinaryFunction>
void transform_nested(const Filter& in1, const Filter& in2, Filter& out
    , BinaryFunction f)
{
  Filter::const_iterator it1 = in1.begin();
  Filter::const_iterator it2 = in2.begin();

  for (Filter::iterator it3 = out.begin()
      ; it3 != out.end()
      ; ++it3)
  {
    if (it1 == in1.end() || it1->zero)
    {
      if (it2 == in2.end() || it2->zero)
      {
        it3->zero = true;
      }
      else
      {
        assert(it2->size() == it3->size());
        std::transform(it2->begin(), it2->end(), it3->begin()
            , std::bind1st(f, 0));
        it3->zero = false;
      }
    }
    else
    {
      if (it2 == in2.end() || it2->zero)
      {
        assert(it1->size() == it3->size());
        std::transform(it1->begin(), it1->end(), it3->begin()
            , std::bind2nd(f, 0));
        it3->zero = false;
      }
      else
      {
        assert(it1->size() == it2->size());
        assert(it1->size() == it3->size());
        std::transform(it1->begin(), it1->end(), it2->begin(), it3->begin(), f);
        it3->zero = false;
      }
    }
    if (it1 != in1.end()) ++it1;
    if (it2 != in2.end()) ++it2;
  }
}

}  // namespace conv

}  // namespace apf

#endif

// Settings for Vim (http://www.vim.org/), please do not remove:
// vim:softtabstop=2:shiftwidth=2:expandtab:textwidth=80:cindent
// vim:fdm=expr:foldexpr=getline(v\:lnum)=~'/\\*\\*'&&getline(v\:lnum)!~'\\*\\*/'?'a1'\:getline(v\:lnum)=~'\\*\\*/'&&getline(v\:lnum)!~'/\\*\\*'?'s1'\:'='