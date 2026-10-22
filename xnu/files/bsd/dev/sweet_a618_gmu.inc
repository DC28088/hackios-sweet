/* SPDX-License-Identifier: MIT
 * A618 cold-start implementation using the pinned SM7150 Linux register and
 * firmware protocols. See docs/adreno-port.md for scope and source hashes.
 * Single caller. No dynamic clock scaling, warm boot, IRQs or CP submission.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#if defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#define GMU_CM3_DTCM_START 0x7000u
#define GMU_GENERAL_7 0x14730u
#define GMU_CM3_BOOT_CONFIG 0x14004u
#define GMU_HFI_QTBL_ADDR 0x14614u
#define GMU_HFI_QTBL_INFO 0x14610u
#define GMU_AHB_FENCE_RANGE_0 0x24c44u
#define GMU_CM3_CFG 0x140b4u
#define GMU_HFI_SFR_ADDR 0x14608u
#define GPU_GMU_CX_GMU_PWR_COL_CP_MSG 0x14400u
#define GPU_GMU_CX_GMU_PWR_COL_CP_RESP 0x14404u
#define GMU_SYS_BUS_CONFIG 0x1303cu
#define GMU_ICACHE_CONFIG 0x13000u
#define GMU_DCACHE_CONFIG 0x13004u
#define GMU_PWR_COL_INTER_FRAME_CTRL 0x14300u
#define GMU_RPMH_CTRL 0x143a0u
#define GMU_BOOT_SLUMBER_OPTION 0x8fe0u
#define GMU_GX_VOTE_IDX 0x8fe4u
#define GMU_MX_VOTE_IDX 0x8fe8u
#define GMU_HOST2GMU_INTR_SET 0x14650u
#define GMU_GMU2HOST_INTR_INFO 0x14648u
#define GMU_GMU2HOST_INTR_CLR 0x14644u
#define GMU_GMU2HOST_INTR_MASK 0x1464cu
#define GMU_AO_HOST_INTERRUPT_MASK 0x24c18u
#define GMU_GX_SPTPRAC_POWER_CONTROL 0x204u
#define GMU_SPTPRAC_PWR_CLK_STATUS 0x14340u
#define GMU_HFI_CTRL_INIT 0x14618u
#define GMU_HFI_CTRL_STATUS 0x14600u

static int
valid_io(const struct a618_io *io)
{
 return io && io->read && io->write && io->delay_us;
}
static int
legacy_gmu_poll(const struct a618_io *io, uint32_t offset, uint32_t mask,
 uint32_t expected, unsigned timeout, struct a618_gmu_boot_result *result)
{
 result->offset = offset;
 for (unsigned us = 0; us <= timeout; us += 10) {
  result->value = io->read(io->cookie, offset);
  if (result->value == 0xffffffffu) return EIO;
  if ((result->value & mask) == expected) return 0;
  if (us != timeout) io->delay_us(io->cookie, 10);
 }
 return ETIMEDOUT;
}
static void
rscc_write(const struct a618_io *gmu, unsigned reg, uint32_t value)
{
 gmu->write(gmu->cookie, 0x23000 + reg * 4, value);
}
static void
pdc_program(const struct a618_io *gmu, const struct a618_io *pdc,
 const struct a618_io *seq)
{
 /* SM7150 A618 constants, not the newer A650/A7xx sequence. */
 static const uint32_t rsc[] = {
  0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
 static const uint32_t program[] = {
  0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
 rscc_write(gmu, 4, 1u << 24);
 rscc_write(gmu, 0xb, 1);
 for (unsigned i = 0; i < 3; ++i) {
  rscc_write(gmu, 0xe + i * 2, i == 2 ? 0x80000000 : 0);
  rscc_write(gmu, 0xd + i * 2, 0);
 }
 rscc_write(gmu, 0x100, 0);
 rscc_write(gmu, 8, 0x4520);
 rscc_write(gmu, 9, 0x4510);
 rscc_write(gmu, 0xa, 0x4514);
 for (unsigned i = 0; i < 5; ++i) {
  rscc_write(gmu, 0x180 + i, rsc[i]);
  seq->write(seq->cookie, i * 4, program[i]);
 }
 /* Third address is the target-specific A618 PDC resource from Linux.
  * Do not label it xo.lvl without command-DB identity evidence. */
 static const uint32_t address[] = {0x30010, 0x30000, 0x30090};
 static const uint32_t sleep[] = {1, 0, 0}, wake[] = {2, 2, 3};
 for (unsigned bank = 0; bank < 2; ++bank) {
  unsigned base = bank ? 0x15d6 : 0x1572; /* TCS3 / TCS1 DWORD offsets */
  pdc->write(pdc->cookie, (base + 1) * 4, 7);
  pdc->write(pdc->cookie, (base + 2) * 4, 0);
  pdc->write(pdc->cookie, base * 4, 0);
  for (unsigned i = 0; i < 3; ++i) {
   unsigned command = base + 3 + 4 * i;
   pdc->write(pdc->cookie, command * 4, 0x10108);
   pdc->write(pdc->cookie, (command + 1) * 4, address[i]);
   pdc->write(pdc->cookie, (command + 2) * 4, bank ? wake[i] : sleep[i]);
  }
 }
 pdc->write(pdc->cookie, 0x1148 * 4, 0);
 pdc->write(pdc->cookie, 0x1140 * 4, 0x80000001);
}
int
a618_gmu_boot_legacy(const struct a618_io *gmu, const struct a618_io *pdc,
 const struct a618_io *seq, struct a618_hfi *hfi, uint32_t gpu_vote,
 uint32_t gmu_vote, struct a618_gmu_boot_result *result)
{
 if (!valid_io(gmu) || !valid_io(pdc) || !valid_io(seq) || !hfi ||
     !hfi->table || !hfi->barrier || hfi->poisoned || !result ||
     !(gpu_vote >> 16) || !(gmu_vote >> 16)) return EINVAL;
 result->phase = result->offset = result->value = result->firmware_version = 0;
 /* Never reload a running M3 or pretend a warm boot is cold. The caller
  * has already validated every SMMU page and the unmapped guard. */
 if (gmu->read(gmu->cookie, A618_GMU_SYSRESET) != 1) return EBUSY;
 uint32_t version = gmu->read(gmu->cookie, GMU_CM3_DTCM_START + 0xff8 * 4);
 if (version == 0xffffffffu) return EIO;
 /* Polled faults only; no unhandled GMU interrupts reach the host GIC. */
 gmu->write(gmu->cookie, GMU_GMU2HOST_INTR_MASK, 0xffffffffu);
 gmu->write(gmu->cookie, GMU_AO_HOST_INTERRUPT_MASK, 0xffffffffu);
 gmu->write(gmu->cookie, GMU_GMU2HOST_INTR_CLR, 0xffffffffu);
 pdc_program(gmu, pdc, seq);
 gmu->write(gmu->cookie, GMU_GENERAL_7, 1);
 gmu->write(gmu->cookie, A618_GMU_FW_INIT_RESULT, 0);
 gmu->write(gmu->cookie, GMU_CM3_BOOT_CONFIG, 2);
 gmu->write(gmu->cookie, GMU_HFI_QTBL_ADDR, A618_HFI_IOVA);
 gmu->write(gmu->cookie, GMU_HFI_QTBL_INFO, 1);
 gmu->write(gmu->cookie, GMU_AHB_FENCE_RANGE_0, (1u << 31) | (0xau << 18) | 0xa0);
 gmu->write(gmu->cookie, GMU_CM3_CFG, 0x4052);
 /* 0x06010800 chipid in the GMU's packed minor/patch layout. */
 gmu->write(gmu->cookie, GMU_HFI_SFR_ADDR, 0x06018000);
 gmu->write(gmu->cookie, GPU_GMU_CX_GMU_PWR_COL_CP_MSG, A618_LOG_IOVA | 3);
 gmu->write(gmu->cookie, GMU_SYS_BUS_CONFIG, 1);
 gmu->write(gmu->cookie, GMU_ICACHE_CONFIG, 1);
 gmu->write(gmu->cookie, GMU_DCACHE_CONFIG, 1);
 /* ACTIVE idle policy: no automatic GX collapse until recovery exists. */
 gmu->write(gmu->cookie, GMU_PWR_COL_INTER_FRAME_CTRL, 0x09c40400);
 uint32_t rpmh = gmu->read(gmu->cookie, GMU_RPMH_CTRL);
 if (rpmh == 0xffffffffu) return EIO;
 gmu->write(gmu->cookie, GMU_RPMH_CTRL, rpmh | 0xf11);
 gmu->write(gmu->cookie, GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);
 hfi->barrier(hfi->cookie);
 result->phase = 1;
 gmu->write(gmu->cookie, A618_GMU_SYSRESET, 0);
 int error = legacy_gmu_poll(gmu, A618_GMU_FW_INIT_RESULT,
     version <= 0x20010004 ? 0xffffffffu : 0x1ff,
     version <= 0x20010004 ? 0xbabeface : 0x100, 10000, result);
 if (error) goto failed;
 result->phase = 2;
 gmu->write(gmu->cookie, GMU_BOOT_SLUMBER_OPTION, 0);
 gmu->write(gmu->cookie, GMU_GX_VOTE_IDX, gpu_vote & 255);
 gmu->write(gmu->cookie, GMU_MX_VOTE_IDX, (gpu_vote >> 8) & 255);
 gmu->write(gmu->cookie, GMU_HOST2GMU_INTR_SET, 1u << 22);
 error = legacy_gmu_poll(gmu, GMU_GMU2HOST_INTR_INFO, 1u << 30, 1u << 30, 10000, result);
 gmu->write(gmu->cookie, GMU_GMU2HOST_INTR_CLR, 1u << 30);
 if (error) goto failed;
 result->phase = 3;
 gmu->write(gmu->cookie, GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000);
 error = legacy_gmu_poll(gmu, GMU_SPTPRAC_PWR_CLK_STATUS, 0x38, 0x28, 100, result);
 if (error) goto failed;
 result->phase = 4;
 gmu->write(gmu->cookie, GMU_HFI_CTRL_INIT, 1);
 error = legacy_gmu_poll(gmu, GMU_HFI_CTRL_STATUS, 1, 1, 10000, result);
 if (error) goto failed;
 result->phase = 5;
 error = a618_hfi_bootstrap(hfi, gmu, gpu_vote, gmu_vote, &result->firmware_version);
 if (error) goto failed;
 result->phase = 6; /* GMU/HFI ready; this does not imply a working GPU CP. */
 return 0;
failed:
 /* Do not reset/power down behind an in-flight RPMh transaction or free
  * its DMA storage. Preserve the failure registers and poison the session.
  * Controlled shutdown/recovery is still required before any retry. */
 hfi->poisoned = 1;
 return error;
}
#endif
