.. SPDX-License-Identifier: GPL-2.0

=============
Printk design
=============

:Date: April, 2025
:Author: Petr Mladek <pmladel@suse.com>

Introduction
============

printk() is the kernel mouth. It allows kernel to print a message
about what is going on.

The messages might be of different meaning from debugging, normal
information, though warnings, to various levels of warnings,
errors, and critical situations. The meaning is defined by a perpended
loglevel, see printk-basics for more info.

The messages are first stored into a ringbuffer and then the kernel
actively tries to show on registered consoles. Userspace applications
might read the messages either via the obsolete syslog syscall or
the special /dev/kmsg device. Finally, the messages might be dumped
during the kernel panic using registered kmsg dumpers. Of course,
they might be also found in the memory image produced by crashdump.

printk() has been available since the very first kernel release.
It was rather easy code at that time. It became more and more
with addind new features, namely:

  - IRQs safe
  - SMP safe
  - deserialized SMP console flushing
  - non-mixed messages (record based ring buffer)
  - continuous lines
  - NMI safe
  - lock-less ring buffer
  - RT safe

At the moment, there are actually two parallel implementations
of consoles handling. Legacy console drivers serialized using
console_lock (semaphor). NBcon console drivers which are more
reliable while providing RT guarantees which is helpful even
on huge systems with hundreds of CPUs and other devices.

Lockless ring buffer
====================

Console registration
====================

Legacy console drivers
======================

The legacy console drivers are synchronized using console_lock which
is implemented using console_sem.

A semaphore is used because an alternative mutex_trylock() would not
work in an atomic context. The mutex implementation is strictly connected
with a process context to prevent problems with reverse scheduling priority.

The lock has been historically used to sychronize several operations and
suffers by a big kernel lock symtoms. Namely it sychronizes:

  1. From the top level perspective it sychronizes flushing pending
     messages to registered consoles. The current owner is responsible
     for flushing all pending messages.

     This behavior prevents serialization of parallel printk() calls.
     Note that writing to a console is a relatively slow operations,
     especially on slow serial consoles.

     But it also brings the possibility of possible softlockups or
     even livelockups. It might happend when the current owner is in
     an atomic context and other CPUs are adding too many new messages
     in parallel.

  2. The console_lock sometimes synchronizes con->write() operations on
     the same HW device between the early/boot console and regular console
     drivers.

     Note that the console drivers typically use another device-specific
     lock for sychronizing operations on the same device. But for example,
     the uart 8250 driver uses port->lock. But the early uart 8250 console
     driver uses the device before a struct uart_8250_port is assigned and
     initialized.

  3. The conosle_lock is used to sychronize various operations in the tty VT
     implementation. It is a historicall inheritance. The printk(), tty, and
     vt subsystems were added early into the kernel and were closely connected.

The console_lock has been hisorically used to synchronize even more operations
but they were already untangled:

  1. Console registration is synchronized by console_list_lock() implemented
     by console_mutex.

  2. The list of registered console can be iterated under console_srcu lock.

  3. Syslog syscall opeartions are sychronized using syslog_lock (mutex).

  4. /dev/kmsg read is sychronized using per-user mutex stored in
     struct devkmsg_user.

  5. The registration of kmsg dumpers is sychronized using dump_list_lock
     spin lock. Note that the dumping operation does not need to be synchronized
     because it happens only during panic() from a single CPU.


nbcon consoles
==============

nbcon consoles, aka non-blocking or no-big-lock consoles, are the new and
preferred implementation of console drivers. They were developed when adding
RT support into the upstream kernel. But even the non-RT mode benefits from
the changes.

The main feature is that they are flushed from a dedicated per-console
kthread and printk() does not longer block the caller for an upredictable
amount of time. It does not longer matter how many console drivers are
registered, how how they are, or how many messages are added in parallel.
Also faster console drivers could flush the messages more quickly then
the slower ones.

The above is true only when the system works as expected. In case of,
emergency, or panic(), printk() tries to flush the messages immediately,
similar to the legacy consoles.

