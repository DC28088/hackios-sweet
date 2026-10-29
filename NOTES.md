## A618 step 1: stages 1-4 reproduced on the current kernel (2026-10-29)

Source: ports/sweet/input/sweet_gpu.c (from ios-sweet fe987bf patches 0081-0088,
docs/GPU_PROGRESS.md there), wired through hakios_sweet_input_init(); boot-arg
sweetgpu=1..4, off by default, independent of the A642L driver (gpu=0 stays).
Kernel CI40890746478 (commit 94df36f) SUCCESS; kernel SHA256
c75c39ec9433fd5ae12dde49699a8e6b43703b966367a3293ed1e1b698de8cac in
out/hakios-20261029/kernel-sweet-gpu1. Boot run logs/195939-run.log.live with
BOOTARGS + sweetgpu=4 (cpus=8 gl=0), helper out/hakios-input-20261029-hook.
HARDWARE RESULT: audit CX_GDSCR=0x222001 CX_HW_CTRL=0x5ff4 GX_GDSCR=0x222001
GX_DOMAIN=0x1; all ten GPUCC CBCRs clk_off; ARC tables read; at 60 s the CX
GDSC powered: errno=0 control=0xf8228000 status=0x80001fe0 waited_us=1 -- the
same values the original port logged on 2026-10-19. No panic or reset.
Not done: GPU/GMU clocks, GX, firmware, GMU boot, HFI, SMMU, ring/SQE/zap,
any command, any rendering; GPU acceleration is NOT established.
Next (stage 5/6 of the original plan): GMU/bus clocks with frequency counter
checks, SMMU IDs, GMU TCM firmware staging (a618_gmu.bin in fw-gpu), then GMU
boot over legacy HFI; the earlier ios-sweet patches 0089-0094 are the source.

## Performance: gl=0 and cpus=8 (2026-10-29)

Run 193912 (cpus=8, gl=1): hakios-gl frames ~1 s each (softpipe, 3 draws,
1158887 us); touch path fine (read lag avg 54 us, send avg 227 us). User: minutes
to open an app. Run 194253 (cpus=8, gl=0 = CoreAnimation's own CPU renderer):
user: much better, still under 10 fps. tl: v intervals while in use: 229 of 329
are 70-120 ms (about 10 fps), ready->done small, so the CPU renderer takes
~100 ms/frame at 1080x2400 and is mostly single-threaded. sweet-current.conf
BOOTARGS now cpus=8 and gl=0. Screenshot: real iOS 16.5 home screen; black
wallpaper (PosterBoard services missing, upstream HakiOS also has none), grey
unblurred dock/search, blank Weather/Calendar widgets, battery shown nearly
empty (no battery driver). The original port's wallpaper/blur came from its
Metal->Vulkan->Turnip stack on a custom A618 kernel driver (35 GPU patches
0081-0144 in ios-sweet/xnu/patches, ~12-30 fps); none of it exists here, so
wallpaper correctness depends on the GPU path. CPU0 remains with the loader;
using it would cost live logs/clean return for about +1 core.

## Touch accepted by iOS; SMP enabled with cpus=8 (2026-10-29)

Kernel unchanged (0d1d544d...). Bridge CI40886524209 (commit after e53a58e): the
patch ports/sweet/patches/inject-service-race.patch now also hooks backboardd's
-[BKIOHIDService primaryUsage] to return 4 for a digitizer whose usage reads 0
and logs BK* "supported"/"builtIn" selectors. Libraries
out/hakios-20261029/current-gles-hook, helper
out/hakios-input-20261029-hook/root.apfs SHA256
1e13742b58dc60f6c63cfec29951f62a080e7c8b2b675370cf28298cbabe408e.
Intermediate attempt (race fix only, CI40884383086): attach line became
builtin/authenticated but backboardd still read primaryUsage 0 and ignored 288
events; run 191851.
Run 192802 (hook): "digitizer attached" shows primaryUsage 0x4 builtIn YES,
0 "ignoring digitizer" lines, 273 tl: i, backboardd logs "contact 1 presence:
touching/none". User: touch reaches Settings (press-in), Settings later crashed
(not investigated), system very slow. Observation: the boot line said cpus=1
because the profile never passed cpus; the loader defaults to one core.
Run 193912 with cpus=8 added to sweet-current.conf BOOTARGS: loader started
core 0x700 (boot) and the kernel reports "7 cpu" (cores 7,6,5,4,3,2,1; CPU0
stays with the firmware/loader). Responsiveness not yet judged by the user.
Power: Lock events reach backboardd, but without a backlight driver the screen
cannot visibly sleep. Settings crash, brightness, battery, Wi-Fi/BT, GPU pending.

## Touch service race fix built (2026-10-29)

Commit e53a58e; bridge CI40884383086 SUCCESS. Libraries downloaded to
out/hakios-20261029/current-gles-race (SHA256SUMS verified, upstream 88513176,
libhakios.dylib contains the new "stays/was never opened" log line). New helper
out/hakios-input-20261029-race/root.apfs, SHA256
0d6dd70e0bb4739b3bef17adb4c990f2d8ae74da23ca7da4d4643cd1dfe9cd4f, built with
prepare-current-helper.py --no-demo; sweet-current.conf now selects it. Kernel
unchanged (0d1d544d...). Resident loader and system image still in phone RAM at
last check. NOT yet booted: the race is probabilistic (1 of 2 runs bad), so one
good run does not prove the fix; success criteria are zero "ignoring digitizer
event" lines in the log and "builtIn: YES" in the "digitizer attached" line.

## Run 185703: touch events reach backboardd but are ignored (2026-10-29)

First attempt 185252 reset to stock fastboot before any log (no .live; same
kernel/helper/upload as earlier runs); retry 185703 booted to a dim lock
screen. User: touch, swipe, Power and Volume Down all did nothing.
Observations (logs/185703-run.log.live): kernel posted 420 touch events
(tl: i) and backboardd answered 423x "[TouchEvents] ignoring digitizer event
for display <main> from unsupported service" for our service: displayUUID 0x0,
eventSource externalAccessory, builtIn NO, authenticated NO. backboardd also
logged "[HID] strangeness: primary page is zero" for the service, then a second
"IOHIDEventSystemServer died. Reestablishing connection" ~2 s after our service
was made, and holds two BKIOHIDService objects for sender 0x1010403600001.
Run 173540 (same kernel) had 0 "ignoring" lines, 225 tl: i, and a
"system gesture stealing the touches" log, so this is a startup race in the
helper's virtual HID service (inject.c make_service), not a Goodix problem.
Guess (unverified): the event server restart after the service is opened drops
or half-restores its properties; fix is to re-make the service when the server
dies again, or to wait for backboardd's reconnect before making it.
Buttons (corrected): key 0 (Lock) logged 11 down/up pairs and key 2 (Vol Down)
5; they line up with backboardd "Lock page:0xC usage:0x30" lines, so they are
probably the user's repeated presses, not glitches (earlier note of "spurious"
was unverified). iOS set the backlight factor to 0/1 on them, but there is no
Sweet backlight driver yet, so the screen cannot visibly turn off or dim: the
missing visible Power effect belongs to the brightness work.
Fix in progress: ports/sweet/patches/inject-service-race.patch (each make_service
attempt gets its own ProductID, 2 s pause after removal, final state logged),
applied by build-current-bridge.sh; then CI bridge build, new helper, retry.

