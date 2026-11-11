# Chromatron for Nuke (OpenFX)

This build uses Nuke's OpenFX interface, so it does not require a different NDK binary for each
Nuke major/minor version. Separate binaries are still needed for Windows, macOS and Linux.
Compatibility with every Nuke release is not assumed: this integration has not been run inside
Nuke yet. The SDK-independent tests and the Windows build are verified locally.

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
Nuke Non-commercial does not support third-party plug-ins. Other editions remain subject to their
Foundry licensing restrictions (including Python automation limits in Indie).

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
- **Rendering:** the Nuke profile requests CPU buffers and hides Use GPU. Resolve's CUDA/OpenCL/Metal
  extensions remain available in other hosts. Nuke GPU acceleration is not claimed for this build.
- **ROI:** processing uses a full input canvas for consistent blur pyramids even if the host requests
  only part of the output. This costs memory on very large bounding boxes.

## Host smoke test

Run in a separate licensed Nuke process, after installing the bundle:

```text
Nuke -t smoke.py
```

The source script lives at `tests/nuke/smoke.py`; it creates its own graph and checks HDR values,
identity, a cropped input, expanded output, transparent edges, all three modes and node recreation.
If Nuke registers a version-suffixed class, set `CHROMATRON_NUKE_CLASS` to the exact class shown by
`nuke.selectedNode().Class()` after creating Chromatron manually.

Also verify these in the Viewer before production use: ACEScg vs explicit Linear, proxy at 1/2,
animated Crop, transparent edges, non-square pixel formats, saved-script reload and batch renders.
The smoke script requires a Nuke edition with unrestricted Python automation; passing it alone
is not a substitute for the Viewer checks.

## References

- [Foundry: loading OFX plug-ins](https://learn.foundry.com/nuke/content/comp_environment/configuring_nuke/loading_ofx_plugins.html)
- [Foundry: bounding boxes](https://learn.foundry.com/nuke/17.0v2/content/comp_environment/reformatting_elements/adjusting_bbox.html)
- [Foundry: Crop](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/crop.html)
- [Foundry: native NDK compatibility](https://support.foundry.com/hc/en-us/articles/207353210-Q100234-NDK-plug-in-compatibility-between-different-Nuke-versions)