A special lock has been invented to support the desribed hehavior.
It is called "nbcon_context" and has the following features:

  - Three priorities: normal, emergency, panic.

  - Distinguish code sections where it is safe or unsafe to take over
    the ownership. For example, it is not safe when the HW device is
    manipulated and in an inconsistent state. Otherwise, it is safe,
    for example, when preparing the buffer, or between emitting
    sigle characters.

  - Safe takeover from a higher priority context.

  - Unsafe takeover in the panic context for the final flush.

The nbcon context lock is connected with struct console. Each driver has
on such structure. It is connected with a particular physical device
during the console registration by setting a valid con->index.

Obviously, the nbcon console context can't be used to synchronize
all operations on the physical device because it is assigned too late.
Instead, the sychronization is still done using the native per-device
lock, for example, port->lock.

The trick is that, both the device-specific lock and nbcon context
are taken together once the device gets assigned to the console driver
during the console registration.

The device-specific lock is the primary synchronization primitive for
all operations on the device, including con->write_thread(). It does
not only make it safe but it also makes the locking RT-friendly
when the messages are flushed by the dedicated kthread.

Only the nbcon context is acquired in emergency and panic situations.
It allows to take over the ownership a safe way even before
the currently handled message is fully emitted. Which increases
the change to see the emergency and panic messages.

Limitations
===========

Early boot consoles can't be serialized with most regular console drivers
because the device-specific lock gets assinged and initialized too late.
It is true for both legacy and nbcon consoles.

Note that the sychronization using the nbcon context does not help
because works only when a particular device is assigned to the related
struct console. It happens during the console registration when
the console is being enabled.

The problem is "temporarily" solved by console_lock(). All consoles, including
nbcon ones, are flushed using the legacy loop as long as any boot console
is registered. Also newcon->setup() is called under console_lock() during
a console registration.

Hints for nbcon cosole driver developers
========================================

1. Add a wrapper over the device-specific lock. It must be synchronized
   with the nbcon context. Namely, the console context must be taken under
   the device-specific lock once the particulal device gets registred
   as a console. For example, see uart_port_lock() API.

   Use the wrapper everywhere instead of taking the device->specific
   lock directly.

2. Implement con->device_lock() and con->device_unlock() using
   the device-specific lock.

   These callbacks will prevent races when the device is being
   registered as a console and the nbcon context gets assigned.

   Also they are used to synchronize con->write_thread() callback
   against other operations using the same device. They must be
   RT-friendly. For example, they must use normal spin_lock() API
   and _never_ the raw_spin_lock() one.

   IMPORTANT: The lock must prevent CPU migration. Otherwise,
   it would not allow to take the nbcon context as safe way.

   For example, see pl011_console_device_lock()/unlock().

3. Implement con->write_thread() callback. It is taken under both
   con->device_lock() and nbcon normal context. It must be
   a schedulable process context in CONFIG_RT. It might be
   a process context in non_CONFIG_RT. Just the CPU migration
   must always be disabled to preserve the nbcon context a
   safe way.

4. Implement con->write_atomic() when possible. It must work
   in any atomic context, including NMI. It is sychronized only
   by taking the nbcon context with either emergency or
   panic priority.

Both con->write_thread() and con->write_atomic() callbacks are
entered in safe nbcon context by default. The only exception is
when they are called after an unsafe takeover by the final
desperate flush in panic().

Note that "safe" means that the context is safe for takeover by
a higher priority context.

The write_*() callback must enter nbcon unsafe context before
it tries to modify any shared resources on the device. It will
ensure an exclusive access as long as the kernel works properly,
except for the final flush in panic().

The write_*() callback should exit the unsafe context everytime
the device is in a consitent state so that it would be taken
over by a higher priority context. Ideally, it should do this
after emitting each single character of the given message.

Note that both nbcon_enter_unsafe() and nbcon_exit_unsafe() might
fail. It happens when a higher priority context took over the ownership
and there is a pending request. In this case, the callback must
not continue emitting the message because the given buffer
might be already reused.

On the other hand, the callback must try to reacquire the lost
context ownership using nbcon_reacquire_nobuf() and restore
the original setting of the device, for example, restore
the control register. Note that reacquire() call would only
succeed when the nester higher priority context finished
writing and released the nbcon context.
