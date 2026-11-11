"""Run in a licensed Nuke terminal: Nuke -t smoke.py.

Creates a temporary test graph in a fresh process. Does not open or save user projects.
CHROMATRON_NUKE_CLASS can override the OFX class name if the host adds a version suffix.
"""
import math
import os
import nuke


def knob(node, name):
    if name in node.knobs():
        return node[name]
    # OFX hosts can prefix the internal parameter name. Fail on ambiguous matches.
    matches = [k for k in node.allKnobs() if k.name().endswith('_' + name)]
    if len(matches) != 1:
        raise RuntimeError('Cannot identify Chromatron parameter: ' + name)
    return matches[0]


def pick(node, name, label):
    k = knob(node, name)
    values = k.values()
    for value in values:
        if value == label:
            k.setValue(value)
            return
    raise RuntimeError('Missing option %s on %s: %r' % (label, name, values))


def main():
    nuke.addFormat('128 96 1 ChromatronSmoke')
    nuke.root()['format'].setValue('ChromatronSmoke')
    constant = nuke.nodes.Constant(color=[2.0, 1.0, 0.5, 1.0])
    crop = nuke.nodes.Crop(inputs=[constant], box=[48, 32, 80, 64], reformat=False, crop=True)
    cls = os.environ.get('CHROMATRON_NUKE_CLASS', 'OFXcom.chromatron.Chromatron')
    effect = nuke.createNode(cls, inpanel=False)
    effect.setInput(0, crop)
    print('Nuke:', nuke.NUKE_VERSION_STRING, 'class:', effect.Class())
    pick(effect, 'colorSpace', 'Auto')
    knob(effect, 'mix').setValue(1.0)
    pick(effect, 'mode', 'RGB Separation')
    knob(effect, 'spread').setValue(0.0)
    knob(effect, 'angle').setValue(0.0)
    knob(effect, 'distance').setValue(0.0)
    for channel, expected in [('red', 2.0), ('green', 1.0), ('blue', 0.5), ('alpha', 1.0)]:
        assert abs(effect.sample(channel, 60.5, 48.5) - expected) < 1e-4, channel
    knob(effect, 'distance').setValue(12.0)
    bounds = effect.bbox()
    assert bounds.x() < 48 and bounds.r() > 80, 'Effect did not expand the input bounding box'
    assert effect.sample('red', 82.5, 48.5) > 0.1, 'Red fringe clipped outside input bbox'
    assert abs(effect.sample('alpha', 10.5, 10.5)) < 1e-5, 'Pixels outside data window not black'
    for mode in ['RGB Separation', 'Lens Aberration', 'Physical Lens']:
        pick(effect, 'mode', mode)
        for y in [31.5, 48.5, 64.5]:
            for x in [47.5, 64.5, 80.5]:
                for channel in ['red', 'green', 'blue', 'alpha']:
                    assert math.isfinite(effect.sample(channel, x, y)), (mode, channel)
    # Compare both settings through the host; log cannot prove device use if the machine falls back.
    pick(effect, 'mode', 'Physical Lens')
    knob(effect, 'useGpu').setValue(False)
    points = [(x, y, c) for x in [47.5, 64.5, 80.5] for y in [31.5, 48.5, 64.5]
              for c in ['red', 'green', 'blue', 'alpha']]
    reference = [effect.sample(c, x, y) for x, y, c in points]
    knob(effect, 'useGpu').setValue(True)
    accelerated = [effect.sample(c, x, y) for x, y, c in points]
    assert max(abs(a-b) for a,b in zip(reference, accelerated)) < 3e-3, 'GPU/CPU mismatch'
    # Recreate a node to check the host's class registration and object teardown as well.
    nuke.delete(effect)
    effect = nuke.createNode(cls, inpanel=False)
    effect.setInput(0, crop)
    assert math.isfinite(effect.sample('red', 60.5, 48.5))
    print('PASS: HDR, identity, expanded bbox, black edges, all modes, GPU/CPU parity, node recreation')


if __name__ == '__main__':
    main()
