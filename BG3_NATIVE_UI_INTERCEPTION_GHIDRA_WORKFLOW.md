# BG3 Native UI Interception V6: Ghidra Workflow

## Purpose and boundary

This is the durable recipe for reproducing the V6 DX11 static discovery. The
Ghidra helpers, project database, and generated evidence remain ignored local
developer material. They are not BG3SE or BG3Access product files.

The current program had already received its full initial Auto Analysis before
V6 began. Every V6 discovery invocation opened it read-only with `-noanalysis`.
The completed initial analysis was not rerun.

## Exact tool and project identity

Ghidra:

- Installation: `D:\Repositories\bg3se-SR\tools\ghidra_12.1.2`
- Version: `12.1.2 PUBLIC`
- Headless entry point:
  `D:\Repositories\bg3se-SR\tools\ghidra_12.1.2\support\analyzeHeadless.bat`
- Java: Oracle JDK 21.0.7, 64-bit
- Java path: `C:\Program Files\Java\jdk-21`
- Python: not used. The workflow uses Ghidra Java scripts and PowerShell 7, so
  it does not depend on or change the user's globally installed Python.

Precomputed project:

- Project root: `D:\Repositories\bg3se-SR\tools\ghidra_projects`
- Project name: `bg3_accessibility`
- Program name: `bg3_dx11.exe`
- Image base: `0x140000000`
- Language: `x86:LE:64:default`
- Compiler specification: `windows`
- Imported SHA-256:
  `e899c67cb90b9c6b0f052e3f758ba8615bc2012e52561c2af2e6a1e06ef61a2f`
- Ghidra reported that analysis was already complete when V6 began.

Ignored automation and output root:

`D:\Repositories\bg3se-SR\tools\ghidra_work`

## Authoritative DX11 checkpoint

- Path:
  `D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe`
- Filesystem size: `104363072` bytes
- SHA-256:
  `e899c67cb90b9c6b0f052e3f758ba8615bc2012e52561c2af2e6a1e06ef61a2f`
- Product version: `4.1.1.7398727`
- PE format: PE32+, AMD64
- PE timestamp: `0x6A21507E`, 2026-06-04 10:16:30 UTC
- MD5, supplemental only: `5f4843e2708790c24a71088f6bc612e4`

The filesystem size is authoritative. Ghidra's import-summary `# of Bytes`
value `106644048` is not the filesystem file size and is not an identity gate.

Vulkan was not opened, searched, analyzed, or validated.

## Local helpers and hashes

The consequential ignored helper files used for this result were:

- `Invoke-BG3AccessibilityDiscovery.ps1`
  SHA-256
  `6763aee6da762d6aa0e6408743c95936e3532f96866b621d5231e70c3a9ac136`
- `BG3InventoryAndAnchors.java`
  SHA-256
  `717036a5d1413fac47811e2965e19ce3a66d3c84f908e82c2c29c18d4db88ae2`
- `BG3CandidateEvidence.java`
  SHA-256
  `af72db8384e2aec31f615304ab65d697270799d1b14e8d8b26a949ccadfc6fb0`
- `BG3DataXrefs.java`
  SHA-256
  `9e2b3e475299ea6cf2ed2006ceadeccbef7c3435874b2e53a6f7b1352f9ec65c`
- `BG3PointerTables.java`
  SHA-256
  `e0d9484e45f450cb17b7f135e75056691a1c66bf33b86033e58be18c82f4c6da`

The wrapper has deterministic comment-based help:

```powershell
Get-Help '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' -Detailed
```

It verifies file size, SHA-256, and product version before Ghidra starts. It
deletes the prior success marker and requires a newly written marker after a
zero exit. Missing input, an identity mismatch, an absent project, script
failure, a nonzero Ghidra exit, or a stale/missing marker is a terminating
error.

## Entry commands used

Run from `D:\Repositories\bg3se-SR` in PowerShell 7:

```powershell
& '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' -Mode anchors
& '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' -Mode candidates
& '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' -Mode dataxrefs
& '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' -Mode vtables
```

The one-command equivalent for the exact current checkpoint is:

```powershell
& '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' -Mode all
```

Each child command passed this fixed headless boundary:

```text
<analyzeHeadless.bat> <project-root> bg3_accessibility
  -process bg3_dx11.exe
  -readOnly
  -noanalysis
  -scriptPath <ghidra_work>
  -postScript <bounded-script> <input-tsv> <output-directory>
```

No command used `-import`, `-overwrite`, or write access to the project.

## Inputs generated from real source anchors

`anchors.tsv` contains 87 line-oriented search anchors assembled from:

