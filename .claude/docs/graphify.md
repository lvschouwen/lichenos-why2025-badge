# graphify (moved from CLAUDE.md)

- Local AST graph of `firmware/` only (gitignored; vendored esp_hosted/meshcore/third_party are not in it). `graphify query "<q>"` before grepping for code questions, and in every agent brief that touches code (agents cannot run graphify).
- After merges run `graphify update` WITHOUT a path (`.` rescans the whole repo).
- A separate MeshCore upstream graph lives at `graphify-meshcore/graphify-out/graph.json` (`--graph` flag; excluded via .git/info/exclude; `meshcore/` is a submodule, never write in it).
- A v3 graph (Rust; branch `v3`) lives at `graphify-v3/src/graphify-out/graph.json` (excluded via .git/info/exclude): `graphify query "<q>" --graph graphify-v3/src/graphify-out/graph.json`. Refresh after each v3 merge: `rm -r graphify-v3/src/v3 && git archive origin/v3 v3 | tar -x -C graphify-v3/src && graphify update graphify-v3/src`. AST only, no tokens.
