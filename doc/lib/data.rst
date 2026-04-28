Data Logger
===========

Format-agnostic telemetry data logger for flight/sensor applications.
Sensor readings are captured as :c:struct:`datapoint` and serialised
through a pluggable formatter backend (vtable pattern).

Built-in Formatters
-------------------

The flight-time path uses a fixed-size **binary** formatter that writes
directly to a raw flash partition or to a raw region of a disk-access
block device.  Two text formatters are provided as post-flight
conversion targets.

- **Binary** (``CONFIG_DATA_LOGGER_BIN``): live flight-time log, no
  filesystem.  Two backends are selectable via
  ``CONFIG_DATA_LOGGER_BIN_BACKEND``:

  - ``DATA_LOGGER_BIN_BACKEND_FLASH`` — internal flash partition,
    treated as a circular ring with **boost-freeze** semantics.
  - ``DATA_LOGGER_BIN_BACKEND_DISK`` — sector range of a Zephyr
    disk-access device (typically SD card), written linearly through a
    RAM ring buffer.

  See :ref:`bin-format` below.
- **CSV** (``CONFIG_DATA_LOGGER_CONVERT_CSV``): conversion target.  One
  header row, then one row per ``CONFIG_DATA_LOGGER_CSV_WINDOW_NS``
  time window.  Each row carries one full sensor snapshot — every
  datapoint whose timestamp falls within the window is merged into the
  same row.  Cells for sensor groups that did not produce a sample in a
  given window are left empty.
- **InfluxDB Line Protocol** (``CONFIG_DATA_LOGGER_CONVERT_INFLUX``):
  conversion target.  One line per datapoint, no header row.

.. _bin-format:

Binary Flight-Time Log
----------------------

The binary formatter is the live writer used during a flight.  It
bypasses the filesystem entirely and writes frame-aligned blocks
straight to one of two backends.  The on-storage layout is identical
across backends: a sequence of fixed-size *frames*
(``CONFIG_DATA_LOGGER_BIN_FRAME_SIZE``, typically one flash erase block
of 4096 bytes).  Each frame begins with a 32-byte
:c:struct:`aurora_bin_frame_header` followed by densely packed 32-byte
:c:struct:`aurora_bin_record` entries; unused bytes at the tail of a
partial frame remain in the erased (``0xFF``) state.  Records preserve
the :c:struct:`sensor_value` channels losslessly so post-flight tooling
can replay filters and the state machine bit-exactly.

Flash Backend (circular)
~~~~~~~~~~~~~~~~~~~~~~~~

Targets a fixed flash partition selected via the device-tree chosen
entry ``auxspace,flight-log``:

.. code-block:: dts

   &flash0 {
       partitions {
           compatible = "fixed-partitions";
           #address-cells = <1>;
           #size-cells  = <1>;
           flight_log: partition@300000 {
               label = "flight_log";
               reg   = <0x300000 DT_SIZE_M(1)>;
           };
       };
   };
   / { chosen { auxspace,flight-log = &flight_log; }; };

The partition is treated as a **circular ring**.  Pre-boost telemetry
overwrites old data continuously; once a :c:enumerator:`DLE_BOOST`
event is delivered via :c:func:`data_logger_event`, the ring is frozen
forward from that point and writes proceed linearly until the partition
fills or :c:enumerator:`DLE_LANDED` plus the post-landed pad window
closes the logger.  This trades unbounded pre-boost history for a
guaranteed BOOST→LANDED window on partitions too small to hold a full
flight.

Producer-side records are memcpy'd into DMA-aligned RAM staging
buffers; ``CONFIG_DATA_LOGGER_BIN_BUF_COUNT`` (default 3, triple
buffering) absorbs the per-sector erase latency (~25 ms on QSPI NOR).
The writer thread issues one :c:func:`flash_area_erase` plus one
:c:func:`flash_area_write` per frame.

Disk Backend (linear ring buffer)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Targets a sector range of a Zephyr disk-access device (typically an SD
card via ``zephyr,sdmmc-disk``).  The region is described by an
``auxspaceev,flight-log-disk`` node referenced through the chosen entry
``auxspace,flight-log-disk``:

.. code-block:: dts

   / {
       chosen {
           auxspace,flight-log-disk = &flight_log_disk;
       };

       flight_log_disk: flight-log-disk {
           compatible = "auxspaceev,flight-log-disk";
           disk-name = "MMC";
           offset-sectors = <2097152>;  /* skip first 1 GiB of card */
           size-sectors   = <1048576>;  /* 512 MiB region */
       };
   };

The byte size (``size-sectors * sector-size``) must be a whole multiple
of ``CONFIG_DATA_LOGGER_BIN_FRAME_SIZE``.  The application is
responsible for ensuring any filesystem on the same card does not
overlap this range.

