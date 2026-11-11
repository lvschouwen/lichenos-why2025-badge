# graphify (moved from CLAUDE.md)

- Local AST graph of `firmware/` only (gitignored; vendored esp_hosted/meshcore/third_party are not in it). `graphify query "<q>"` before grepping for code questions, and in every agent brief that touches code (agents cannot run graphify).
- After merges run `graphify update` WITHOUT a path (`.` rescans the whole repo).
- A separate MeshCore upstream graph lives at `graphify-meshcore/graphify-out/graph.json` (`--graph` flag; excluded via .git/info/exclude; `meshcore/` is a submodule, never write in it).