## Log review of 173540 run: touch and key events reach iOS (2026-10-29)

Observation from logs/173540-run.log.live (device now absent from USB, so no
new run): after Goodix selection the kernel posted "sample type 1/2 finger 0"
records (e.g. 22411,355 -> 21318,4316; HakiOS-normalized coordinates, a
vertical swipe) and backboardd logged "[TouchEvents] cancel clients due to
system gesture stealing the touches", i.e. SpringBoard's gesture recognizer
consumed a touch stream. SWEET KEYS posted key=0 down/up pairs (3 presses).
Not yet confirmed: visible UI response (unlock/swipe) and Power lock behavior;
the user must report that. Keys log only key=0 in this run (no Volume Down).
Phone state: ./xl state reports "absent"; no adb/fastboot device. Next action
once the phone is back in fastboot: ./xl enter, re-upload system if RAM lost,
rerun with the same profile and check unlock swipe, then start battery/brightness.

## Corrected input kernel: regulator vote and Goodix detection pass (2026-10-29)

CI40869721439/08c2e6e passed. Verified kernel and all source manifests:
out/hakios-20261029/kernel-sweet-input2/mach.development.bcm2837,
SHA0d1d544d7de84b94c63ae2932bed00684c49d2054c47b14886c244a0058de57f.
Current profile selects it with the same benchmark-free helper. Asked old
session to stop via ./xl stop; it returned cleanly with642159 log bytes,
logs/172639-run.log. Screenshot ios-input-first-home.png actually captures
the dim lock screen; user earlier reported home. Key events for actual power
and volume-down presses were posted; user says no reliable response, so
full button behavior not yet qualified.

Reused the same resident loader/system APFS after clean return (no reflash
or reupload). Host session45604, logs/173540-run.log.live, duration900s.
New touch trace: empty ACTIVE TCS1 normalized, L11C3-command vote done=1,
status0/1/2=0x10101, then Goodix PID9896, firmware03.02.00.0b read successfully;
FocalTech correctly NACKs. Selected GT9896 Yellowstone at1080x2400, poll10ms.
No touch sample claim yet. Asked user to swipe/tap/press keys once UI appears;
keep live USB capture and check tl: p / tl: i alongside actual screen result.
Wi-Fi, BT, brightness, battery and GPU hardware migration still pending.
No partition writes; original working HFS+ installation remains intact.

## Input hardware result: keys post; touch blocked by inherited TCS mode (2026-10-29)

Temporary input run logs/172639-run.log.live: keys find PMIC arbiter v5,
PON APID199 and PM6150L GPIO APID240, post volume transitions. Some one-poll
low glitches observed; not all transitions should be claimed as user presses.
Touch maps correct SM7150 SE7, I2C firmware0x303, FIFO packing readback good.
L11C vote stops with EBUSY: TCS0 idle1 control0x1010000 enable1; TCS1 idle1
control0x10000 enable0 wait0. Original rh_free requires control0, so no slot
accepted and touchscreen probe correctly aborts. No touch events confirmed.

Follow-up08c2e6e: allow only a completely empty idle ACTIVE slot with control
0 or AMC_EN (no trigger), no wait/enabled commands/pending IRQ; recheck and
clear inherited AMC_EN before requiring the original free-state predicate.
Matches the sequence in current HakiOS goodix rpmh_write; populated TCS0,
sleep/wake payloads and pending transactions still untouched. Key poller now
requires released-since-boot and two matching samples, following existing
HakiOS button debounce policy. CI40869721439 building. Current profile still
selects first input kernel until next artifact verified. Asked user to return
to FASTBOOT while building. Host run72941 may still poll until manual reset.

## Sweet input kernel compiled and verified; temporary boot underway (2026-10-29)

CI40868348864/e6ceba3 passed. Downloaded verified kernel to
out/hakios-20261029/kernel-sweet-input/mach.development.bcm2837,
SHA559bcf0341918f93f66d0ea125dbab8d66abed505a735ab03c2d0ca5f11eebaf.
SWEET_PATCH.sha256 and all4 SWEET_INPUT.sha256 entries match the local source;
UPSTREAM_COMMIT remains88513176b1e96ecf0b764cd62c6f4e6ac71345e3.
Current profile now selects that kernel, out/hakios-input-20261029/root.apfs,
latest loader-usb-replies; adds sweettouch=1 sweetkeys=1 touch_lat=1.
Temporary RAM boot started in host session72941,
logs/session-20261029-172450.log, duration900s. Loader accepted, system APFS
upload in progress. Input hardware not yet qualified; follow live log for
SWEET TOUCH/KEYS/RPMH and tl: p samples, then ask user to swipe/press buttons.
No raw partitions written. Previous kernel/helper retained for rollback.

## Home screen confirmed; Sweet input port building (2026-10-29)

User confirmed current GLES boot reached home screen, but neither touch nor
buttons work. This advances the previous logo-only observation. Live log
logs/170520-run.log.live records systemShellDidFinishLaunching, dismissing
Apple logo, and frames up to300. Software softpipe rendering only; still no
Adreno acceleration. User manually returned to FASTBOOT; previous run session
6283 ended with stock-fastboot detection (expected manual reset, not a proven
kernel fault).

