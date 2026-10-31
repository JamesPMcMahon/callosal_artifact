"""Experiment harness for the callosal-kv benchmark.

The harness owns everything outside the measurement hot path: orchestrating
populate/run across one or more nodes via ssh, coordinating cluster start
over a UDP barrier, fetching per-node stats JSON back to the driver, and
aggregating/plotting results. The public entry points are documented in the
repo root ``README.md``.
"""

__all__ = ["__version__"]

__version__ = "0.1.0"
