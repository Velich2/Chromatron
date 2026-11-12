# Chromatron 1.3.0 installation

Chromatron provides chromatic aberration for After Effects, Premiere Pro, DaVinci Resolve and Nuke.
Choose the archive for your host and operating system, extract it, close the host and copy the plug-in.

| Host | Archive | Copy | Destination |
|---|---|---|---|
| After Effects / Premiere Pro on Windows | `Chromatron-1.3.0-AE-Premiere-Windows.zip` | `Chromatron.aex` | `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` |
| Resolve / Nuke on Windows | `Chromatron-1.3.0-OFX-Windows.zip` | `Chromatron.ofx.bundle` | `C:\Program Files\Common Files\OFX\Plugins` |
| Resolve / Nuke on macOS | `Chromatron-1.3.0-OFX-macOS.zip` | `Chromatron.ofx.bundle` | `/Library/OFX/Plugins` |
| Resolve / Nuke on Linux | `Chromatron-1.3.0-OFX-Linux.zip` | `Chromatron.ofx.bundle` | `/usr/OFX/Plugins` |

Copy the complete OFX bundle, including its `Contents` directory. Restart the host after installation.
System plug-in folders may require administrator permission. The macOS OFX bundle includes arm64 and x86_64.

Nuke also scans `C:\Program Files\Common Files\OFX\Nuke`, `/Library/OFX/Nuke` and `/usr/OFX/Nuke` on their
respective systems. Choose one visible location for the bundle. Search for Chromatron in the node menu.
Nuke Indie supports third-party OFX from 12.2v3 onwards. Nuke Non-commercial cannot load third-party plug-ins.
Nuke integration has been tested through the shared core, with in-host verification still pending.

## GPU and colour setup

- The Windows AE/Premiere archive uses CUDA 13.4 and includes Turing and newer GPU architectures, requiring
  driver R580 or newer. Premiere uses Mercury GPU acceleration when the selected renderer is supported.
- The Windows OFX archive uses CUDA 12.4 and includes Pascal and newer GPU architectures. It requires
  a compatible CUDA 12 NVIDIA driver. OpenCL is also included.
- Linux OFX includes CPU and OpenCL. macOS OFX includes CPU and Metal.
- In Nuke, Use GPU tries an owned GPU queue and returns the result to Nuke's CPU image buffer. OpenCL is
  tried after CUDA. Failed GPU attempts fall back to CPU. Disable Use GPU to force CPU processing.
- Nuke Auto uses reported colour metadata and otherwise assumes linear Rec.709. Choose ACEScg manually
  for an ACEScg working space when the host does not expose usable metadata.

The OFX archive includes `NUKE.md` and `smoke.py` for Nuke setup and a separate-process host test.
The [Nuke instructions](https://github.com/Velich2/Chromatron/blob/v1.3.0/docs/NUKE.md) are also available online.
Python automation depends on the Nuke edition. The AE/Premiere archive includes this installation guide.
The full parameter guide and examples are in the [project README](https://github.com/Velich2/Chromatron#readme).

Installation paths follow the [Foundry OFX guide](https://learn.foundry.com/nuke/content/comp_environment/configuring_nuke/loading_ofx_plugins.html).
Edition support is documented in the [Nuke Indie FAQ](https://www.foundry.com/products/nuke-family/nuke-indie/faq)
and [Non-commercial FAQ](https://www.foundry.com/non-commercial-faqs).
