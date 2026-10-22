/* SPDX-License-Identifier: MIT
 * Legacy GMU HFI v1 wire format, original bounded host transport.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#if defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#define QUEUE_WORDS 1024u
#define HEADER_WORDS 12u
static volatile uint32_t *
queue_header(struct a618_hfi *hfi, unsigned queue)
{
 return hfi->table + 6 + queue * HEADER_WORDS;
}
static int
indices(struct a618_hfi *hfi, unsigned queue, unsigned *read, unsigned *write)
{
 volatile uint32_t *header = queue_header(hfi, queue);
 hfi->barrier(hfi->cookie);
 if (header[0] != 1 || header[3] != QUEUE_WORDS) return EIO;
 *read = header[10]; *write = header[11];
 if (*read >= QUEUE_WORDS || *write >= QUEUE_WORDS) return EIO;
 hfi->barrier(hfi->cookie);
 return 0;
}
int
a618_hfi_init(struct a618_hfi *hfi, const struct a618_dma_region *memory,
 void (*barrier)(void *), void *cookie)
{
 if (!hfi || !memory || !memory->cpu || !memory->uncached || !barrier ||
     memory->bytes != 0x4000 || ((uintptr_t)memory->cpu & 3))
  return EINVAL;
 hfi->table = (volatile uint32_t *)memory->cpu;
 hfi->barrier = barrier; hfi->cookie = cookie;
 hfi->sequence = hfi->poisoned = hfi->responses = 0;
 hfi->last_request = hfi->last_response = hfi->firmware_error = 0;
 for (unsigned i = 0; i < 4096; ++i) hfi->table[i] = 0;
 hfi->table[0] = 0;
 hfi->table[1] = 24 + 3 * 48; /* size in bytes; offsets below in DWORDs */
 hfi->table[2] = 6; hfi->table[3] = 12;
 hfi->table[4] = hfi->table[5] = 3;
 const unsigned id[] = {0, 4, 5};
 for (unsigned q = 0; q < 3; ++q) {
  volatile uint32_t *header = queue_header(hfi, q);
  header[0] = 1;
  header[1] = A618_HFI_IOVA + (q + 1) * 4096;
  header[2] = (10u << 8) | id[q];
  header[3] = QUEUE_WORDS;
  header[6] = header[7] = header[8] = 1;
 }
 barrier(cookie);
 return 0;
}
int
a618_hfi_write(struct a618_hfi *hfi, const uint32_t *words, unsigned count)
{
 if (!hfi || !hfi->table || !hfi->barrier || !words || !count || count > 255 ||
     ((words[0] >> 8) & 255) != count || (words[0] & 0xf0000u)) return EINVAL;
 unsigned read, write;
 int error = indices(hfi, 0, &read, &write);
 if (error) return error;
 unsigned free_words = (read + QUEUE_WORDS - write - 1) % QUEUE_WORDS;
 if (count > free_words) return ENOSPC;
 volatile uint32_t *data = hfi->table + QUEUE_WORDS;
 for (unsigned i = 0; i < count; ++i) data[(write + i) % QUEUE_WORDS] = words[i];
 hfi->barrier(hfi->cookie);
 queue_header(hfi, 0)[11] = (write + count) % QUEUE_WORDS;
 hfi->barrier(hfi->cookie);
 return 0;
}
int
a618_hfi_read(struct a618_hfi *hfi, unsigned queue, uint32_t *words,
 unsigned capacity, unsigned *count)
{
 if (!count) return EINVAL;
 *count = 0;
 if (!hfi || !hfi->table || !hfi->barrier || !words || !capacity ||
     queue < 1 || queue > 2) return EINVAL;
 unsigned read, write;
 int error = indices(hfi, queue, &read, &write);
 if (error) return error;
 unsigned available = (write + QUEUE_WORDS - read) % QUEUE_WORDS;
 if (!available) {
  queue_header(hfi, queue)[8] = 1;
  hfi->barrier(hfi->cookie);
  return EAGAIN;
 }
 volatile uint32_t *data = hfi->table + (queue + 1) * QUEUE_WORDS;
 unsigned size = (data[read] >> 8) & 255;
 if (!size) return EIO;
 if (size > available) return EAGAIN;
 if (size > capacity) return EMSGSIZE;
 for (unsigned i = 0; i < size; ++i) words[i] = data[(read + i) % QUEUE_WORDS];
 hfi->barrier(hfi->cookie);
 queue_header(hfi, queue)[10] = (read + size) % QUEUE_WORDS;
 hfi->barrier(hfi->cookie);
 *count = size;
 return 0;
}

/* GMU-relative byte offsets from the A6xx register XML. Keep host IRQs
 * masked: this backend polls, and no XNU interrupt handler is installed. */
