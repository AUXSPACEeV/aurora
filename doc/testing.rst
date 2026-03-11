Testing
=======

AURORA uses the Zephyr :external+zephyr:ref:`ztest <test-framework>` framework
and the :external+zephyr:ref:`twister <twister_script>` test runner.
Unit tests run on ``qemu_x86`` and require the ``x86_64-zephyr-elf`` toolchain.

Running Tests
-------------

.. code-block:: shell

   # Run all unit tests
   west twister -T tests -v --inline-logs

   # Run a single test suite by path
   west twister -T tests/lib/state -v --inline-logs

   # Build-verify sensor_board against all supported boards
   west twister -T sensor_board -v --inline-logs --integration

Test Layout
-----------

Each test suite lives under ``tests/`` in a directory that mirrors the
library it covers.  A ``testcase.yaml`` file alongside the test source
tells Twister how to build and run the suite::

   tests/
   └── lib/
       └── state/
           ├── testcase.yaml
           └── src/
               └── main.c

Writing Tests
-------------

Tests use the ``ZTEST_SUITE`` / ``ZTEST`` macros.  Per-test setup and
teardown callbacks can be registered to initialize and reset state:

.. code-block:: c

   ZTEST_SUITE(my_suite, NULL, NULL, before_fn, after_fn, NULL);

   ZTEST(my_suite, test_something)
   {
       zassert_equal(result, expected, "message");
   }

See ``tests/lib/state/src/main.c`` for a complete example that exercises
the simple state machine through its full flight sequence.