- current BG3SE Noesis and client UI source;
- the read-only live BG3Access Lua handlers;
- reflected Larian and Noesis names present in the exact binary;
- target/combat property names already read by BG3Access.

The anchors cover widget lifecycle, focus/selection, SoftwareCursor,
ViewModels/INPC, commands, and final binding/text commitment. The candidate TSV
then bounds deeper analysis to functions reached by those anchors and to a
small set of explicitly recorded RVAs. The registered-storage TSV records 25
dependency-property, routed-event, or reflected-type storage addresses. The
vtable TSV records four structurally useful Noesis tables.

## Deterministic evidence outputs

Anchor pass:

- Input anchors: 87
- Xref rows: 4,986
- `anchors\program_inventory.json`
- `anchors\anchor_summary.json`
- `anchors\anchor_xrefs.csv`

Candidate pass:

- Bounded candidate functions: 116
- Aggregate call edges: 4,310
- `candidates\candidate_index.json`
- Per-candidate `function_info.json`, bounded `instructions.txt`, bounded
  `decompile.c`, caller/callee CSV, strings, symbols, and context bytes
- `candidates\call_edges.csv`
- `candidates\call_graph.graphml`

The decompiler writer truncates each function output at its fixed cap and marks
the truncation. It does not emit a whole-program dump.

Registered-storage pass:

- Storage targets: 25
- Xref rows: 1,080
- `dataxrefs\data_xrefs_summary.json`
- `dataxrefs\data_xrefs.csv`

Pointer-table pass:

- Tables: 4
- `vtables\pointer_tables.csv`

Every output directory contains a freshly written `_SUCCESS.txt`. The helper
sorts candidate rows, call edges, xrefs, and graph nodes before writing them.

## How candidate evidence was derived

For each supplied RVA, the candidate exporter resolves the containing Ghidra
function and records:

- function entry and module-relative RVA;
- recovered signature and calling convention;
- bounded bytes and instructions;
- bounded decompiler text;
- direct callers and callees;
- nearby symbols and referenced strings;
- function-context bytes;
- a bounded aggregate call graph.

For each proposed masked signature, it scans initialized executable memory
blocks in the exact analyzed program. Uniqueness counts therefore come from the
script, not visual inspection. All selected signatures reproduced one match
except the deliberately blocked `C-LS-VIEWMODEL-INPC-TYPE-LEAD`, whose pattern
matched twice.

RTTI, reflected-property storage, routed-event storage, and vtable slot evidence
was then correlated with bounded decompilation. Inferences are labeled as such;
Noesis 3.1.7 public header layout was used only as structural corroboration for
the embedded 3.1.6-era binary, never as binary identity evidence.

The tracked, normalized result is
`BG3_NATIVE_UI_INTERCEPTION_CANDIDATES.json`. Local decompiler text and raw
xref output remain ignored.

## Reproducing after a BG3 update without the GUI

Do not overwrite or silently reinterpret the current checkpoint. Capture the
new executable path, filesystem size, SHA-256, and product version first.

For a genuinely new executable, create a distinct project headlessly and allow
its one required initial analysis. That is a new-program import, not a rerun of
the completed V6 analysis:

```powershell
& '.\tools\ghidra_12.1.2\support\analyzeHeadless.bat' `
  '.\tools\ghidra_projects' 'bg3_accessibility_<product-version>' `
  -import 'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe'
```

After that command completes successfully, rerun the bounded discovery without
the GUI. Replace the four placeholder identity values with measured values:

```powershell
& '.\tools\ghidra_work\Invoke-BG3AccessibilityDiscovery.ps1' `
  -Mode all `
  -ProjectName 'bg3_accessibility_<product-version>' `
  -ProgramName 'bg3_dx11.exe' `
  -GameExecutable 'D:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe' `
  -ExpectedFileSize <filesystem-byte-count> `
  -ExpectedSha256 '<lowercase-sha256>' `
  -ExpectedProductVersion '<product-version>'
```

The old RVAs and signatures are leads, not update guarantees. A missing or
ambiguous signature must fail the hook closed. Rebuild anchors and inspect new
bounded call paths before approving replacement RVAs or masks. Record the new
checkpoint in a new candidate/report revision; do not relabel V6 evidence as
valid for the updated binary.

## Accessibility and review notes

- All durable findings are text, JSON, or CSV; visual Ghidra inspection is not
  required.
- GraphML is supplemental. No conclusion depends on a rendered graph.
- Report sections are narrow and line-oriented for NVDA.
- No Ghidra binaries, project database, helper, or generated scratch output is
  intended for staging.