#define HFI_INTR_INFO 0x14648u
#define HFI_INTR_CLEAR 0x14644u
#define HFI_DOORBELL 0x14650u
#define HFI_CM3_FAULT (1u << 23)
int
a618_hfi_call(struct a618_hfi *hfi, const struct a618_io *io, unsigned id,
 const uint32_t *body, unsigned body_words, uint32_t *payload,
 unsigned payload_words)
{
 if (!hfi || !hfi->table || !hfi->barrier || !io || !io->read ||
     !io->write || !io->delay_us || id > 255 || body_words > 254 ||
     (body_words && !body) || payload_words > 252 ||
     (payload_words && !payload)) return EINVAL;
 if (hfi->poisoned) return EIO;
 uint32_t message[255];
 hfi->sequence = (hfi->sequence + 1) % 0xfffu;
 uint32_t sent = (hfi->sequence << 20) | ((body_words + 1) << 8) | id;
 message[0] = sent;
 hfi->last_request = sent;
 for (unsigned i = 0; i < body_words; ++i) message[i + 1] = body[i];
 int error = a618_hfi_write(hfi, message, body_words + 1);
 if (error) return error; /* Nothing published on a rejected append. */
 io->write(io->cookie, HFI_DOORBELL, 1);
 for (unsigned elapsed = 0; elapsed < 1000000; elapsed += 100) {
  uint32_t interrupt = io->read(io->cookie, HFI_INTR_INFO);
  if (interrupt == 0xffffffffu || (interrupt & HFI_CM3_FAULT)) {
   error = EIO; goto failed;
  }
  /* W1C before reading: a concurrent firmware append remains visible via
   * the queue indices, including when no fresh interrupt edge is seen. */
  if (interrupt & 1) io->write(io->cookie, HFI_INTR_CLEAR, 1);
  unsigned words = 0;
  error = a618_hfi_read(hfi, 1, message, 255, &words);
  if (error != EAGAIN && error) goto failed;
  if (!error) {
   ++hfi->responses;
   unsigned received_id = message[0] & 255;
   unsigned type = (message[0] >> 16) & 15;
   if (received_id == 100) { error = EIO; goto failed; }
   if (received_id != 126 || type != 2 || words < 3) {
    error = EPROTO; goto failed;
   }
   /* Match command ID and transaction sequence as in the Linux transport;
    * request length bits are not used to size the ACK payload. The actual
    * response length/type were validated above. Stale ACKs cannot extend
    * the fixed one-second deadline. */
   hfi->last_response = message[1];
   if ((message[1] & 0xfff000ffu) == (sent & 0xfff000ffu)) {
    hfi->firmware_error = message[2];
    if (message[2]) { error = EIO; goto failed; }
    if (words < 3 + payload_words) { error = EPROTO; goto failed; }
    for (unsigned i = 0; i < payload_words; ++i) payload[i] = message[3 + i];
    return 0;
   }
  }
  /* Drain a single debug packet per iteration so logs cannot deadlock FW.
   * All queue indices and packet sizes pass the same validation. */
  error = a618_hfi_read(hfi, 2, message, 255, &words);
  if (error != EAGAIN && error) goto failed;
  if (!error && (message[0] & 255) == 100) { error = EIO; goto failed; }
  io->delay_us(io->cookie, 100);
 }
 error = ETIMEDOUT;
failed:
 hfi->poisoned = 1;
 return error;
}

int
a618_hfi_bootstrap(struct a618_hfi *hfi, const struct a618_io *io,
 uint32_t gpu_vote, uint32_t gmu_vote, uint32_t *firmware_version)
{
 if (!firmware_version || !(gpu_vote >> 16) || !(gmu_vote >> 16)) return EINVAL;
 *firmware_version = 0;
 uint32_t init[] = {0, A618_DEBUG_IOVA, 0x4000, 1}; /* cold boot = 1 */
 int error = a618_hfi_call(hfi, io, 0, init, 4, 0, 0);
 if (error) return error;
 uint32_t supported = (1u << 28) | (1u << 19) | (1u << 17); /* HFI 1.10 */
 error = a618_hfi_call(hfi, io, 1, &supported, 1, firmware_version, 1);
 if (error) return error;
 /* Two levels include the mandatory off entry; units are kHz. Upper
  * corners stay absent until power, thermal control and recovery exist. */
 uint32_t perf[42] = {0}; /* body of the fixed 43-DWORD v1 message */
 perf[0] = perf[1] = 2;
 perf[4] = gpu_vote; perf[5] = 180000;
 perf[36] = gmu_vote; perf[37] = 200000;
 error = a618_hfi_call(hfi, io, 4, perf, 42, 0, 0);
 if (error) return error;
 /* A618 uses a single DDR off entry: GMU does not scale its bus. Exact
  * target-specific wire constants from Linux a618_build_bw_table().
  * Host interconnect bandwidth remains a separate prerequisite. */
 uint32_t bw[159] = {0}; /* body of the fixed 160-DWORD message */
 bw[0] = 1; bw[1] = 1; bw[2] = 3; bw[3] = bw[4] = 1;
 bw[5] = 0x5007c;
 bw[11] = 0x40000000; bw[17] = 0x60000001;
 bw[23] = 0x50000; bw[24] = 0x5003c; bw[25] = 0x5000c;
 bw[31] = bw[32] = bw[33] = 0x40000000;
 error = a618_hfi_call(hfi, io, 3, bw, 159, 0, 0);
 if (error) return error;
 /* Wire message 5 ends legacy initialization (named TEST in Linux). */
 return a618_hfi_call(hfi, io, 5, 0, 0, 0, 0);
}
#endif
