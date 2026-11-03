#!/usr/bin/env python3
"""Generates src/shared/capsule_models.cpp: capsule sizes fitted to each player model of the game.

Usage: generate_capsule_models.py <path to the game's main folder> <output .cpp>
Needs numpy. Reads xmodel, xmodelparts and xmodelsurfs out of the game's iw_*.iwd files.

Gear is left out of the fit, so it does not count as a hit. The mesh falls apart into separate pieces
along its edges. Cloth blends over several bones, gear is on an equipment bone or stiff on one bone:
- all of the piece is on equipment bones (j_back_equip, j_spade, j_gasmask, j_pack...)
- all of the piece is on one bone of the torso and it is on the back of the body

Numbers are in the coordinates of the model's default pose: x forward, y left, z up. Percentiles are used
instead of the extremes, so a strap or a buckle that was not recognised as gear does not move a capsule.
"""
import glob
import math
import os
import re
import struct
import sys
import zipfile

import numpy as np

ARCHIVES = {}


def open_archives(main_dir):
    for path in sorted(glob.glob(os.path.join(main_dir, "iw_*.iwd"))):
        archive = zipfile.ZipFile(path)
        for name in archive.namelist():
            if name.lower().startswith("xmodel"):
                ARCHIVES[name.lower()] = (archive, name)


def read(name):
    archive, real_name = ARCHIVES[name.lower()]
    return archive.read(real_name)


def qmul(a, b):
    x1, y1, z1, w1 = a
    x2, y2, z2, w2 = b
    return (w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2)


def qinv(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]))
    return (v[0] + w * t[0] + y * t[2] - z * t[1], v[1] + w * t[1] + z * t[0] - x * t[2], v[2] + w * t[2] + x * t[1] - y * t[0])


def read_parts(name):
    """Bone names with their positions and rotations in the default pose"""
    data = read("xmodelparts/" + name)
    num_bones, num_roots = struct.unpack_from("<HH", data, 2)
    pos = 6
    parents = [-1] * num_roots
    translations = [(0.0, 0.0, 0.0)] * num_roots
    rotations = [(0.0, 0.0, 0.0, 1.0)] * num_roots
    for _ in range(num_bones):
        parent = data[pos]
        translation = struct.unpack_from("<3f", data, pos + 1)
        quat = [v / 32767.0 for v in struct.unpack_from("<3h", data, pos + 13)]
        pos += 19
        parents.append(parent)
        translations.append(translation)
        rotations.append((quat[0], quat[1], quat[2], math.sqrt(max(0.0, 1.0 - sum(v * v for v in quat)))))

    names = []
    for _ in range(num_bones + num_roots):
        end = data.index(0, pos)
        names.append(data[pos:end].decode().lower())
        pos = end + 1

    origins = [None] * len(names)
    quats = [None] * len(names)
    for i in range(len(names)):
        if parents[i] < 0:
            origins[i] = (0.0, 0.0, 0.0)
            quats[i] = (0.0, 0.0, 0.0, 1.0)
        else:
            quats[i] = qmul(quats[parents[i]], rotations[i])
            rotated = qrot(quats[parents[i]], translations[i])
            origins[i] = tuple(origins[parents[i]][k] + rotated[k] for k in range(3))
    return names, origins, quats


def is_unit_vector(data, pos):
    if pos + 12 > len(data):
        return False
    return abs(sum(v * v for v in struct.unpack_from("<3f", data, pos)) - 1.0) < 0.02


def read_mesh(name):
    """Vertices in the default pose, the bone that moves each of them most and the triangles"""
    bone_names, origins, quats = read_parts(name)
    data = read("xmodelsurfs/" + name)
    num_surfaces = struct.unpack_from("<H", data, 2)[0]
    pos = 4
    vertices = []
    bones = []
    triangles = []
    for _ in range(num_surfaces):
        num_vertices, num_triangles, rigid_bone = struct.unpack_from("<HHh", data, pos + 1)
        pos += 7
        if rigid_bone == -1:
            pos += 2
        base = len(vertices)
        for i in range(num_vertices):
            pos += 48  # normal, color, uv, binormal, tangent
            if rigid_bone != -1:
                bone = rigid_bone
                offset = struct.unpack_from("<3f", data, pos)
                pos += 12
            else:
                extra = data[pos]
                bone = struct.unpack_from("<H", data, pos + 1)[0]
                offset = struct.unpack_from("<3f", data, pos + 3)
                pos += 15
                if extra:
                    for size in (16 * extra + 1, 16 * extra):
                        if is_unit_vector(data, pos + size) or i == num_vertices - 1:
                            pos += size
                            break
                    else:
                        raise ValueError("lost track of the vertices of " + name)
            rotated = qrot(quats[bone], offset)
            vertices.append(tuple(origins[bone][k] + rotated[k] for k in range(3)))
            bones.append(bone_names[bone])
        for _ in range(num_triangles):
            triangles.append(tuple(base + v for v in struct.unpack_from("<3H", data, pos)))
            pos += 6
    return bone_names, origins, quats, np.array(vertices), np.array(bones), triangles


