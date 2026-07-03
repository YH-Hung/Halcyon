# Documentation

The published site combines the narrative guide (MkDocs Material, source in
`docs/guide/`) with a generated API reference (Doxygen, from `include/halcyon/`).

Published URLs:

- Site: https://yh-hung.github.io/Halcyon/
- API reference: https://yh-hung.github.io/Halcyon/api/index.html

## Build locally

```bash
python -m pip install -r docs/requirements.txt   # mkdocs-material
# plus Doxygen, e.g. `brew install doxygen` or `apt-get install doxygen`
docs/build.sh
```

Output lands in `site/` (gitignored). `docs/build.sh` runs Doxygen first
(into `docs/api/`, also gitignored) and then `mkdocs build --strict`.
