# Framework changes

For this source snapshot, apply **only `framework-current.patch`** after
initializing the pinned submodule:

```bash
git -C snesrecomp apply ../patches/framework-current.patch
```

This consolidated patch includes all local framework changes used by the
prototype, including HD presentation, rollback/audio fixes, interpreter
pacing and profiling. The individual patches are retained for reference;
do not apply them on top of the consolidated patch.

Framework base commit: `a6be9756285f26ccc74539c811d4d23d57d8c33d`.
