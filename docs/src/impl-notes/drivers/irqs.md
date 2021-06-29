# IRQ Handling

IRQ handling is one of the most tricky parts in a driver.
Handling IRQs incorrectly can not only render the driver unusable
but also impact the functionality of other drivers that share
the same hardware IRQ line.

## General IRQ handling strategy

Handling an IRQ in your driver generally involves the following steps:

1. **Wait** for an IRQ (using `helix_ng::awaitEvent()`).

1. **Determine if the IRQ was triggered by your device**
   (and not by some other device that shares the same IRQ line).
   This usually requires reading some interrupt status register ("ISR").

   IRQs that are indeed caused by your device will need
   to be ACKed at a later step, IRQs that were caused by other devices
   that share the same IRQ line need to be NACKed.

1. Instruct the hardware to **clear** the IRQ.

   For level-triggered IRQs, this will de-assert the IRQ
   (i.e., cause it not be be fired immediately again).
   For edge-triggered IRQs, this step usually
   re-enables the hardware device to issue an edge once a new
   (device-specific) event occurs.

1. **ACK** or **NACK** the IRQ (using `helAcknowledgeIrq()` with appropriate flags).

1. Depending on the device: do **further processing**.
   For example, some hardware devices will require you to inspect
   ring buffers to collect the requests that were finished by
   the device.

The order of these steps is significant:
determining if the IRQ was caused by your device only makes
sense after an IRQ occurred,
and you cannot ACK/NACK without knowing
whether the IRQ was caused by your device.
Furthermore, not following the above order can lead to subtle
bugs; hence, drivers should always stick to this order
(see below for [additional justification
in the case of level-triggered IRQs](#guidelines-for-drivers-and-potential-pitfalls)).

## Kernel behavior

The following rules form the basis of the [guidelines
for drivers below](#guidelines-for-drivers-and-potential-pitfalls).

The kernel handles IRQs in a strictly sequenced way:
when an IRQ line is raised, the kernel marks the IRQ
as "in-service" and invokes all IRQ handlers
(i.e., IRQ descriptors used by drivers, as well as in-kernel IRQ handlers).
While the IRQ is in-service, IRQ handlers (of the same IRQ line)
will not be invoked again.
The kernel resets the in-service status
only after all IRQ handlers (i.e., all drivers)
have either ACKed or NACKed the IRQ.

If all drivers NACK an IRQ, the kernel will
*stall* the IRQ line. More precisely,
it will mask the line, preventing further IRQs from being raised.
This mechanism protects against
"IRQ storms", i.e., recurring IRQs that block overall
system progress since the system
does not know how to handle them.
Drivers need to take care to not accidentially cause a stall.

## Guidelines for drivers and potential pitfalls

To avoid stalls,
drivers should take the following constraints into consideration:

* A driver must avoid to clear IRQs that the kernel did not deliver
  to the driver yet.
  Clearing an IRQ that was not delivered yet causes
  the IRQ to not be recognized by any device;
  that is, it causes the IRQ to become "spurious".

  If spurious IRQs cannot be avoided (e.g., because
  a device might assert and then clear its IRQs during a hardware
  reset), drivers must call `kHelAcknowledge()` with the
  `kHelAckKick` flag. Failing to "kick" the spurious IRQ
  usually lead to all drivers NACKing the IRQ
  (and hence stall the IRQ line).

* "Wrong ACK": ACKing an IRQ that was not actually caused by the
  device in question is *not* harmful as long as it happens only rarely:
  if another driver that shares the IRQ line also ACKs,
  the wrong ACK does not impact system-wide IRQ behavior.
  Otherwise, if all other drivers
  NACK, the ACK causes the IRQ temporarily
  to not be stalled.

* "Wrong NACK": the reverse, namely NACKing an IRQ that actually came from
  the driver's corresponding device is problematic since it can cause stalls
  (if all other drivers NACK).

* After obtaining a handle to an IRQ descriptor, a driver
  *must* handle IRQs in a timely manner. Keeping IRQs
  in-service for extended periods of time can delay other drivers
  that share the same IRQ line.

<!--- TODO: move this point to an "IRQ initialization section once we have one. -->
* Handling an IRQ by a kernlet requires special attention during initialization:
  if an IRQ is currently in-service, attaching a kernlet *does not*
  immediately run the kernlet. Thus, if a driver wants to handle IRQs *exclusively*
  through the use of kernlets, the IRQ must potentially be ACKed after
  installing the kernlet. This can be achieved by using
  `helAcknowledgeIrq()` with the `kHelAckKick | kHelAckClear` flags
  (which is not harmful since it is equivalent to a wrong ACK).

**Considerations for level-triggered IRQs**:

* Level-triggered IRQs must always clear an IRQ before ACKing it
  ([see above](#general-irq-handling-strategy)).
  ACKing a level-triggered IRQ before clearing will cause
  the IRQ to be re-raised immediately.
  Clearing the IRQ after the re-raise may cause
  the IRQ to be become spurious
  (and thus stall the IRQ line).

**Considerations for edge-triggered IRQs**:

* The kernel will buffer (a single) IRQ if an edge
  happens while an edge-triggered IRQ is still in-service.
  This ensures that IRQs that occur after clearing the ISR
  but before ACKing are handled correctly.

* Spurious IRQs are problematic for edge-triggered IRQs:
  similarly to level-triggered IRQs,
  drivers will NACK spurious IRQs and thus to stall the IRQ line.
  This is not an issue for devices that do not re-issue
  edges until the ISR register is cleared yet.
  However, various legacy devices (such as the `x86`'s keyboard controller)
  have no ISR register. If these devices are not on shared
  IRQ lines, they can simply always ACK all IRQs to avoid stalls.

<!---
TODO: Add a section on the initialization of IRQ handling;
    Discuss `enableBusIRQ()` etc.
-->
