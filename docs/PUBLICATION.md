# Public repository provenance

Prepared on November 12, 2026 for publication of the Suffice / New Runtime Tech project.

## Preserved inputs

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Revised complete paper packet | 20,804,781 | `304d999230902d27ff7d42c3901ae087d5af312f685577e85f5f4f0edb8ab8b4` |
| Runtime 0.13.0 release ZIP | 12,612,566 | `1cda9360c3d0d013309236a0720d13b7c3f01735a590761894a8bb68adb77205` |
| Offline evidence ZIP | 3,837,891 | `c6ba35462a6e1e997808aa68efda16e75ec5ec8a6e0812e59cc2a3df0ef57eac` |

`paper-packet/` reproduces every member of the revised packet with its original relative path and file bytes. Its original `MANIFEST.json` describes that historical packet. `paper-packet/source/Suffice_0_13_0.zip` is the unchanged full runtime release, including preserved earlier lineage.

`runtime/` exposes the release as browsable source and evidence. It preserves every original file byte except that the two platform-specific executables below are omitted from the expanded checkout:

- `build/transport_release/suffice`
- `build/transport_release/suffice_catalog`

Both remain inside the unmodified release archive. Historical build manifests and test logs are retained. New local objects, dependency caches, interpreter caches, transfer helpers, account data, and temporary publication work are not part of the repository. The original release inventory continues to describe the complete archived release, so the two documented omissions must be accounted for when comparing it with the expanded tree.

## Added presentation files

The repository landing page, project description, runtime execution workbench notes, ignore/attribute rules, and asset filenames were added for public navigation. The workbench image is labeled as a synthetic demo and concept UI. These presentation files do not modify the frozen experiment or the bridge's historical validation. The complete source, research, test, and evidence directories retain their internal structure.

The source archives were checked for ZIP integrity and matched the recorded hashes before packaging. A targeted publication review inspected the recovered files and nested archives for credential/private-key patterns and unrelated private material. No such material was identified. This is a publication-content check, not a comprehensive security assessment of the runtime.

## Reproduction boundaries

The repository-root README supplies commands for the `runtime/` and `paper-packet/` layout. Historical documents inside the preserved packet may describe their original adjacent extracted-release layout. Use new output directories for local work and keep the retained evidence unchanged.

The notebook and offline auditor verify retained records. Native reruns, new measurements, and workbench implementation are separate operations. Existing records are not reclassified as new results merely because the project is published.

Existing MIT notices are preserved. This publication does not assert that the research paper was submitted to, accepted by, or awarded by a competition.