def first_lod(model):
    data = read("xmodel/" + model)
    end = data.index(0, 31)
    return data[31:end].decode().lower()


TORSO_BONES = ("j_spine1", "j_spine2", "j_spine4", "back_up", "back_mid", "back_low", "pelvis", "j_mainroot", "j_clavicle_le", "j_clavicle_ri", "j_neck")
GEAR_BONES = ("equip", "spade", "gasmask", "j_pack")
ARM_BONES = ("shoulder", "elbow", "wrist", "thumb", "index", "mid_", "ring", "pinky")
LEG_BONES = ("hip", "knee", "ankle", "ball", "legextra", "shorts")


def has_any(name, parts):
    return any(p in name for p in parts)


def find_gear(vertices, bones, triangles):
    """True for each vertex that belongs to gear, see the top of this file"""
    parent = list(range(len(vertices)))

    def root(v):
        while parent[v] != v:
            parent[v] = parent[parent[v]]
            v = parent[v]
        return v

    for a, b, c in triangles:
        parent[root(a)] = root(b)
        parent[root(a)] = root(c)
    pieces = np.array([root(v) for v in range(len(vertices))])

    gear = np.zeros(len(vertices), bool)
    for piece in set(pieces):
        mask = pieces == piece
        names = set(bones[mask])
        on_gear_bone = sum(has_any(b, GEAR_BONES) for b in bones[mask]) > mask.sum() * 0.5
        on_back = len(names) == 1 and next(iter(names)) in TORSO_BONES and vertices[mask][:, 0].mean() < -7.5
        if on_gear_bone or on_back:
            gear[mask] = True
    return gear


def clamp(v, low, high):
    return float(max(low, min(high, v)))


class Skeleton:
    def __init__(self, names, origins, quats):
        self.bones = {n: (np.array(origins[i]), quats[i]) for i, n in enumerate(names)}

    def origin(self, name):
        return self.bones[name][0]

    def point(self, name, local):
        origin, quat = self.bones[name]
        return origin + np.array(qrot(quat, local))


# Same as capsule_trace.cpp
SLICE_ALONG = (0.04, 0.28, 0.52, 0.76, 0.90)
SHARED_SLICES = ((6.0, 2.0, 0.8), (6.0, 1.8, 0.7), (6.3, 2.1, 0.2), (6.6, 2.2, -1.1), (5.4, 3.6, -1.2))
SHARED_TRAPEZIUS = (3.2, 3.0, -1.4)
TRAPEZIUS_ALONG = 0.8
THIGH_START_OFFSET = 3.0


def slice_center(skeleton, along):
    lower, middle, upper = skeleton.origin("j_spine1"), skeleton.origin("j_spine2"), skeleton.origin("j_spine4")
    shoulders = (skeleton.origin("j_shoulder_le") + skeleton.origin("j_shoulder_ri")) * 0.5 - upper
    length_lower = np.linalg.norm(middle - lower)
    length_upper = np.linalg.norm(upper - middle)
    distance = along * (length_lower + length_upper)
    if distance < length_lower:
        center = lower + (middle - lower) * (distance / length_lower)
    else:
        center = middle + (upper - middle) * ((distance - length_lower) / length_upper)
    return center + shoulders * along


def fit_slice(points, base_x, shared):
    if len(points) < 12:
        return shared
    back, front = np.percentile(points[:, 0], (4, 96))
    radius = clamp((front - back) * 0.5, 3.5, 8.5)
    width = clamp(np.percentile(np.abs(points[:, 1]), 97), 4.0, 10.5)
    return (radius, max(0.5, width - radius), (front + back) * 0.5 - base_x)


