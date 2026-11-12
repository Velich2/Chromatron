# Chromatron for Nuke (OpenFX)

This build uses Nuke's OpenFX interface, so it does not require a different NDK binary for each
Nuke major/minor version. Separate binaries are still needed for Windows, macOS and Linux.
Compatibility with every Nuke release is not assumed: this integration has not been run inside
Nuke yet. Core tests and the Windows build are verified locally, and all platform builds pass CI.

## Installation

Quit Nuke and copy `Chromatron.ofx.bundle` into one OFX directory:

| OS | Nuke-only folder | Shared with Resolve |
|---|---|---|
| Windows | `C:\Program Files\Common Files\OFX\Nuke` | `C:\Program Files\Common Files\OFX\Plugins` |
| macOS | `/Library/OFX/Nuke` | `/Library/OFX/Plugins` |
| Linux | `/usr/OFX/Nuke` | `/usr/OFX/Plugins` |

Alternatively set `OFX_PLUGIN_PATH` to the directory containing the bundle before starting Nuke.
Keep only one copy of Chromatron visible to Nuke; the shared bundle includes both Nuke and Resolve
behaviour. Restart and search for Chromatron in the node menu.
Nuke Indie supports third-party OFX from 12.2v3 onwards. Nuke Non-commercial does not support third-party
plug-ins. Python automation in Indie is restricted, so use the manual Viewer checks below for that edition.

## Windows startup compatibility

The Windows 1.3.0 OFX build can crash with MSVC 14.36.32532 DLLs preloaded by Nuke 15.1v4. The
1.3.1 compatibility build uses Microsoft's legacy mutex constructor, and its regression tests run
with those exact runtime versions. GPU rendering remains available through the existing private queues.

Replace the complete existing Chromatron bundle when testing the compatibility build. Nuke scans both
its own OFX folder and the shared Resolve folder, so an older shared copy is also visible to Nuke.
Keep one visible copy. The separate Nuke test archive contains the shared rendering engine and a
Nuke-specific installation guide. Runtime compatibility is applied to the shared Windows code.

## Nuke-specific behaviour

- **Colour:** Auto first honours usable OpenFX clip colour metadata. Without metadata it chooses
  **Linear (Rec.709 primaries)** because the Nuke graph normally carries scene-linear float pixels.
  For an ACEScg working space choose **ACEScg** explicitly unless the host reports it. The Viewer
  transform is not applied by the plug-in. Do not select a display gamma merely because the Viewer
  is displaying sRGB or Rec.709. Highlight Boost is off by default; HDR data remains unclipped.
- **Optical Center:** normalised relative to the OFX project format, not the source bounding box.
  Match the Nuke root/project format to the intended lens frame when processing mixed-format inputs.
  Changing a Crop/animated data window does not move the optical axis. Proxy render scales and
  pixel aspect ratio are used to map the format to the buffer's pixel coordinates.
- **Bounding box:** the output data window expands for the fringe and blur. Input is requested in
  full; pixels outside its data window are transparent black. Negative origins and negative row
  strides are supported. Very large or infinite data windows need an upstream Crop.
- **Alpha:** follows the input clip's OFX premultiplication flag. RGB and alpha are processed together.
  The plug-in consumes ordinary float RGBA, not deep images or arbitrary AOV planes. Use DeepToImage
  for deep data and Shuffle/Copy to route the intended layer and preserve other channels explicitly.
- **GPU rendering:** **Use GPU** is enabled by default. Nuke supplies CPU image buffers; Chromatron uploads
  the padded float canvas to a private GPU queue, runs the same kernels and copies the result back. Windows/Linux
  try CUDA first, then OpenCL GPU devices; macOS uses Metal. OpenCL checks all GPU devices across platforms
  until a device creates a queue and builds every kernel, then reuses that device and its compiled program.
  Selection is independent of Nuke's GPU preference. CUDA device setup and work run on a separate joined
  thread with its own stream. Frame buffers and private queues are released before returning the frame.
  On unsupported hardware, insufficient GPU memory or a failed GPU attempt, the frame is rendered on CPU.
  If RAM allocation for the GPU output staging copy fails, CPU rendering writes directly into Nuke's output
  image. The input canvas and CPU working buffers still require memory.
  Disable Use GPU to force CPU processing. The extra uploads/downloads can make small images slower on GPU.
  The Windows OFX release uses CUDA 12.4, with Pascal and newer architectures and a compatible CUDA 12
  driver. Linux OFX includes CPU/OpenCL and macOS OFX includes CPU/Metal. OpenCL can serve supported AMD,
  Intel and NVIDIA GPUs. Resolve uses host-provided GPU buffers when the selected host renderer supports them.
- **ROI:** processing uses a full input canvas for consistent blur pyramids even if the host requests
  only part of the output. This costs memory on very large bounding boxes.

## Host smoke test

The core tests discover GPU devices separately from compiling kernels and creating queues. Each available
CUDA, OpenCL and Metal backend must pass its own upload/render/download comparison against CPU in all three
modes, including look controls and offset output windows. A kernel or queue failure on a detected OpenCL
or Metal device fails the tests; only an absent device allows those tests to be skipped.

Run in a separate licensed Nuke process, after installing the bundle:

```text
Nuke -t smoke.py
```

The release archive includes `smoke.py`, whose source lives at `tests/nuke/smoke.py`. It creates its own graph and checks HDR values,
identity, a cropped input, expanded output, transparent edges, all three modes and node recreation.
If Nuke registers a version-suffixed class, set `CHROMATRON_NUKE_CLASS` to the exact class shown by
`nuke.selectedNode().Class()` after creating Chromatron manually.

Also verify these in the Viewer before production use: ACEScg vs explicit Linear, proxy at 1/2,
animated Crop, transparent edges, non-square pixel formats, saved-script reload, batch renders and
Use GPU on/off parity.
The smoke script requires a Nuke edition with unrestricted Python automation; passing it alone
is not a substitute for the Viewer checks.

## References

- [Foundry: loading OFX plug-ins](https://learn.foundry.com/nuke/content/comp_environment/configuring_nuke/loading_ofx_plugins.html)
- [Foundry: bounding boxes](https://learn.foundry.com/nuke/content/comp_environment/reformatting_elements/adjusting_bbox.html)
- [Foundry: Crop](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/crop.html)
- [Foundry: native NDK compatibility](https://support.foundry.com/hc/en-us/articles/207353210-Q100234-NDK-plug-in-compatibility-between-different-Nuke-versions)
- [Foundry: Nuke Indie OFX support](https://www.foundry.com/products/nuke-family/nuke-indie/faq)
- [Foundry: Non-commercial limitations](https://www.foundry.com/non-commercial-faqs)
- [Microsoft STL: mutex compatibility with older runtimes](https://github.com/microsoft/STL/issues/4730)
