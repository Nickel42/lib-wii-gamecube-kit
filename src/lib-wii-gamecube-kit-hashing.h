// SPDX-License-Identifier: GPL-2.0-or-later

// Hashing for lib-wii-gamecube-kit: CRC32, MD5 and SHA-1 on their own threads, reading a
// disc with several decoders at once, and hashing what a conversion reads as it goes.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"

namespace wii_gamecube_kit
{
// The same bits as WII_GAMECUBE_KIT_HASH_*.
enum : unsigned
{
  HASH_CRC32 = 1,
  HASH_MD5 = 2,
  HASH_SHA1 = 4,
  HASH_ALL = 7,
};

// Lowercase hex digests; "" for each hash that wasn't asked for.
struct Digests
{
  std::string crc32;
  std::string md5;
  std::string sha1;
};

// A piece of disc data passed between threads. Never changed once handed on.
struct Buffer
{
  std::unique_ptr<u8[]> data;
  size_t capacity = 0;
  size_t size = 0;
};
using Chunk = std::shared_ptr<Buffer>;

// Recycles buffers, so a stream of chunks doesn't allocate (and page-fault) for every piece.
class ChunkPool : public std::enable_shared_from_this<ChunkPool>
{
public:
  static std::shared_ptr<ChunkPool> Create(size_t chunk_size);
  Chunk Get();
  size_t ChunkSize() const { return m_chunk_size; }

private:
  explicit ChunkPool(size_t chunk_size) : m_chunk_size(chunk_size) {}
  void Put(Buffer* buffer);

  size_t m_chunk_size;
  std::mutex m_mutex;
  std::vector<std::unique_ptr<Buffer>> m_free;
};

template <typename T>
class BoundedQueue
{
public:
  explicit BoundedQueue(size_t capacity) : m_capacity(capacity) {}

  // False once closed.
  bool Push(T value)
  {
    std::unique_lock lock(m_mutex);
    m_not_full.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
    if (m_closed)
      return false;
    m_items.push_back(std::move(value));
    m_not_empty.notify_one();
    return true;
  }

  // Waits for an item. nullopt once closed and empty.
  std::optional<T> Pop()
  {
    std::unique_lock lock(m_mutex);
    m_not_empty.wait(lock, [&] { return m_closed || !m_items.empty(); });
    if (m_items.empty())
      return std::nullopt;
    T value = std::move(m_items.front());
    m_items.pop_front();
    m_not_full.notify_one();
    return value;
  }

  void Close()
  {
    std::lock_guard lock(m_mutex);
    m_closed = true;
    m_not_full.notify_all();
    m_not_empty.notify_all();
  }

private:
  size_t m_capacity;
  std::mutex m_mutex;
  std::condition_variable m_not_full;
  std::condition_variable m_not_empty;
  std::deque<T> m_items;
  bool m_closed = false;
};

// Hashes a stream of chunks, one thread per requested hash, so the slowest hash (MD5)
// sets the pace instead of the three together, and none of it runs on the reading thread.
class ParallelHasher
{
public:
  ParallelHasher(unsigned kinds, std::shared_ptr<ChunkPool> pool);
  ~ParallelHasher();
  ParallelHasher(const ParallelHasher&) = delete;
  ParallelHasher& operator=(const ParallelHasher&) = delete;

  // Hashes chunk next. The chunk must not change afterwards.
  void Add(Chunk chunk);
  // Copies data in, collecting it into whole chunks.
  void Write(const u8* data, size_t size);
  // Waits for the hashing to finish. False if a hashing thread failed (out of memory).
  bool Finish(Digests* out);

private:
  struct Worker;
  void Stop();

  std::shared_ptr<ChunkPool> m_pool;
  std::vector<std::unique_ptr<Worker>> m_workers;
  Chunk m_pending;
  bool m_finished = false;
};

// Reads [0, data size) of a disc image in order, in pool-sized pieces, from one or more
// BlobReaders on their own threads. Piece i comes from reader i % readers, so compressed
// containers (and Wii partitions, which must be re-encrypted and re-hashed) decode in
// parallel while the pieces still arrive in order.
//
// Each reader fetches its piece in Reads of read_size bytes. Decoders need one Wii block
// (0x8000, the default; see the .cpp). Plain images should read whole pieces: on a network
// share or a cold disk, several large reads at once are what make reading fast.
//
// Pieces are the pool's chunk size, except that piece 0 can be shorter (first_piece), to
// put every later boundary on a Wii group boundary. group_threads is what
// WiiGroupThreadCount() returns on the readers' threads: the share of the context's
// threads each decoder may use to re-encrypt a Wii group (0: Dolphin's own scheduling).
class OrderedBlobReading
{
public:
  static constexpr u64 BLOCK_READS = 0x8000;
  static constexpr u64 WHOLE_PIECES = 0;

  OrderedBlobReading(std::vector<std::unique_ptr<DiscIO::BlobReader>> readers, u64 total,
                     std::shared_ptr<ChunkPool> pool, u64 read_size = BLOCK_READS,
                     u64 first_piece = 0, unsigned group_threads = 0);
  ~OrderedBlobReading();
  OrderedBlobReading(const OrderedBlobReading&) = delete;
  OrderedBlobReading& operator=(const OrderedBlobReading&) = delete;

  u64 PieceCount() const { return m_pieces; }
  // Where piece i starts.
  u64 PieceOffset(u64 i) const { return i == 0 ? 0 : m_first + (i - 1) * m_piece; }
  // The next piece, or nullptr if reading it failed.
  Chunk Next();

private:
  struct Worker;
  void Stop();