def limb_radii(points, a, b, shared, low, high, start_from=-0.1, end_from=0.65, end_to=1.0):
    """Radius near each end of the capsule from a to b"""
    if len(points) < 8:
        return None
    axis = b - a
    length = np.linalg.norm(axis)
    along = (points - a).dot(axis) / (length * length)
    radial = np.linalg.norm((points - a) - np.outer(along, axis), axis=1)
    start = radial[(along >= start_from) & (along < 0.35)]
    end = radial[(along >= end_from) & (along < end_to)]
    return (clamp(np.percentile(start, 92), low, high) if len(start) >= 4 else shared[0],
            clamp(np.percentile(end, 92), low, high) if len(end) >= 4 else shared[1])


def smaller(left, right, shared):
    """A body is symmetric, gear is not: a thigh with a pouch on it is wider than the other one"""
    if left is None and right is None:
        return shared
    if left is None or right is None:
        return left or right
    return (min(left[0], right[0]), min(left[1], right[1]))


def fit_body(model):
    names, origins, quats, vertices, bones, triangles = read_mesh(first_lod(model))
    skeleton = Skeleton(names, origins, quats)
    keep = ~find_gear(vertices, bones, triangles)
    points = vertices[keep]
    point_bones = bones[keep]
    is_torso = np.array([not has_any(b, ARM_BONES) and not has_any(b, LEG_BONES) and b != "j_head" for b in point_bones])
    torso = points[is_torso]

    slices = []
    for along, shared in zip(SLICE_ALONG, SHARED_SLICES):
        center = slice_center(skeleton, along)
        band = torso[np.abs(torso[:, 2] - center[2]) < 2.0]
        base_x = center[0]
        slices.append(fit_slice(band, base_x, shared))

    upper, neck = skeleton.origin("j_spine4"), skeleton.origin("j_neck")
    center = upper + (neck - upper) * TRAPEZIUS_ALONG
    band = torso[(torso[:, 2] > center[2] - 1.2) & (torso[:, 2] < center[2] + 2.5) & (np.abs(torso[:, 1]) < 6.5)]
    trapezius = SHARED_TRAPEZIUS
    if len(band) >= 12:
        radius = clamp(np.percentile(band[:, 2], 97) - center[2], 2.2, 3.4)
        trapezius = (radius, clamp(np.percentile(np.abs(band[:, 1]), 97) - radius, 1.0, 4.0), np.percentile(band[:, 0], 4) + radius - center[0])

    band = torso[(torso[:, 2] > 36.0) & (torso[:, 2] < 39.0)]
    hips = 4.8
    if len(band) >= 12:
        # The ends of this capsule are the hip joints, so its radius is also how far it sticks out sideways
        back, front = np.percentile(band[:, 0], (4, 96))
        sideways = np.percentile(np.abs(band[:, 1]), 97) - abs(skeleton.origin("j_hip_le")[1])
        hips = clamp(min((front - back) * 0.5, sideways), 3.5, 5.5)

    def of_side(side, parts):
        mask = np.array([has_any(b, parts) and b.endswith(side) for b in point_bones])
        return points[mask]

    def both(parts, a_of, b_of, shared, low, high, **kwargs):
        return smaller(*[limb_radii(of_side(side, parts), a_of(side), b_of(side), shared, low, high, **kwargs) for side in ("_le", "_ri")], shared)

    def knee(side):
        return skeleton.point("j_knee" + side, (0.0, 1.15 if side == "_le" else -1.15, 0.0))

    def thigh_top(side):
        hip = skeleton.origin("j_hip" + side)
        axis = knee(side) - hip
        return hip + axis * (THIGH_START_OFFSET / np.linalg.norm(axis))

    def heel(side):
        return skeleton.point("j_ankle" + side, (2.5, -0.4, 0.0) if side == "_le" else (-2.5, 0.4, 0.0))

    def toe(side):
        return skeleton.point("j_ball" + side, (2.4, 0.7, 0.0) if side == "_le" else (-2.4, -0.7, 0.0))

    thigh = both(("j_hip", "j_knee", "shorts", "legextra"), thigh_top, knee, (5.0, 4.3), 3.0, 6.5)
    shin = both(("j_knee", "j_ankle", "legextra"), knee, lambda s: skeleton.origin("j_ankle" + s), (4.0, 2.9), 2.2, 5.5, end_to=0.85)

    # Long coats are much wider than the legs inside of them. Same levels as cap_skirt_shapes.
    not_arm = points[np.array([not has_any(b, ARM_BONES) for b in point_bones])]
    hip, knee_left, ankle = skeleton.origin("j_hip_le"), skeleton.origin("j_knee_le"), skeleton.origin("j_ankle_le")
    skirt = []
    leg_y = abs(hip[1])
    for center, leg_radius, last in ((hip + (knee_left - hip) * 0.35, thigh[0], False), (hip + (knee_left - hip) * 0.85, thigh[1], False), (knee_left + (ankle - knee_left) * 0.25, shin[0], True)):
        band = not_arm[np.abs(not_arm[:, 2] - center[2]) < 3.5]
        level = (0.0, 0.0, 0.0)
        if len(band) >= 12:
            back, front = np.percentile(band[:, 0], (4, 96))
            depth = (front - back) * 0.5
            if depth > leg_radius + 1.0:
                # Round in the width of the coat, the capsule must not stick out at the sides
                radius = clamp(np.percentile(np.abs(band[:, 1]), 97) - leg_y, leg_y, 8.0)
                if last:
                    # Nor below the hem
                    wide = not_arm[np.abs(not_arm[:, 0] - center[0]) > leg_radius + 1.5]
                    wide = wide[wide[:, 2] < center[2] + 4.0]
                    if len(wide):
                        radius = clamp(center[2] - np.percentile(wide[:, 2], 5) + 0.5, 3.0, radius)
                radius = min(radius, depth)
                level = (radius, depth - radius, (front + back) * 0.5 - center[0])
        skirt.append(level)
    # A long coat hangs past the thigh. A wide belt or the top of a thigh alone is not one.
    if skirt[1][0] == 0.0:
        skirt = [(0.0, 0.0, 0.0)] * 3

    return {
        "name": model,
        "skirt": skirt,
        "slices": slices,
        "trapezius": trapezius,
        "hips": hips,
        "upperArm": both(("j_shoulder", "j_elbow"), lambda s: skeleton.origin("j_shoulder" + s), lambda s: skeleton.origin("j_elbow" + s), (2.6, 2.5), 1.6, 3.8, start_from=0.12),
        "forearm": both(("j_elbow", "j_wrist"), lambda s: skeleton.origin("j_elbow" + s), lambda s: skeleton.origin("j_wrist" + s), (2.3, 1.9), 1.5, 4.0),
        "thigh": thigh,
        "shin": shin,
        "foot": both(("j_ankle", "j_ball"), heel, toe, (2.45, 2.15), 1.8, 3.5),
    }


