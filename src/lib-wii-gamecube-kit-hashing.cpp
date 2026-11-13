// SPDX-License-Identifier: GPL-2.0-or-later

#include "lib-wii-gamecube-kit-hashing.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <mbedtls/md5.h>
#include <zlib.h>

#include "Common/Crypto/SHA1.h"

namespace wii_gamecube_kit
{
namespace
{
template <typename Bytes>
std::string Hex(const Bytes& bytes)
{
  std::string hex;
  hex.reserve(std::size(bytes) * 2);
  for (const auto b : bytes)
    hex += fmt::format("{:02x}", static_cast<unsigned>(b));
  return hex;
}
}  // namespace

// ------------------------------------------------------------------ ChunkPool

std::shared_ptr<ChunkPool> ChunkPool::Create(size_t chunk_size)
{
  return std::shared_ptr<ChunkPool>(new ChunkPool(chunk_size));
}

Chunk ChunkPool::Get()
{
  std::unique_ptr<Buffer> buffer;
  {
    std::lock_guard lock(m_mutex);
    if (!m_free.empty())
    {
      buffer = std::move(m_free.back());
      m_free.pop_back();
    }
  }
  if (!buffer)
  {
    buffer = std::make_unique<Buffer>();
    buffer->data = std::make_unique_for_overwrite<u8[]>(m_chunk_size);
    buffer->capacity = m_chunk_size;
  }
  buffer->size = 0;
  // The deleter keeps the pool alive for as long as any of its chunks is.
  return Chunk(buffer.release(), [pool = shared_from_this()](Buffer* b) { pool->Put(b); });
}

void ChunkPool::Put(Buffer* buffer)
{
  std::unique_ptr<Buffer> owned(buffer);
  std::lock_guard lock(m_mutex);
  m_free.push_back(std::move(owned));
}

// ------------------------------------------------------------- ParallelHasher

struct ParallelHasher::Worker
{
  explicit Worker(unsigned kind_) : kind(kind_) {}

  void Run()
  {
    try
    {
      switch (kind)
      {
      case HASH_CRC32:
      {
        uLong crc = crc32(0L, Z_NULL, 0);
        while (const std::optional<Chunk> chunk = queue.Pop())
        {
          const u8* data = (*chunk)->data.get();
          for (size_t left = (*chunk)->size; left > 0;)
          {
            const size_t n = std::min<size_t>(left, 0x40000000);
            crc = crc32(crc, data, static_cast<uInt>(n));
            data += n;
            left -= n;
          }
        }
        digest = fmt::format("{:08x}", static_cast<u32>(crc));
        break;
      }
      case HASH_MD5:
      {
        mbedtls_md5_context md5;
        mbedtls_md5_init(&md5);
        mbedtls_md5_starts_ret(&md5);
        while (const std::optional<Chunk> chunk = queue.Pop())
          mbedtls_md5_update_ret(&md5, (*chunk)->data.get(), (*chunk)->size);
        unsigned char md5_digest[16];
        mbedtls_md5_finish_ret(&md5, md5_digest);
        mbedtls_md5_free(&md5);
        digest = Hex(md5_digest);
        break;
      }
      case HASH_SHA1:
      {
        const std::unique_ptr<Common::SHA1::Context> sha1 = Common::SHA1::CreateContext();
        while (const std::optional<Chunk> chunk = queue.Pop())
          sha1->Update((*chunk)->data.get(), (*chunk)->size);
        digest = Hex(sha1->Finish());
        break;
      }
      }
    }
    catch (...)
    {
      failed = true;
      // Keep taking chunks, so the producer never waits on a worker that has stopped.
      while (queue.Pop())
      {
      }
    }
  }

