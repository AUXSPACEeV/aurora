Data Logger
===========

Format-agnostic telemetry data logger for flight/sensor applications.
Sensor readings are captured as :c:struct:`datapoint` and serialised
through a pluggable formatter backend (vtable pattern).

Built-in Formatters
-------------------

The flight-time path uses a fixed-size **binary** formatter that writes
directly to a raw flash partition.  Two text formatters are provided as
post-flight conversion targets.

- **Binary** (``CONFIG_DATA_LOGGER_BIN``): live flight-time log, raw
  flash, no filesystem.  See :ref:`bin-format` below.
- **CSV** (``CONFIG_DATA_LOGGER_CONVERT_CSV``): conversion target.  One
  header row then ``CONFIG_DATA_LOGGER_CSV_WINDOW_NS``-sized lines with
  collected data points for that time window's duration.
- **InfluxDB Line Protocol** (``CONFIG_DATA_LOGGER_CONVERT_INFLUX``):
  conversion target.  One line per datapoint, no header row.

.. _bin-format:

Binary Flight-Time Log
----------------------

The binary formatter is the live writer used during a flight.  It
bypasses the filesystem entirely and writes frame-aligned blocks
straight to a fixed flash partition selected via the device-tree chosen
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

The partition is laid out as a sequence of fixed-size *frames*
(``CONFIG_DATA_LOGGER_BIN_FRAME_SIZE``, typically one flash erase block
of 4096 bytes).  Each frame begins with a 32-byte
:c:struct:`aurora_bin_frame_header` followed by densely packed 32-byte
:c:struct:`aurora_bin_record` entries; unused bytes at the tail of a
partial frame remain in the erased (``0xFF``) state.  Records preserve
the :c:struct:`sensor_value` channels losslessly so post-flight tooling
can replay filters and the state machine bit-exactly.

Buffering and Threading
~~~~~~~~~~~~~~~~~~~~~~~

The producer (the upstream logger thread, holding the data_logger
mutex) memcpys each record into a DMA-aligned RAM staging buffer.  When
a buffer fills, its index is handed to a dedicated writer thread via a
small free/flush index ring; the producer immediately picks up a fresh
buffer with a new frame header.  The writer issues one
:c:func:`flash_area_erase` plus one :c:func:`flash_area_write` per
frame, collapsing thousands of tiny writes per second into a handful of
page-aligned raw-flash operations with no FATFS or littlefs overhead.

Triple-buffering (``CONFIG_DATA_LOGGER_BIN_BUF_COUNT = 3``) is the
default — it absorbs the per-sector erase latency (~25 ms on QSPI NOR)
without backpressuring the sensor pipeline.  If the producer cannot get
a free buffer within
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
the first frame whose magic is invalid (== unwritten flash) **or**
whose ``flight_id`` differs from the first frame's — that is how the
boundary between the current flight and any leftover data from a
previous flight is detected.  The partition is **not** erased between
flights; old data is simply ignored.

Post-Flight Conversion
~~~~~~~~~~~~~~~~~~~~~~

After landing, :c:func:`data_logger_convert` replays the binary log
through any text formatter:

.. code-block:: c

   /* Convert the on-flash log to CSV on the filesystem. */
   data_logger_convert(&data_logger_csv_formatter, "/data/flight.csv");

The flash partition is left intact.  Conversion must not run
concurrently with active logging.

Example Usage
-------------

.. code-block:: c

   static struct data_logger logger;

   data_logger_init(&logger, "flight");
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

   data_logger_write(&logger, &dp);
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