  u64 m_total;
  u64 m_piece;
  u64 m_first;  // the size of piece 0
  u64 m_pieces;
  u64 m_next = 0;
  std::shared_ptr<ChunkPool> m_pool;
  std::atomic<bool> m_stop{false};
  std::vector<std::unique_ptr<Worker>> m_workers;
};

// The BlobReader a conversion reads its input through: it passes everything on to the real
// reader and hashes the disc data, [0, data size), as the converter reads it. Dolphin's
// converters read in order, so this normally costs no extra reads; any part they skip or
// read out of order is read again here, so the hashes are always of the whole disc.
class HashingBlobReader final : public DiscIO::BlobReader
{
public:
  HashingBlobReader(std::unique_ptr<DiscIO::BlobReader> inner, unsigned kinds);

  // Hashes whatever the converter didn't read, then waits for the hashes.
  bool Finish(Digests* out);

  DiscIO::BlobType GetBlobType() const override { return m_inner->GetBlobType(); }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override { return m_inner->CopyReader(); }
  u64 GetRawSize() const override { return m_inner->GetRawSize(); }
  u64 GetDataSize() const override { return m_inner->GetDataSize(); }
  DiscIO::DataSizeType GetDataSizeType() const override { return m_inner->GetDataSizeType(); }
  u64 GetBlockSize() const override { return m_inner->GetBlockSize(); }
  bool HasFastRandomAccessInBlock() const override { return m_inner->HasFastRandomAccessInBlock(); }
  std::string GetCompressionMethod() const override { return m_inner->GetCompressionMethod(); }
  std::optional<int> GetCompressionLevel() const override { return m_inner->GetCompressionLevel(); }
  bool Read(u64 offset, u64 size, u8* out_ptr) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override
  {
    return m_inner->SupportsReadWiiDecrypted(offset, size, partition_data_offset);
  }
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) override
  {
    return m_inner->ReadWiiDecrypted(offset, size, out_ptr, partition_data_offset);
  }

private:
  bool CatchUp(u64 offset);

  std::unique_ptr<DiscIO::BlobReader> m_inner;
  std::shared_ptr<ChunkPool> m_pool;
  ParallelHasher m_hasher;
  u64 m_hashed = 0;  // [0, m_hashed) has gone to the hasher
};

// The BlobReader a conversion reads its input through. Dolphin's converters read their
// input on one thread, one block at a time (128 KiB for a GameCube disc at the default RVZ
// block size), and wait for each read. For a plain image (ISO/GCM) that's fine when it is
// in the file cache, but on a network share or a cold disk the reading sets the pace: on a
// NAS, 128 KiB reads one after another manage a third of what four 4 MiB reads at once do.
// For a compressed container (GCZ/WIA/RVZ) every block is decompressed, and every Wii group
// re-encrypted and re-hashed, on that one thread. This reads the whole disc ahead, in
// order, with several readers (file handles, or decoders each decoding their own pieces),
// and hands the converter what it asks for from memory. Reads the stream can't serve
// (before the current piece, past the end, or after a read error) go to the first reader
// directly.
class ReadAheadBlobReader final : public DiscIO::BlobReader
{
public:
  // inner answers everything else (sizes, types, direct reads); ahead (other readers of
  // the same image, at least one) read ahead on their own threads. The defaults suit a
  // plain image; decoders take the OrderedBlobReading arguments hash_ex uses.
  ReadAheadBlobReader(std::unique_ptr<DiscIO::BlobReader> inner,
                      std::vector<std::unique_ptr<DiscIO::BlobReader>> ahead,
                      size_t piece_size = PIECE_SIZE,
                      u64 read_size = OrderedBlobReading::WHOLE_PIECES, u64 first_piece = 0,
                      unsigned group_threads = 0);

  DiscIO::BlobType GetBlobType() const override { return m_inner->GetBlobType(); }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override { return m_inner->CopyReader(); }
  u64 GetRawSize() const override { return m_inner->GetRawSize(); }
  u64 GetDataSize() const override { return m_inner->GetDataSize(); }
  DiscIO::DataSizeType GetDataSizeType() const override { return m_inner->GetDataSizeType(); }
  u64 GetBlockSize() const override { return m_inner->GetBlockSize(); }
  bool HasFastRandomAccessInBlock() const override { return m_inner->HasFastRandomAccessInBlock(); }
  std::string GetCompressionMethod() const override { return m_inner->GetCompressionMethod(); }
  std::optional<int> GetCompressionLevel() const override { return m_inner->GetCompressionLevel(); }
  bool Read(u64 offset, u64 size, u8* out_ptr) override;
  bool SupportsReadWiiDecrypted(u64 offset, u64 size, u64 partition_data_offset) const override
  {
    return m_inner->SupportsReadWiiDecrypted(offset, size, partition_data_offset);
  }
  bool ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr, u64 partition_data_offset) override
  {
    return m_inner->ReadWiiDecrypted(offset, size, out_ptr, partition_data_offset);
  }

  static constexpr size_t PIECE_SIZE = 0x400000;
  static constexpr unsigned READERS = 4;

private:
  std::unique_ptr<DiscIO::BlobReader> m_inner;
  std::unique_ptr<OrderedBlobReading> m_reading;
  Chunk m_chunk;           // the piece reads are served from
  u64 m_chunk_index = 0;   // its number
  u64 m_chunk_start = 0;   // its offset
  bool m_streaming = true;  // false after a read-ahead failure: read directly from then on
};

// The size of the pieces disc data is hashed in.
constexpr size_t HASH_CHUNK_SIZE = 0x200000;

// Sets what WiiGroupThreadCount() returns on this thread.
void SetWiiGroupThreadCount(unsigned threads);
}  // namespace wii_gamecube_kit
