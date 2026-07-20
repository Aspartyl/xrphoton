"""Extract one static Blender mesh and compile it through xrPhoton's OGFx writer.

Run this file with Blender, not a system Python interpreter. The script emits a
private XRBM stream to xrPhotonAssetCompiler over stdin; it never writes OGFx.
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import bpy


STREAM_MAGIC = b"XRBM"
STREAM_VERSION_1 = 1
STREAM_VERSION_2 = 2
STREAM_HEADER_SIZE_V1 = 96
STREAM_HEADER_SIZE_V2 = 112
CORNER_RECORD_SIZE = 32
MAXIMUM_TRIANGLE_COUNT = 1_000_000
MAXIMUM_TEXTURE_REFERENCE_BYTES = 4096
FLAG_HAS_UVS = 1
MATERIAL_FLAG_ALPHA_TESTED = 1


class ExportError(RuntimeError):
    pass


@dataclass(frozen=True)
class MaterialProfile:
    texture_reference: str
    alpha_tested: bool
    alpha_cutoff: float
    image_path: Path


def source_name(object_name: str) -> str:
    blend_path = bpy.data.filepath or "<unsaved Blender file>"
    return f'{blend_path}::object "{object_name}"'


def reject(condition: bool, object_name: str, field: str, reason: str) -> None:
    if condition:
        raise ExportError(
            f"{source_name(object_name)}: Blender extractor field {field}: {reason}."
        )


def parse_arguments() -> argparse.Namespace:
    try:
        separator = sys.argv.index("--")
    except ValueError as error:
        raise ExportError(
            "Blender exporter arguments must follow Blender's `--` separator"
        ) from error

    parser = argparse.ArgumentParser(
        description="Compile one static Blender mesh to OGFx"
    )
    parser.add_argument("--compiler", required=True, help="xrPhotonAssetCompiler path")
    parser.add_argument("--output", required=True, help="destination .ogfx path")
    parser.add_argument("--object", required=True, help="mesh object name")
    parser.add_argument(
        "--texture-root",
        help=(
            "root used to derive an extensionless logical DDS reference for "
            "the optional textured material"
        ),
    )
    return parser.parse_args(sys.argv[separator + 1 :])


def validate_blender_version() -> None:
    major, minor, patch = bpy.app.version
    if major != 5 or minor < 1:
        raise ExportError(
            "Blender extractor requires Blender 5.1.x or newer within major "
            f"version 5; found {major}.{minor}.{patch}"
        )


def paths_alias(left: Path, right: Path) -> bool:
    try:
        if left.resolve(strict=False) == right.resolve(strict=False):
            return True
    except OSError:
        pass
    try:
        return left.exists() and right.exists() and os.path.samefile(left, right)
    except OSError:
        return False


def validate_output_path(output: Path, compiler: Path) -> None:
    protected_paths = [
        (compiler, "asset compiler"),
        (Path(bpy.app.binary_path), "Blender executable"),
        (Path(__file__), "export script"),
    ]
    if bpy.data.filepath:
        protected_paths.append((Path(bpy.data.filepath), "loaded .blend source"))
    for protected_path, label in protected_paths:
        if paths_alias(output, protected_path):
            raise ExportError(
                f"output path must not identify the {label}: {protected_path}"
            )


def validate_source_object(object_name: str) -> bpy.types.Object:
    scene_matches = [obj for obj in bpy.context.scene.objects if obj.name == object_name]
    if len(scene_matches) != 1:
        raise ExportError(
            f'{source_name(object_name)}: expected exactly one active-scene object '
            f"with that name, found {len(scene_matches)}."
        )

    obj = scene_matches[0]
    reject(obj.type != "MESH", object_name, "type", "expected MESH")
    reject(
        obj.library is not None or obj.data.library is not None,
        object_name,
        "linked library data",
        "external Blender libraries are not supported yet",
    )
    reject(
        obj.override_library is not None or obj.data.override_library is not None,
        object_name,
        "library override",
        "Blender library overrides are not supported yet",
    )
    reject(obj.parent is not None, object_name, "parent", "hierarchy is not supported yet")
    reject(bool(obj.constraints), object_name, "constraints", "constraints are not supported yet")
    reject(bool(obj.modifiers), object_name, "modifiers", "modifiers are not supported yet")
    reject(
        obj.animation_data is not None,
        object_name,
        "animation data",
        "animated objects are not supported yet",
    )
    reject(
        obj.data.animation_data is not None,
        object_name,
        "mesh animation data",
        "animated meshes are not supported yet",
    )
    reject(
        obj.data.shape_keys is not None,
        object_name,
        "shape keys",
        "shape keys are not supported yet",
    )
    return obj


def finite_values(values: tuple[float, ...] | list[float]) -> bool:
    return all(math.isfinite(value) for value in values)


def canonical_texture_reference(relative_path: Path, object_name: str) -> str:
    reject(
        relative_path.suffix != ".dds",
        object_name,
        "material image path",
        "expected a lowercase .dds image beneath --texture-root",
    )
    components = relative_path.with_suffix("").parts
    reject(
        not components,
        object_name,
        "material texture reference",
        "expected at least one path component",
    )
    for component in components:
        reject(
            not component
            or any(
                not (byte.isascii() and (byte.isalnum() or byte in "_-"))
                for byte in component
            ),
            object_name,
            "material texture reference",
            "components may contain only ASCII letters, digits, underscores, and hyphens",
        )
    reference = "\\".join(components)
    encoded = reference.encode("ascii")
    reject(
        len(encoded) > MAXIMUM_TEXTURE_REFERENCE_BYTES,
        object_name,
        "material texture reference",
        f"the {MAXIMUM_TEXTURE_REFERENCE_BYTES}-byte profile cap was exceeded",
    )
    return reference


def validate_material_profile(
    obj: bpy.types.Object, texture_root: Path | None
) -> MaterialProfile | None:
    object_name = obj.name
    slots = list(obj.material_slots)
    reject(
        len(slots) > 1,
        object_name,
        "material slots",
        "at most one material is supported",
    )
    if not slots:
        reject(
            bool(obj.data.materials),
            object_name,
            "mesh materials",
            "material-free objects must have no mesh material entries",
        )
        return None

    reject(
        len(obj.data.materials) != 1,
        object_name,
        "mesh materials",
        "the one material slot must identify exactly one mesh material",
    )
    material = slots[0].material
    reject(
        material is None,
        object_name,
        "material slot 0",
        "expected a material",
    )
    reject(
        texture_root is None,
        object_name,
        "texture root",
        "--texture-root is required for a textured material",
    )
    assert material is not None
    assert texture_root is not None
    reject(
        material.library is not None or material.override_library is not None,
        object_name,
        "material library data",
        "linked and overridden materials are not supported yet",
    )
    reject(
        material.animation_data is not None,
        object_name,
        "material animation data",
        "animated materials are not supported yet",
    )
    alpha_tested = material.get("xrphoton_alpha_tested")
    reject(
        type(alpha_tested) is not bool,
        object_name,
        "material alpha-test classification",
        "set the Boolean custom property xrphoton_alpha_tested to true or false",
    )
    reject(
        material.surface_render_method != "DITHERED",
        object_name,
        "material surface render method",
        "expected Dithered rather than blended transparency",
    )
    alpha_cutoff = float(material.alpha_threshold)
    reject(
        not math.isfinite(alpha_cutoff) or alpha_cutoff < 0.0 or alpha_cutoff > 1.0,
        object_name,
        "material alpha threshold",
        "expected a finite value in [0, 1]",
    )
    reject(
        not alpha_tested and alpha_cutoff != 0.5,
        object_name,
        "opaque material alpha threshold",
        "expected the canonical unused value 0.5",
    )
    reject(
        material.node_tree is None,
        object_name,
        "material nodes",
        "the textured profile requires nodes",
    )

    node_tree = material.node_tree
    reject(
        node_tree.animation_data is not None,
        object_name,
        "material node-tree animation data",
        "animated node trees are not supported yet",
    )
    nodes = list(node_tree.nodes)
    reject(
        len(nodes) != 3,
        object_name,
        "material node count",
        "expected exactly Material Output, Principled BSDF, and Image Texture nodes",
    )
    reject(
        any(node.mute for node in nodes),
        object_name,
        "material node mute state",
        "all three validated nodes must be enabled",
    )
    outputs = [
        node
        for node in nodes
        if node.bl_idname == "ShaderNodeOutputMaterial" and node.is_active_output
    ]
    reject(
        len(outputs) != 1,
        object_name,
        "material output",
        "expected exactly one active Material Output node",
    )
    output = outputs[0]
    reject(
        output.target != "ALL",
        object_name,
        "material output target",
        "expected All so the same graph is active in every Blender renderer",
    )
    surface_links = list(output.inputs["Surface"].links)
    reject(
        len(surface_links) != 1
        or surface_links[0].from_node.bl_idname != "ShaderNodeBsdfPrincipled"
        or surface_links[0].from_socket.name != "BSDF",
        object_name,
        "material surface link",
        "expected Principled BSDF to feed Material Output Surface directly",
    )
    reject(
        bool(output.inputs["Volume"].links)
        or bool(output.inputs["Displacement"].links),
        object_name,
        "material output links",
        "volume and displacement must be unconnected",
    )
    principled = surface_links[0].from_node
    base_color_links = list(principled.inputs["Base Color"].links)
    alpha_links = list(principled.inputs["Alpha"].links)
    reject(
        len(base_color_links) != 1
        or base_color_links[0].from_node.bl_idname != "ShaderNodeTexImage"
        or base_color_links[0].from_socket.name != "Color"
        or (alpha_tested and (
            len(alpha_links) != 1
            or base_color_links[0].from_node != alpha_links[0].from_node
            or alpha_links[0].from_socket.name != "Alpha"
        ))
        or (not alpha_tested and len(alpha_links) != 0),
        object_name,
        "material image links",
        (
            "one Image Texture must feed Principled Base Color and Alpha directly"
            if alpha_tested
            else "one Image Texture must feed Principled Base Color directly with Alpha unlinked"
        ),
    )
    image_node = base_color_links[0].from_node
    reject(
        len(node_tree.links) != (3 if alpha_tested else 2)
        or any(link.is_muted or not link.is_valid for link in node_tree.links)
        or bool(image_node.inputs["Vector"].links),
        object_name,
        "material node links",
        (
            "only the direct image color, image alpha, and surface links are supported"
            if alpha_tested
            else "only the direct image color and surface links are supported"
        ),
    )
    reject(
        image_node.interpolation != "Linear"
        or image_node.extension != "REPEAT"
        or image_node.projection != "FLAT",
        object_name,
        "material image sampling",
        "expected Linear, Repeat, Flat image sampling",
    )
    texture_mapping = image_node.texture_mapping
    reject(
        texture_mapping.vector_type != "POINT"
        or tuple(texture_mapping.translation) != (0.0, 0.0, 0.0)
        or tuple(texture_mapping.rotation) != (0.0, 0.0, 0.0)
        or tuple(texture_mapping.scale) != (1.0, 1.0, 1.0)
        or texture_mapping.mapping_x != "X"
        or texture_mapping.mapping_y != "Y"
        or texture_mapping.mapping_z != "Z"
        or texture_mapping.use_min
        or texture_mapping.use_max
        or image_node.projection_blend != 0.0,
        object_name,
        "material texture mapping",
        "expected the default identity point mapping with no clipping or projection blend",
    )
    color_mapping = image_node.color_mapping
    reject(
        color_mapping.blend_factor != 0.0
        or color_mapping.brightness != 1.0
        or color_mapping.contrast != 1.0
        or color_mapping.saturation != 1.0
        or color_mapping.use_color_ramp,
        object_name,
        "material color mapping",
        "expected unmodified image color with no blend or color ramp",
    )
    image = image_node.image
    reject(image is None, object_name, "material image", "expected an image")
    assert image is not None
    reject(
        image.source != "FILE",
        object_name,
        "material image source",
        "generated, tiled, sequence, and movie images are not supported",
    )
    reject(
        image.packed_file is not None,
        object_name,
        "material image packing",
        "the runtime requires an external DDS file",
    )
    reject(
        image.colorspace_settings.is_data
        or image.colorspace_settings.name != "sRGB",
        object_name,
        "material image color space",
        "base color must use the exact sRGB color space",
    )
    reject(
        image.alpha_mode != "STRAIGHT",
        object_name,
        "material image alpha mode",
        "expected Straight alpha so Blender and the runtime interpret cutouts identically",
    )

    resolved_texture_root = texture_root.expanduser().resolve()
    reject(
        not resolved_texture_root.is_dir(),
        object_name,
        "texture root",
        f"expected an existing directory: {resolved_texture_root}",
    )
    image_path = Path(
        bpy.path.abspath(image.filepath, library=image.library)
    ).expanduser().resolve()
    reject(
        not image_path.is_file(),
        object_name,
        "material image path",
        f"expected an existing file: {image_path}",
    )
    try:
        relative_path = image_path.relative_to(resolved_texture_root)
    except ValueError as error:
        raise ExportError(
            f"{source_name(object_name)}: Blender extractor field material image path: "
            f"expected a file beneath texture root {resolved_texture_root}, found "
            f"{image_path}."
        ) from error

    return MaterialProfile(
        texture_reference=canonical_texture_reference(relative_path, object_name),
        alpha_tested=alpha_tested,
        alpha_cutoff=alpha_cutoff,
        image_path=image_path,
    )


def extract_stream(
    obj: bpy.types.Object, material_profile: MaterialProfile | None
) -> bytes:
    object_name = obj.name
    unit_scale = float(bpy.context.scene.unit_settings.scale_length)
    reject(
        not math.isfinite(unit_scale) or unit_scale <= 0.0,
        object_name,
        "scene unit scale",
        "expected a positive finite value",
    )

    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    matrix = evaluated.matrix_world
    matrix_values = [float(matrix[row][column]) for row in range(3) for column in range(4)]
    reject(
        not finite_values(matrix_values),
        object_name,
        "object transform",
        "expected finite affine values",
    )
    bottom_row = tuple(float(matrix[3][column]) for column in range(4))
    reject(
        not finite_values(list(bottom_row))
        or any(abs(value) > 1.0e-6 for value in bottom_row[:3])
        or abs(bottom_row[3] - 1.0) > 1.0e-6,
        object_name,
        "object transform bottom row",
        "expected affine (0, 0, 0, 1)",
    )

    mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    if mesh is None:
        raise ExportError(
            f"{source_name(object_name)}: Blender extractor could not evaluate the mesh."
        )
    try:
        expected_material_count = 1 if material_profile is not None else 0
        reject(
            len(mesh.materials) != expected_material_count
            or any(material is None for material in mesh.materials),
            object_name,
            "evaluated materials",
            f"expected exactly {expected_material_count} complete material entries",
        )
        reject(
            material_profile is not None
            and any(polygon.material_index != 0 for polygon in mesh.polygons),
            object_name,
            "polygon material indices",
            "every polygon must use the sole material slot 0",
        )
        reject(
            len(mesh.uv_layers) > 1,
            object_name,
            "UV layers",
            "at most one UV layer is supported",
        )
        reject(
            bool(mesh.color_attributes),
            object_name,
            "color attributes",
            "vertex colors are not supported yet",
        )
        reject(
            any(edge.is_loose for edge in mesh.edges),
            object_name,
            "loose edges",
            "every edge must belong to exported polygon geometry",
        )

        mesh.calc_loop_triangles()
        triangles = mesh.loop_triangles
        reject(
            len(triangles) == 0,
            object_name,
            "triangle count",
            "expected at least one evaluated triangle",
        )
        reject(
            len(triangles) > MAXIMUM_TRIANGLE_COUNT,
            object_name,
            "triangle count",
            f"the {MAXIMUM_TRIANGLE_COUNT:,}-triangle profile cap was exceeded",
        )
        referenced_vertices = {
            vertex_index
            for triangle in triangles
            for vertex_index in triangle.vertices
        }
        reject(
            len(referenced_vertices) != len(mesh.vertices),
            object_name,
            "loose vertices",
            "every evaluated vertex must belong to polygon geometry",
        )

        uv_layer = mesh.uv_layers[0] if mesh.uv_layers else None
        reject(
            material_profile is not None and uv_layer is None,
            object_name,
            "UV layers",
            "the textured material requires exactly one UV layer",
        )
        flags = FLAG_HAS_UVS if uv_layer is not None else 0
        texture_reference_bytes = (
            material_profile.texture_reference.encode("ascii")
            if material_profile is not None
            else b""
        )
        stream_version = (
            STREAM_VERSION_2 if material_profile is not None else STREAM_VERSION_1
        )
        stream_header_size = (
            STREAM_HEADER_SIZE_V2
            if material_profile is not None
            else STREAM_HEADER_SIZE_V1
        )
        stream_size = (
            stream_header_size
            + len(texture_reference_bytes)
            + len(triangles) * 3 * CORNER_RECORD_SIZE
        )
        major, minor, patch = bpy.app.version
        payload = bytearray(
            struct.pack(
                "<4s7IfI12f2I",
                STREAM_MAGIC,
                stream_version,
                stream_header_size,
                flags,
                len(triangles),
                major,
                minor,
                patch,
                unit_scale,
                0,
                *matrix_values,
                0,
                0,
            )
        )
        if len(payload) != STREAM_HEADER_SIZE_V1:
            raise ExportError("Blender extractor internal XRBM header-size mismatch")
        if material_profile is not None:
            payload.extend(
                struct.pack(
                    "<IfII",
                    (
                        MATERIAL_FLAG_ALPHA_TESTED
                        if material_profile.alpha_tested
                        else 0
                    ),
                    material_profile.alpha_cutoff,
                    len(texture_reference_bytes),
                    0,
                )
            )
        if len(payload) != stream_header_size:
            raise ExportError("Blender extractor internal XRBM header-size mismatch")
        payload.extend(texture_reference_bytes)

        corner_normals = mesh.corner_normals
        for triangle_index, triangle in enumerate(triangles):
            reject(
                len(triangle.loops) != 3,
                object_name,
                f"triangles[{triangle_index}].loop count",
                "expected exactly three corners",
            )
            for loop_index in triangle.loops:
                reject(
                    loop_index < 0 or loop_index >= len(mesh.loops),
                    object_name,
                    f"triangles[{triangle_index}].loop index",
                    "index is outside the evaluated loop array",
                )
                vertex_index = mesh.loops[loop_index].vertex_index
                reject(
                    vertex_index < 0 or vertex_index >= len(mesh.vertices),
                    object_name,
                    f"triangles[{triangle_index}].vertex index",
                    "index is outside the evaluated vertex array",
                )
                position = tuple(float(value) for value in mesh.vertices[vertex_index].co)
                normal = tuple(float(value) for value in corner_normals[loop_index].vector)
                uv = (
                    tuple(float(value) for value in uv_layer.uv[loop_index].vector)
                    if uv_layer is not None
                    else (0.0, 0.0)
                )
                reject(
                    not finite_values(list(position + normal + uv)),
                    object_name,
                    f"triangles[{triangle_index}].corner data",
                    "expected finite position, normal, and UV values",
                )
                payload.extend(struct.pack("<8f", *(position + normal + uv)))

        if len(payload) != stream_size:
            raise ExportError("Blender extractor internal XRBM stream-size mismatch")
        return bytes(payload)
    finally:
        evaluated.to_mesh_clear()


def run() -> int:
    arguments = parse_arguments()
    validate_blender_version()
    compiler = Path(arguments.compiler).expanduser().resolve()
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        raise ExportError(f"asset compiler is not an executable file: {compiler}")
    output = Path(arguments.output).expanduser()
    validate_output_path(output, compiler)

    obj = validate_source_object(arguments.object)
    texture_root = (
        Path(arguments.texture_root) if arguments.texture_root is not None else None
    )
    material_profile = validate_material_profile(obj, texture_root)
    if material_profile is not None and paths_alias(output, material_profile.image_path):
        raise ExportError(
            "output path must not identify the material image: "
            f"{material_profile.image_path}"
        )
    payload = extract_stream(obj, material_profile)
    diagnostic_name = source_name(obj.name)
    result = subprocess.run(
        [str(compiler), "convert-blender", diagnostic_name, str(output)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.stdout:
        sys.stdout.write(result.stdout.decode("utf-8", errors="replace"))
    if result.returncode != 0:
        diagnostic = result.stderr.decode("utf-8", errors="replace").strip()
        raise ExportError(
            diagnostic
            or f"xrPhotonAssetCompiler exited with status {result.returncode}"
        )

    print(
        f'Compiled Blender object "{obj.name}" to {Path(arguments.output)} '
        f"through the canonical OGFx writer."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except ExportError as error:
        print(f"Blender OGFx export failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