User authorized migrating working original ios-sweet features and temporary
boots. Input source reconstructed from original fe987bf's ordered patches for
sweet_touch.c/sweet_keys.c/sweet_rpmh.c in ignored research/sweet-input-20261029
(isolated scratch git repo). Committed portable source under ports/sweet/input.
Latest standalone SMMU/DDR probe additions excluded from RPMh, retaining the
existing L11C and resource helpers. HakiOS adapter normalizes coordinates,
tracks touch down/move/up, maps lock/volume/home keys. sweet=1 switches input
startup from Redwood drivers to Sweet drivers; individual bootarg disables
remain. Live diagnostics route through hakios_tl. No tests added or run.

CI40868194811 at a03f88c superseded/cancelled by CI40868348864 at e6ceba3,
which corrects kernel type include ordering and documents provenance. Latest
kernel still building; no input hardware claim yet. Workflow adds input source
hash manifest. Keep latest complete matching kernel for next device boot.
Separate helper out/hakios-input-20261029/root.apfs prepared with --no-demo;
SHA5247fe1fc472f4ca0229e09c7170eccf7522290c6722c4dc5ae88a06c0556c3b.
Inherited com.redwood.demo CPU benchmark and diagnostics are disabled in
that helper. Current config still points at previous kernel/helper until
new build is downloaded and verified. Phone detected product sweetin.
Original ios-sweet working HFS+ root remains intact; no device writes.

## Current GLES context and frames verified; logo remains (2026-10-29)

User confirmed stock FASTBOOT. Started `bash common/run.sh sweet-current 900`
(host process session6283) with the matched current GLES helper and USB reply
loader. RAM upload2376MiB completed95s. Live log logs/170520-run.log.live
confirms all8 EAGLContext methods replaced and an OpenGL ES3.1 Mesa25.0.7
softpipe context created. First2 frames completed in1.70s and1.50s. Previous
Metal/OpenGLES context initialization failure is resolved in this run.
This is software rendering, NOT A618 acceleration. CPU count remains1.
Screenshots ios-gles-screen.png and ios-gles-screen-2.png under
out/hakios-20261029 show Apple logo only, no verbose text. SpringBoard pid7
connected to BackBoard; BootUI reports no explicit screen owner. Home screen
not verified. Live capture continues; asked user for screen state. Do not
restart/reflash until collecting this result. No partition writes performed.
The inherited demo launches cpu_test240; investigate removing that benchmark
from future helper/UI runs (do not alter executing scripts or original root).

## FULL APFS IOS USERSPACE VERIFIED; GLES package correction (2026-10-29)

Fresh-loader run logs/163515-run.log completed180s, returning207,498 kernel
log bytes. It mounted iOS16.5 APFS, ran Apple's launchd and SpringBoard/
BackBoard. No kernel panic in this run. Screenshot out/hakios-20261029/
ios-first-screen.png shows verbose text and boot logo. UI home screen NOT
verified. CoreAnimation explicitly failed to initialize Metal/OpenGLES contexts.
The user saw Apple logo plus lsd CPU-monitor entitlement warning spam. The
180-second capture returned normally to loader; screen then stays frozen.
The24-second IRQ-mask behavior did NOT prevent full capture; do not change
it based on earlier suspicion. Live USB logs continued to the end.

Found two protocol defects: set_result/getvar screen format via bounded
log_buf, so a full log breaks replies; empty logs stage DATA00000000, which
stalls firmware USB uploads. Loader c144c84 separates protocol formatting,
stages a newline for empty logs, and keeps returned kernel logs from being
mirrored back into Sweet's own small log buffer. CI40864379277 passed.

Current helper's new hook expects libhakios.gl.dylib but original helper
contained only libredwood-gl.dylib. Built the current matching EAGL bridge,
inject and SpringBoard hook from88513176 using the verified Mesa binary from
CI39772020663 (softpipe+freedreno, SHA15b698258864d0ef...). Khronos XML pinned
6af574a14089ccfee87efe230ebcdd8742859813; table uses actual Mesa exports.
CI40864530667 passed. This Mesa binary still has the old hardware backend;
only software GLES bringup is qualified by this reuse, not A618 acceleration.

Verified outputs: out/hakios-20261029/current-gles and loader-usb-replies.
New separate helper out/hakios-gles-20261029/root.apfs includes all matching
libraries. common/devices/sweet-current.conf now selects it and current loader,
quiet screen (no -v), gpu=0 gl=1. Next full RAM boot after user enters FASTBOOT:
`bash common/run.sh sweet-current 900`. No raw partition changes.
Latest package out/hakios-20261029/hakios-sweet-ui.img; APFS upload required.
User asked to switch current HakiOS; full A618 and touch/WiFi/BT/audio driver
migration remains pending. Original working ios-sweet/HFS+ root is intact.

## VERIFIED MINIMAL USERSPACE (2026-10-29)

logs/162730-run.log includes `Hello from user space on Darwin!`, process1,
gic virtual timer PPI19 and Darwin22.5.0. Tiny upstream hello deliberately
exits process1, giving the expected initproc panic; the loader returned the
full21,966-byte log over USB. Thus current XNU plus minimal userspace works
on Sweet with the corrected resident loader. This is not an iOS GUI result.

First full APFS attempt reused that returned loader: upload94s and kernel
staging passed, but preflight get_staged yielded zero bytes and oem run was
not acknowledged (logs/163007-run.log.*). No new kernel stage shown, so this
was a USB/loader session failure before actual launch, not a proven kernel
reset. User manually returned to FASTBOOT. Fresh-loader full APFS retry uses
`bash common/run.sh sweet-current 180`, logs/session-20261029-163326.log.
Matched profile explicitly chooses current kernel/working loader/separate
helper; source changes are on codex/hakios-refresh-20261029.

Latest package: out/hakios-20261029/hakios-sweet-resident.img (separate APFS
RAM upload still required). Older hakios-sweet-gic.img embeds the older,
failing cache-walk loader; use the resident image/current run profile instead.

## FIRST VERIFIED XNU ENTRY/RETURN (2026-10-29)

