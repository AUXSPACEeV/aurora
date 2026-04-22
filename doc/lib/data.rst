Data Logger
===========

Format-agnostic telemetry data logger for flight/sensor applications.
Sensor readings are captured as :c:struct:`datapoint` and serialised
through a pluggable formatter backend (vtable pattern).

Built-in Formatters
-------------------

Two formatters are provided out of the box:

- **CSV** (``CONFIG_DATA_LOGGER_CSV``) — one header row then one data
  row per datapoint.
- **InfluxDB Line Protocol** (``CONFIG_DATA_LOGGER_INFLUX``) — one line
  per datapoint, no header row.

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
