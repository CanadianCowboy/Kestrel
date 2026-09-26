## What problem does this solve?

<!--
Explain the user problem or engineering motivation. A reviewer who cannot
state the problem in one paragraph cannot judge whether the change is
proportionate to it.
-->

## Approach and tradeoffs

<!--
Describe the chosen approach and, more importantly, what you rejected and
why. Tradeoffs are the most valuable part of a PR and the easiest to omit.
-->

## Validation

<!--
State which optional dependencies were actually available while you
validated. Kestrel has several optional paths (Qt, CUDA, TensorRT), and
"it works" is not actionable without knowing which ones were exercised.
-->

- [ ] Built without Qt (`-DKESTREL_BUILD_UI=OFF`)
- [ ] Built with Qt
- [ ] `ctest` passes
- [ ] Optional dependencies available during validation:

| Dependency | Version | Available? |
| ---------- | ------- | ---------- |
| Compiler   |         |            |
| Qt         |         |            |
| CUDA       |         |            |
| GPU        |         |            |
| Driver     |         |            |
| TensorRT   |         |            |

- [ ] For runtime changes: relevant GPU, driver, CUDA, TensorRT, compiler, and
      model/engine versions are recorded above.

## Checklist

- [ ] Tests included for behavior changes.
- [ ] No machine-specific absolute paths, local build output, or dependency
      caches committed.
- [ ] No blocking calls introduced on the UI thread.
- [ ] Architectural boundaries respected (CUDA headers confined to
      `src/runtime/cudadiscovery_cuda.cpp`; the portable stub still builds
      without a toolkit).
- [ ] Documentation updated where behavior or build options changed.
- [ ] Unrelated formatting churn removed from the diff.

## Screenshots

<!-- UI-only changes. Remove this section otherwise. -->
