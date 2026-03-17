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

   data_logger_init(&logger, &data_logger_csv_formatter, "/lfs/log.csv");

   struct datapoint dp = {
       .timestamp_ms = k_uptime_get(),
       .type         = AURORA_DATA_BARO,
       .channel_count = 2,
       .channels = {
           { .val1 = 23, .val2 = 500000 },  /* temperature: 23.5 °C */
           { .val1 = 101325, .val2 = 0 },   /* pressure: 101325 Pa  */
       },
   };

   data_logger_log(&logger, &dp);
   data_logger_flush(&logger);
   data_logger_close(&logger);

API Reference
-------------

.. doxygengroup:: lib_data_logger
   :content-only:
