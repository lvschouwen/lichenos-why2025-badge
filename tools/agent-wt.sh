#!/usr/bin/env bash
# The warm worktree pool is the shared agent-wt (lvschouwen/.claude#22, #253); settings in .claude/agent-wt.conf.
exec "$(command -v agent-wt || echo "$HOME/.claude/bin/agent-wt")" "$@"