Loader CI40861302389 (fb4588d), SHA256
2f849fc4a362994ac7f258f3c084fcc1d9d4bb004cfab76f795d0546b64c5fb7,
reached XNU checkpoint1 on CPU7 and returned cleanly over USB. Full log:
logs/162645-run-cp1.log. Stages06/0A/07/0B/08/09 all passed; PSCI CPU_ON
core0x700 returned0. Kernel return reason checkpoint1. This verifies actual
kernel entry and return on Sweet, not iOS userspace or graphics rendering.

Skipping broad firmware cache walk first advanced to07; skipping the guessed
firmware stack +/-8KiB clean then got kernel entry working. Cache handling now
matches separate-CPU residency: leave firmware CPU state/cache intact; clean
kernel window and explicitly shared handoff data. Original userdata untouched.
Running the current Sweet GIC kernel with the upstream tiny raw-syscall hello
image and no checkpoint to capture later startup (logs/162730-run.log.*).

## Resident-CPU cache change advances to07 (2026-10-29)

Loader CI40860952000 SHA5750dbe0819a2b424ffa423c3f4dd31a6872b51852fcfb1165509d499606bd8f
advanced actual checkpoint1 attempt logs/162334* from0A to07 (user-confirmed),
then reset. Broad UEFI cache walk has been bypassed. Remaining stall is either
the guessed stack cache range or the subsequent kernel-window flush.
Removed Sweet's sp +/-8KiB clean (firmware CPU/MMU/cache remains live and
kernel CPU never accesses its stack); add0B immediately before kernel-window
flush. Other platforms retain existing stack handling. CI40861302389 atfb4588d
running. No kernel-entry checkpoint reached yet; do not call this an XNU boot.

## Context save completes; broad UEFI cache pass stalls (2026-10-29)

Preparation logs/162021-run.log reports CPACR_EL1=0x300000 and ICC_SRE_EL1=1:
FP/GIC accesses were already enabled on this firmware. Guards are defensive,
not an established root-cause fix. Actual checkpoint1 attempt logs/162030*
reached marker0A (user confirmed06 ->0A), then reset. This proves loader_save_ctx
completed and places the failure in the broad firmware memory-map/cache pass
before07. No kernel instruction has yet been shown to run.

For Sweet's separate-CPU mode, keep firmware CPU/MMU/cache state resident:
skip the unrelated UEFI descriptor cache walk, retain explicit kernel-window,
stack, context, entry-stub and start-record maintenance. Reject actual Sweet
same-CPU kernel runs (preparation-only still allowed), require bootcpu=7.
CI40860952000 (1da9028) passed. Download/boot first kernel checkpoint with this
loader next. This is a scoped handoff change; other platforms retain their
existing cache walk. Original HFS+ installation remains untouched.

## Preparation succeeds; actual handoff remains at06 (2026-10-29)

Loader CI40859960844 (23e5fb5), SHAda9c1f9bb1692d226d600d75c028f66458a4593a84c32f8def1816ac080dc72b,
with loader_prepare_only=1 returned `prepared` over USB. Full logs/161610-run.log
shows kernel at103004000..104284000, entry10344e4a8, return slot10344f7f0,
DT10428c000, boot_args10429c000, top1042a0000; framebuffer/identity/preparation
completed. Follow-up earliest kernel checkpoint1 (logs/161636-run-cp1*) reset;
user read the enlarged marker as06. This is before kernel entry, in context
save or the broad firmware cache-cleaning section. Prior clipped marker was
unreliable (user reported06/08); the preparation-only06 is deliberate.

Added FPEN guards for FPCR/FPSR save/restore; restore originally wrote FP
control after restoring potentially disabled CPACR. GIC ICC state save/restore
now checks/preserves ICC_SRE. Added CPACR/SRE preparation logging and stage0A
immediately after loader_save_ctx to separate context saving from cache walk.
CI40860579008 (b005741) passed; downloading/booting guarded preparation profile.
No root/peripheral changes. Kernel/iOS boot still unqualified.

## Small-image handoff isolation (2026-10-29)

Full-root Sweet GIC attempt logs/160417-run.log.* reset, host captured stock
fastboot product in .reset. Small upstream raw-syscall hello (no APFS upload)
also reset with bootcpu=7 (logs/160736*) and on the firmware's own CPU (no
bootcpu arg, logs/160943*). Thus the large APFS transfer is not necessary for
the reset, and the secondary-CPU path alone does not explain it. User also
confirmed each reset. No live kernel log has yet returned.

Cache maintenance DC/DSB and SMC/HVC wrappers lacked compiler memory clobbers;
corrected them and restored upstream's O1 loader build. Added a Sweet-only
framebuffer breadcrumb (two hex digits at top left), independent of ConOut:
01 enter run_kernel, 02 before window selection, 03 before segment loading,
04 before argument parsing, 05 before kernel metadata scans/device tree,
06 before context save/cache work, 07 after memory-map cache cleaning,
08 after kernel-window cache flushing, 09 before CPU handoff.
Guarded exception code retained. CI40859401177 (5439f66) passed. Downloading
and booting the stage loader with the same Sweet GIC kernel and tiny image.
The breadcrumb is diagnostic; kernel and iOS boot remain unqualified.

## Sweet GIC build/artifact ready (2026-10-29)

CI40857795079 (15de26b) passed. Upstream remains88513176b1e96ecf0b764cd62c6f4e6ac71345e3;
SWEET_PATCH.sha256 matches the reviewed local patch script. Kernel SHA256
f63881fd45383ed03db826daa0979d03d9e428c66ecb0cef6649712bf4370c13.
Guarded loader CI40857678413 (ddf46b0), SHA256
a8d2d6c84210e171e6454ba2a6b61fc5926511231199a059c04a056295074cb9.
Packaged out/hakios-20261029/hakios-sweet-gic.img, 8,046,592 bytes, SHA256
2d2292bc5a9cffe7b564d65a0df02c162257f7eea381bd37cf75a43c00f57b06.
The matching JSON contains firmware, DTB and loader hashes. Loader enumerated;
APFS upload and boot with -v sweet=1 gic_vtimer=19 bootcpu=7 gpu=0 underway.
Hardware/kernel/UI results still pending. Existing HFS+ userdata untouched.

## Follow-up source/loader builds (2026-10-29)

