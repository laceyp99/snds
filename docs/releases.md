# Release checks

Before publishing a release or distributing binaries:

- Run the build, tests, and lint scripts for the exact release commit.
- Inventory shipped components, including native libraries, Python wheels, model weights, and artwork.
- Review upstream license files and redistribution terms for those exact versions. The [spike license notes](spikes.md#licence-notes-python-side) are a starting point, not release clearance.
- Confirm the JUCE licensing route and satisfy its applicable source, notice, and license requirements.
- Review bundled Python and native dependencies, including CUDA components if shipped, for notices and redistribution obligations.
- Confirm model-weight redistribution rights and record their source and checksum. Keep weights out of the repository.
- Create `THIRD_PARTY_NOTICES.md` with required notices and include applicable license texts in the release package.
- Confirm distribution permission for sample audio and artwork. Exclude secrets and local data.

Resolve licensing questions before publishing. Record the review date, release commit, shipped component versions, and exclusions in the release notes.
