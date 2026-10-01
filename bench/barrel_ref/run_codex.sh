#!/usr/bin/env bash
# Run the reference-match barrel benchmark with a Codex model.
# usage: bench/barrel_ref/run_codex.sh <model> <reasoning_effort> <service_tier> <workspace_dir>
# The workspace gets TASK.md, reference.png and skills/; the transcript goes to codex_log.jsonl,
# timing to meta.json. The stylized-environment-art skill must also be in ~/.codex/skills.
set -u
model=$1
effort=$2
tier=$3
ws=$4
here=$(cd "$(dirname "$0")" && pwd)
"$here/prepare.sh" "$ws"
start=$(date +%s)
codex exec -m "$model" -c model_reasoning_effort="$effort" -c service_tier="$tier" --skip-git-repo-check -s workspace-write \
  -C "$(cygpath -w "$ws")" --json -o "$ws/codex_last_message.md" \
  "Read TASK.md in the current directory and complete it fully and autonomously. Use the stylized-environment-art skill." \
  > "$ws/codex_log.jsonl" 2> "$ws/codex_stderr.log"
code=$?
end=$(date +%s)
printf '{"model": "%s", "effort": "%s", "service_tier": "%s", "harness": "codex exec (workspace-write sandbox)", "started": %s, "finished": %s, "exit_code": %s}\n' \
  "$model" "$effort" "$tier" "$start" "$end" "$code" > "$ws/meta.json"
echo "$model done in $(( (end - start) / 60 )) min (exit $code)"