Port patch now also carries Sweet's original working nonsecure GIC distributor
bits (0x3), Group1NS redistributor grouping and combined EOI mode. Invoke with
sweet=1 gic_vtimer=19. Current CI40857795079 at15de26b; superseded timer-only
CI40857525430 cancelled. Guarded loader CI40857678413 atddf46b0 succeeded.
User described rapid text cycling while UEFI remained on screen; exact words
not available. Guard stops recursive exception reporting if ConOut faults.
Cause still unproven. Phone returned manually to FASTBOOT and is detected.

Full ramdisk prepare/stage command output is now saved by xl so upload
failures retain actual fastboot error text. Earlier grep-only output lost it.

## Sweet timer routing correction (2026-10-29)

A concrete generic-kernel mismatch was found: upstream gic_v3_irq.c hardcodes
virtual timer INTID27; original Sweet shim and 0004-sweet-machine-config.patch
prove this phone uses INTID19 (DT PPI3). Added a validated gic_vtimer boot arg,
default27, applied before boot-CPU interrupt enable and shared with secondary
CPU initialization and the IRQ dispatch path. Sweet boot must pass gic_vtimer=19.
The CI workflow applies this patch before xnu/apply.sh and records its hash.

The rebuilt generic kernel has not yet launched: revised loader started but
APFS upload lost USB. User reports UEFI/Project Silicium text still displayed;
this latest event does not establish a phone reset. Need FASTBOOT to restart
loader after building the timer correction. Do not confuse this with the
first attempt, where user explicitly reported reset. No kernel live log yet.

## Pinned source build succeeded (2026-10-29)

CI40805130176 (0bf18cb) passed using XNU's kernel `build` target.
Verified kernel SHA256 5cf38782067f1b1357936a25cd44782712b940156e0cc3dd4811e1dab6234fa3.
Loader CI40805161464 passed; SHA256
52bb08281adbfdcd47387e8bb5b34e742122925f04f922844a240d5d39253d06.
Packaged out/hakios-20261029/hakios-sweet-current.img (8,046,592 bytes), SHA256
a4785313f2fb744bb2f097256b749437293288144b2658b5fd974a378042b71d.
This is a resident loader/kernel container, requiring separate APFS RAM upload.
Kernel is current generic HakiOS, not yet a fully adapted A618/peripheral port.
Original Sweet UEFI retained; its Project Silicium banner is expected.

Prepared kernel boot args: -v bootcpu=7 gpu=0 (omit optional cpus enumeration).
All artifacts and logs in out/hakios-20261029 and logs/ remain ignored.
A host launch was interrupted by updating xl while Bash was reading it;
file passes syntax parsing and launch was restarted after the edit completed.
Do not edit an executing shell script in place. No phone write from that
interrupted launch occurred. Hardware result of the new boot is still pending.

## First refreshed-kernel attempt and follow-up (2026-10-29)

Current upstream prebuilt SHA03e794e8 boot attempted with gpu=0 cpus=1
bootcpu=7 and separately prepared helper SHA6e439dff. Sweet USB loader came
up, APFS upload completed (2376MiB/93s), kernel/helper staging and launch were
acknowledged. User saw Project Silicium/text, then reset; fastboot sweetin
returned. No kernel progress was captured. Logs: logs/074146-run.log.*.
This is a failed boot, not an iOS/GPU result. Original userdata untouched.

CI40804541130 linked/stripped the new kernel, then failed generating Apple
KEXT export symbolsets. Use XNU's `build` target (MakeInc.top:527), which
compiles/links the kernel without the separate `config` export packaging.
Do not ignore compiler/linker failures. Restore upstream USB-mode console
muting during run: our old debugging change re-enabled ConOut during handoff.
This is a possible stall source, not yet a confirmed reset diagnosis.

# Current HakiOS refresh (2026-10-29)

User requests current OOSEX GANG HakiOS on Sweet, while retaining Claude's
working original ios-sweet installation. Work is on codex/hakios-refresh-20261029.
Original ios-sweet HEAD fe987bf remains untouched. Phone identifies as sweetin.
No userdata/recovery/boot partition writes are needed: this is a RAM boot.

Reviewed kernel 88513176b1e96ecf0b764cd62c6f4e6ac71345e3,
Metal 54a46fadec5bd9684ebd77f3a0834c84b5eb01a6,
UEFI 687fa6c6a5a70f43184de92980e0a76eea5f2c4b and SM7325
04fbfff933f52faf203a55aca9dd5680092e6ec5. Userland moved into the kernel
repository; its old standalone repository is now a pointer. Source/readmes
and metadata are saved under research/upstream-20261029 (ignored).

Current loader/efi.h is identical to our existing version. Loader diff consists
of our Sweet platform, RAM-only logs, compiler compatibility and diagnostics,
plus upstream file renames. Keep the Sweet loader rather than the upstream
Redwood-only prebuilt. Current upstream A642L startup is not A618 compatible:
initial boot must use gpu=0 and omit adreno/display_probe and Redwood peripheral
arguments. Native hakios-metal currently implements copies/fills; its README
explicitly leaves shader execution unfinished. Mesa GLES supplies the UI.

Added a pinned upstream kernel CI workflow and XL_KERNEL/XL_LOADER overrides
so experimental outputs do not replace previous compiled components. Build,
boot and rendering qualification remain separate. Existing APFS system/helper
are retained; inspect helper compatibility before starting iOS.

User supplied Poco reference: https://github.com/nomorecoolnicknames/hakios-sm8150
README qualifies GPU, partial audio and main-camera capture on prior hardware
runs; latest rebuilt tree is not yet rebooted, haptics unverified. Pixel 5 and
Lisa exact references remain unresolved. CrackerCat/UniXNU-sweet explicitly
reports no bootable Sweet image yet; it is a different Darwin 23 baseline.

# HackiOS Sweet handoff

## UEFI boot failure correction (2026-10-22)

**Corrected firmware and boot image built successfully.** Firmware/loader
CI39821747578 on bb39a7e SUCCESS; package CI39822553764 SUCCESS. New image:
`out/boot-ramfix-39822553764/hackios-sweet-boot.img`, 7,833,600 bytes,
SHA256 `cd33fa30aba36911580fd9a0bcd4ced6f23a44e4928de3b4987f16a1ad3e8744`.
Embedded firmware SHA256
`c23089d46e15264dd418a30f6d445d90d331c2e0a27417a2545580876e1a5f8f`.
Android header, DTB magic, firmware, loader, USB flag and original XNU payload
checked against the component manifest. Kernel+helper is about20MiB (<64MiB).