The disk writer is purely **linear** from the configured offset — the
region is sized for many minutes of flight, so circular wrap is not
used.  :c:enumerator:`DLE_BOOST` is recorded but does not freeze a
ring.

Producer records are memcpy'd into a contiguous in-RAM ring of
``CONFIG_DATA_LOGGER_BIN_RING_FRAMES`` slots (must be a power of two;
effective capacity is ``RING_FRAMES - 1`` committed frames since one
slot is always reserved for the producer).  The writer thread coalesces
up to ``CONFIG_DATA_LOGGER_BIN_MAX_BATCH_FRAMES`` contiguous committed
frames into a single ``disk_access_write()`` call to amortise SD-stack
overhead and play to the card's preference for multi-block transfers.
At the default 16 slots × 4 KiB frames the ring buffers ~64 KiB of
telemetry, enough to ride out an SD card's 100+ ms internal
garbage-collection stall before the producer back-pressures.

Common Behaviour
~~~~~~~~~~~~~~~~

If the producer cannot get a free buffer/slot within
``CONFIG_DATA_LOGGER_BIN_PRODUCER_TIMEOUT_MS`` it drops records and
sets a sticky error on the logger context.

Only one binary logger instance can be live at a time;
:c:func:`data_logger_init` rejects a second concurrent open of the bin
formatter.

Per-Flight Framing
~~~~~~~~~~~~~~~~~~

Every frame header carries a ``flight_id`` (the high-resolution uptime
clock at the moment the logger was opened) and a monotonic ``seq``
starting at 0.  The converter walks frames from offset 0 and stops at
the first frame whose magic is invalid (== unwritten storage) **or**
whose ``flight_id`` differs from the first frame's — that is how the
boundary between the current flight and any leftover data from a
previous flight is detected.  The storage region is **not** erased
between flights; old data is simply ignored.

Lifecycle Events
~~~~~~~~~~~~~~~~

The upstream application drives flight transitions into the formatter
via :c:func:`data_logger_event`:

- :c:enumerator:`DLE_BOOST` — boost detected.  The flash backend
  freezes its ring forward from this point; the disk backend records
  the event without behavioural change.
- :c:enumerator:`DLE_LANDED` — landed.  The upstream caller keeps the
  logger open for ``CONFIG_DATA_LOGGER_BIN_POST_LANDED_PAD_MS`` to
  capture post-landed telemetry, then closes it.

Combined with pre-boost data already captured by the circular ring on
the flash backend, the converted log spans roughly
[BOOST minus whatever pre-boost padding fits in the ring,
LANDED + post-landed pad].

Post-Flight Conversion
~~~~~~~~~~~~~~~~~~~~~~

After landing, :c:func:`data_logger_convert` replays the binary log
through any text formatter:

.. code-block:: c

   /* Convert the on-storage log to CSV on the filesystem. */
   data_logger_convert(&data_logger_csv_formatter, "/data/flight.csv");

The flight-log region is left intact.  Conversion must not run
concurrently with active logging.

Example Usage
-------------

.. code-block:: c

   static struct data_logger logger;

   data_logger_init(&logger, "flight", &data_logger_bin_formatter);
   data_logger_set_default(&logger);
   data_logger_start(&logger);

   struct datapoint dp = {
       .timestamp_ns = k_ticks_to_ns_floor64(k_uptime_ticks()),
       .type         = AURORA_DATA_BARO,
       .channel_count = 2,
       .channels = {
           { .val1 = 23, .val2 = 500000 },  /* temperature: 23.5 °C */
           { .val1 = 101325, .val2 = 0 },   /* pressure: 101325 Pa  */
       },
   };

   data_logger_log(&dp);                          /* default logger  */
   data_logger_event(&logger, DLE_BOOST);         /* state-machine   */
   /* ... */
   data_logger_event(&logger, DLE_LANDED);
   data_logger_flush(&logger);
   data_logger_close(&logger);

Shell Commands
--------------

Enabling ``CONFIG_DATA_LOGGER_SHELL`` registers the ``data_logger``
command group. All commands operate on loggers registered through
:c:func:`data_logger_init` logger names tab-complete.

.. list-table::
   :header-rows: 1
   :widths: auto

   * - Command
     - Description
   * - ``data_logger list``
     - List every registered logger with its formatter and current state
       (``running``, ``stopped`` or ``closed``).
   * - ``data_logger start <name>``
     - Start a logger. Writes the formatter header if applicable.
   * - ``data_logger stop <name>``
     - Stop a logger. Subsequent writes are dropped until restarted.
   * - ``data_logger status <name>``
     - Show the state of a single logger.
   * - ``data_logger flush <name>``
     - Flush buffered data to the backing storage.

API Reference
-------------

.. doxygengroup:: lib_data_logger
   :content-only:
