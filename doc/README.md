# AURORA Documentation

Documentation is built with [Doxygen](https://www.doxygen.nl/) (API extraction)
and [Sphinx](https://www.sphinx-doc.org/) + [Breathe](https://breathe.readthedocs.io/)
(final HTML output).

## Prerequisites

```shell
pip install -r requirements.txt
```

Doxygen must also be installed (e.g. `sudo dnf install doxygen`).

## Building

From the `doc/` directory:

```shell
# 1. Generate Doxygen XML (required by Breathe)
doxygen

# 2. Build the Sphinx site
make html
```

The output will be in `_build_sphinx/html/`. Open `index.html` to browse.