def to_head_space(names, origins, quats, vertices):
    head = names.index("j_head")
    inverse = qinv(quats[head])
    return np.array([qrot(inverse, tuple(v[k] - origins[head][k] for k in range(3))) for v in vertices])


def fit_head(model):
    names, origins, quats, vertices, _, _ = read_mesh(first_lod(model))
    if "j_head" not in names:
        return None
    p = to_head_space(names, origins, quats, vertices)
    x, y, z = p[:, 0], p[:, 1], np.abs(p[:, 2])

    top = np.percentile(x, 99.7)
    radius = clamp(np.percentile(z[(x >= 2) & (x <= 7)], 98), 3.5, 4.8)
    back = np.percentile(y[x >= 3], 1)
    skull_x, skull_y = top - radius, back + radius

    nose = min(np.percentile(y[(x >= 1.5) & (x <= 5)], 99), 7.2)
    chin = min(np.percentile(y[(x >= -1.5) & (x <= 1.5)], 99), 7.2)
    face_radius = clamp(np.percentile(z[(x >= -0.5) & (x <= 2)], 98), 2.4, 3.4)
    jaw = clamp(np.percentile(x[(y > 2) & (x > -3.5)], 3), -3.0, -0.5)
    top_radius = face_radius * 0.93

    return {
        "name": model,
        "skull": ((skull_x - 2.3, skull_y + 0.6), (skull_x, skull_y), radius, radius),
        "face": ((min(jaw + face_radius, 3.0), chin - face_radius), (4.2, nose - top_radius), face_radius, top_radius),
    }


def fit_headgear(model, helmet_bone):
    names, origins, quats, vertices, _, _ = read_mesh(first_lod(model))
    if "j_head" in names:
        p = to_head_space(names, origins, quats, vertices)
    elif "j_helmet" in names:
        translation, rotation = helmet_bone
        p = np.array([tuple(translation[k] + qrot(rotation, tuple(v))[k] for k in range(3)) for v in vertices])
    else:
        return None
    x, y, z = p[:, 0], p[:, 1], np.abs(p[:, 2])

    top = x.max()
    # The dome only: the brim is lower down and sticks out
    band = (x >= top - 4.5) & (x <= top - 2.0)
    if band.sum() < 6:
        band = x >= top - 5.0
    radius = clamp(np.percentile(z[band], 97), 2.5, 5.5)
    front, back = np.percentile(y[band], 97), np.percentile(y[band], 3)
    center_y = (front + back) * 0.5
    center_x = top - radius
    return {"name": model, "dome": ((center_x - 1.5, center_y), (center_x, center_y), radius, radius)}


