# iPod Photo retained crash record

Native A1099 builds reserve 256 bytes at the top of SDRAM for one structured
crash record. The reservation is outside the codec and plugin buffers, is not
part of the firmware image, and is not cleared by Rockbox startup. It costs 256
bytes of the normal audio buffer and performs no routine storage writes.

The record captures panic or ARM exception kind, CPU/COP identity, PC, LR and
SP when available, CPSR, exception SPSR, the Rockbox tick, and the most recent
native IPVF phase, frame, slot, and heartbeat age. Panic text is truncated to
47 bytes. The producer invalidates the old magic first, writes the body, then
publishes the CRC and magic with memory barriers. Random or partially retained
RAM is rejected by magic, version, size, and CRC checks.

The SP is the banked fatal-handler stack after entering `UIE`, not the
interrupted thread's original SYS-mode SP. IPVF fields are best-effort liveness
context: CPU and COP update one uncached structure without blocking, so a crash
that lands during an update can combine adjacent field generations. This does
not affect record integrity or control policy. If CPU and COP enter fatal paths
simultaneously, their writes can contend for the single slot; the result is
either one valid winner or a record rejected by CRC.

Open **System > Debug > View crash record** after reboot to inspect the last
valid record. Hold the context/menu action on that page to clear it. Viewing
or clearing the record does not write to storage.

This is warm-reset retention, not nonvolatile storage. It is expected to
survive a PP5020 system reset when SDRAM contents remain intact, including a
normal Rockbox bootloader pass that only loads the firmware at the bottom of
SDRAM. A battery disconnect, full power loss, RetailOS boot, disk mode, or a
bootloader that reinitializes all SDRAM may destroy it. Hardware testing must
therefore verify retention before relying on it for field diagnosis.

No watchdog, COP reset, or automatic recovery policy is attached to the
record. The fatal-screen and reboot behavior remains unchanged.