  unsigned kind;
  BoundedQueue<Chunk> queue{4};
  std::thread thread;
  std::string digest;
  bool failed = false;
};

ParallelHasher::ParallelHasher(unsigned kinds, std::shared_ptr<ChunkPool> pool)
    : m_pool(std::move(pool))
{
  for (const unsigned kind : {HASH_CRC32, HASH_MD5, HASH_SHA1})
  {
    if ((kinds & kind) == 0)
      continue;
    m_workers.push_back(std::make_unique<Worker>(kind));
    Worker* worker = m_workers.back().get();
    worker->thread = std::thread([worker] { worker->Run(); });
  }
}

ParallelHasher::~ParallelHasher()
{
  Stop();
}

void ParallelHasher::Stop()
{
  for (const auto& worker : m_workers)
    worker->queue.Close();
  for (const auto& worker : m_workers)
  {
    if (worker->thread.joinable())
      worker->thread.join();
  }
}

void ParallelHasher::Add(Chunk chunk)
{
  if (chunk->size == 0)
    return;
  for (const auto& worker : m_workers)
    worker->queue.Push(chunk);
}

void ParallelHasher::Write(const u8* data, size_t size)
{
  while (size > 0)
  {
    if (!m_pending)
      m_pending = m_pool->Get();
    const size_t n = std::min(size, m_pending->capacity - m_pending->size);
    std::memcpy(m_pending->data.get() + m_pending->size, data, n);
    m_pending->size += n;
    data += n;
    size -= n;
    if (m_pending->size == m_pending->capacity)
      Add(std::move(m_pending));
  }
}

bool ParallelHasher::Finish(Digests* out)
{
  if (m_pending)
    Add(std::move(m_pending));
  Stop();
  m_finished = true;
  Digests digests;
  for (const auto& worker : m_workers)
  {
    if (worker->failed)
      return false;
    (worker->kind == HASH_CRC32 ? digests.crc32 :
     worker->kind == HASH_MD5   ? digests.md5 :
                                  digests.sha1) = worker->digest;
  }
  *out = std::move(digests);
  return true;
}

// --------------------------------------------------------- OrderedBlobReading

struct OrderedBlobReading::Worker
{
  std::unique_ptr<DiscIO::BlobReader> reader;
  // One piece waiting here, one being read: enough to keep the hasher fed.
  BoundedQueue<Chunk> queue{1};
  std::thread thread;
};

OrderedBlobReading::OrderedBlobReading(std::vector<std::unique_ptr<DiscIO::BlobReader>> readers,
                                       u64 total, std::shared_ptr<ChunkPool> pool)
    : m_total(total), m_pool(std::move(pool))
{
  const u64 piece = m_pool->ChunkSize();
  m_pieces = (total + piece - 1) / piece;
  const u64 count = readers.size();
  for (u64 k = 0; k < count; ++k)
  {
    m_workers.push_back(std::make_unique<Worker>());
    Worker* worker = m_workers.back().get();
    worker->reader = std::move(readers[k]);
    worker->thread = std::thread([this, worker, k, count, piece] {
      for (u64 i = k; i < m_pieces && !m_stop.load(); i += count)
      {
        Chunk chunk;
        try
        {
          chunk = m_pool->Get();
          const u64 offset = i * piece;
          chunk->size = static_cast<size_t>(std::min(piece, m_total - offset));
          // One Wii block per read. When a single Read makes Dolphin's WIA/RVZ reader
          // re-encrypt more than one Wii group it hasn't cached, the hash exceptions it
          // collected for the first group are applied to the next one too, giving wrong
          // data. Sequential readers (ConvertToPlain) only escape that because the earlier
          // group is cached; a decoder starting mid-disc wouldn't. Block-sized reads
          // never span two groups, and the reader's caches make them cheap.
          constexpr u64 READ_SIZE = 0x8000;
          for (u64 done = 0; chunk && done < chunk->size;)
          {
            const u64 n = std::min<u64>(READ_SIZE, chunk->size - done);
            if (!worker->reader->Read(offset + done, n, chunk->data.get() + done))
              chunk = nullptr;
            done += n;
          }
        }
        catch (...)
        {
          chunk = nullptr;
        }
        const bool ok = chunk != nullptr;
        if (!worker->queue.Push(std::move(chunk)) || !ok)
          break;
      }
    });
  }
}

OrderedBlobReading::~OrderedBlobReading()
{
  Stop();
}

void OrderedBlobReading::Stop()
{
  m_stop.store(true);
  for (const auto& worker : m_workers)
    worker->queue.Close();
  for (const auto& worker : m_workers)
  {
    if (worker->thread.joinable())
      worker->thread.join();
  }
}

Chunk OrderedBlobReading::Next()
{
  if (m_next >= m_pieces || m_workers.empty())
    return nullptr;
  const std::optional<Chunk> chunk = m_workers[m_next % m_workers.size()]->queue.Pop();
  ++m_next;
  return chunk ? *chunk : nullptr;
}

// ---------------------------------------------------------- HashingBlobReader

HashingBlobReader::HashingBlobReader(std::unique_ptr<DiscIO::BlobReader> inner, unsigned kinds)
    : m_inner(std::move(inner)), m_pool(ChunkPool::Create(HASH_CHUNK_SIZE)),
      m_hasher(kinds, m_pool)
{
}

bool HashingBlobReader::CatchUp(u64 offset)
{
  while (m_hashed < offset)
  {
    Chunk chunk = m_pool->Get();
    chunk->size = static_cast<size_t>(std::min<u64>(chunk->capacity, offset - m_hashed));
    if (!m_inner->Read(m_hashed, chunk->size, chunk->data.get()))
      return false;
    m_hashed += chunk->size;
    m_hasher.Add(std::move(chunk));
  }
  return true;
}

bool HashingBlobReader::Read(u64 offset, u64 size, u8* out_ptr)
{
  // Clamp to the disc: a read past its end has nothing to hash.
  if (!CatchUp(std::min(offset, m_inner->GetDataSize())))
    return false;
  if (!m_inner->Read(offset, size, out_ptr))
    return false;
  const u64 end = std::min(offset + size, m_inner->GetDataSize());
  if (offset <= m_hashed && m_hashed < end)
  {
    m_hasher.Write(out_ptr + (m_hashed - offset), static_cast<size_t>(end - m_hashed));
    m_hashed = end;
  }
  return true;
}

bool HashingBlobReader::Finish(Digests* out)
{
  return CatchUp(m_inner->GetDataSize()) && m_hasher.Finish(out);
}
}  // namespace wii_gamecube_kit
