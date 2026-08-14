# Scheduled stress evidence through 2026-08-13

This record captures the public GitHub Actions evidence reviewed on 2026-08-13 before closing the
1.0 fuzzing/concurrency evidence item. The `stress` workflow executes the ASan/UBSan workflow
fuzzer for ten minutes and then runs 80,000 concurrent Runs (`8` threads × `10,000` iterations).

Ten consecutive scheduled runs on `main` completed successfully:

1. [stress #1](https://github.com/sxf51/DAGE/actions/runs/30882787114)
2. [stress #2](https://github.com/sxf51/DAGE/actions/runs/30979997657)
3. [stress #3](https://github.com/sxf51/DAGE/actions/runs/31076133596)
4. [stress #4](https://github.com/sxf51/DAGE/actions/runs/31149372414)
5. [stress #5](https://github.com/sxf51/DAGE/actions/runs/31239565503)
6. [stress #6](https://github.com/sxf51/DAGE/actions/runs/31294981706)
7. [stress #7](https://github.com/sxf51/DAGE/actions/runs/31357009072)
8. [stress #8](https://github.com/sxf51/DAGE/actions/runs/31459262283)
9. [stress #9](https://github.com/sxf51/DAGE/actions/runs/31565319314)
10. [stress #10](https://github.com/sxf51/DAGE/actions/runs/31669256330)

The latest run reports success for `nightly-fuzz-and-soak` at source revision
`54b26912b66277321ef3c710f2f7fe6a00e29176`. No failure artifact was produced. The evolving corpus
remains cached by the workflow; any later crash or race must still be minimized into
`fuzz/corpus/workflow` or a dedicated regression test before a release proceeds.
