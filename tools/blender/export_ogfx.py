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
from pathlib import Path

import bpy


STREAM_MAGIC = b"XRBM"
STREAM_VERSION = 1
STREAM_HEADER_SIZE = 96
CORNER_RECORD_SIZE = 32
MAXIMUM_TRIANGLE_COUNT = 1_000_000
FLAG_HAS_UVS = 1


class ExportError(RuntimeError):
    pass


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
        description="Compile one material-free static Blender mesh to OGFx"
    )
    parser.add_argument("--compiler", required=True, help="xrPhotonAssetCompiler path")
    parser.add_argument("--output", required=True, help="destination .ogfx path")
    parser.add_argument("--object", required=True, help="mesh object name")
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
    reject(
        bool(obj.material_slots),
        object_name,
        "material slots",
        "the first opaque milestone accepts only material-free meshes",
    )
    return obj


def finite_values(values: tuple[float, ...] | list[float]) -> bool:
    return all(math.isfinite(value) for value in values)


def extract_stream(obj: bpy.types.Object) -> bytes:
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
        reject(
            bool(mesh.materials),
            object_name,
            "evaluated materials",
            "the first opaque milestone accepts only material-free meshes",
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

        stream_size = STREAM_HEADER_SIZE + len(triangles) * 3 * CORNER_RECORD_SIZE

        uv_layer = mesh.uv_layers[0] if mesh.uv_layers else None
        flags = FLAG_HAS_UVS if uv_layer is not None else 0
        major, minor, patch = bpy.app.version
        payload = bytearray(
            struct.pack(
                "<4s7IfI12f2I",
                STREAM_MAGIC,
                STREAM_VERSION,
                STREAM_HEADER_SIZE,
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
        if len(payload) != STREAM_HEADER_SIZE:
            raise ExportError("Blender extractor internal XRBM header-size mismatch")

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
    payload = extract_stream(obj)
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