Updated canonical out/firmware and out/loader to the verified new artifacts;
previous files saved as out/firmware-before-ramfix-39821747578 and
out/loader-before-ramfix-39821747578. Regenerated xnu-usb.img with corrected
Sweet firmware. mkxnuimg.py now defaults to Sweet so manual calls cannot
silently choose Redwood firmware. common/run.sh stops if system upload fails.
Both prepared APFS hashes were rechecked and match. No phone commands or
hardware boot were performed; no GPU rendering result is established yet.

Next: phone on FASTBOOT, run `bash common/run.sh sweet 900` from this repo.
It temporarily starts the loader, uploads system.apfs into RAM, then runs
the compiled kernel with root.apfs as its helper. Do not flash userdata for
this attempt. First CI39820327732 was cancelled after its Azure Ubuntu
package downloads stalled; fresh runner39821747578 completed normally.

User photo shows old boot14ab4eb6 stopping in DynamicRamDxe.c:151:
AddMemorySpace(A2370000,5DC90000) and a larger request from the same base
return Access Denied, followed by EFI_NOT_READY. XNU has not started. The added
F0000000..F4000000 payload reservation intersects the first dynamic RAM bank.
Original DynamicRamDxe adds the whole bank without excluding GCD reservations;
after that failure it incorrectly treats the next bank as another first bank.

Added Sweet firmware patch to walk GCD descriptors, register only unclaimed
RAM, preserve reserved/MMIO regions including the payload, and process each
SMEM bank independently. Registration/attribute errors propagate rather than
being hidden. Building corrected UEFI next; no hardware result yet. Kernel,
GLES and verified APFS images are unchanged. No tests or device commands.
Firmware CI39820327732 on addd022 is running; loader job already passed.
Rechecked both APFS SHA256 values locally: match. Also fixed package.py to
atomically replace its generated output: xl enter's repeated xnu-usb.img
build otherwise failed with FileExistsError. Package source0566e64 or later
is required for repeated sessions; the RAM firmware change itself is addd022.

## APFS root prepared (2026-10-22)

iOS16.5/20F66 system and helper images are built and verified locally:

- `userland/system.apfs`: 2,492,203,008 bytes, SHA256
  `ccfdbc0a8c7f3166eed76ca6e14fe9a1060d58884cda408aca2354447d7c55e7`.
- `userland/root.apfs`: 5,799,936 bytes, SHA256
  `a54bb3e83114c140b287fcc6e73720e8378c8084ad4f56d20045ff1cbf668125`.
- `out/root/manifest.json` and `out/root/SHA256SUMS` record the inputs;
  `hardware_tested` is false.

The helper was rebuilt from private APFS run39778207769 (source8c9682a),
GLES run39772020663 and the local A618/A630 firmware. `record-root.py`
verified the matching GLES libraries, firmware hashes, backboardd/SpringBoard
injection, APFS NXSB geometry and the absence of SSH/Wi-Fi credentials.
The private artifact was staged through a temporary private release
(run39796248307) because the Actions artifact signed link expired and its
endpoint was throttled; the transferred image hash matches the private build.
Temporary release/local staging is being removed. Apple images stay out of
Git and outside the public repository.

**No boot, flash, phone mount, USB command or userdata write was performed.**
Hardware boot and GPU/UI acceleration remain unverified. The existing
iOS16.7.16 HFS+ userdata installation is untouched; these APFS images are
prepared for a separate HackiOS USB-loader session.

## Built image (read first, 2026-10-22)

- **Final kernel/loader CI39773636216 SUCCESS**, source659167c3c90ed64f6db6ec24adacb6886b12babd.
  Kernel out/kernel/mach.development.bcm2837,15179440bytes,
  SHA25677b897c58d441944b6956413391ec439caac3ba114fe3ba437980f6e6fae0c40.
- Sweet firmware job CI39773154339 SUCCESS on3febfc8. The run as a whole
  had a standalone-loader missing-linker failure, fixed in later loader CI.
- GLES CI39772020663 SUCCESS on2048b0f. Local out/gles-39772020663 contains
  libOSMesa/EAGL/backboardd/SpringBoard dylibs, checksum/ARM64 validated.
- **Package CI39774118533 SUCCESS**, sourcec9be3d8. Downloadable artifact:
  https://github.com/DC28088/hackios-sweet/actions/runs/39774118533/artifacts/12423103290
- Final image: /home/dan/hackios-sweet/out/boot-39774118533/hackios-sweet-boot.img,
  7833600bytes, SHA25614ab4eb6d16d503eabe3a67d037f24b83d3b6bc7b8a81d6c1dff176c74d83072.
  Local SHA256, Android v1 header/page2048, exact shim/UEFI/loader/kernel
  payload and USB mode checked; embedded DTB hash matches manifest and its
  board identity is[46,0]. Decompressed local repack matches every byte;
  compressed bytes differ across host zlib versions. Header OS byte now
  explicitly pinned in packager, but cross-zlib byte reproducibility is not
  claimed. Compiler output has only upstream documented export-list failures
  after link; no source compilation errors were found in the initial log.
- **No phone command, hardware boot, runtime test, flash or userdata write.**
  Hardware behavior and GPU/UI acceleration remain unverified. Image starts
  resident USB loader, carrying compiled XNU; iOS16.5 APFS system/helper
  images must be prepared/uploaded separately. Existing16.7.16 HFS+ root is
  incompatible and preserved. Host lacks the ~8GB extraction space, so no
  Apple download/root preparation began. Vendor firmware remains private in
  original repo research/gpu/device-firmware. See ports/sweet/README.md.


2026-10-22: user changed direction after reporting Sweet0116 booted with zero
performance improvement. Requests a new repository porting HackiOS to Sweet
and a compiled boot image. Repository: https://github.com/DC28088/hackios-sweet.
Working tree /home/dan/hackios-sweet. Original /home/dan/ios-sweet is preserved.

Upstream kernel8e61187098f2c26c5daee8f9f72b6416aa64b9ab, userland906425b75e02b4f99eb5d7bb1ef7c9583472d772.
Both source trees combined here; see upstream-lock.json and LICENSE.userland.
Sweet UEFI pin098e303fd9703669ab06d38744cad14d49c0f702 (tagicmi Mu tree).