def f(v):
    return "%.2ff" % v


def write_table(models, output):
    bodies, heads, headgear = models
    out = ["// Generated by tools/capsule_fit/generate_capsule_models.py, do not edit by hand.",
           '#include "capsule_trace.h"', "", "#include <stddef.h>", ""]

    def pair(p):
        return "{ %s, %s }" % (f(p[0]), f(p[1]))

    def shape(s):
        return "{ { %s, %s, 0.0f }, { %s, %s, 0.0f }, %s, %s }" % (f(s[0][0]), f(s[0][1]), f(s[1][0]), f(s[1][1]), f(s[2]), f(s[3]))

    def sl(s):
        return "{ %s, %s, %s }" % (f(s[0]), f(s[1]), f(s[2]))

    out.append("static const capsuleBodyFit_t capsule_bodies[] =\n{")
    for b in bodies:
        out.append('\t{ "%s", { %s }, %s, %s, %s, %s, %s, %s, %s, { %s } },' % (
            b["name"], ", ".join(sl(s) for s in b["slices"]), sl(b["trapezius"]), f(b["hips"]),
            pair(b["upperArm"]), pair(b["forearm"]), pair(b["thigh"]), pair(b["shin"]), pair(b["foot"]),
            ", ".join(sl(level) for level in b["skirt"])))
    out.append("};\n")

    out.append("static const capsuleHeadFit_t capsule_heads[] =\n{")
    for h in heads:
        out.append('\t{ "%s", %s, %s },' % (h["name"], shape(h["skull"]), shape(h["face"])))
    out.append("};\n")

    out.append("static const capsuleHeadgearFit_t capsule_headgear[] =\n{")
    for h in headgear:
        out.append('\t{ "%s", %s },' % (h["name"], shape(h["dome"])))
    out.append("};\n")

    out.append("""static int capsule_compare_name(const char* a, const char* b)
{
	for (;; a++, b++)
	{
		char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if (ca != cb)
			return ca < cb ? -1 : 1;
		if (ca == 0)
			return 0;
	}
}

// The tables are sorted by name
template <typename T, int N>
static const T* capsule_find(const T (&table)[N], const char* name)
{
	int low = 0;
	int high = N - 1;
	while (low <= high)
	{
		int middle = (low + high) / 2;
		int order = capsule_compare_name(name, table[middle].name);
		if (order == 0)
			return &table[middle];
		if (order < 0)
			high = middle - 1;
		else
			low = middle + 1;
	}
	return NULL;
}

void capsule_fit_add_model(capsuleFit_t* fit, const char* modelName)
{
	if (const capsuleBodyFit_t* body = capsule_find(capsule_bodies, modelName))
		fit->body = body;
	else if (const capsuleHeadFit_t* head = capsule_find(capsule_heads, modelName))
		fit->head = head;
	else if (const capsuleHeadgearFit_t* headgear = capsule_find(capsule_headgear, modelName))
		fit->headgear = headgear;
}
""")
    with open(output, "w", newline="\n") as file:
        file.write("\n".join(out))


def main():
    open_archives(sys.argv[1])
    models = sorted(k[len("xmodel/"):] for k in ARCHIVES if k.startswith("xmodel/"))

    # Headgear has only the helmet bone, its position comes from a head that has both
    names, origins, quats = read_parts(first_lod("head_us_ranger_david"))
    head, helmet = names.index("j_head"), names.index("j_helmet")
    inverse = qinv(quats[head])
    helmet_bone = (qrot(inverse, tuple(origins[helmet][k] - origins[head][k] for k in range(3))), qmul(inverse, quats[helmet]))

    bodies = [fit_body(m) for m in models if m.startswith("playerbody_")]
    heads = [h for h in (fit_head(m) for m in models if m.startswith("head_")) if h]
    headgear = [h for h in (fit_headgear(m, helmet_bone) for m in models if m.startswith(("helmet_", "hat_"))) if h]
    write_table((bodies, heads, headgear), sys.argv[2])
    print("%d bodies, %d heads, %d headgear" % (len(bodies), len(heads), len(headgear)))


if __name__ == "__main__":
    main()
