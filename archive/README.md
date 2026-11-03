# Local archives (not published)

The original working tree keeps recoverable baselines and pre-change copies in
a directory outside the repository (previously `D:/Xilinx/FPGA_Archive/...`).
Those archives contain generated artefacts, full project snapshots and
per-version evidence bundles of several gigabytes; they are intentionally not
part of this release.

Where a document references `/archive/<milestone>/`, it points at such a local
snapshot. The same information can be reconstructed from the tagged commit plus
the matching `software/verification/<milestone>/` records.
