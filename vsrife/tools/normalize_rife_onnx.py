#!/usr/bin/env python3
"""Normalize a vs-mlrt / dynamo-exported RIFE ONNX to the naming and input
layout that vsrife's vendored rife_plan expects.

The libaji native kernels identify a RIFE graph by node names:
    /Split                                  Split(input, [6, 1, 2, 2])
    /encode/...                             optional feature encoder convs
    /blockN/conv0/conv0.0/conv0.0.0/Conv
    /blockN/conv0/conv0.1/conv0.1.0/Conv
    /blockN/convblock/convblock.K/{conv/Conv,Mul,Add,relu/LeakyRelu}
    /blockN/lastconv/lastconv.0/ConvTranspose
    /blockN/lastconv/lastconv.1/DepthToSpace
    /blockN/Resize                          only when the block runs at 1/s

Models exported by torch-dynamo (opset 21) carry the same structure but name
every node `Conv_6`, `Mul_11`, ... and split the input as [3, 3, 1, 2, 2]
(two frames split apart), which rife_plan reads as "ensemble". This tool
rewrites exactly that: names, and the input Split, without touching weights.

    python normalize_rife_onnx.py in.onnx out.onnx [--check]

Only the graph description is used at runtime; the model is never executed.
"""
import re
import sys

import onnx
from onnx import TensorProto, helper, numpy_helper


def _set_split(node, sizes, name):
    """Point a Split node at a fresh int64 initializer holding `sizes`."""
    const = numpy_helper.from_array(
        __import__("numpy").asarray(sizes, dtype="int64"), name=name
    )
    if len(node.input) > 1 and node.input[1]:
        node.input[1] = name
    else:
        node.input.append(name)
    return const