Initial source c84a685: Sweet loader/GIC/CPU/display profile, no raw logdump
writes, Sweet UEFI payload reservation + handoff, independent BootShim,
A618 firmware/clock/legacy GMU/HFI adaptation retaining HackiOS RedwoodGPU
MSM DRM service and Mesa/EAGL frontend. Build39771804932 is running kernel
and firmware jobs. No tests, phone commands or device writes performed.

Hardware port is NOT complete/qualified. Pending source review: parent rail
votes, A618 core register profile/GMEM and cleanup, Sweet I2C touchscreen (the
upstream touch driver is SPI), buttons, CPU frequency and USB adaptation.
Current default Sweet bootargs omit touchscreen/cpufreq/wifi/adsp. Need correct
Sweet input before a usable iOS boot. Preserve evidence and explicit gaps.

The boot image design is HackiOS's resident USB loader, with the compiled XNU
artifact separate. iOS16.5 APFS system/root images must be prepared separately;
the existing iOS16.7.16 HFS+ userdata cannot serve as that system image. Do not
reuse the old injected sweetgl.dylib with this kernel. The GLES build here is
the HackiOS original /redwood/lib frontend, not the previous NativeAdreno ABI.

Local vendor firmware exists in /home/dan/ios-sweet/research/gpu/device-firmware
(a618_gmu.bin, a630_sqe.fw, a615_zap.*); never commit it. Sweet live FDT is
/home/dan/ios-sweet/backup/live-fdt.dtb; use it read-only for public board facts.

Board adaptation continued: imported Sweet's existing GENI I2C touch (FocalTech/
GT9896), PMIC keys and serialized RPMh driver from ios-sweet through0114.
Adapted their output to HackiOS /dev/redwood-touch; no old HID service reused.
GPU rail prerequisites now use that same RPMh owner; A618 GMEM window programmed.
Initial CI39771804932 failed before kernel compilation (exported headers) and
loaderCOFF fixup.39772127622 identifies ADRP block_buffer+1MiB addend and SMC
assembler feature check. Replaced constant-addend pointer with opaque base,
SMC encoding with identical architectural .inst. Kernel export log now emitted
on failure. GLES39772020663 still building at time of this entry.

GLES CI39772020663 SUCCESS; artifacts downloaded out/gles-39772020663:
libOSMesa.8.dylib, libredwood-gl.dylib and backboardd/SpringBoard libraries.
Source2048b0f. No runtime test. Kernel39772464704 failed on missing host
uuid/uuid.h before exporthdrs; added uuid-dev. Firmware still running.
Latest work also closes direct write_logs partition paths (not only flush_log),
adds bounded touch-power synchronization and A618 register protection table.

GLES files locally checksum/ARM64 validated. libOSMesa.8.dylib14822264bytes
SHA25615b698258864d0ef4294d7d426d55cfb360edce1fb5aabfddc045b2dc5da2b19;
libredwood-gl.dylib541896bytes SHA25615de976fbd8a796ec455710b6855c0290908a63ac9212077e0e6823fa4a59861.
New strict Sweet packager ports/sweet/package.py validates Sweet FD size,
ARM64 BootShim/PE, DTB magic; no fallback to upstream Redwood prebuilt pieces.
Loader in USB mode now refuses rootless one-shot boot if USB setup fails.
No device commands performed. Firmware39772464704 still installing dependencies.

Runtime scripts adapted for the combined repository: common/setup.sh no longer
creates a second userland checkout over tracked files; xl requires out/loader
and out/kernel Sweet artifacts and defaults to Sweet bootargs. No Redwood
prebuilt fallback. Root preparation still needed for iOS16.5; host free space
~4.8GiB is below upstream extraction's ~8GB requirement, so no download begun.

A618 review caught two further A660 assumptions before hardware boot: SQE
needs CP_RB_RPTR_ADDR plus CP_WHERE_AM_I memory shadow; ring_send now emits
it, reads invalidate/acquire the dedicated scratch line, submit reserves3
extra words. Core GPU_SET OOB uses legacy request16/ack24, and shutdown uses
legacy BOOT_SLUMBER request22/ack30, not the uninitialized modern HFI table.
Latest kernel build is required; earlier source artifacts are superseded.

CI39773154339: kernel and firmware jobs both SUCCESS. Overall run failed only
because the separate loader job lacked lld-link; that was corrected and loader
CI39773276597 onaca7259 SUCCEEDED. Final A618 SQE/cleanup kernel source659167c
build39773636216 is running; it supersedes the earlier compiled kernel.
Artifacts downloading: firmware out/firmware and initial kernel
out/kernel-initial-3febfc8. Loader already in out/loader. Userland in
out/gles-39772020663. Package workflow added to produce downloadable boot image
from explicit component run IDs. No tests or device access.

Firmware39773154339 has downloaded and hashes match all components. Template
Android header v1/page2048 and192-byte ARM64 shim checked. Loader39773276597
is72KiB ARM64 PE. Final kernel39773636216 and loader job still awaiting
kernel completion; loader job already SUCCESS. Packaging now patches only
qcom,board-id to measured Sweetin[46,0] and records embedded DTB hash; no live
FDT dump is copied. See ports/sweet/README.md for exact scope and root layout.
APFS preparation requested. Added ports/sweet/prepare-root.sh to verify the
matching GLES artifact/source lock and private A618 firmware, fetch Apple's
iOS16.5 components, then build GL=1/PREBUILT=1/REST=1/SHARE=1/WIFID=0 RAM images.
userland/build.sh now accepts GLES_PREBUILT_DIR after its static prebuilt copy,
and does not require an optional gl_test executable for a runtime-only build.
No tests run. Host has 7-Zip26.01 and cc; unzbm utility compiled locally.
App cryptex096-03536-083.dmg downloaded (10,485,760bytes), extracted and LZBITMAP
files expanded; webbookmarksd is364KiB, no remaining compressed sidecars.
Full preparation currently waiting on storage:4.6GiB free versus12GiB guard.
Asked permission to remove only the old generated16.7.16 .simg (12GB) while
retaining its source .img, downloads and phone installation, or use another
storage path. No deletion or phone access performed.


