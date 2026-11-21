import random
import struct


ORIGINAL_INSERT = """        for (int slot = 0; slot < 4; ++slot)
            if (weight > weights[slot] ||
                (weight == weights[slot] && layer < ids[slot]))
            {
                for (int move = 3; move > slot; --move)
                {
                    weights[move] = weights[move - 1];
                    ids[move] = ids[move - 1];
                }
                weights[slot] = weight;
                ids[slot] = layer;
                break;
            }"""


def replacement():
    lines = []
    for slot in range(4):
        keyword = "if" if slot == 0 else "else if"
        lines += [
            f"        {keyword} (weight > weights[{slot}] ||",
            f"            (weight == weights[{slot}] && layer < ids[{slot}]))",
            "        {",
        ]
        for move in range(3, slot, -1):
            lines += [
                f"            weights[{move}] = weights[{move - 1}];",
                f"            ids[{move}] = ids[{move - 1}];",
            ]
        lines += [
            f"            weights[{slot}] = weight;",
            f"            ids[{slot}] = layer;",
            "        }",
        ]
    return "\n".join(lines)


def apply(source):
    if source.count(ORIGINAL_INSERT) != 1:
        raise RuntimeError("Coverage top4 prototype requires exactly one unchanged insertion loop")
    return source.replace(ORIGINAL_INSERT, replacement())


def cpu_check():
    generator = random.Random(117034)
    tested = 0
    for count in range(17):
        for iteration in range(256):
            identities = generator.sample(range(256), count)
            candidates = [
                (identity, struct.unpack("<f", struct.pack("<f",
                    generator.choice((0.0, -0.0, 0.125, 0.25, 0.5, 1.0,
                                      generator.random()))))[0])
                for identity in identities
            ]
            original_ids, original_weights = [0xffffffff] * 4, [0.0] * 4
            fixed_ids, fixed_weights = [0xffffffff] * 4, [0.0] * 4
            for identity, weight in candidates:
                for slot in range(4):
                    if weight > original_weights[slot] or (
                            weight == original_weights[slot] and identity < original_ids[slot]):
                        for move in range(3, slot, -1):
                            original_weights[move] = original_weights[move - 1]
                            original_ids[move] = original_ids[move - 1]
                        original_weights[slot] = weight
                        original_ids[slot] = identity
                        break
                selected = next((slot for slot in (0, 1, 2, 3)
                                 if weight > fixed_weights[slot] or
                                 (weight == fixed_weights[slot] and identity < fixed_ids[slot])), None)
                if selected is not None:
                    fixed_weights[selected + 1:] = fixed_weights[selected:3]
                    fixed_ids[selected + 1:] = fixed_ids[selected:3]
                    fixed_weights[selected] = weight
                    fixed_ids[selected] = identity
            expected = sorted(candidates, key=lambda entry: (-entry[1], entry[0]))[:4]
            expected += [(0xffffffff, 0.0)] * (4 - len(expected))
            if fixed_ids != original_ids or fixed_ids != [entry[0] for entry in expected] or \
                    struct.pack("<4f", *fixed_weights) != struct.pack("<4f", *original_weights):
                raise RuntimeError(f"Coverage top4 mismatch at count={count}, iteration={iteration}")
            tested += 1
    return {"cases": tested, "candidateCounts": [0, 16], "weightBitsPreserved": True,
            "tieRule": "descending weight then ascending global ID",
            "includes": ["zero", "negative zero", "equal weights", "sparse permuted global IDs"]}
