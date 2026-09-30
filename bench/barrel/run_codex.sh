#!/usr/bin/env bash
# Run the barrel benchmark with a Codex model.
# usage: bench/barrel/run_codex.sh <model> <reasoning_effort> <workspace_dir>
# The workspace gets TASK.md; the transcript goes to codex_log.jsonl, timing to meta.json.
set -u
model=$1
effort=$2
ws=$3
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$ws"
cp "$here/TASK.md" "$ws/TASK.md"
start=$(date +%s)
codex exec -m "$model" -c model_reasoning_effort="$effort" --skip-git-repo-check -s workspace-write \
  -C "$(cygpath -w "$ws")" --json -o "$ws/codex_last_message.md" \
  "Read TASK.md in the current directory and complete it fully and autonomously." \
  > "$ws/codex_log.jsonl" 2> "$ws/codex_stderr.log"
code=$?
end=$(date +%s)
printf '{"model": "%s", "effort": "%s", "harness": "codex exec (workspace-write sandbox)", "started": %s, "finished": %s, "exit_code": %s}\n' \
  "$model" "$effort" "$start" "$end" "$code" > "$ws/meta.json"
echo "$model done in $(( (end - start) / 60 )) min (exit $code)"