2026-10-22 USB peripheral fix: user reports Project Silicium screen and
UsbDeviceOpenProtocols USB FN IO NOT_FOUND after the RAM fix. Pinned Sweet
Device-Binaries e9ebb074 selects UsbConfigDxe.patched.efi. Disassembly shows
its sole changed instruction at 0x3100 NOPs the platform-type branch, forcing
PortMode8 / CurrentMode override1 (host). ConfigUsbEvent requests peripheral4
but UsbStartController substitutes that host override; UsbfnDwc3 cannot bind.
Sweet preparation now selects the original same-pin UsbConfigDxe.efi; its
non-platform29/default path sets override4 (peripheral). Both input binaries
are hash-guarded. No imported Redwood USB binary or new binary patch. Loader
also reports init-event creation/signalling errors instead of hiding them.
Firmware rebuild pending; no hardware validation or GPU rendering claim.

Topaz reference reviewed at e4772911e116ab4ab6c4ac1f38e6106fefe8b483:
0901 TopazAdreno.cpp, eagl.c, topaz_inject.c, WLAN0123 and CREDITS. It shares
HackiOS RedwoodGPU/Mesa/EAGL ABI, but its A610 has no GMU; A618 needs its
existing GMU/HFI path. Found a concrete A618 mismatch: hardware UBWC setup
uses highest-bank-bit14, while msm GET_PARAM exposed13 to Mesa. Corrected
GET_PARAM to14, matching Linux v6.12 a6xx_calc_ubwc_config and our registers.
This fixes a layout mismatch, not evidence of completed GPU rendering.
Topaz's fixed QuartzCore hooks target16.7; our prepared HackiOS root is16.5,
so keep the matching16.5 injection addresses. Topaz WLAN0123 maps CE under
0x0c800000 with stream0x1a0; Sweet public DTS has ICNSS0x18800000 and stream
0x240, SMEM0x86000000. Its modem/transport code needs a Sweet hardware port;
do not enable the Redwood ath11k path or copy Topaz addresses blindly.

Build milestone: firmware/loader39825784943 SUCCESS (source d913d0b),
A618 UBWC parameter kernel39826212188 SUCCESS (3648248), combined boot image
39826923724 SUCCESS. Downloaded firmware/loader hashes verified; canonical
out/firmware and out/loader updated with backups before-usbfix-39825784943.
Canonical out/kernel updated after SHA256 verification; previous kernel in
out/kernel-before-bankbit-39826212188. New kernel SHA256:
0cd4677d830d6add40465d302171c15e1c882c4dce2057d0c5fd125c5696e4a8.
xnu-usb.img regenerated from corrected firmware and loader. APFS images kept.
No device access, tests, partition writes or hardware rendering verification.

User boot observation after39826923724: USB loader enumerated and host reported
2376MB system upload in93s/25MB/s, followed by reset into Android after another
2–3 minutes (not the900s session expiry). logs/ is empty; no saved kernel trace
supports a specific panic, watchdog or GPU diagnosis. wdt_arm currently only
arms the Redwood platform, so do not label this reset our Haven watchdog.
Default bootcpu is the loader's current core: live USB requests cannot be
served while XNU owns it. Next diagnostic should use bootcpu=7 so the loader
stays on the original core. common/run.sh now records a host transcript and
defaults XL_FOLLOW=5. xl saves staging responses, a preflight loader log, and
the launch response, and rejects a failed launch acknowledgment. No device
commands or tests run; this host-script change needs no new kernel build.

Next diagnostic run session20261022-233733: system uploaded92s; stage of
20979592-byte kernel/helper succeeded0.800s, launch acknowledged0.001s.
Preflight claims upper RAM100000000+f8000000. No live/final kernel log saved.
Read-only fastboot query reports stock sweetin, not xnuloader. Host xl run
still waiting, so no claim that XNU/GMU was reached. Found screen_quiet=1 in
USB command loop hid run preparation and UEFI exception text; exception
handler immediately ResetSystem'd. Loader now unmutes during launch and
exceptions, holds exception text60s using architectural counter before its
reset. Existing hardware watchdog may still reset earlier. Added loader_only
CI input to compile this without rebuilding unchanged kernel/firmware.
No device writes/boots performed; this is diagnostic instrumentation.

Diagnostic loader CI39829205280 SUCCESS (320a663), artifact checksum verified;
installed in out/loader with previous copy in
out/loader-before-handoff-39829205280. Regenerated xnu-usb.img and local
out/hackios-sweet-handoff.img using unchanged UBWC-corrected kernel and
USB-peripheral firmware. Package CI39829459841 dispatched for same components.
Next manual boot uses same bootcpu=7 command; obtain photo of last visible
loader/exception lines if it resets before USB polling resumes. No flashing.

2026-10-23 BDS integration audit: confirmed pinned Sweet Mu098e303f uses
PlatformBootManagerLib with AfterConsole and priority/fallback hooks. Our
old injection never returned from the beginning of AfterConsole, skipping
ConnectSequence/dispatch. EndOfDxe was already signalled in BeforeConsole;
the loader already disables the standard UEFI watchdog. Neither is a proven
reset cause. Move XNUP handoff to PriorityBoot (after full AfterConsole, before
disk BootOrder), with upstream UnableToBoot fallback. Using only the latter
would allow Mu's default disk EFI entries to intercept the payload. Retain
Sweet firmware/USB/RAM fixes and add visible handoff/load errors, bounded
payload traversal and a stopped screen on loader failure rather than reset.
Enable Sweet's existing other-CPU startup progress and default bootcpu=7 so
USB logging remains available. Raw partition logging remains disabled.
Build pending; this corrects integration/visibility, not a verified boot or
GPU fix. No hardware access or tests performed.

BDS build milestone: firmware and loader CI39891529224 SUCCESS (e02ce5f),
firmware4m10s/loader22s. Downloaded both artifacts, all SHA256SUMS match.
Canonical out/firmware and out/loader updated; backups retained in
out/{firmware,loader}-before-bds-39891529224. xnu-usb.img regenerated for xl.
Local combined image out/hackios-sweet-bds.img (7991296 bytes), SHA256
479e8f26140ef60cf3f0acadc5c9b2c9dea43e51ed0c8897a567480240a4b20e.
Kernel stays39826212188; APFS/GLES unchanged. Package39892018739 uses these
explicit component run IDs. No phone boot, device writes or runtime tests.
Boot success/reset resolution and GPU rendering remain unverified.
