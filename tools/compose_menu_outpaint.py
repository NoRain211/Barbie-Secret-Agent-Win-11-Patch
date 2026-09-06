from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
HD_UI = ROOT / "game-files" / "HD_UI"
SOURCE = HD_UI / "Extracted" / "main_menu_source_640x480.png"
GENERATED = HD_UI / "Generated" / "main_menu_outpaint_ai_v1.png"
OUTPUT = HD_UI / "main_menu_widescreen_v1.png"
LEFT_GENERATED = HD_UI / "Generated" / "main_menu_left_outpaint_ai_v1.png"
RIGHT_GENERATED = HD_UI / "Generated" / "main_menu_right_outpaint_ai_v1.png"
PANEL_OUTPUT = HD_UI / "main_menu_widescreen_v2.png"
COMPRESSED_OUTPUT = HD_UI / "main_menu_widescreen_compressed_640x480.png"
RIGHT_GENERATED_V2 = HD_UI / "Generated" / "main_menu_right_outpaint_ai_v2.png"
PANEL_OUTPUT_V3 = HD_UI / "main_menu_widescreen_v3.png"
COMPRESSED_OUTPUT_V3 = HD_UI / "main_menu_widescreen_v3_compressed_640x480.png"
OVERRIDE_DIR = HD_UI / "Override"

TILE_POSITIONS = (
    (0, 0),
    (256, 0),
    (448, 0),
    (0, 240),
    (256, 240),
    (448, 240),
)


def tile_with_edge_padding(image: Image.Image, x: int, y: int) -> Image.Image:
    width = min(256, image.width - x)
    height = min(256, image.height - y)
    tile = Image.new("RGBA", (256, 256))
    valid = image.crop((x, y, x + width, y + height))
    tile.alpha_composite(valid, (0, 0))

    if width < 256:
        edge = valid.crop((width - 1, 0, width, height)).resize((256 - width, height))
        tile.alpha_composite(edge, (width, 0))
    if height < 256:
        edge = tile.crop((0, height - 1, 256, height)).resize((256, 256 - height))
        tile.alpha_composite(edge, (0, height))
    return tile


def feather_right_panel(
    source: Image.Image, panel: Image.Image, width: int = 24
) -> Image.Image:
    panel = panel.copy()
    mirrored_edge = source.crop((source.width - width, 0, source.width, 480))
    mirrored_edge = mirrored_edge.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    generated_edge = panel.crop((0, 0, width, 480))
    mask = Image.new("L", (width, 480))
    for x in range(width):
        mask.paste(round(255 * x / (width - 1)), (x, 0, x + 1, 480))
    panel.paste(Image.composite(generated_edge, mirrored_edge, mask), (0, 0))
    return panel


def main() -> None:
    generated = Image.open(GENERATED).convert("RGBA")
    generated = generated.resize((854, 480), Image.Resampling.LANCZOS)
    source = Image.open(SOURCE).convert("RGBA")

    generated.alpha_composite(source, (107, 0))
    generated.convert("RGB").save(OUTPUT)

    left = Image.open(LEFT_GENERATED).convert("RGBA")
    left = left.resize((320, 480), Image.Resampling.LANCZOS).crop((0, 0, 107, 480))
    right = Image.open(RIGHT_GENERATED).convert("RGBA")
    right = right.resize((320, 480), Image.Resampling.LANCZOS).crop(
        (213, 0, 320, 480)
    )

    panel_composite = Image.new("RGBA", (854, 480))
    panel_composite.alpha_composite(left, (0, 0))
    panel_composite.alpha_composite(source, (107, 0))
    panel_composite.alpha_composite(right, (747, 0))
    panel_composite.convert("RGB").save(PANEL_OUTPUT)

    compressed = panel_composite.resize((640, 480), Image.Resampling.LANCZOS)
    compressed.convert("RGB").save(COMPRESSED_OUTPUT)

    right_v2 = Image.open(RIGHT_GENERATED_V2).convert("RGBA")
    right_v2 = right_v2.resize((320, 480), Image.Resampling.LANCZOS).crop(
        (213, 0, 320, 480)
    )
    right_v2 = feather_right_panel(source, right_v2)
    panel_composite_v3 = Image.new("RGBA", (854, 480))
    panel_composite_v3.alpha_composite(left, (0, 0))
    panel_composite_v3.alpha_composite(source, (107, 0))
    panel_composite_v3.alpha_composite(right_v2, (747, 0))
    panel_composite_v3.convert("RGB").save(PANEL_OUTPUT_V3)

    compressed_v3 = panel_composite_v3.resize((640, 480), Image.Resampling.LANCZOS)
    compressed_v3.convert("RGB").save(COMPRESSED_OUTPUT_V3)
    OVERRIDE_DIR.mkdir(exist_ok=True)
    for index, (x, y) in enumerate(TILE_POSITIONS):
        tile = tile_with_edge_padding(compressed_v3, x, y)
        tile.save(OVERRIDE_DIR / f"main_menu_tile_{index}.bmp")

    print(OUTPUT)
    print(PANEL_OUTPUT)
    print(COMPRESSED_OUTPUT)
    print(PANEL_OUTPUT_V3)
    print(COMPRESSED_OUTPUT_V3)


if __name__ == "__main__":
    main()
