# Patina benchmarks

Standard tasks for comparing agents (models + harnesses) on real asset work with Patina.

## barrel: stylized barrel, modelling + texturing

- `barrel/TASK.md` is the whole brief, given verbatim to every agent in an empty workspace. It asks for
  a coopered barrel modelled headless in Blender (staves, board heads in the chime, riveted hoops,
  bevels, 50k triangle budget), UVs laid out for wood grain, stylized texturing in Patina, and
  engine-ready deliverables plus notes.
- `barrel/run_codex.sh <model> <effort> <workspace>` runs a Codex model on it (`codex exec`,
  workspace-write sandbox) and records timing in `meta.json`. Other harnesses just need the same
  brief and workspace rules.
- `barrel/score.py <patina.exe> <results_dir> <run_dir>...` checks the hard requirements (deliverables,
  validation, triangle budget, texture sets, size and placement, UV overlap and texel density from
  `patina inspect`) and re-renders every submission with identical cameras and lighting (lit hero,
  back, top and hoop close-ups, clay, UV checker) for judging.

Workspaces live outside the repository so agents cannot see the example barrel. Judging of
modelling, UVs and texturing uses the standardized renders against the rubric in `TASK.md`.