def normalize(model):
    g = model.graph
    nodes = list(g.node)
    by_out = {o: n for n in nodes for o in n.output}
    inits = {i.name: i for i in g.initializer}
    extra_inits = []

    def consumers(name):
        return [n for n in nodes if name in n.input]

    # ---- 1. input Split: [3, 3, 1, 2, 2] -> [6, 1, 2, 2] -------------------
    inp = g.input[0].name
    sp = None
    for n in nodes:
        if n.op_type == "Split" and n.input and n.input[0] == inp:
            sp = n
            break
    if sp is None:
        raise SystemExit("no Split on the graph input")
    sizes = list(numpy_helper.to_array(inits[sp.input[1]])) if len(sp.input) > 1 else None
    if sizes == [3, 3, 1, 2, 2]:
        a, b, t, grid, mult = sp.output
        g.initializer.append(_set_split(sp, [6, 1, 2, 2], "vsrife_split_sizes"))
        del sp.output[:]
        sp.output.extend(["vsrife_in6", "vsrife_t", "vsrife_grid", "vsrife_mult"])
        sp.name = "/Split"
        # A+B were only ever concatenated back together: fold them to the 6ch tensor
        for n in nodes:
            if n.op_type == "Concat" and list(n.input) == [a, b, t]:
                n.input[0] = "vsrife_in6"
                n.input[1] = "vsrife_t"
                del n.input[2]
            elif n.op_type == "Concat" and list(n.input) == [a, b]:
                old = n.output[0]
                for m in nodes:
                    if m is n:
                        continue
                    m.input[:] = ["vsrife_in6" if x == old else x for x in m.input]
                nodes.remove(n)
                g.node.remove(n)
        for n in nodes:
            if n is sp:
                continue
            n.input[:] = [
                {"vsrife_in6": "vsrife_in6"}.get(x) if x in (a, b) else
                "vsrife_t" if x == t else "vsrife_grid" if x == grid else
                "vsrife_mult" if x == mult else x
                for x in n.input
            ]
    elif sizes != [6, 1, 2, 2]:
        raise SystemExit(f"unexpected split sizes {sizes}")
    else:
        sp.name = "/Split"

    # ---- 2. conv names from the weight initializer names --------------------
    pats = [
        (re.compile(r"block(\d+)\.conv0\.0\.0\.weight$"), "/block{0}/conv0/conv0.0/conv0.0.0/Conv"),
        (re.compile(r"block(\d+)\.conv0\.1\.0\.weight$"), "/block{0}/conv0/conv0.1/conv0.1.0/Conv"),
        (re.compile(r"block(\d+)\.convblock\.(\d+)\.conv\.weight$"),
         "/block{0}/convblock/convblock.{1}/conv/Conv"),
        (re.compile(r"block(\d+)\.lastconv\.0\.weight$"), "/block{0}/lastconv/lastconv.0/ConvTranspose"),
    ]
    enc = 0
    for n in nodes:
        if n.op_type not in ("Conv", "ConvTranspose"):
            continue
        w = n.input[1] if len(n.input) > 1 else ""
        for rx, fmt in pats:
            m = rx.search(w)
            if m:
                n.name = fmt.format(*m.groups())
                break
        else:
            if re.match(r"(encode|encoder)\.", w):
                n.name = f"/encode/conv{enc}/Conv"
                enc += 1

    # ---- 3. residual triples, from the beta initializers -------------------
    for name in inits:
        m = re.match(r"block(\d+)\.convblock\.(\d+)\.beta$", name)
        if not m:
            continue
        b, k = m.group(1), m.group(2)
        pre = f"/block{b}/convblock/convblock.{k}"
        mul = next((n for n in nodes if n.op_type == "Mul" and name in n.input), None)
        if mul is None:
            raise SystemExit(f"no Mul for {name}")
        mul.name = pre + "/Mul"
        add = next((n for n in nodes if n.op_type == "Add" and mul.output[0] in n.input), None)
        if add is None:
            raise SystemExit(f"no Add after {mul.name}")
        add.name = pre + "/Add"
        act = next((n for n in nodes if n.op_type in ("LeakyRelu", "Relu") and add.output[0] in n.input), None)
        if act is not None:
            act.name = pre + "/relu/LeakyRelu"

    # ---- 4. block scale Resize + DepthToSpace ------------------------------
    for n in nodes:
        m = re.match(r"/block(\d+)/lastconv/lastconv\.0/ConvTranspose$", n.name)
        if not m:
            continue
        pre = f"/block{m.group(1)}/lastconv"
        d2s = next((x for x in nodes if x.op_type == "DepthToSpace" and n.output[0] in x.input), None)
        if d2s is not None:
            d2s.name = pre + "/lastconv.1/DepthToSpace"
    for n in nodes:
        m = re.match(r"/block(\d+)/conv0/conv0\.0/conv0\.0\.0/Conv$", n.name)
        if not m:
            continue
        src = by_out.get(n.input[0])
        if src is not None and src.op_type == "Resize":
            src.name = f"/block{m.group(1)}/Resize"

    # ---- 5. names must be unique ------------------------------------------
    seen = {}
    for n in nodes:
        if n.name in seen:
            raise SystemExit(f"duplicate node name {n.name}")
        seen[n.name] = n
    return model


def preview(model):
    """Print what rife_plan will see, so a mismatch is obvious before running."""
    g = model.graph
    names = [n.name for n in g.node]
    blocks = sorted({int(m.group(1)) for m in (re.match(r"/block(\d+)/", x) for x in names) if m})
    print("split      :", "/Split" in names)
    print("encoder    :", sum(1 for x in names if x.startswith("/encode/")))
    for b in blocks:
        pre = f"/block{b}/"
        res = sorted(int(m.group(1)) for m in (re.match(re.escape(pre) + r"convblock/convblock\.(\d+)/conv/Conv$", x) for x in names) if m)
        rz = next((n for n in g.node if n.name == pre + "Resize"), None)
        s = 1
        if rz is not None:
            sc = numpy_helper.to_array({i.name: i for i in g.initializer}[rz.input[2]])
            s = round(1.0 / float(sc[2]))
        print(f"block{b}     : s={s} residuals={len(res)} resize={'yes' if rz else 'no'}")
    print("names      :", len(names), "unique:", len(set(names)) == len(names))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    model = onnx.load(src)
    normalize(model)
    preview(model)
    onnx.checker.check_model(model)
    onnx.save(model, dst)
    print("wrote", dst)
