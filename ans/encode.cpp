#include <algorithm>
#include <random>
#include <vector>

#include "bits.h"
#include "ans.h"
#include "ans_utils.h"

namespace ans {

////////////////////////////////////////////////////////////////////////////////
//
// rANS
//

class rANS_Encoder : public Encoder {
public:
  // The constructor initializes M to be the sum of all Fs. It uses k
  // to determine the proper normalization interval for encoding. It uses
  // b to know how many bytes/bits etc to emit at a time.
  rANS_Encoder(const std::vector<uint32_t> &Fs, uint32_t b, uint32_t k);

  virtual void Encode(uint32_t symbol, BitWriter *w) override;
  virtual uint32_t GetState() const override { return _state; }

private:
  const std::vector<uint32_t> _F;
  const std::vector<uint32_t> _B;

  const uint32_t _M;
  const uint32_t _b;
  const uint32_t _k;
  const int _log_b;

  uint32_t _state;
};

rANS_Encoder::rANS_Encoder(const std::vector<uint32_t> &Fs, uint32_t b, uint32_t k)
  : Encoder()
  , _F(Fs)
  , _B(CumulativeSum(Fs))
  , _M(_B.back() + _F.back())
  , _b(b)
  , _k(k)
  , _log_b(IntLog2(b))
  , _state(_k * _M)
{
  assert((b & (b - 1)) == 0 || "rANS encoder may only emit powers-of-two for renormalization!");
  assert(_k * _M < (1ULL << 32));
  assert((b * _k * _M) < (1ULL << 32));
}

void rANS_Encoder::Encode(uint32_t symbol, BitWriter *w) {
  assert(_k * _M <= _state && _state < _b * _k * _M);
  assert(symbol < _F.size());

  // Renormalize
  uint32_t upper_bound = _b * _k * _F[symbol];
  while (_state >= upper_bound) {
    w->WriteBits(_state & (_b - 1), _log_b);
    _state /= _b;
  }

  // Encode
  _state = ((_state / _F[symbol]) * _M) + _B[symbol] + (_state % _F[symbol]);
}

////////////////////////////////////////////////////////////////////////////////
//
// tANS
//

class tANS_Encoder : public Encoder {
public:
  // The constructor initializes M to be the sum of all Fs. It uses k
  // to determine the proper normalization interval for encoding. It uses
  // b to know how many bytes/bits etc to emit at a time.
  tANS_Encoder(const std::vector<uint32_t> &Fs, uint32_t b, uint32_t k);

  virtual void Encode(uint32_t symbol, BitWriter *w) override;
  virtual uint32_t GetState() const override { return _state; }

private:
  const std::vector<uint32_t> _F;
  const std::vector<uint32_t> _B;

  const uint32_t _M;
  const uint32_t _b;
  const uint32_t _k;
  const int _log_b;

  // This is a table of the rearranged symbols to be used for tANS.
  //
  // Whenever we encode a symbol, s, we have to make sure that the current
  // state is within the range [Fs, bFs). If not, then we need to stream
  // out bits until it is. We also know that if the state is within
  // [Fs, q*Fs), then the state after encoding will end up within [M, q*M)
  // for any integer q <= b. To be more specific, if the state is within
  // [q*Fs, (q+1)*Fs) prior to encoding, then the state after encoding will
  // be within [q*M, (q+1)*M). Moreover, if x is in [Fs, 2*Fs) and x' = C(x)
  // is within [M, 2*M), and y is in [q*Fs, (q+1)*Fs) such that y = x mod Fs,
  // then y' = C(y) = x' mod M.
  //
  // This means that regardless of k and b, we only need a table of size M
  // and that when we do an encoding step, we need to find
  //
  // x' = (x / Fs) * M + _enc_table[Bs + (x % Fs)];
  const std::vector<uint32_t> _enc_table;

  uint32_t _state;
};

static std::vector<uint32_t> BuildEncTable(const std::vector<uint32_t> &Fs, const uint32_t M) {
  // Collect symbols...
  std::vector<uint32_t> unpacked_table = std::vector<uint32_t>(M);

  // Let's just start with randomizing the array...
  for (uint32_t i = 0, sidx = 0; i < Fs.size(); ++i) {
    for (uint32_t j = 0; j < Fs[i]; ++j, ++sidx) {
      // unpacked_table[BitReverse(sidx, IntLog2(M))] = i;
      unpacked_table[sidx] = i;
    }
  }

  unsigned seed = 0;
  std::shuffle(unpacked_table.begin(), unpacked_table.end(), std::default_random_engine(seed));

  std::vector<std::vector<uint32_t> > table_parts(Fs.size());
  for (uint32_t i = 0; i < Fs.size(); ++i) {
    table_parts[i].reserve(Fs[i]);
  }

  for (uint32_t i = 0; i < M; ++i) {
    table_parts[unpacked_table[i]].push_back(i);
  }

  // Collect the table
  std::vector<uint32_t> result;
  result.reserve(M);
  for (uint32_t i = 0; i < Fs.size(); ++i) {
    assert(table_parts[i].size() == Fs[i]);
    result.insert(result.end(), table_parts[i].begin(), table_parts[i].end());
  }

  return std::move(result);
}

tANS_Encoder::tANS_Encoder(const std::vector<uint32_t> &Fs, uint32_t b, uint32_t k)
  : Encoder()
  , _F(Fs)
  , _B(CumulativeSum(Fs))
  , _M(_B.back() + _F.back())
  , _b(b)
  , _k(k)
  , _log_b(IntLog2(b))
  , _enc_table(std::move(BuildEncTable(Fs, _M)))
  , _state(_k * _M)
{
  assert((b & (b - 1)) == 0 || "rANS encoder may only emit powers-of-two for renormalization!");
  assert(_k * _M < (1ULL << 32));
  assert((b * _k * _M) < (1ULL << 32));
}

void tANS_Encoder::Encode(uint32_t symbol, BitWriter *w) {
  assert(_k * _M <= _state && _state < _b * _k * _M);
  assert(symbol < _F.size());

  // Renormalize
  uint32_t upper_bound = _b * _k * _F[symbol];
  while (_state >= upper_bound) {
    w->WriteBits(_state & (_b - 1), _log_b);
    _state /= _b;
  }

  // Make sure it's normalized...
  assert(_state >= _k * _F[symbol]);

  // Encode
  _state = ((_state / _F[symbol]) * _M) + _enc_table[_B[symbol] + (_state % _F[symbol])];
}

////////////////////////////////////////////////////////////////////////////////
//
// ANS Factory
//

std::unique_ptr<Encoder> Encoder::Create(const std::vector<uint32_t> &Fs, const Options &opts) {
  std::unique_ptr<Encoder> enc;
  switch (opts.type) {
    case eType_rANS:
      enc.reset(new rANS_Encoder(Fs, opts.b, opts.k));
      break;
    case eType_tANS:
      enc.reset(new tANS_Encoder(Fs, opts.b, opts.k));
      break;
  }

  return std::move(enc);
}

}  // namespace ans